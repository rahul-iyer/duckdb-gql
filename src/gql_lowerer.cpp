#include "gql_lowerer.hpp"

#include "gql_relational.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

namespace duckdb {

// The table-function executor still accepts the original flattened MATCH
// payload. Keep that representation at this compatibility boundary; the
// binder and all future optimizer passes operate on the ordered logical tree.
struct GqlCompatibilityMatch {
  vector<GqlBoundPattern> patterns;
  vector<shared_ptr<GqlBoundExpression>> predicates;
  vector<shared_ptr<GqlBoundExpression>> optional_predicates;
  vector<idx_t> optional_predicate_stages;
  idx_t binding_count = 0;
  bool optional = false;
};

static void FlattenMatchStage(const shared_ptr<GqlLogicalOperator> &operation,
                              bool optional, idx_t optional_stage,
                              GqlCompatibilityMatch &result) {
  if (!operation) {
    throw InternalException("GQL APPLY has an empty right match stage");
  }
  if (operation->type == GqlLogicalOperatorType::FILTER) {
    const auto &filter = operation->Cast<GqlLogicalFilter>();
    FlattenMatchStage(filter.child, optional, optional_stage, result);
    if (optional && optional_stage > 0) {
      result.optional_predicates.push_back(filter.predicate);
      result.optional_predicate_stages.push_back(optional_stage);
    } else {
      result.predicates.push_back(filter.predicate);
    }
    return;
  }
  if (operation->type != GqlLogicalOperatorType::MATCH || operation->child) {
    throw InternalException("GQL APPLY right side must be one MATCH stage");
  }
  const auto &match = operation->Cast<GqlLogicalMatch>();
  if (match.patterns.empty()) {
    throw InternalException("GQL logical MATCH stage has no patterns");
  }
  for (const auto &source_pattern : match.patterns) {
    if (source_pattern.optional || source_pattern.optional_stage > 0) {
      throw InternalException("GQL logical MATCH contains compatibility optional metadata");
    }
    auto pattern = source_pattern;
    pattern.optional = optional;
    pattern.optional_stage = optional ? optional_stage : 0;
    result.patterns.push_back(std::move(pattern));
  }
}

static void FlattenMatchPipeline(const shared_ptr<GqlLogicalOperator> &operation,
                                 GqlCompatibilityMatch &result) {
  if (!operation) {
    throw InternalException("GQL logical MATCH pipeline is empty");
  }
  switch (operation->type) {
  case GqlLogicalOperatorType::MATCH:
    FlattenMatchStage(operation, false, 0, result);
    return;
  case GqlLogicalOperatorType::FILTER: {
    const auto &filter = operation->Cast<GqlLogicalFilter>();
    FlattenMatchPipeline(filter.child, result);
    result.predicates.push_back(filter.predicate);
    return;
  }
  case GqlLogicalOperatorType::INNER_APPLY: {
    const auto &apply = operation->Cast<GqlLogicalInnerApply>();
    FlattenMatchPipeline(apply.child, result);
    FlattenMatchStage(apply.right, false, 0, result);
    return;
  }
  case GqlLogicalOperatorType::LEFT_APPLY: {
    const auto &apply = operation->Cast<GqlLogicalLeftApply>();
    if (apply.child && apply.child->type == GqlLogicalOperatorType::UNIT) {
      if (apply.child->child || apply.optional_stage != 0 ||
          !result.patterns.empty()) {
        throw InternalException("Invalid leading GQL LEFT_APPLY");
      }
      result.optional = true;
    } else {
      if (apply.optional_stage == 0) {
        throw InternalException("Nested GQL LEFT_APPLY has no optional stage");
      }
      FlattenMatchPipeline(apply.child, result);
    }
    FlattenMatchStage(apply.right, true, apply.optional_stage, result);
    return;
  }
  case GqlLogicalOperatorType::UNIT:
    throw InternalException("GQL UNIT is only valid below a leading LEFT_APPLY");
  case GqlLogicalOperatorType::PROJECT:
  case GqlLogicalOperatorType::CALL:
    throw InternalException("Nested GQL projection in MATCH pipeline");
  }
  throw InternalException("Unknown GQL logical operator");
}

static void FindRecursivePattern(
    const shared_ptr<GqlLogicalOperator> &operation,
    const GqlBoundPatternElement *&recursive_edge,
    const GqlBoundPattern *&recursive_pattern) {
  if (!operation) {
    return;
  }
  if (operation->type == GqlLogicalOperatorType::MATCH) {
    const auto &match = operation->Cast<GqlLogicalMatch>();
    for (const auto &pattern : match.patterns) {
      for (const auto &element : pattern.elements) {
        if (!element.unbounded) {
          continue;
        }
        if (recursive_edge) {
          throw NotImplementedException(
              "Multiple unbounded quantified GQL path factors");
        }
        recursive_edge = &element;
        recursive_pattern = &pattern;
      }
    }
    return;
  }
  FindRecursivePattern(operation->child, recursive_edge, recursive_pattern);
  if (operation->type == GqlLogicalOperatorType::INNER_APPLY) {
    FindRecursivePattern(operation->Cast<GqlLogicalInnerApply>().right,
                         recursive_edge, recursive_pattern);
  } else if (operation->type == GqlLogicalOperatorType::LEFT_APPLY) {
    FindRecursivePattern(operation->Cast<GqlLogicalLeftApply>().right,
                         recursive_edge, recursive_pattern);
  }
}

struct GqlSerializedLogicalProgram {
  vector<Value> node_types;
  vector<Value> child_indices;
  vector<Value> right_indices;
  vector<Value> payload_indices;
  vector<Value> match_pattern_counts;
  vector<GqlBoundPattern> patterns;
  vector<Value> predicates;
  idx_t root = DConstants::INVALID_INDEX;
};

static idx_t SerializeLogicalNode(
    const shared_ptr<GqlLogicalOperator> &operation,
    GqlSerializedLogicalProgram &result) {
  if (!operation) {
    throw InternalException("Cannot serialize an empty GQL logical operator");
  }
  auto invalid = NumericLimits<uint64_t>::Maximum();
  uint64_t child_index = invalid;
  uint64_t right_index = invalid;
  uint64_t payload_index = invalid;
  switch (operation->type) {
  case GqlLogicalOperatorType::UNIT:
    if (operation->child) {
      throw InternalException("GQL UNIT cannot have a child");
    }
    break;
  case GqlLogicalOperatorType::MATCH: {
    if (operation->child) {
      throw InternalException("A GQL MATCH stage cannot have a child");
    }
    const auto &match = operation->Cast<GqlLogicalMatch>();
    if (match.patterns.empty()) {
      throw InternalException("A GQL MATCH stage cannot be empty");
    }
    payload_index = result.match_pattern_counts.size();
    result.match_pattern_counts.emplace_back(
        Value::UBIGINT(match.patterns.size()));
    for (const auto &pattern : match.patterns) {
      if (pattern.optional || pattern.optional_stage > 0) {
        throw InternalException(
            "GQL logical MATCH contains compatibility optional metadata");
      }
      result.patterns.push_back(pattern);
    }
    break;
  }
  case GqlLogicalOperatorType::FILTER: {
    const auto &filter = operation->Cast<GqlLogicalFilter>();
    child_index = SerializeLogicalNode(filter.child, result);
    payload_index = result.predicates.size();
    result.predicates.push_back(GqlSerializeExpression(*filter.predicate));
    break;
  }
  case GqlLogicalOperatorType::INNER_APPLY: {
    const auto &apply = operation->Cast<GqlLogicalInnerApply>();
    child_index = SerializeLogicalNode(apply.child, result);
    right_index = SerializeLogicalNode(apply.right, result);
    break;
  }
  case GqlLogicalOperatorType::LEFT_APPLY: {
    const auto &apply = operation->Cast<GqlLogicalLeftApply>();
    child_index = SerializeLogicalNode(apply.child, result);
    right_index = SerializeLogicalNode(apply.right, result);
    break;
  }
  case GqlLogicalOperatorType::PROJECT:
  case GqlLogicalOperatorType::CALL:
    throw InternalException("Nested GQL projection in MATCH pipeline");
  }
  auto node_index = result.node_types.size();
  result.node_types.emplace_back(
      Value::UTINYINT(static_cast<uint8_t>(operation->type)));
  result.child_indices.emplace_back(Value::UBIGINT(child_index));
  result.right_indices.emplace_back(Value::UBIGINT(right_index));
  result.payload_indices.emplace_back(Value::UBIGINT(payload_index));
  return node_index;
}

ParserExtensionPlanResult GqlLower(const GqlLogicalPlan &plan) {
  if (!plan.root || plan.root->type != GqlLogicalOperatorType::PROJECT) {
    throw InternalException("GQL logical plan must end in projection");
  }
  if (!plan.root->properties_valid ||
      plan.root->properties.output_bindings.size() != plan.binding_count) {
    throw InternalException(
        "GQL logical plan must be optimized before physical lowering");
  }
  const auto &project = plan.root->Cast<GqlLogicalProject>();
  const GqlBoundPatternElement *recursive_edge = nullptr;
  const GqlBoundPattern *recursive_pattern = nullptr;
  FindRecursivePattern(project.child, recursive_edge, recursive_pattern);

  vector<Value> projection_programs;
  vector<Value> projection_names;
  for (const auto &projection : project.projections) {
    projection_programs.push_back(
        GqlSerializeExpression(*projection.expression));
    projection_names.emplace_back(projection.name);
  }

  vector<Value> order_indices;
  vector<Value> order_descending;
  vector<Value> order_nulls;
  for (const auto &order : project.order_by) {
    order_indices.emplace_back(Value::UBIGINT(order.projection_index));
    order_descending.emplace_back(order.descending);
    order_nulls.emplace_back(Value::UTINYINT(
        order.null_order_specified ? (order.nulls_first ? 1 : 2) : 0));
  }
  auto append_result_modifiers = [&](vector<Value> &parameters,
                                     bool optional) {
    parameters.emplace_back(optional);
    parameters.emplace_back(project.distinct);
    parameters.emplace_back(Value::LIST(LogicalType::UBIGINT, order_indices));
    parameters.emplace_back(
        Value::LIST(LogicalType::BOOLEAN, order_descending));
    parameters.emplace_back(Value::LIST(LogicalType::UTINYINT, order_nulls));
    parameters.emplace_back(project.has_limit);
    parameters.emplace_back(Value::UBIGINT(project.limit));
    parameters.emplace_back(project.has_offset);
    parameters.emplace_back(Value::UBIGINT(project.offset));
  };

  if (recursive_edge) {
    GqlCompatibilityMatch match;
    match.binding_count = plan.binding_count;
    FlattenMatchPipeline(project.child, match);
    vector<Value> filter_programs;
    for (const auto &predicate : match.predicates) {
      filter_programs.push_back(GqlSerializeExpression(*predicate));
    }
    if (match.patterns.size() != 1 || !recursive_pattern ||
        recursive_pattern->elements.size() != 3 ||
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
    result.parameters.emplace_back(
        Value::UBIGINT(recursive_edge->binding_index));
    result.parameters.emplace_back(Value::UBIGINT(target.binding_index));
    result.parameters.emplace_back(
        Value::LIST(LogicalType::VARCHAR, std::move(source_labels)));
    result.parameters.emplace_back(
        Value::LIST(LogicalType::VARCHAR, std::move(edge_labels)));
    result.parameters.emplace_back(
        Value::LIST(LogicalType::VARCHAR, std::move(target_labels)));
    result.parameters.emplace_back(recursive_edge->edge_direction ==
                                   GqlEdgeDirection::LEFT);
    result.parameters.emplace_back(
        Value::UBIGINT(recursive_edge->minimum_repetitions));
    result.parameters.emplace_back(Value::LIST(GqlExpressionProgramType(),
                                               std::move(projection_programs)));
    result.parameters.emplace_back(
        Value::LIST(LogicalType::VARCHAR, std::move(projection_names)));
    result.parameters.emplace_back(
        Value::LIST(GqlExpressionProgramType(), std::move(filter_programs)));
    append_result_modifiers(result.parameters, match.optional);
    return result;
  }

  GqlSerializedLogicalProgram program;
  program.root = SerializeLogicalNode(project.child, program);
  vector<Value> pattern_sizes;
  vector<Value> element_types;
  vector<Value> binding_indices;
  vector<Value> labels;
  vector<Value> reverses;
  for (const auto &pattern : program.patterns) {
    pattern_sizes.emplace_back(Value::UBIGINT(pattern.elements.size()));
    for (const auto &element : pattern.elements) {
      element_types.emplace_back(
          Value::UTINYINT(static_cast<uint8_t>(element.type)));
      binding_indices.emplace_back(Value::UBIGINT(element.binding_index));
      labels.emplace_back(StringUtil::Join(element.labels, ";"));
      reverses.emplace_back(element.edge_direction == GqlEdgeDirection::LEFT);
    }
  }

  ParserExtensionPlanResult result;
  result.requires_valid_transaction = true;
  result.return_type = StatementReturnType::QUERY_RESULT;
  result.function = GqlRelationalMatchFunction();
  result.parameters.emplace_back(
      Value::UTINYINT(GQL_LOGICAL_PROGRAM_VERSION));
  result.parameters.emplace_back(
      Value::LIST(LogicalType::UBIGINT, std::move(pattern_sizes)));
  result.parameters.emplace_back(
      Value::LIST(LogicalType::UTINYINT, std::move(element_types)));
  result.parameters.emplace_back(
      Value::LIST(LogicalType::UBIGINT, std::move(binding_indices)));
  result.parameters.emplace_back(
      Value::LIST(LogicalType::VARCHAR, std::move(labels)));
  result.parameters.emplace_back(
      Value::LIST(LogicalType::BOOLEAN, std::move(reverses)));
  result.parameters.emplace_back(
      Value::LIST(LogicalType::UBIGINT,
                  std::move(program.match_pattern_counts)));
  result.parameters.emplace_back(
      Value::LIST(LogicalType::UTINYINT, std::move(program.node_types)));
  result.parameters.emplace_back(
      Value::LIST(LogicalType::UBIGINT, std::move(program.child_indices)));
  result.parameters.emplace_back(
      Value::LIST(LogicalType::UBIGINT, std::move(program.right_indices)));
  result.parameters.emplace_back(
      Value::LIST(LogicalType::UBIGINT, std::move(program.payload_indices)));
  result.parameters.emplace_back(Value::UBIGINT(program.root));
  result.parameters.emplace_back(
      Value::LIST(GqlExpressionProgramType(), std::move(projection_programs)));
  result.parameters.emplace_back(
      Value::LIST(LogicalType::VARCHAR, std::move(projection_names)));
  result.parameters.emplace_back(
      Value::LIST(GqlExpressionProgramType(), std::move(program.predicates)));
  result.parameters.emplace_back(project.distinct);
  result.parameters.emplace_back(Value::LIST(LogicalType::UBIGINT,
                                             std::move(order_indices)));
  result.parameters.emplace_back(Value::LIST(LogicalType::BOOLEAN,
                                             std::move(order_descending)));
  result.parameters.emplace_back(
      Value::LIST(LogicalType::UTINYINT, std::move(order_nulls)));
  result.parameters.emplace_back(project.has_limit);
  result.parameters.emplace_back(Value::UBIGINT(project.limit));
  result.parameters.emplace_back(project.has_offset);
  result.parameters.emplace_back(Value::UBIGINT(project.offset));
  return result;
}

static unique_ptr<QueryNode> LowerSelectNode(const GqlLogicalPlan &plan) {
  if (!plan.root || plan.root->type != GqlLogicalOperatorType::PROJECT) {
    throw InternalException("GQL logical plan must end in projection");
  }
  const auto &project = plan.root->Cast<GqlLogicalProject>();
  if (project.child && project.child->type == GqlLogicalOperatorType::CALL) {
    const auto &call = project.child->Cast<GqlLogicalCall>();
    if (!call.child || call.child->type != GqlLogicalOperatorType::PROJECT) {
      throw InternalException("GQL CALL input must end in projection");
    }
    GqlLogicalPlan child_plan;
    child_plan.bindings = plan.bindings;
    child_plan.binding_count = plan.binding_count;
    child_plan.root = call.child;
    auto child_query = LowerSelectNode(child_plan);
    auto child_statement = make_uniq<SelectStatement>();
    child_statement->node = std::move(child_query);
    auto child_expression = make_uniq<SubqueryExpression>();
    child_expression->subquery_type = SubqueryType::SCALAR;
    child_expression->subquery = std::move(child_statement);

    vector<Value> configuration_values;
    vector<Value> configuration_types;
    for (const auto &literal : call.configuration_arguments) {
      configuration_values.emplace_back(literal.value);
      configuration_types.emplace_back(
          Value::UTINYINT(static_cast<uint8_t>(literal.type)));
    }
    vector<unique_ptr<ParsedExpression>> arguments;
    arguments.push_back(std::move(child_expression));
    arguments.push_back(
        make_uniq<ConstantExpression>(Value(call.procedure_name)));
    arguments.push_back(make_uniq<ConstantExpression>(Value::LIST(
        LogicalType::VARCHAR, std::move(configuration_values))));
    arguments.push_back(make_uniq<ConstantExpression>(Value::LIST(
        LogicalType::UTINYINT, std::move(configuration_types))));
    auto function_ref = make_uniq<TableFunctionRef>();
    function_ref->function = make_uniq<FunctionExpression>(
        "gql_algorithm_call", std::move(arguments));

    auto select = make_uniq<SelectNode>();
    select->from_table = std::move(function_ref);
    for (const auto &projection : project.projections) {
      if (!projection.expression ||
          projection.expression->expression_type !=
              GqlExpressionType::VARIABLE_REFERENCE ||
          projection.expression->binding_source !=
              GqlBinding::Source::PROCEDURE ||
          projection.expression->binding_index >= call.output_names.size()) {
        throw NotImplementedException(
            "GQL procedure RETURN currently supports yielded variables");
      }
      auto expression = make_uniq<ColumnRefExpression>(
          call.output_names[projection.expression->binding_index]);
      expression->SetAlias(projection.name);
      select->select_list.push_back(std::move(expression));
    }
    if (project.distinct) {
      select->modifiers.push_back(make_uniq<DistinctModifier>());
    }
    if (!project.order_by.empty()) {
      auto order = make_uniq<OrderModifier>();
      for (const auto &entry : project.order_by) {
        if (entry.projection_index >= project.projections.size()) {
          throw InternalException("Invalid GQL CALL order projection");
        }
        auto expression = make_uniq<ColumnRefExpression>(
            project.projections[entry.projection_index].name);
        order->orders.emplace_back(
            entry.descending ? OrderType::DESCENDING : OrderType::ASCENDING,
            entry.null_order_specified
                ? (entry.nulls_first ? OrderByNullType::NULLS_FIRST
                                     : OrderByNullType::NULLS_LAST)
                : OrderByNullType::ORDER_DEFAULT,
            std::move(expression));
      }
      select->modifiers.push_back(std::move(order));
    }
    if (project.has_limit || project.has_offset) {
      auto limit = make_uniq<LimitModifier>();
      if (project.has_limit) {
        limit->limit = make_uniq<ConstantExpression>(
            Value::UBIGINT(project.limit));
      }
      if (project.has_offset) {
        limit->offset = make_uniq<ConstantExpression>(
            Value::UBIGINT(project.offset));
      }
      select->modifiers.push_back(std::move(limit));
    }
    return std::move(select);
  }
  auto lowered = GqlLower(plan);
  vector<unique_ptr<ParsedExpression>> arguments;
  for (auto &parameter : lowered.parameters) {
    arguments.push_back(make_uniq<ConstantExpression>(std::move(parameter)));
  }

  auto function_ref = make_uniq<TableFunctionRef>();
  function_ref->function = make_uniq<FunctionExpression>(lowered.function.name,
                                                         std::move(arguments));

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
