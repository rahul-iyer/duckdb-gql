#include "gql_optimizer.hpp"

#include "gql_catalog.hpp"
#include "gql_csr.hpp"

#include "duckdb/common/string_util.hpp"

namespace duckdb {

static vector<bool> EmptyBindingSet(idx_t binding_count) {
	return vector<bool>(binding_count, false);
}

static void UnionInto(vector<bool> &target, const vector<bool> &source) {
	if (target.size() != source.size()) {
		throw InternalException("Incompatible GQL logical binding sets");
	}
	for (idx_t index = 0; index < target.size(); index++) {
		target[index] = target[index] || source[index];
	}
}

static vector<bool> Intersection(const vector<bool> &left, const vector<bool> &right) {
	if (left.size() != right.size()) {
		throw InternalException("Incompatible GQL logical binding sets");
	}
	vector<bool> result(left.size(), false);
	for (idx_t index = 0; index < left.size(); index++) {
		result[index] = left[index] && right[index];
	}
	return result;
}

static bool IsSubset(const vector<bool> &subset, const vector<bool> &superset) {
	if (subset.size() != superset.size()) {
		throw InternalException("Incompatible GQL logical binding sets");
	}
	for (idx_t index = 0; index < subset.size(); index++) {
		if (subset[index] && !superset[index]) {
			return false;
		}
	}
	return true;
}

static vector<bool> ExpressionBindings(const GqlBoundExpression &expression, idx_t binding_count) {
	auto result = EmptyBindingSet(binding_count);
	auto collect = [&](auto &self, const GqlBoundExpression &entry) -> void {
		if (entry.binding_index != DConstants::INVALID_INDEX && entry.binding_source == GqlBinding::Source::GRAPH) {
			if (entry.binding_index >= binding_count) {
				throw InternalException("GQL expression binding is outside the plan");
			}
			result[entry.binding_index] = true;
		}
		if (entry.left) {
			self(self, *entry.left);
		}
		if (entry.right) {
			self(self, *entry.right);
		}
		for (const auto &argument : entry.arguments) {
			if (!argument) {
				throw InternalException("GQL expression contains an empty argument");
			}
			self(self, *argument);
		}
	};
	collect(collect, expression);
	return result;
}

static const GqlLogicalProperties &InferProperties(const shared_ptr<GqlLogicalOperator> &operation,
                                                   idx_t binding_count) {
	if (!operation) {
		throw InternalException("Cannot infer properties for an empty GQL plan");
	}
	if (operation->properties_valid) {
		return operation->properties;
	}
	GqlLogicalProperties result;
	result.output_bindings = EmptyBindingSet(binding_count);
	result.required_bindings = EmptyBindingSet(binding_count);
	result.nullable_bindings = EmptyBindingSet(binding_count);
	result.correlated_bindings = EmptyBindingSet(binding_count);

	switch (operation->type) {
	case GqlLogicalOperatorType::UNIT:
		if (operation->child) {
			throw InternalException("GQL UNIT cannot have a child");
		}
		result.minimum_cardinality = 1;
		break;
	case GqlLogicalOperatorType::MATCH: {
		const auto &match = operation->Cast<GqlLogicalMatch>();
		if (operation->child || match.patterns.empty()) {
			throw InternalException("Invalid GQL MATCH stage");
		}
		for (const auto &pattern : match.patterns) {
			for (const auto &element : pattern.elements) {
				if (element.binding_index >= binding_count) {
					throw InternalException("GQL MATCH binding is outside the plan");
				}
				result.output_bindings[element.binding_index] = true;
			}
		}
		break;
	}
	case GqlLogicalOperatorType::FILTER: {
		const auto &filter = operation->Cast<GqlLogicalFilter>();
		if (!filter.predicate) {
			throw InternalException("GQL FILTER has no predicate");
		}
		const auto &child = InferProperties(filter.child, binding_count);
		result = child;
		auto dependencies = ExpressionBindings(*filter.predicate, binding_count);
		for (idx_t index = 0; index < binding_count; index++) {
			if (dependencies[index] && !child.output_bindings[index]) {
				result.required_bindings[index] = true;
			}
		}
		result.minimum_cardinality = 0;
		break;
	}
	case GqlLogicalOperatorType::INNER_APPLY:
	case GqlLogicalOperatorType::LEFT_APPLY: {
		const shared_ptr<GqlLogicalOperator> *right_ptr;
		if (operation->type == GqlLogicalOperatorType::INNER_APPLY) {
			right_ptr = &operation->Cast<GqlLogicalInnerApply>().right;
		} else {
			right_ptr = &operation->Cast<GqlLogicalLeftApply>().right;
		}
		const auto &left = InferProperties(operation->child, binding_count);
		const auto &right = InferProperties(*right_ptr, binding_count);
		auto right_dependencies = right.required_bindings;
		auto right_output_correlations = Intersection(left.output_bindings, right.output_bindings);
		auto right_required_correlations = Intersection(left.output_bindings, right_dependencies);
		result.correlated_bindings = std::move(right_output_correlations);
		UnionInto(result.correlated_bindings, right_required_correlations);
		for (idx_t index = 0; index < binding_count; index++) {
			if (right_dependencies[index] && !left.output_bindings[index]) {
				throw InternalException("GQL APPLY right side requires an unavailable binding");
			}
		}
		result.output_bindings = left.output_bindings;
		UnionInto(result.output_bindings, right.output_bindings);
		result.required_bindings = left.required_bindings;
		result.nullable_bindings = left.nullable_bindings;
		UnionInto(result.nullable_bindings, right.nullable_bindings);
		if (operation->type == GqlLogicalOperatorType::LEFT_APPLY) {
			for (idx_t index = 0; index < binding_count; index++) {
				if (right.output_bindings[index] && !left.output_bindings[index]) {
					result.nullable_bindings[index] = true;
				}
			}
			result.minimum_cardinality = left.minimum_cardinality;
		}
		break;
	}
	case GqlLogicalOperatorType::CALL: {
		const auto &call = operation->Cast<GqlLogicalCall>();
		if (!call.child || call.output_names.empty() || call.output_names.size() != call.output_types.size()) {
			throw InternalException("Invalid GQL CALL operator");
		}
		// CALL is a relation-replacement barrier. Its child is still part of this
		// plan and transaction, but graph bindings are not visible after a
		// blocking batch/no-input procedure unless a future contract explicitly
		// declares preservation.
		(void)InferProperties(call.child, binding_count);
		result.minimum_cardinality = 0;
		break;
	}
	case GqlLogicalOperatorType::PROJECT: {
		const auto &project = operation->Cast<GqlLogicalProject>();
		const auto &child = InferProperties(project.child, binding_count);
		result = child;
		for (const auto &projection : project.projections) {
			if (!projection.expression ||
			    !IsSubset(ExpressionBindings(*projection.expression, binding_count), child.output_bindings)) {
				throw InternalException("GQL projection requires an unavailable binding");
			}
		}
		for (const auto &order : project.order_by) {
			if (!order.expression ||
			    !IsSubset(ExpressionBindings(*order.expression, binding_count), child.output_bindings)) {
				throw InternalException("GQL ordering requires an unavailable binding");
			}
		}
		break;
	}
	}
	operation->properties = std::move(result);
	operation->properties_valid = true;
	return operation->properties;
}

static void ClearProperties(const shared_ptr<GqlLogicalOperator> &operation) {
	if (!operation) {
		return;
	}
	operation->properties_valid = false;
	ClearProperties(operation->child);
	if (operation->type == GqlLogicalOperatorType::INNER_APPLY) {
		ClearProperties(operation->Cast<GqlLogicalInnerApply>().right);
	} else if (operation->type == GqlLogicalOperatorType::LEFT_APPLY) {
		ClearProperties(operation->Cast<GqlLogicalLeftApply>().right);
	}
}

static shared_ptr<GqlLogicalOperator> PushFilter(const shared_ptr<GqlLogicalOperator> &filter_operation,
                                                 idx_t binding_count) {
	auto &filter = filter_operation->Cast<GqlLogicalFilter>();
	auto dependencies = ExpressionBindings(*filter.predicate, binding_count);
	auto child = filter.child;
	if (!child ||
	    (child->type != GqlLogicalOperatorType::INNER_APPLY && child->type != GqlLogicalOperatorType::LEFT_APPLY)) {
		return filter_operation;
	}

	shared_ptr<GqlLogicalOperator> right;
	if (child->type == GqlLogicalOperatorType::INNER_APPLY) {
		right = child->Cast<GqlLogicalInnerApply>().right;
	} else {
		right = child->Cast<GqlLogicalLeftApply>().right;
	}
	const auto &left_properties = InferProperties(child->child, binding_count);
	if (IsSubset(dependencies, left_properties.output_bindings)) {
		filter.child = child->child;
		filter_operation->properties_valid = false;
		child->child = PushFilter(filter_operation, binding_count);
		child->properties_valid = false;
		return child;
	}

	// A predicate that touches the null-extended side must remain above a
	// LEFT_APPLY. Moving it into the join condition would preserve rows that the
	// original post-join filter rejects.
	if (child->type == GqlLogicalOperatorType::LEFT_APPLY) {
		return filter_operation;
	}

	filter.child = right;
	filter_operation->properties_valid = false;
	child->Cast<GqlLogicalInnerApply>().right = PushFilter(filter_operation, binding_count);
	child->properties_valid = false;
	return child;
}

static shared_ptr<GqlLogicalOperator> RewritePredicates(shared_ptr<GqlLogicalOperator> operation, idx_t binding_count) {
	if (!operation) {
		throw InternalException("Cannot optimize an empty GQL logical operator");
	}
	operation->child = operation->child ? RewritePredicates(operation->child, binding_count) : nullptr;
	if (operation->type == GqlLogicalOperatorType::INNER_APPLY) {
		auto &apply = operation->Cast<GqlLogicalInnerApply>();
		apply.right = RewritePredicates(apply.right, binding_count);
	} else if (operation->type == GqlLogicalOperatorType::LEFT_APPLY) {
		auto &apply = operation->Cast<GqlLogicalLeftApply>();
		apply.right = RewritePredicates(apply.right, binding_count);
	}
	operation->properties_valid = false;
	if (operation->type != GqlLogicalOperatorType::FILTER) {
		return operation;
	}
	return PushFilter(operation, binding_count);
}

void GqlOptimize(GqlLogicalPlan &plan) {
	if (!plan.root || plan.binding_count == 0) {
		throw InternalException("Cannot optimize an empty GQL logical plan");
	}
	plan.root = RewritePredicates(std::move(plan.root), plan.binding_count);
	ClearProperties(plan.root);
	const auto &properties = InferProperties(plan.root, plan.binding_count);
	for (const auto required : properties.required_bindings) {
		if (required) {
			throw InternalException("GQL logical plan has an unresolved binding dependency");
		}
	}

	// Do not reorder mandatory MATCH stages here. Fixed table-backed regions are
	// lowered into one native DuckDB join graph, where DuckDB can use table and
	// column statistics for join enumeration. This pass owns graph semantics and
	// barriers such as LEFT_APPLY; graph-specific costing belongs only to
	// physical alternatives DuckDB cannot see, such as recursive path engines.
}

static idx_t AccessExpressionEnd(const GqlExpressionProgram &program, idx_t node) {
	if (node >= program.node_types.size()) {
		throw InternalException("Truncated GQL access-path expression program");
	}
	auto type = static_cast<GqlExpressionType>(program.node_types[node]);
	auto cursor = node + 1;
	switch (type) {
	case GqlExpressionType::LITERAL:
	case GqlExpressionType::VARIABLE_REFERENCE:
		return cursor;
	case GqlExpressionType::LIST_CONSTRUCTOR:
	case GqlExpressionType::RECORD_CONSTRUCTOR:
		throw InternalException("GQL collection constructor reached access-path optimization");
	case GqlExpressionType::PROPERTY_REFERENCE:
	case GqlExpressionType::ELEMENT_ID:
	case GqlExpressionType::UNARY:
	case GqlExpressionType::IS_NULL:
	case GqlExpressionType::LABELED:
		return AccessExpressionEnd(program, cursor);
	case GqlExpressionType::BINARY:
		return AccessExpressionEnd(program, AccessExpressionEnd(program, cursor));
	case GqlExpressionType::FUNCTION:
		for (idx_t child = 0; child < program.child_counts[node]; child++) {
			cursor = AccessExpressionEnd(program, cursor);
		}
		return cursor;
	}
	throw InternalException("Unknown GQL access-path expression node");
}

struct GqlLiteralEquality {
	string property_name;
	GqlLiteralType literal_type;
	string literal_value;
};

static void CollectLiteralEqualityConjuncts(const GqlExpressionProgram &program, idx_t node, idx_t binding_index,
                                            vector<GqlLiteralEquality> &result) {
	if (node >= program.node_types.size() ||
	    static_cast<GqlExpressionType>(program.node_types[node]) != GqlExpressionType::BINARY) {
		return;
	}
	auto operation = static_cast<GqlBinaryOperator>(program.operators[node]);
	auto left = node + 1;
	auto right = AccessExpressionEnd(program, left);
	if (operation == GqlBinaryOperator::AND) {
		CollectLiteralEqualityConjuncts(program, left, binding_index, result);
		CollectLiteralEqualityConjuncts(program, right, binding_index, result);
		return;
	}
	if (operation != GqlBinaryOperator::EQUAL) {
		return;
	}
	auto is_binding_property = [&](idx_t root) {
		return root < program.node_types.size() &&
		       static_cast<GqlExpressionType>(program.node_types[root]) == GqlExpressionType::PROPERTY_REFERENCE &&
		       program.binding_indices[root] == binding_index;
	};
	auto is_literal = [&](idx_t root) {
		return root < program.node_types.size() &&
		       static_cast<GqlExpressionType>(program.node_types[root]) == GqlExpressionType::LITERAL;
	};
	idx_t property_root;
	idx_t literal_root;
	if (is_binding_property(left) && is_literal(right)) {
		property_root = left;
		literal_root = right;
	} else if (is_literal(left) && is_binding_property(right)) {
		property_root = right;
		literal_root = left;
	} else {
		return;
	}
	result.push_back({program.properties[property_root], static_cast<GqlLiteralType>(program.operators[literal_root]),
	                  program.values[literal_root]});
}

static bool TryFindCaseInsensitive(const unordered_map<string, string> &entries, const string &requested,
                                   string &value) {
	for (const auto &entry : entries) {
		if (StringUtil::CIEquals(entry.first, requested)) {
			value = entry.second;
			return true;
		}
	}
	value.clear();
	return false;
}

static bool ProgramReadsBindingProperty(const GqlExpressionProgram &program, idx_t binding_index) {
	for (idx_t node = 0; node < program.node_types.size(); node++) {
		auto type = static_cast<GqlExpressionType>(program.node_types[node]);
		if ((type == GqlExpressionType::PROPERTY_REFERENCE || type == GqlExpressionType::LABELED) &&
		    program.binding_indices[node] == binding_index) {
			return true;
		}
	}
	return false;
}

static bool ProgramProjectsGraphValue(const GqlExpressionProgram &program, idx_t binding_index) {
	if (program.result_types.empty()) {
		throw InternalException("GQL projection has no result type");
	}
	auto result_type = static_cast<GqlTypeId>(program.result_types[0]);
	if (result_type != GqlTypeId::EDGE && result_type != GqlTypeId::PATH) {
		return false;
	}
	for (const auto index : program.binding_indices) {
		if (index == binding_index) {
			return true;
		}
	}
	return false;
}

static idx_t LabelPostingCardinality(const GqlCsrSnapshot &snapshot, const string &label) {
	auto entry = snapshot.label_ids.find(StringUtil::Lower(label));
	if (entry == snapshot.label_ids.end() || entry->second + 1 >= snapshot.vertex_label_posting_offsets.size()) {
		return 0;
	}
	auto start = snapshot.vertex_label_posting_offsets[entry->second];
	auto end = snapshot.vertex_label_posting_offsets[entry->second + 1];
	return NumericCast<idx_t>(end - start);
}

static bool LabelPostingBeatsScan(const GqlCsrSnapshot &snapshot, const string &label) {
	auto threshold = MaxValue<idx_t>(1024, snapshot.vertex_ids.size() / 16);
	return LabelPostingCardinality(snapshot, label) <= threshold;
}

static bool ContainsLabel(const vector<string> &labels, const string &candidate) {
	for (const auto &label : labels) {
		if (StringUtil::CIEquals(label, candidate)) {
			return true;
		}
	}
	return false;
}

static const GqlCsrEdgeLabelStats *TryEdgeLabelStats(const GqlCsrSnapshot &snapshot, const string &label) {
	auto entry = snapshot.label_ids.find(StringUtil::Lower(label));
	if (entry == snapshot.label_ids.end() || entry->second >= snapshot.edge_label_stats.size()) {
		return nullptr;
	}
	const auto &stats = snapshot.edge_label_stats[entry->second];
	return stats.edge_count == 0 ? nullptr : &stats;
}

static idx_t ClampCardinality(long double value) {
	const auto maximum = static_cast<long double>(DConstants::INVALID_INDEX - 1);
	if (value >= maximum) {
		return DConstants::INVALID_INDEX - 1;
	}
	return static_cast<idx_t>(std::ceil(value));
}

static idx_t EstimateFixedExpansionRows(const GqlCsrSnapshot &snapshot, const string &label, const string &direction,
                                        idx_t seed_rows) {
	if (seed_rows == DConstants::INVALID_INDEX) {
		return DConstants::INVALID_INDEX;
	}
	auto stats = TryEdgeLabelStats(snapshot, label);
	if (!stats) {
		return 0;
	}
	auto active_vertices = direction == "out" ? stats->outgoing_vertex_count : stats->incoming_vertex_count;
	if (active_vertices == 0 || seed_rows == 0) {
		return 0;
	}
	auto estimate = static_cast<long double>(seed_rows) * stats->edge_count / active_vertices;
	return MinValue<idx_t>(ClampCardinality(estimate), NumericCast<idx_t>(stats->edge_count));
}

static idx_t EstimateBoundedPathRows(const GqlCsrSnapshot &snapshot, const GqlBindingAccessPath &path,
                                     idx_t seed_rows) {
	if (seed_rows == DConstants::INVALID_INDEX) {
		return DConstants::INVALID_INDEX;
	}
	if (path.unbounded) {
		auto stats = TryEdgeLabelStats(snapshot, path.edge_label);
		if (!stats) {
			return 0;
		}
		auto maximum_degree =
		    path.expansion_direction == "out" ? stats->max_outgoing_degree : stats->max_incoming_degree;
		if (maximum_degree > 1) {
			return DConstants::INVALID_INDEX;
		}
		// Functional relationships such as REPLY_OF produce one chain per
		// seed rather than a branching traversal. Four rows per seed is a
		// conservative small-frontier estimate; it affects only the choice
		// between direct row fetch and bulk scan, never query semantics.
		auto estimate = static_cast<long double>(seed_rows) * 4;
		return MinValue<idx_t>(ClampCardinality(estimate), NumericCast<idx_t>(stats->edge_count + seed_rows));
	}
	idx_t frontier = seed_rows;
	idx_t result = 0;
	for (idx_t depth = 1; depth <= path.maximum_repetitions; depth++) {
		frontier = EstimateFixedExpansionRows(snapshot, path.edge_label, path.expansion_direction, frontier);
		if (frontier == DConstants::INVALID_INDEX) {
			return frontier;
		}
		if (depth >= path.minimum_repetitions) {
			result = ClampCardinality(static_cast<long double>(result) + frontier);
		}
	}
	return result;
}

static constexpr idx_t GQL_BATCHED_ELEMENT_FETCH_THRESHOLD = 4096;

static bool BatchedElementFetchBeatsScan(idx_t frontier_rows) {
	return frontier_rows != DConstants::INVALID_INDEX && frontier_rows <= GQL_BATCHED_ELEMENT_FETCH_THRESHOLD;
}

static bool CsrFrontierBeatsBulkScan(const GqlCsrSnapshot &snapshot, const string &label, idx_t frontier_rows) {
	auto stats = TryEdgeLabelStats(snapshot, label);
	if (!stats || frontier_rows == DConstants::INVALID_INDEX) {
		return false;
	}
	auto threshold = MaxValue<idx_t>(1024, NumericCast<idx_t>(stats->edge_count / 64));
	return frontier_rows <= threshold;
}

static bool CsrAvailableSeedBeatsBulkScan(const GqlCsrSnapshot &snapshot, const string &label,
                                          const string &direction) {
	auto stats = TryEdgeLabelStats(snapshot, label);
	if (!stats) {
		return false;
	}
	auto maximum_degree = direction == "out" ? stats->max_outgoing_degree : stats->max_incoming_degree;
	auto threshold = MaxValue<uint64_t>(1024, stats->edge_count / 64);
	return maximum_degree <= threshold;
}

GqlAccessPathPlan GqlOptimizeAccessPaths(ClientContext &context, const string &graph_name,
                                         const GqlTableGraphBinding &graph, const GqlAccessPathInput &input) {
	if (input.root >= input.nodes.size() || input.binding_types.empty() || input.match_stages.empty()) {
		throw InternalException("Cannot optimize empty GQL graph access paths");
	}

	auto snapshot = GqlTryGetCsrSnapshot(context, graph_name);
	const bool can_use_vertex_postings = StringUtil::CIEquals(graph.vertex.key_column, "__gql_id") &&
	                                     StringUtil::CIEquals(graph.vertex.label_column, "__gql_label") &&
	                                     graph.vertex.label_is_list && snapshot;
	const bool can_use_edge_expansion = StringUtil::CIEquals(graph.edge.key_column, "__gql_edge_id") &&
	                                    StringUtil::CIEquals(graph.edge_source_column, "__gql_source_id") &&
	                                    StringUtil::CIEquals(graph.edge_target_column, "__gql_target_id") &&
	                                    StringUtil::CIEquals(graph.edge.label_column, "__gql_type") &&
	                                    !graph.edge.label_is_list && snapshot && snapshot->edge_labels_single;
	const bool can_fetch_vertices =
	    snapshot && snapshot->vertex_ids_match_rowids && StringUtil::CIEquals(graph.vertex.ownership, "MANAGED");
	const bool can_fetch_edges =
	    snapshot && snapshot->edge_ids_match_rowids && StringUtil::CIEquals(graph.edge.ownership, "MANAGED");

	vector<bool> edge_expandable(input.binding_types.size(), false);
	vector<bool> edge_requires_table(input.binding_types.size(), false);
	if (can_use_edge_expansion) {
		for (idx_t binding_index = 0; binding_index < input.binding_types.size(); binding_index++) {
			edge_expandable[binding_index] = input.binding_types[binding_index] == GqlPatternElementType::EDGE;
		}
		for (idx_t binding_index = 0; binding_index < input.binding_types.size(); binding_index++) {
			if (!edge_expandable[binding_index]) {
				continue;
			}
			for (const auto &program : input.predicates) {
				if (ProgramReadsBindingProperty(program, binding_index)) {
					edge_requires_table[binding_index] = true;
				}
			}
			for (const auto &program : input.projections) {
				if (ProgramReadsBindingProperty(program, binding_index) ||
				    ProgramProjectsGraphValue(program, binding_index)) {
					edge_requires_table[binding_index] = true;
				}
			}
		}
	}

	vector<vector<GqlLiteralEquality>> vertex_literal_equalities(input.binding_types.size());
	for (idx_t binding_index = 0; binding_index < input.binding_types.size(); binding_index++) {
		if (input.binding_types[binding_index] != GqlPatternElementType::VERTEX) {
			continue;
		}
		for (const auto &predicate : input.predicates) {
			CollectLiteralEqualityConjuncts(predicate, 0, binding_index, vertex_literal_equalities[binding_index]);
		}
	}

	GqlAccessPathPlan result;
	result.stages.resize(input.match_stages.size());
	vector<bool> stage_planned(input.match_stages.size(), false);
	vector<idx_t> propagated_estimated_rows(input.binding_types.size(), DConstants::INVALID_INDEX);
	auto optimize_stage = [&](idx_t stage_index, const vector<bool> &available) {
		if (stage_index >= input.match_stages.size() || stage_planned[stage_index]) {
			throw InternalException("Invalid GQL access-path stage");
		}
		stage_planned[stage_index] = true;
		const auto &stage = input.match_stages[stage_index];
		const bool stage_allows_element_fetch = stage.patterns.size() == 1;
		auto &stage_plan = result.stages[stage_index];
		stage_plan.introduced.resize(input.binding_types.size(), false);
		stage_plan.bindings.resize(input.binding_types.size());
		for (idx_t binding_index = 0; binding_index < available.size(); binding_index++) {
			if (available[binding_index]) {
				stage_plan.bindings[binding_index].estimated_rows = propagated_estimated_rows[binding_index];
			}
		}
		for (const auto &pattern : stage.patterns) {
			for (const auto &element : pattern.elements) {
				if (!available[element.binding_index]) {
					stage_plan.introduced[element.binding_index] = true;
				}
			}
		}

		for (idx_t binding_index = 0; binding_index < input.binding_types.size(); binding_index++) {
			if (!stage_plan.introduced[binding_index] ||
			    input.binding_types[binding_index] != GqlPatternElementType::VERTEX) {
				continue;
			}
			for (const auto &equality : vertex_literal_equalities[binding_index]) {
				string index_name;
				string property_column;
				if (!TryFindCaseInsensitive(graph.vertex.property_indexes, equality.property_name, index_name) ||
				    !TryFindCaseInsensitive(graph.vertex.property_columns, equality.property_name, property_column)) {
					continue;
				}
				auto &path = stage_plan.bindings[binding_index];
				path.type = GqlBindingAccessPathType::PROPERTY_INDEX_LOOKUP;
				path.property_name = equality.property_name;
				path.property_column = std::move(property_column);
				path.property_index_name = std::move(index_name);
				path.literal_type = equality.literal_type;
				path.literal_value = equality.literal_value;
				path.estimated_rows = 1;
				break;
			}
		}

		if (can_use_vertex_postings) {
			for (const auto &pattern : stage.patterns) {
				for (const auto &element : pattern.elements) {
					if (element.type != GqlPatternElementType::VERTEX || element.label.empty() ||
					    !stage_plan.introduced[element.binding_index] ||
					    vertex_literal_equalities[element.binding_index].empty() ||
					    stage_plan.bindings[element.binding_index].type ==
					        GqlBindingAccessPathType::PROPERTY_INDEX_LOOKUP) {
						continue;
					}
					auto &path = stage_plan.bindings[element.binding_index];
					for (const auto &label : StringUtil::Split(element.label, ';')) {
						if (!LabelPostingBeatsScan(*snapshot, label) || ContainsLabel(path.label_postings, label)) {
							continue;
						}
						path.label_postings.push_back(label);
						auto cardinality = LabelPostingCardinality(*snapshot, label);
						if (path.estimated_rows == DConstants::INVALID_INDEX || cardinality < path.estimated_rows) {
							path.estimated_rows = cardinality;
						}
					}
				}
			}
		}

		auto reachable = available;
		vector<bool> reached_by_expansion(input.binding_types.size(), false);
		vector<bool> source_scheduled(input.binding_types.size(), false);
		auto schedule_source = [&](idx_t binding_index) {
			if (stage_plan.introduced[binding_index] && !source_scheduled[binding_index]) {
				stage_plan.source_order.push_back(binding_index);
				source_scheduled[binding_index] = true;
			}
		};
		for (idx_t binding_index = 0; binding_index < stage_plan.bindings.size(); binding_index++) {
			if (stage_plan.bindings[binding_index].type != GqlBindingAccessPathType::PROPERTY_INDEX_LOOKUP) {
				continue;
			}
			reachable[binding_index] = true;
			schedule_source(binding_index);
		}
		for (const auto &pattern : stage.patterns) {
			if (pattern.elements.size() != 3 || !pattern.elements[1].quantified) {
				continue;
			}
			const auto &left = pattern.elements[0];
			const auto &right = pattern.elements[2];
			if (!reachable[left.binding_index] && !reachable[right.binding_index]) {
				// A quantified path needs one endpoint as its correlated seed. If
				// no selective lookup or prior MATCH binding supplies one, retain
				// generic semantics by scanning the left endpoint first.
				reachable[left.binding_index] = true;
				schedule_source(left.binding_index);
			}
		}
		bool expanded = true;
		while (expanded) {
			expanded = false;
			idx_t selected_pattern = DConstants::INVALID_INDEX;
			bool selected_expand_from_left = true;
			idx_t selected_estimated_rows = DConstants::INVALID_INDEX;
			uint8_t selected_priority = NumericLimits<uint8_t>::Maximum();
			for (idx_t pattern_index = 0; pattern_index < stage.patterns.size(); pattern_index++) {
				const auto &pattern = stage.patterns[pattern_index];
				if (pattern.elements.size() != 3) {
					continue;
				}
				const auto &left = pattern.elements[0];
				const auto &edge = pattern.elements[1];
				const auto &right = pattern.elements[2];
				auto &edge_path = stage_plan.bindings[edge.binding_index];
				if (edge_path.type == GqlBindingAccessPathType::CSR_EXPANSION ||
				    edge_path.type == GqlBindingAccessPathType::CSR_EDGE_PROPERTY_EXPANSION ||
				    edge_path.type == GqlBindingAccessPathType::CSR_PATH_EXPANSION ||
				    edge_path.type == GqlBindingAccessPathType::RELATIONAL_PATH_EXPANSION ||
				    !stage_plan.introduced[edge.binding_index] ||
				    (!edge.quantified && !edge_expandable[edge.binding_index]) ||
				    (edge.quantified && edge.unbounded && !can_use_edge_expansion) ||
				    (!reachable[left.binding_index] && !reachable[right.binding_index]) || edge.label.empty() ||
				    edge.label.find(';') != string::npos) {
					continue;
				}
				bool expand_from_left = reachable[left.binding_index];
				if (expand_from_left && reachable[right.binding_index]) {
					const auto left_rows = stage_plan.bindings[left.binding_index].estimated_rows;
					const auto right_rows = stage_plan.bindings[right.binding_index].estimated_rows;
					if (right_rows != DConstants::INVALID_INDEX &&
					    (left_rows == DConstants::INVALID_INDEX || right_rows < left_rows)) {
						expand_from_left = false;
					} else if (left_rows == DConstants::INVALID_INDEX && right_rows == DConstants::INVALID_INDEX &&
					           available[right.binding_index] && !available[left.binding_index]) {
						expand_from_left = false;
					}
				}
				const auto expansion_vertex_binding = expand_from_left ? left.binding_index : right.binding_index;
				const auto expansion_direction =
				    expand_from_left ? (edge.reverse ? "in" : "out") : (edge.reverse ? "out" : "in");
				const auto seed_rows = stage_plan.bindings[expansion_vertex_binding].estimated_rows;
				if (!edge.quantified && reached_by_expansion[expansion_vertex_binding] &&
				    !available[expansion_vertex_binding] &&
				    (stage_plan.bindings[expansion_vertex_binding].type ==
				         GqlBindingAccessPathType::BATCHED_ELEMENT_FETCH ||
				     !CsrFrontierBeatsBulkScan(*snapshot, edge.label, seed_rows))) {
					// A correlated CSR expansion should beat scanning this edge
					// relation by a meaningful margin. This applies equally to
					// topology-only and property-bearing hybrid expansions.
					continue;
				}
				const auto seed_is_independent =
				    available[expansion_vertex_binding] || stage_plan.bindings[expansion_vertex_binding].type ==
				                                               GqlBindingAccessPathType::PROPERTY_INDEX_LOOKUP;
				const auto priority =
				    edge.quantified ? uint8_t(0)
				                    : (seed_is_independent &&
				                               CsrAvailableSeedBeatsBulkScan(*snapshot, edge.label, expansion_direction)
				                           ? uint8_t(1)
				                           : uint8_t(2));
				if (selected_pattern != DConstants::INVALID_INDEX && priority >= selected_priority) {
					continue;
				}
				idx_t estimated_rows = DConstants::INVALID_INDEX;
				if (snapshot) {
					if (edge.quantified) {
						GqlBindingAccessPath estimate_path;
						estimate_path.edge_label = edge.label;
						estimate_path.expansion_direction = expansion_direction;
						estimate_path.minimum_repetitions = edge.minimum_repetitions;
						estimate_path.maximum_repetitions = edge.maximum_repetitions;
						estimate_path.unbounded = edge.unbounded;
						estimated_rows = EstimateBoundedPathRows(*snapshot, estimate_path, seed_rows);
					} else {
						estimated_rows =
						    EstimateFixedExpansionRows(*snapshot, edge.label, expansion_direction, seed_rows);
					}
				}
				selected_pattern = pattern_index;
				selected_expand_from_left = expand_from_left;
				selected_estimated_rows = estimated_rows;
				selected_priority = priority;
			}
			if (selected_pattern != DConstants::INVALID_INDEX) {
				const auto &pattern = stage.patterns[selected_pattern];
				const auto &left = pattern.elements[0];
				const auto &edge = pattern.elements[1];
				const auto &right = pattern.elements[2];
				auto &edge_path = stage_plan.bindings[edge.binding_index];
				edge_path.type = edge.quantified
				                     ? (can_use_edge_expansion ? GqlBindingAccessPathType::CSR_PATH_EXPANSION
				                                               : GqlBindingAccessPathType::RELATIONAL_PATH_EXPANSION)
				                     : (edge_requires_table[edge.binding_index]
				                            ? GqlBindingAccessPathType::CSR_EDGE_PROPERTY_EXPANSION
				                            : GqlBindingAccessPathType::CSR_EXPANSION);
				edge_path.expansion_vertex_binding =
				    selected_expand_from_left ? left.binding_index : right.binding_index;
				edge_path.expansion_direction =
				    selected_expand_from_left ? (edge.reverse ? "in" : "out") : (edge.reverse ? "out" : "in");
				edge_path.edge_label = edge.label;
				edge_path.minimum_repetitions = edge.minimum_repetitions;
				edge_path.maximum_repetitions = edge.maximum_repetitions;
				edge_path.unbounded = edge.unbounded;
				edge_path.fetch_edge_properties =
				    stage_allows_element_fetch &&
				    edge_path.type == GqlBindingAccessPathType::CSR_EDGE_PROPERTY_EXPANSION && can_fetch_edges &&
				    BatchedElementFetchBeatsScan(selected_estimated_rows);
				schedule_source(edge.binding_index);
				auto neighbor_binding = selected_expand_from_left ? right.binding_index : left.binding_index;
				auto &neighbor_rows = stage_plan.bindings[neighbor_binding].estimated_rows;
				if (selected_estimated_rows != DConstants::INVALID_INDEX &&
				    (neighbor_rows == DConstants::INVALID_INDEX || selected_estimated_rows < neighbor_rows)) {
					neighbor_rows = selected_estimated_rows;
				}
				auto &neighbor_path = stage_plan.bindings[neighbor_binding];
				if (stage_allows_element_fetch && can_fetch_vertices && stage_plan.introduced[neighbor_binding] &&
				    neighbor_path.type == GqlBindingAccessPathType::TABLE_SCAN &&
				    BatchedElementFetchBeatsScan(selected_estimated_rows)) {
					neighbor_path.type = GqlBindingAccessPathType::BATCHED_ELEMENT_FETCH;
					neighbor_path.fetch_id_binding = edge.binding_index;
					if (neighbor_binding == left.binding_index) {
						neighbor_path.fetch_id_column = edge.reverse ? "__gql_target_id" : "__gql_source_id";
					} else {
						neighbor_path.fetch_id_column = edge.reverse ? "__gql_source_id" : "__gql_target_id";
					}
				}
				reachable[neighbor_binding] = true;
				reached_by_expansion[neighbor_binding] = true;
				schedule_source(neighbor_binding);
				expanded = true;
			}
		}
		for (idx_t binding_index = 0; binding_index < stage_plan.introduced.size(); binding_index++) {
			schedule_source(binding_index);
			if (stage_plan.bindings[binding_index].estimated_rows != DConstants::INVALID_INDEX) {
				propagated_estimated_rows[binding_index] = stage_plan.bindings[binding_index].estimated_rows;
			}
		}
		for (const auto &pattern : stage.patterns) {
			for (const auto &element : pattern.elements) {
				if (element.quantified &&
				    stage_plan.bindings[element.binding_index].type != GqlBindingAccessPathType::CSR_PATH_EXPANSION &&
				    stage_plan.bindings[element.binding_index].type !=
				        GqlBindingAccessPathType::RELATIONAL_PATH_EXPANSION) {
					throw InvalidInputException("Unbounded composed GQL paths require a current CSR snapshot; "
					                            "run CALL gql_build_csr('%s') first",
					                            graph_name);
				}
			}
		}
		return stage_plan.introduced;
	};

	auto optimize_right_stage = [&](auto &self, idx_t node_index, const vector<bool> &available) -> vector<bool> {
		if (node_index >= input.nodes.size()) {
			throw InternalException("Invalid GQL access-path right stage");
		}
		const auto &node = input.nodes[node_index];
		if (node.type == GqlLogicalOperatorType::MATCH) {
			return optimize_stage(node.payload, available);
		}
		if (node.type != GqlLogicalOperatorType::FILTER) {
			throw InternalException("GQL access-path APPLY right side is not one MATCH stage");
		}
		return self(self, node.child, available);
	};
	auto optimize_pipeline = [&](auto &self, idx_t node_index) -> vector<bool> {
		if (node_index >= input.nodes.size()) {
			throw InternalException("Invalid GQL access-path logical node");
		}
		const auto &node = input.nodes[node_index];
		switch (node.type) {
		case GqlLogicalOperatorType::UNIT:
			return vector<bool>(input.binding_types.size(), false);
		case GqlLogicalOperatorType::MATCH:
			return optimize_stage(node.payload, vector<bool>(input.binding_types.size(), false));
		case GqlLogicalOperatorType::FILTER:
			return self(self, node.child);
		case GqlLogicalOperatorType::INNER_APPLY:
		case GqlLogicalOperatorType::LEFT_APPLY: {
			auto available = self(self, node.child);
			auto introduced = optimize_right_stage(optimize_right_stage, node.right, available);
			for (idx_t index = 0; index < available.size(); index++) {
				available[index] = available[index] || introduced[index];
			}
			return available;
		}
		case GqlLogicalOperatorType::PROJECT:
		case GqlLogicalOperatorType::CALL:
			throw InternalException("Nested GQL operator in access-path pipeline");
		}
		throw InternalException("Unknown GQL access-path logical operator");
	};
	(void)optimize_pipeline(optimize_pipeline, input.root);
	for (const auto planned : stage_planned) {
		if (!planned) {
			throw InternalException("Unreachable GQL access-path stage");
		}
	}
	return result;
}

} // namespace duckdb
