#include "gql_lowerer.hpp"

#include "gql_relational.hpp"

#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

namespace duckdb {

ParserExtensionPlanResult GqlLower(const GqlLogicalPlan &plan) {
	if (!plan.root || plan.root->type != GqlLogicalOperatorType::PROJECT) {
		throw InternalException("GQL logical plan must end in projection");
	}
	const auto &project = plan.root->Cast<GqlLogicalProject>();
	vector<const GqlLogicalFilter *> filters;
	auto current = project.child;
	while (current && current->type == GqlLogicalOperatorType::FILTER) {
		const auto &filter = current->Cast<GqlLogicalFilter>();
		filters.push_back(&filter);
		current = filter.child;
	}
	if (!current || current->type != GqlLogicalOperatorType::MATCH || current->child) {
		throw InternalException("GQL logical plan has an invalid MATCH pipeline");
	}
	const auto &match = current->Cast<GqlLogicalMatch>();
	const GqlBoundPatternElement *recursive_edge = nullptr;
	const GqlBoundPattern *recursive_pattern = nullptr;
	for (const auto &pattern : match.patterns) {
		for (const auto &element : pattern.elements) {
			if (!element.unbounded) {
				continue;
			}
			if (recursive_edge) {
				throw NotImplementedException("Multiple unbounded quantified GQL path factors");
			}
			recursive_edge = &element;
			recursive_pattern = &pattern;
		}
	}

	vector<Value> projection_programs;
	vector<Value> projection_names;
	for (const auto &projection : project.projections) {
		projection_programs.push_back(GqlSerializeExpression(*projection.expression));
		projection_names.emplace_back(projection.name);
	}

	vector<Value> filter_programs;
	for (auto entry = filters.rbegin(); entry != filters.rend(); entry++) {
		filter_programs.push_back(GqlSerializeExpression(*(*entry)->predicate));
	}
	vector<Value> order_indices;
	vector<Value> order_descending;
	vector<Value> order_nulls;
	for (const auto &order : project.order_by) {
		order_indices.emplace_back(Value::UBIGINT(order.projection_index));
		order_descending.emplace_back(order.descending);
		order_nulls.emplace_back(Value::UTINYINT(order.null_order_specified ? (order.nulls_first ? 1 : 2) : 0));
	}
	auto append_result_modifiers = [&](vector<Value> &parameters) {
		parameters.emplace_back(match.optional);
		parameters.emplace_back(project.distinct);
		parameters.emplace_back(Value::LIST(LogicalType::UBIGINT, order_indices));
		parameters.emplace_back(Value::LIST(LogicalType::BOOLEAN, order_descending));
		parameters.emplace_back(Value::LIST(LogicalType::UTINYINT, order_nulls));
		parameters.emplace_back(project.has_limit);
		parameters.emplace_back(Value::UBIGINT(project.limit));
		parameters.emplace_back(project.has_offset);
		parameters.emplace_back(Value::UBIGINT(project.offset));
	};

	if (recursive_edge) {
		if (match.patterns.size() != 1 || !recursive_pattern || recursive_pattern->elements.size() != 3 ||
		    recursive_pattern->elements[1].type != GqlPatternElementType::EDGE ||
		    &recursive_pattern->elements[1] != recursive_edge) {
			throw NotImplementedException("Unbounded quantified GQL paths combined "
			                              "with other pattern factors or patterns");
		}
		const auto &source = recursive_pattern->elements[0];
		const auto &target = recursive_pattern->elements[2];
		vector<Value> source_labels;
		vector<Value> edge_labels;
		vector<Value> target_labels;
		for (const auto &label : source.labels) {
			source_labels.emplace_back(label);
		}
		for (const auto &label : recursive_edge->labels) {
			edge_labels.emplace_back(label);
		}
		for (const auto &label : target.labels) {
			target_labels.emplace_back(label);
		}

		ParserExtensionPlanResult result;
		result.requires_valid_transaction = true;
		result.return_type = StatementReturnType::QUERY_RESULT;
		result.function = GqlRecursiveMatchFunction();
		result.parameters.emplace_back(Value::UBIGINT(match.binding_count));
		result.parameters.emplace_back(Value::UBIGINT(source.binding_index));
		result.parameters.emplace_back(Value::UBIGINT(recursive_edge->binding_index));
		result.parameters.emplace_back(Value::UBIGINT(target.binding_index));
		result.parameters.emplace_back(Value::LIST(LogicalType::VARCHAR, std::move(source_labels)));
		result.parameters.emplace_back(Value::LIST(LogicalType::VARCHAR, std::move(edge_labels)));
		result.parameters.emplace_back(Value::LIST(LogicalType::VARCHAR, std::move(target_labels)));
		result.parameters.emplace_back(recursive_edge->edge_direction == GqlEdgeDirection::LEFT);
		result.parameters.emplace_back(Value::UBIGINT(recursive_edge->minimum_repetitions));
		result.parameters.emplace_back(Value::LIST(GqlExpressionProgramType(), std::move(projection_programs)));
		result.parameters.emplace_back(Value::LIST(LogicalType::VARCHAR, std::move(projection_names)));
		result.parameters.emplace_back(Value::LIST(GqlExpressionProgramType(), std::move(filter_programs)));
		append_result_modifiers(result.parameters);
		return result;
	}

	vector<Value> pattern_sizes;
	vector<Value> element_types;
	vector<Value> binding_indices;
	vector<Value> labels;
	vector<Value> reverses;
	for (const auto &pattern : match.patterns) {
		pattern_sizes.emplace_back(Value::UBIGINT(pattern.elements.size()));
		for (const auto &element : pattern.elements) {
			element_types.emplace_back(Value::UTINYINT(static_cast<uint8_t>(element.type)));
			binding_indices.emplace_back(Value::UBIGINT(element.binding_index));
			labels.emplace_back(element.labels.empty() ? string() : element.labels[0]);
			reverses.emplace_back(element.edge_direction == GqlEdgeDirection::LEFT);
		}
	}

	ParserExtensionPlanResult result;
	result.requires_valid_transaction = true;
	result.return_type = StatementReturnType::QUERY_RESULT;
	result.function = GqlRelationalMatchFunction();
	result.parameters.emplace_back(Value::LIST(LogicalType::UBIGINT, std::move(pattern_sizes)));
	result.parameters.emplace_back(Value::LIST(LogicalType::UTINYINT, std::move(element_types)));
	result.parameters.emplace_back(Value::LIST(LogicalType::UBIGINT, std::move(binding_indices)));
	result.parameters.emplace_back(Value::LIST(LogicalType::VARCHAR, std::move(labels)));
	result.parameters.emplace_back(Value::LIST(LogicalType::BOOLEAN, std::move(reverses)));
	result.parameters.emplace_back(Value::LIST(GqlExpressionProgramType(), std::move(projection_programs)));
	result.parameters.emplace_back(Value::LIST(LogicalType::VARCHAR, std::move(projection_names)));
	result.parameters.emplace_back(Value::LIST(GqlExpressionProgramType(), std::move(filter_programs)));
	append_result_modifiers(result.parameters);
	return result;
}

static unique_ptr<QueryNode> LowerSelectNode(const GqlLogicalPlan &plan) {
	auto lowered = GqlLower(plan);
	vector<unique_ptr<ParsedExpression>> arguments;
	for (auto &parameter : lowered.parameters) {
		arguments.push_back(make_uniq<ConstantExpression>(std::move(parameter)));
	}

	auto function_ref = make_uniq<TableFunctionRef>();
	function_ref->function = make_uniq<FunctionExpression>(lowered.function.name, std::move(arguments));

	auto select = make_uniq<SelectNode>();
	select->from_table = std::move(function_ref);
	select->select_list.push_back(make_uniq<StarExpression>());
	return std::move(select);
}

unique_ptr<SQLStatement> GqlLowerSelect(vector<GqlLogicalPlan> plans) {
	if (plans.empty()) {
		throw InternalException("GQL MATCH produced no native alternatives");
	}
	auto statement = make_uniq<SelectStatement>();
	if (plans.size() == 1) {
		statement->node = LowerSelectNode(plans[0]);
		return std::move(statement);
	}
	auto set_operation = make_uniq<SetOperationNode>();
	set_operation->setop_type = SetOperationType::UNION;
	set_operation->setop_all = true;
	for (const auto &plan : plans) {
		set_operation->children.push_back(LowerSelectNode(plan));
	}
	statement->node = std::move(set_operation);
	return std::move(statement);
}

} // namespace duckdb
