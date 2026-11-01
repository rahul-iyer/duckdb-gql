#include "gql_optimizer.hpp"

#include "gql_catalog.hpp"
#include "gql_csr.hpp"
#include "gql_import.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <algorithm>
#include <cmath>

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

static bool ContainsLabel(const vector<string> &labels, const string &candidate) {
	for (const auto &label : labels) {
		if (StringUtil::CIEquals(label, candidate)) {
			return true;
		}
	}
	return false;
}

static const GqlCsrEdgeLabelStats *TryEdgeLabelStats(const GqlFanoutStatistics &statistics, const string &label) {
	auto entry = statistics.find(StringUtil::Lower(label));
	return entry == statistics.end() ? nullptr : &entry->second;
}

static idx_t ClampCardinality(long double value) {
	const auto maximum = static_cast<long double>(DConstants::INVALID_INDEX - 1);
	if (value >= maximum) {
		return DConstants::INVALID_INDEX - 1;
	}
	return static_cast<idx_t>(std::ceil(value));
}

static idx_t EstimateFixedExpansionRows(const GqlFanoutStatistics &statistics, const string &label,
                                        const string &direction, idx_t seed_rows) {
	if (seed_rows == DConstants::INVALID_INDEX) {
		return DConstants::INVALID_INDEX;
	}
	auto stats = TryEdgeLabelStats(statistics, label);
	if (!stats) {
		return DConstants::INVALID_INDEX;
	}
	auto active_vertices = direction == "out" ? stats->outgoing_vertex_count : stats->incoming_vertex_count;
	if (active_vertices == 0 || seed_rows == 0) {
		return 0;
	}
	auto estimate = static_cast<long double>(seed_rows) * stats->edge_count / active_vertices;
	// A bag of seeds may visit the same edge many times. Edge count is not
	// an upper bound unless seed uniqueness has been proved.
	return ClampCardinality(estimate);
}

static idx_t EstimateBoundedPathRows(const GqlFanoutStatistics &statistics, const GqlBindingAccessPath &path,
                                     idx_t seed_rows) {
	if (seed_rows == DConstants::INVALID_INDEX) {
		return DConstants::INVALID_INDEX;
	}
	if (path.unbounded) {
		auto stats = TryEdgeLabelStats(statistics, path.edge_label);
		if (!stats) {
			return DConstants::INVALID_INDEX;
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
		return ClampCardinality(estimate);
	}
	// Cost estimation must have bounded planning work even for a very large
	// user-supplied repetition bound. This does not restrict execution semantics.
	if (path.maximum_repetitions > 64)
		return DConstants::INVALID_INDEX;
	idx_t frontier = seed_rows;
	idx_t result = 0;
	for (idx_t depth = 1; depth <= path.maximum_repetitions; depth++) {
		frontier = EstimateFixedExpansionRows(statistics, path.edge_label, path.expansion_direction, frontier);
		if (frontier == DConstants::INVALID_INDEX) {
			return frontier;
		}
		if (depth >= path.minimum_repetitions) {
			result = ClampCardinality(static_cast<long double>(result) + frontier);
		}
	}
	return result;
}

// Statistics are read through the caller's catalog/transaction, never by
// executing aggregate queries while binding. Referenced relations without a
// single native table deliberately retain an unknown estimate.
class GqlNativeStatistics {
public:
	GqlNativeStatistics(ClientContext &context_p, const GqlElementTableBinding &binding) : context(context_p) {
		if (!binding.relation_sql.empty())
			return;
		auto &catalog = Catalog::GetCatalog(context, Identifier(binding.catalog_name));
		auto entry = catalog.GetEntry(context, CatalogType::TABLE_ENTRY, Identifier(binding.schema_name),
		                              Identifier(binding.table_name), OnEntryNotFound::RETURN_NULL);
		if (!entry || !entry->Cast<TableCatalogEntry>().IsDuckTable())
			return;
		table = &entry->Cast<TableCatalogEntry>();
		auto info = table->GetStorageInfo(context);
		if (info.cardinality.IsValid())
			rows = info.cardinality.GetIndex();
		for (const auto &index : info.index_info) {
			if (index.column_set.size() == 1) {
				auto column = *index.column_set.begin();
				indexed.insert(column);
				if (index.is_unique)
					unique.insert(column);
			}
		}
	}

	bool HasIndex(const string &column, bool require_unique = false) const {
		if (!table || !table->ColumnExists(Identifier(column)))
			return false;
		auto physical = table->GetColumn(Identifier(column)).StorageOid();
		return (require_unique ? unique : indexed).count(physical) > 0;
	}

	idx_t Distinct(const string &column) {
		auto key = StringUtil::Lower(column);
		auto found = distinct.find(key);
		if (found != distinct.end())
			return found->second;
		idx_t count = DConstants::INVALID_INDEX;
		if (table && table->ColumnExists(Identifier(column))) {
			auto &definition = table->GetColumn(Identifier(column));
			auto stats = table->GetStatistics(context, definition.Logical().index);
			if (stats && stats->GetDistinctCount() > 0)
				count = stats->GetDistinctCount();
		}
		distinct[key] = count;
		return count;
	}

	double Selectivity(const GqlExpressionProgram &program, idx_t node, idx_t binding,
	                   const GqlElementTableBinding &mapping) {
		if (!table || node >= program.node_types.size() ||
		    static_cast<GqlExpressionType>(program.node_types[node]) != GqlExpressionType::BINARY)
			return 1;
		auto op = static_cast<GqlBinaryOperator>(program.operators[node]);
		auto left = node + 1, right = AccessExpressionEnd(program, left);
		if (op == GqlBinaryOperator::AND) {
			// Without joint distributions, avoid multiplying correlated predicates.
			return MinValue<double>(Selectivity(program, left, binding, mapping),
			                        Selectivity(program, right, binding, mapping));
		}
		auto property = [&](idx_t index) {
			return static_cast<GqlExpressionType>(program.node_types[index]) == GqlExpressionType::PROPERTY_REFERENCE &&
			       program.binding_indices[index] == binding;
		};
		auto literal = [&](idx_t index) {
			return static_cast<GqlExpressionType>(program.node_types[index]) == GqlExpressionType::LITERAL;
		};
		bool reversed = false;
		if (!(property(left) && literal(right))) {
			if (!(literal(left) && property(right)))
				return 1;
			std::swap(left, right);
			reversed = true;
		}
		string column;
		if (!TryFindCaseInsensitive(mapping.property_columns, program.properties[left], column) ||
		    !table->ColumnExists(Identifier(column)))
			return 1;
		if (op == GqlBinaryOperator::EQUAL) {
			auto ndv = Distinct(column);
			return ndv == DConstants::INVALID_INDEX ? 1 : 1.0 / MaxValue<idx_t>(1, ndv);
		}
		if (op != GqlBinaryOperator::LESS_THAN && op != GqlBinaryOperator::LESS_THAN_OR_EQUAL &&
		    op != GqlBinaryOperator::GREATER_THAN && op != GqlBinaryOperator::GREATER_THAN_OR_EQUAL)
			return 1;
		auto &definition = table->GetColumn(Identifier(column));
		auto stats = table->GetStatistics(context, definition.Logical().index);
		if (!stats || stats->GetStatsType() != StatisticsType::NUMERIC_STATS || !NumericStats::HasMinMax(*stats))
			return 1;
		auto numeric = [](Value value) {
			switch (value.type().id()) {
			case LogicalTypeId::DATE:
			case LogicalTypeId::TIMESTAMP:
			case LogicalTypeId::TIMESTAMP_SEC:
			case LogicalTypeId::TIMESTAMP_MS:
			case LogicalTypeId::TIMESTAMP_NS:
			case LogicalTypeId::TIMESTAMP_TZ:
				return static_cast<double>(value.DefaultCastAs(LogicalType::TIMESTAMP).GetValue<timestamp_t>().value);
			default:
				return value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
			}
		};
		// Unsupported casts and nonnumeric predicates retain an unknown selectivity.
		// The ordinary binder still checks and reports invalid user expressions.
		try {
			auto value = numeric(Value(program.values[right]).DefaultCastAs(definition.Type()));
			auto low = numeric(NumericStats::Min(*stats)), high = numeric(NumericStats::Max(*stats));
			if (!std::isfinite(value) || !std::isfinite(low) || !std::isfinite(high) || high <= low)
				return 1;
			auto fraction = std::max(0.0, std::min(1.0, (value - low) / (high - low)));
			bool less = op == GqlBinaryOperator::LESS_THAN || op == GqlBinaryOperator::LESS_THAN_OR_EQUAL;
			if (reversed)
				less = !less;
			return less ? fraction : 1 - fraction;
		} catch (const Exception &) {
			return 1;
		}
	}

	idx_t Width(const vector<string> &columns) const {
		idx_t width = 0;
		if (table) {
			for (const auto &column : table->GetColumns().Logical()) {
				if (!columns.empty() && std::none_of(columns.begin(), columns.end(), [&](const string &name) {
					    return StringUtil::CIEquals(name, column.Name().GetIdentifierName());
				    }))
					continue;
				switch (column.Type().id()) {
				case LogicalTypeId::VARCHAR:
					width += 32;
					break;
				case LogicalTypeId::LIST:
				case LogicalTypeId::STRUCT:
					width += 64;
					break;
				default:
					width += 8;
					break;
				}
			}
		}
		return MaxValue<idx_t>(8, width ? width : (columns.empty() ? 64 : columns.size() * 16));
	}

	idx_t rows = DConstants::INVALID_INDEX;

private:
	ClientContext &context;
	optional_ptr<TableCatalogEntry> table;
	unordered_map<string, idx_t> distinct;
	unordered_set<column_t> indexed;
	unordered_set<column_t> unique;
};

static GqlPlanCost AccessCost(const GqlCostModel &cost_model, const GqlBindingAccessPath &path, idx_t total_rows) {
	auto scan = cost_model.Scan(total_rows, path.cardinality.rows, path.cardinality.row_width);
	if (path.type == GqlBindingAccessPathType::LABEL_POSTING_FETCH) {
		return cost_model.Fetch(path.candidate_rows, path.cardinality.row_width);
	}

	if (path.type == GqlBindingAccessPathType::PROPERTY_INDEX_LOOKUP) {
		// The candidate relation guarantees its point predicate before traversal.
		// DuckDB may implement it with either ART or a scan; both are legal costs.
		auto lookup = cost_model.Lookup(total_rows, path.candidate_rows, path.cardinality.row_width);
		auto candidate_scan = cost_model.Scan(total_rows, path.candidate_rows, path.cardinality.row_width);
		auto result = lookup.Total() < candidate_scan.Total() ? lookup : candidate_scan;
		if (path.candidate_rows != DConstants::INVALID_INDEX) {
			result.join += static_cast<double>(path.candidate_rows) * 0.05;
			result.peak_memory_bytes = MaxValue<idx_t>(
			    result.peak_memory_bytes,
			    ClampCardinality(static_cast<long double>(path.candidate_rows) * path.cardinality.row_width));
		}
		return result;
	}
	if (path.native_index_filter) {
		auto lookup = cost_model.Lookup(total_rows, path.cardinality.rows, path.cardinality.row_width);
		return lookup.Total() < scan.Total() ? lookup : scan;
	}
	return scan;
}

static void PublishAccessPlan(ClientContext &context, const string &graph_name, const GqlAccessPathPlan &plan);

GqlAccessPathPlan GqlOptimizeAccessPathsLegacy(ClientContext &context, const string &graph_name,
                                               const GqlTableGraphBinding &graph, const GqlAccessPathInput &input);

static GqlAccessPathPlan GqlOptimizeAccessPathsCost(ClientContext &context, const string &graph_name,
                                                    const GqlTableGraphBinding &graph,
                                                    const GqlAccessPathInput &input) {
	if (input.root >= input.nodes.size() || input.binding_types.empty() || input.match_stages.empty()) {
		throw InternalException("Cannot optimize empty GQL graph access paths");
	}

	auto snapshot = GqlTryGetCsrSnapshot(context, graph_name);
	if (snapshot && graph.cached_reference_ids && snapshot->source_snapshot_id != graph.source_snapshot_id)
		snapshot.reset();
	const bool can_fetch_referenced = snapshot && graph.cached_reference_ids;

	GqlFanoutStatistics statistics;
	// A valid CSR reflects table-write invalidation too, so prefer its fresh
	// statistics when available (including after direct SQL writes).
	if (snapshot) {
		for (const auto &label : snapshot->label_ids) {
			if (label.second < snapshot->edge_label_stats.size() &&
			    snapshot->edge_label_stats[label.second].edge_count > 0) {
				statistics[label.first] = snapshot->edge_label_stats[label.second];
			}
		}
	}
	if (statistics.empty()) {
		statistics = GqlLoadFanoutStatistics(context, graph.graph_id);
	}
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

	GqlCostModel cost_model(TaskScheduler::GetScheduler(context).NumberOfThreads());
	GqlNativeStatistics vertex_statistics(context, graph.vertex);
	GqlNativeStatistics edge_statistics(context, graph.edge);
	const auto vertex_count = snapshot ? snapshot->vertex_ids.size() : vertex_statistics.rows;
	const auto edge_count = snapshot ? snapshot->edge_count : edge_statistics.rows;

	vector<double> predicate_selectivity(input.binding_types.size(), 1);
	for (idx_t binding = 0; binding < input.binding_types.size(); binding++) {
		bool vertex = input.binding_types[binding] == GqlPatternElementType::VERTEX;
		for (const auto &program : input.predicates) {
			auto fraction = (vertex ? vertex_statistics : edge_statistics)
			                    .Selectivity(program, 0, binding, vertex ? graph.vertex : graph.edge);
			predicate_selectivity[binding] = MinValue<double>(predicate_selectivity[binding], fraction);
		}
	}

	GqlAccessPathPlan compatibility_plan;
	if (std::any_of(input.nodes.begin(), input.nodes.end(), [](const GqlAccessLogicalNode &node) {
		    return node.type == GqlLogicalOperatorType::LEFT_APPLY || node.type == GqlLogicalOperatorType::INNER_APPLY;
	    })) {
		compatibility_plan = GqlOptimizeAccessPathsLegacy(context, graph_name, graph, input);
	}

	GqlAccessPathPlan result;
	result.stages.resize(input.match_stages.size());
	vector<bool> stage_planned(input.match_stages.size(), false);
	vector<GqlCardinalityEstimate> propagated_cardinality(input.binding_types.size());
	const auto memory_budget = BufferManager::GetBufferManager(context).GetMaxMemory();
	auto optimize_stage = [&](idx_t stage_index, const vector<bool> &available) {
		if (stage_index >= input.match_stages.size() || stage_planned[stage_index]) {
			throw InternalException("Invalid GQL access-path stage");
		}
		stage_planned[stage_index] = true;
		// The current lowering nests correlated sources as lateral trees. Their
		// actual invocation domain is not yet represented by our single-binding
		// cardinalities. Keep the established planner for these regions instead of
		// pretending that independent average fanouts cost their repeated execution.
		if (std::any_of(available.begin(), available.end(), [](bool value) { return value; })) {
			auto &fallback = result.stages[stage_index];
			fallback = compatibility_plan.stages[stage_index];
			for (idx_t binding = 0; binding < fallback.introduced.size(); binding++) {
				if (fallback.introduced[binding])
					propagated_cardinality[binding] = fallback.bindings[binding].cardinality;
			}
			return fallback.introduced;
		}

		const auto &stage = input.match_stages[stage_index];
		const bool stage_allows_element_fetch = stage.patterns.size() == 1;
		auto &stage_plan = result.stages[stage_index];
		stage_plan.introduced.resize(input.binding_types.size(), false);
		stage_plan.bindings.resize(input.binding_types.size());
		for (idx_t binding_index = 0; binding_index < available.size(); binding_index++) {
			if (available[binding_index]) {
				stage_plan.bindings[binding_index].cardinality = propagated_cardinality[binding_index];
			}
		}
		for (const auto &pattern : stage.patterns) {
			for (const auto &element : pattern.elements) {
				if (!available[element.binding_index]) {
					stage_plan.introduced[element.binding_index] = true;
				}
			}
		}

		// Estimate scans and predicates independently of which physical access wins.
		// NDV is an estimate, never a uniqueness claim or a semantic row limit.
		for (idx_t binding = 0; binding < input.binding_types.size(); binding++) {
			auto &path = stage_plan.bindings[binding];
			auto is_vertex = input.binding_types[binding] == GqlPatternElementType::VERTEX;
			auto &native = is_vertex ? vertex_statistics : edge_statistics;
			path.cardinality.row_width = native.Width(is_vertex && binding < input.vertex_fetch_columns.size()
			                                              ? input.vertex_fetch_columns[binding]
			                                              : vector<string>());
			if (available[binding])
				continue;
			path.cardinality.rows = is_vertex ? vertex_count : edge_count;
			path.cardinality.from_statistics = path.cardinality.rows != DConstants::INVALID_INDEX;
			if (is_vertex) {
				for (const auto &equality : vertex_literal_equalities[binding]) {
					string column;
					if (!TryFindCaseInsensitive(graph.vertex.property_columns, equality.property_name, column))
						continue;
					path.native_index_filter |= native.HasIndex(column);
					if (can_fetch_referenced && GqlReferencedKeyIsUnique(graph.vertex, equality.property_name))
						path.cardinality.rows = 1;
					if (native.HasIndex(column, true))
						path.cardinality.rows = MinValue<idx_t>(path.cardinality.rows, 1);
				}
			}
			for (const auto &pattern : stage.patterns) {
				for (const auto &element : pattern.elements) {
					if (element.binding_index != binding)
						continue;
					if (is_vertex && can_use_vertex_postings) {
						for (const auto &label : StringUtil::Split(element.label, ';')) {
							if (!label.empty())
								path.cardinality.rows =
								    MinValue<idx_t>(path.cardinality.rows, LabelPostingCardinality(*snapshot, label));
						}
					} else if (!is_vertex) {
						path.edge_label = element.label;
						auto stats = TryEdgeLabelStats(statistics, element.label);
						if (stats)
							path.cardinality.rows = stats->edge_count;
					}
				}
			}
			if (predicate_selectivity[binding] < 1 && path.cardinality.rows != DConstants::INVALID_INDEX) {
				auto total = path.cardinality.rows;
				if (total != DConstants::INVALID_INDEX) {
					path.cardinality.rows =
					    MinValue<idx_t>(path.cardinality.rows, ClampCardinality(static_cast<long double>(total) *
					                                                            predicate_selectivity[binding]));
				}
			}

			path.cardinality.distinct_ids = path.cardinality.rows;
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
				auto candidate = path;
				candidate.type = GqlBindingAccessPathType::PROPERTY_INDEX_LOOKUP;
				candidate.property_name = equality.property_name;
				candidate.property_column = property_column;
				candidate.property_index_name = index_name;
				candidate.literal_type = equality.literal_type;
				candidate.literal_value = equality.literal_value;
				auto ndv = vertex_statistics.Distinct(property_column);
				candidate.candidate_rows =
				    vertex_statistics.HasIndex(property_column, true) ? 1
				    : vertex_count == DConstants::INVALID_INDEX || ndv == DConstants::INVALID_INDEX
				        ? vertex_count
				        : ClampCardinality(static_cast<long double>(vertex_count) / MaxValue<idx_t>(1, ndv));
				if (path.type != GqlBindingAccessPathType::PROPERTY_INDEX_LOOKUP ||
				    AccessCost(cost_model, candidate, vertex_count).Total() <
				        AccessCost(cost_model, path, vertex_count).Total()) {
					path = std::move(candidate);
				}
			}
		}

		bool has_literal_seed = false;
		for (idx_t binding_index = 0; binding_index < stage_plan.introduced.size(); binding_index++) {
			has_literal_seed |=
			    stage_plan.introduced[binding_index] && !vertex_literal_equalities[binding_index].empty();
		}
		if (can_use_vertex_postings) {
			for (const auto &pattern : stage.patterns) {
				for (const auto &element : pattern.elements) {
					if (element.type != GqlPatternElementType::VERTEX || element.label.empty() ||
					    !stage_plan.introduced[element.binding_index] ||
					    stage_plan.bindings[element.binding_index].type ==
					        GqlBindingAccessPathType::PROPERTY_INDEX_LOOKUP) {
						continue;
					}
					auto &path = stage_plan.bindings[element.binding_index];
					for (const auto &label : StringUtil::Split(element.label, ';')) {
						if (vertex_literal_equalities[element.binding_index].empty()) {
							// Keep the existing selective single-pattern plan, including
							// native table indexes not registered as graph property indexes.
							if (stage_allows_element_fetch && pattern.elements.size() > 1 && has_literal_seed) {
								continue;
							}
							auto cardinality = LabelPostingCardinality(*snapshot, label);
							// A small, selective posting is a bounded independent
							// frontier; fetch its requested columns, not the table.
							if (can_fetch_vertices && element.binding_index < input.vertex_fetch_columns.size() &&
							    !input.vertex_fetch_columns[element.binding_index].empty() &&
							    cost_model.Fetch(cardinality, path.cardinality.row_width).Total() <
							        AccessCost(cost_model, path, vertex_count).Total() &&
							    (path.type != GqlBindingAccessPathType::LABEL_POSTING_FETCH ||
							     cardinality < path.candidate_rows)) {
								path.type = GqlBindingAccessPathType::LABEL_POSTING_FETCH;
								path.label_postings = {label};
								path.candidate_rows = cardinality;
								path.fetch_columns = input.vertex_fetch_columns[element.binding_index];
							}
							continue;
						}
						if (cost_model.Fetch(LabelPostingCardinality(*snapshot, label), 8).Total() >=
						        AccessCost(cost_model, path, vertex_count).Total() ||
						    ContainsLabel(path.label_postings, label)) {
							continue;
						}
						path.label_postings.push_back(label);
						auto cardinality = LabelPostingCardinality(*snapshot, label);
						if (path.cardinality.rows == DConstants::INVALID_INDEX || cardinality < path.cardinality.rows) {
							path.cardinality.rows = cardinality;
						}
					}
				}
			}
		}

		// Search physical alternatives within this MATCH region. Correlation and
		// OPTIONAL boundaries are fixed by optimize_pipeline, not reordered here.
		struct SearchState {
			GqlStageAccessPlan plan;
			vector<bool> reachable;
			vector<bool> scheduled;
			vector<bool> completed;
		};
		struct SearchEdge {
			GqlAccessPatternElement left;
			GqlAccessPatternElement edge;
			GqlAccessPatternElement right;
			bool expand_allowed;
		};
		vector<SearchEdge> edges;
		vector<bool> seen_edge(input.binding_types.size(), false);
		for (const auto &pattern : stage.patterns) {
			for (idx_t part = 1; part < pattern.elements.size(); part += 2) {
				const auto &edge = pattern.elements[part];
				if (seen_edge[edge.binding_index] || !stage_plan.introduced[edge.binding_index])
					continue;
				seen_edge[edge.binding_index] = true;
				// Quantified factors with sibling edges retain their established lowering;
				// enumerating them requires a path identity contract for TRAIL checks.
				const bool supported = !edge.label.empty() && edge.label.find(';') == string::npos &&
				                       (!edge.quantified || pattern.elements.size() == 3);
				edges.push_back({pattern.elements[part - 1], edge, pattern.elements[part + 1], supported});
			}
		}
		SearchState initial {stage_plan, available, vector<bool>(input.binding_types.size(), false),
		                     vector<bool>(edges.size(), false)};
		auto schedule = [&](SearchState &state, idx_t binding) {
			if (state.plan.introduced[binding] && !state.scheduled[binding]) {
				state.plan.source_order.push_back(binding);
				state.scheduled[binding] = true;
			}
		};
		auto seed = [&](SearchState &state, idx_t binding) {
			if (state.reachable[binding])
				return;
			auto &path = state.plan.bindings[binding];
			path.cost = AccessCost(cost_model, path, vertex_count);
			path.cost.ApplyMemoryBudget(memory_budget);
			state.plan.cost.Add(path.cost);
			schedule(state, binding);
			state.reachable[binding] = true;
		};
		auto attach_vertex = [&](SearchState &state, idx_t binding, const SearchEdge &entry, idx_t fetch_rows) {
			if (state.reachable[binding])
				return;
			auto &path = state.plan.bindings[binding];
			auto fetch = cost_model.Fetch(fetch_rows, path.cardinality.row_width);
			auto scan = AccessCost(cost_model, path, vertex_count);
			if ((can_fetch_vertices || can_fetch_referenced) &&
			    (path.type == GqlBindingAccessPathType::TABLE_SCAN ||
			     path.type == GqlBindingAccessPathType::LABEL_POSTING_FETCH) &&
			    binding < input.vertex_fetch_columns.size() && !input.vertex_fetch_columns[binding].empty() &&
			    fetch.Total() < scan.Total()) {
				path.type = can_fetch_referenced ? GqlBindingAccessPathType::REFERENCED_ELEMENT_FETCH
				                                 : GqlBindingAccessPathType::BATCHED_ELEMENT_FETCH;
				path.label_postings.clear();
				path.fetch_columns = input.vertex_fetch_columns[binding];
				path.fetch_id_binding = entry.edge.binding_index;
				bool native =
				    state.plan.bindings[entry.edge.binding_index].type == GqlBindingAccessPathType::TABLE_SCAN;
				auto source_column = native ? graph.edge_source_column : "__gql_source_id";
				auto target_column = native ? graph.edge_target_column : "__gql_target_id";
				path.fetch_id_column = binding == entry.left.binding_index
				                           ? (entry.edge.reverse ? target_column : source_column)
				                           : (entry.edge.reverse ? source_column : target_column);
				path.cost = fetch;
				state.plan.cost.Add(fetch);
				schedule(state, binding);
				state.reachable[binding] = true;
			} else {
				seed(state, binding);
			}
		};

		// Access alternatives are seeded on demand. Eagerly scheduling all label
		// postings would expand entire labels even when a connected point frontier
		// can reach the same bindings much more cheaply.

		for (idx_t binding = 0; binding < initial.plan.bindings.size(); binding++) {
			if (initial.plan.bindings[binding].type == GqlBindingAccessPathType::PROPERTY_INDEX_LOOKUP)
				seed(initial, binding);
		}

		auto distinct_endpoints = [&](idx_t binding) {
			if (!input.duplicate_insensitive)
				return false;
			idx_t occurrences = 0;
			for (const auto &other_stage : input.match_stages) {
				for (const auto &pattern : other_stage.patterns) {
					for (const auto &element : pattern.elements)
						occurrences += element.binding_index == binding;
				}
			}
			if (occurrences != 1)
				return false;
			for (const auto &programs : {&input.projections, &input.predicates}) {
				for (const auto &program : *programs) {
					for (const auto index : program.binding_indices)
						if (index == binding)
							return false;
				}
			}
			return true;
		};
		// Known cardinalities precede unknown estimates only through cost; a missing
		// statistic is never interpreted as an empty graph or an unavailable path.
		constexpr idx_t BEAM_WIDTH = 16;
		constexpr idx_t SEARCH_BUDGET = 8192;
		idx_t considered = 0;
		bool bounded = false;
		vector<SearchState> beam;
		beam.push_back(std::move(initial));
		for (idx_t step = 0; step < edges.size(); step++) {
			vector<SearchState> next;
			for (const auto &state : beam) {
				bool greedy = considered >= SEARCH_BUDGET;
				for (idx_t index = 0; index < edges.size(); index++) {
					if (state.completed[index])
						continue;
					const auto &entry = edges[index];
					const auto &edge = entry.edge;
					const auto edge_binding = edge.binding_index;
					auto emit = [&](SearchState candidate) {
						candidate.completed[index] = true;
						auto &selected = candidate.plan.bindings[edge_binding];
						if (selected.cardinality.distinct_ids != DConstants::INVALID_INDEX) {
							selected.cardinality.distinct_ids =
							    MinValue<idx_t>(selected.cardinality.distinct_ids, selected.cardinality.rows);
						}
						next.push_back(std::move(candidate));
						considered++;
						std::stable_sort(
						    next.begin(), next.end(), [](const SearchState &left, const SearchState &right) {
							    auto a = left.plan.cost.Total(), b = right.plan.cost.Total();
							    return a < b ||
							           (a == b && left.plan.cost.peak_memory_bytes < right.plan.cost.peak_memory_bytes);
						    });
						auto width = considered >= SEARCH_BUDGET ? idx_t(1) : BEAM_WIDTH;
						if (next.size() > width) {
							bounded = true;
							next.resize(width);
						}
					};
					// A fixed edge can always remain a native relational scan. Quantified
					// factors require path enumeration and have no single-edge scan substitute.
					if (!edge.quantified) {
						auto candidate = state;
						auto &path = candidate.plan.bindings[edge_binding];
						auto scan_rows = path.cardinality.rows;
						auto stats = TryEdgeLabelStats(statistics, edge.label);
						auto filtered_rows = scan_rows;
						if (stats && scan_rows != DConstants::INVALID_INDEX) {
							const auto &left = candidate.plan.bindings[entry.left.binding_index];
							const auto &right = candidate.plan.bindings[entry.right.binding_index];
							auto ratio = [](const GqlBindingAccessPath &binding, idx_t domain) {
								auto ndv = binding.cardinality.distinct_ids;
								if (ndv == DConstants::INVALID_INDEX ||
								    binding.cardinality.rows == DConstants::INVALID_INDEX)
									return 1.0;
								auto participation =
								    MinValue<double>(1, static_cast<double>(ndv) / MaxValue<idx_t>(1, domain));
								auto multiplicity =
								    ndv ? MaxValue<double>(1, static_cast<double>(binding.cardinality.rows) / ndv) : 1;
								return participation * multiplicity;
							};
							filtered_rows = ClampCardinality(static_cast<long double>(scan_rows) *
							                                 ratio(left, edge.reverse ? stats->incoming_vertex_count
							                                                          : stats->outgoing_vertex_count) *
							                                 ratio(right, edge.reverse ? stats->outgoing_vertex_count
							                                                           : stats->incoming_vertex_count));
						}
						schedule(candidate, edge_binding);
						attach_vertex(candidate, entry.left.binding_index, entry, scan_rows);
						attach_vertex(candidate, entry.right.binding_index, entry, scan_rows);
						path.cost = cost_model.Scan(edge_count, scan_rows, path.cardinality.row_width);
						path.cost.Add(
						    cost_model.Join(candidate.plan.bindings[entry.left.binding_index].cardinality.rows,
						                    scan_rows, scan_rows, path.cardinality.row_width));
						path.cost.ApplyMemoryBudget(memory_budget);
						candidate.plan.cost.Add(path.cost);
						path.cardinality.rows = filtered_rows;
						for (auto binding : {entry.left.binding_index, entry.right.binding_index}) {
							if (state.reachable[binding])
								continue;
							auto &neighbor = candidate.plan.bindings[binding];
							auto domain = stats ? (binding == entry.left.binding_index
							                           ? (edge.reverse ? stats->incoming_vertex_count
							                                           : stats->outgoing_vertex_count)
							                           : (edge.reverse ? stats->outgoing_vertex_count
							                                           : stats->incoming_vertex_count))
							                    : DConstants::INVALID_INDEX;
							neighbor.cardinality.distinct_ids = MinValue<idx_t>(neighbor.cardinality.distinct_ids,
							                                                    MinValue<idx_t>(filtered_rows, domain));
							neighbor.cardinality.rows = filtered_rows;
							neighbor.cardinality.rows = filtered_rows;
						}
						schedule(candidate, edge_binding);
						emit(std::move(candidate));
					}
					bool csr = can_use_edge_expansion && (edge.quantified || edge_expandable[edge_binding]);
					if (entry.expand_allowed && (csr || (edge.quantified && !edge.unbounded))) {
						for (const bool from_left : {true, false}) {
							auto candidate = state;
							auto seed_binding = from_left ? entry.left.binding_index : entry.right.binding_index;
							auto neighbor_binding = from_left ? entry.right.binding_index : entry.left.binding_index;
							seed(candidate, seed_binding);
							auto seed_rows = candidate.plan.bindings[seed_binding].cardinality.rows;
							auto &path = candidate.plan.bindings[edge_binding];
							path.type = edge.quantified ? (csr ? GqlBindingAccessPathType::CSR_PATH_EXPANSION
							                                   : GqlBindingAccessPathType::RELATIONAL_PATH_EXPANSION)
							                            : (edge_requires_table[edge_binding]
							                                   ? GqlBindingAccessPathType::CSR_EDGE_PROPERTY_EXPANSION
							                                   : GqlBindingAccessPathType::CSR_EXPANSION);
							path.expansion_vertex_binding = seed_binding;
							path.expansion_direction =
							    from_left ? (edge.reverse ? "in" : "out") : (edge.reverse ? "out" : "in");
							path.edge_label = edge.label;
							path.minimum_repetitions = edge.minimum_repetitions;
							path.maximum_repetitions = edge.maximum_repetitions;
							path.unbounded = edge.unbounded;
							path.distinct_endpoints = edge.quantified && csr && distinct_endpoints(edge_binding);
							path.cardinality.rows =
							    edge.quantified ? EstimateBoundedPathRows(statistics, path, seed_rows)
							                    : EstimateFixedExpansionRows(statistics, edge.label,
							                                                 path.expansion_direction, seed_rows);
							path.cardinality.distinct_ids = edge.quantified
							                                    ? DConstants::INVALID_INDEX
							                                    : MinValue<idx_t>(path.cardinality.rows, edge_count);
							path.cardinality.from_statistics = path.cardinality.rows != DConstants::INVALID_INDEX;
							path.cost =
							    csr ? cost_model.Expand(seed_rows, path.cardinality.rows)
							        : cost_model.Scan(edge_count, path.cardinality.rows, path.cardinality.row_width);

							if (path.type == GqlBindingAccessPathType::CSR_EDGE_PROPERTY_EXPANSION) {
								auto fetch = cost_model.Fetch(path.cardinality.rows, edge_statistics.Width({}));
								auto scan =
								    cost_model.Scan(edge_count, path.cardinality.rows, path.cardinality.row_width);
								path.fetch_edge_properties = can_fetch_edges && fetch.Total() < scan.Total();
								path.cost.Add(path.fetch_edge_properties ? fetch : scan);
							}
							schedule(candidate, edge_binding);
							if (path.cardinality.rows != DConstants::INVALID_INDEX) {
								path.cardinality.rows =
								    ClampCardinality(static_cast<long double>(path.cardinality.rows) *
								                     predicate_selectivity[edge_binding]);
							}
							attach_vertex(candidate, neighbor_binding, entry, path.cardinality.rows);
							auto &neighbor = candidate.plan.bindings[neighbor_binding];
							auto previous_distinct = neighbor.cardinality.distinct_ids;
							// Estimate the joined frontier, including repeated IDs on an already
							// bound side. A unique source can be multiplied by parallel edges.
							auto multiplicity =
							    previous_distinct != DConstants::INVALID_INDEX && previous_distinct > 0 &&
							            neighbor.cardinality.rows != DConstants::INVALID_INDEX
							        ? MaxValue<double>(1, static_cast<double>(neighbor.cardinality.rows) /
							                                  previous_distinct)
							        : 1.0;
							auto endpoint_stats = TryEdgeLabelStats(statistics, edge.label);
							double fraction = 1;
							idx_t domain = DConstants::INVALID_INDEX;
							if (endpoint_stats && previous_distinct != DConstants::INVALID_INDEX) {
								domain = path.expansion_direction == "out" ? endpoint_stats->incoming_vertex_count
								                                           : endpoint_stats->outgoing_vertex_count;
								fraction = MinValue<double>(1, static_cast<double>(previous_distinct) /
								                                   MaxValue<idx_t>(1, domain));
							}
							neighbor.cardinality.rows =
							    path.cardinality.rows == DConstants::INVALID_INDEX
							        ? DConstants::INVALID_INDEX
							        : ClampCardinality(static_cast<long double>(path.cardinality.rows) * multiplicity *
							                           fraction);

							neighbor.cardinality.distinct_ids =
							    MinValue<idx_t>(previous_distinct, MinValue<idx_t>(domain, path.cardinality.rows));

							path.cost.Add(cost_model.Join(seed_rows, path.cardinality.rows, path.cardinality.rows, 32));
							path.cost.ApplyMemoryBudget(memory_budget);
							candidate.plan.cost.Add(path.cost);
							emit(std::move(candidate));
						}
					}
					if (greedy || considered >= SEARCH_BUDGET) {
						bounded = true;
						break;
					}
				}
			}
			if (next.empty())
				break;
			std::stable_sort(next.begin(), next.end(), [](const SearchState &left, const SearchState &right) {
				auto a = left.plan.cost.Total(), b = right.plan.cost.Total();
				return a < b || (a == b && left.plan.cost.peak_memory_bytes < right.plan.cost.peak_memory_bytes);
			});
			auto width = considered >= SEARCH_BUDGET ? idx_t(1) : BEAM_WIDTH;
			if (next.size() > width) {
				bounded = true;
				next.resize(width);
			}
			beam = std::move(next);
		}
		auto best = std::move(beam.front());
		// Isolated vertices and unsupported pattern shapes keep native execution.
		for (idx_t binding = 0; binding < best.plan.introduced.size(); binding++) {
			if (!best.plan.introduced[binding])
				continue;
			if (!best.scheduled[binding]) {
				auto &path = best.plan.bindings[binding];
				path.cost = AccessCost(cost_model, path,
				                       input.binding_types[binding] == GqlPatternElementType::VERTEX ? vertex_count
				                                                                                     : edge_count);
				path.cost.ApplyMemoryBudget(memory_budget);
				best.plan.cost.Add(path.cost);
				schedule(best, binding);
			}
			auto &path = best.plan.bindings[binding];

			propagated_cardinality[binding] = path.cardinality;
		}
		best.plan.costed = true;
		best.plan.alternatives_considered = considered;
		best.plan.search_bounded = bounded;
		stage_plan = std::move(best.plan);

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

GqlAccessPathPlan GqlOptimizeAccessPaths(ClientContext &context, const string &graph_name,
                                         const GqlTableGraphBinding &graph, const GqlAccessPathInput &input) {
	string mode = "legacy";
	context.TryGetCurrentSetting("gql_optimizer", mode);
	auto result = mode == "cost" ? GqlOptimizeAccessPathsCost(context, graph_name, graph, input)
	                             : GqlOptimizeAccessPathsLegacy(context, graph_name, graph, input);
	result.optimizer_mode = mode;
	PublishAccessPlan(context, graph_name, result);
	return result;
}

struct GqlOptimizerClientState : ClientContextState {
	string graph_name;
	GqlAccessPathPlan plan;
};

static void PublishAccessPlan(ClientContext &context, const string &graph_name, const GqlAccessPathPlan &plan) {
	auto state = context.registered_state->GetOrCreate<GqlOptimizerClientState>("gql_optimizer_plan");
	state->graph_name = graph_name;
	state->plan = plan;
}

static const char *AccessPathName(GqlBindingAccessPathType type) {
	switch (type) {
	case GqlBindingAccessPathType::REFERENCED_ELEMENT_FETCH:
		return "REFERENCED_ELEMENT_FETCH";
	case GqlBindingAccessPathType::TABLE_SCAN:
		return "TABLE_SCAN";
	case GqlBindingAccessPathType::PROPERTY_INDEX_LOOKUP:
		return "PROPERTY_INDEX_LOOKUP";
	case GqlBindingAccessPathType::CSR_EXPANSION:
		return "CSR_EXPANSION";
	case GqlBindingAccessPathType::CSR_EDGE_PROPERTY_EXPANSION:
		return "CSR_EDGE_PROPERTY_EXPANSION";
	case GqlBindingAccessPathType::CSR_PATH_EXPANSION:
		return "CSR_PATH_EXPANSION";
	case GqlBindingAccessPathType::RELATIONAL_PATH_EXPANSION:
		return "RELATIONAL_PATH_EXPANSION";
	case GqlBindingAccessPathType::BATCHED_ELEMENT_FETCH:
		return "BATCHED_ELEMENT_FETCH";
	case GqlBindingAccessPathType::LABEL_POSTING_FETCH:
		return "LABEL_POSTING_FETCH";
	}
	throw InternalException("Invalid GQL access path");
}

struct GqlOptimizerPlanData : TableFunctionData {
	string graph_name;
	GqlAccessPathPlan plan;
};

struct GqlOptimizerPlanScan : GlobalTableFunctionState {
	idx_t stage = 0;
	idx_t position = 0;
};

static unique_ptr<FunctionData> OptimizerPlanBind(ClientContext &context, TableFunctionBindInput &,
                                                  vector<LogicalType> &types, vector<Identifier> &names) {
	auto state = context.registered_state->GetOrCreate<GqlOptimizerClientState>("gql_optimizer_plan");
	auto result = make_uniq<GqlOptimizerPlanData>();
	result->graph_name = state->graph_name;
	result->plan = state->plan;
	names = {"graph_name",
	         "stage_id",
	         "binding_id",
	         "source_position",
	         "access_path",
	         "direction",
	         "estimated_rows",
	         "estimated_distinct_ids",
	         "row_width",
	         "operator_cost",
	         "stage_cost",
	         "peak_memory_bytes",
	         "alternatives_considered",
	         "search_bounded",
	         "scan_cost",
	         "traversal_cost",
	         "fetch_cost",
	         "join_cost",
	         "from_statistics",
	         "edge_label",
	         "fetch_edge_properties",
	         "spill_cost",
	         "optimizer_mode"};
	types = {LogicalType::VARCHAR, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT,
	         LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::UBIGINT, LogicalType::UBIGINT,
	         LogicalType::UBIGINT, LogicalType::DOUBLE,  LogicalType::DOUBLE,  LogicalType::UBIGINT,
	         LogicalType::UBIGINT, LogicalType::BOOLEAN, LogicalType::DOUBLE,  LogicalType::DOUBLE,
	         LogicalType::DOUBLE,  LogicalType::DOUBLE,  LogicalType::BOOLEAN, LogicalType::VARCHAR,
	         LogicalType::BOOLEAN, LogicalType::DOUBLE,  LogicalType::VARCHAR};
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> OptimizerPlanInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<GqlOptimizerPlanScan>();
}

static Value EstimateValue(idx_t estimate) {
	return estimate == DConstants::INVALID_INDEX ? Value(LogicalType::UBIGINT) : Value::UBIGINT(estimate);
}

static void OptimizerPlanScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<GqlOptimizerPlanData>();
	auto &scan = input.global_state->Cast<GqlOptimizerPlanScan>();
	idx_t count = 0;
	while (scan.stage < data.plan.stages.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &stage = data.plan.stages[scan.stage];
		if (scan.position >= stage.source_order.size()) {
			scan.stage++;
			scan.position = 0;
			continue;
		}
		auto binding = stage.source_order[scan.position];
		const auto &path = stage.bindings[binding];
		output.data[0].SetValue(count, Value(data.graph_name));
		output.data[1].SetValue(count, Value::UBIGINT(scan.stage));
		output.data[2].SetValue(count, Value::UBIGINT(binding));
		output.data[3].SetValue(count, Value::UBIGINT(scan.position));
		output.data[4].SetValue(count, Value(AccessPathName(path.type)));
		output.data[5].SetValue(count, Value(path.expansion_direction));
		output.data[6].SetValue(count, EstimateValue(path.cardinality.rows));
		output.data[7].SetValue(count, EstimateValue(path.cardinality.distinct_ids));
		output.data[8].SetValue(count, Value::UBIGINT(path.cardinality.row_width));
		output.data[9].SetValue(count, Value(path.cost.Total()));
		output.data[10].SetValue(count, Value(stage.cost.Total()));
		output.data[11].SetValue(count, Value::UBIGINT(stage.cost.peak_memory_bytes));
		output.data[12].SetValue(count, Value::UBIGINT(stage.alternatives_considered));
		output.data[13].SetValue(count, Value::BOOLEAN(stage.search_bounded));
		output.data[14].SetValue(count, Value(path.cost.scan));
		output.data[15].SetValue(count, Value(path.cost.traversal));
		output.data[16].SetValue(count, Value(path.cost.fetch));
		output.data[17].SetValue(count, Value(path.cost.join));
		output.data[18].SetValue(count, Value::BOOLEAN(path.cardinality.from_statistics));
		output.data[19].SetValue(count, Value(path.edge_label));
		output.data[20].SetValue(count, Value::BOOLEAN(path.fetch_edge_properties));
		output.data[21].SetValue(count, Value(path.cost.spill));
		auto mode = data.plan.optimizer_mode == "cost" && !stage.costed ? "legacy_fallback" : data.plan.optimizer_mode;
		output.data[22].SetValue(count, Value(mode));
		if (!stage.costed) {
			for (idx_t column : {idx_t(8), idx_t(9), idx_t(10), idx_t(11), idx_t(12), idx_t(13), idx_t(14), idx_t(15),
			                     idx_t(16), idx_t(17), idx_t(18), idx_t(20), idx_t(21)}) {
				output.data[column].SetValue(count, Value(output.data[column].GetType()));
			}
		}
		scan.position++;
		count++;
	}
	output.SetChildCardinality(count);
}

TableFunction GqlOptimizerPlanFunction() {
	TableFunction function("gql_optimizer_plan", {}, OptimizerPlanScan);
	function.bind = OptimizerPlanBind;
	function.init_global = OptimizerPlanInit;
	return function;
}

} // namespace duckdb
