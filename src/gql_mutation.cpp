#include "gql_mutation.hpp"

#include "gql_catalog.hpp"
#include "gql_lowerer.hpp"
#include "gql_storage.hpp"

#include "duckdb/common/enums/join_type.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/delete_statement.hpp"
#include "duckdb/parser/statement/drop_statement.hpp"
#include "duckdb/parser/statement/merge_into_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

#include <atomic>

namespace duckdb {

static unique_ptr<ParsedExpression> Column(const string &table,
                                           const string &column) {
  return make_uniq<ColumnRefExpression>(column, table);
}

static unique_ptr<ParsedExpression> Constant(Value value) {
  return make_uniq<ConstantExpression>(std::move(value));
}

static unique_ptr<ParsedExpression> Equal(unique_ptr<ParsedExpression> left,
                                          unique_ptr<ParsedExpression> right) {
  return make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL,
                                         std::move(left), std::move(right));
}

static unique_ptr<ParsedExpression>
NotDistinct(unique_ptr<ParsedExpression> left,
            unique_ptr<ParsedExpression> right) {
  return make_uniq<ComparisonExpression>(
      ExpressionType::COMPARE_NOT_DISTINCT_FROM, std::move(left),
      std::move(right));
}

static unique_ptr<ParsedExpression> And(unique_ptr<ParsedExpression> left,
                                        unique_ptr<ParsedExpression> right) {
  return make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
                                          std::move(left), std::move(right));
}

static unique_ptr<ParsedExpression> Or(unique_ptr<ParsedExpression> left,
                                       unique_ptr<ParsedExpression> right) {
  return make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_OR,
                                          std::move(left), std::move(right));
}

static unique_ptr<ParsedExpression>
Function(const string &name, vector<unique_ptr<ParsedExpression>> arguments) {
  return make_uniq<FunctionExpression>(name, std::move(arguments));
}

static unique_ptr<ParsedExpression>
HasLabel(unique_ptr<ParsedExpression> labels, const string &label) {
  vector<unique_ptr<ParsedExpression>> split_arguments;
  split_arguments.push_back(std::move(labels));
  split_arguments.push_back(Constant(Value(";")));
  vector<unique_ptr<ParsedExpression>> contains_arguments;
  contains_arguments.push_back(
      Function("string_split", std::move(split_arguments)));
  contains_arguments.push_back(Constant(Value(label)));
  return Function("list_contains", std::move(contains_arguments));
}

static unique_ptr<ParsedExpression>
AppendLabel(unique_ptr<ParsedExpression> labels, const string &label) {
  auto result = make_uniq<CaseExpression>();

  CaseCheck missing;
  missing.when_expr = make_uniq<OperatorExpression>(
      ExpressionType::OPERATOR_IS_NULL, labels->Copy());
  missing.then_expr = Constant(Value(label));
  result->case_checks.push_back(std::move(missing));

  CaseCheck present;
  present.when_expr = HasLabel(labels->Copy(), label);
  present.then_expr = labels->Copy();
  result->case_checks.push_back(std::move(present));

  vector<unique_ptr<ParsedExpression>> arguments;
  arguments.push_back(std::move(labels));
  arguments.push_back(Constant(Value(";")));
  arguments.push_back(Constant(Value(label)));
  result->else_expr = Function("concat", std::move(arguments));
  return std::move(result);
}

static unique_ptr<ParsedExpression>
EraseLabel(unique_ptr<ParsedExpression> labels, const string &label) {
  vector<unique_ptr<ParsedExpression>> padded_arguments;
  padded_arguments.push_back(Constant(Value(";")));
  padded_arguments.push_back(std::move(labels));
  padded_arguments.push_back(Constant(Value(";")));

  vector<unique_ptr<ParsedExpression>> needle_arguments;
  needle_arguments.push_back(Constant(Value(";")));
  needle_arguments.push_back(Constant(Value(label)));
  needle_arguments.push_back(Constant(Value(";")));

  vector<unique_ptr<ParsedExpression>> replace_arguments;
  replace_arguments.push_back(
      Function("concat", std::move(padded_arguments)));
  replace_arguments.push_back(
      Function("concat", std::move(needle_arguments)));
  replace_arguments.push_back(Constant(Value(";")));

  vector<unique_ptr<ParsedExpression>> trim_arguments;
  trim_arguments.push_back(Function("replace", std::move(replace_arguments)));
  trim_arguments.push_back(Constant(Value(";")));
  return Function("trim", std::move(trim_arguments));
}

static string TargetColumn(idx_t mutation_index) {
  return "gql_target_id_" + to_string(mutation_index);
}

static string ValueColumn(idx_t mutation_index) {
  return "gql_mutation_value_" + to_string(mutation_index);
}

static const char *ElementKind(const GqlBoundMutation &mutation) {
  return mutation.binding_type.id == GqlTypeId::EDGE ? "EDGE" : "VERTEX";
}

static const char *KeyColumn(const GqlBoundMutation &mutation) {
  return mutation.binding_type.id == GqlTypeId::EDGE ? "__gql_edge_id"
                                                     : "__gql_id";
}

static const char *LabelColumn(const GqlBoundMutation &mutation) {
  return mutation.binding_type.id == GqlTypeId::EDGE ? "__gql_type"
                                                     : "__gql_label";
}

static unique_ptr<TableRef>
FunctionTable(const string &name, vector<Value> values, const string &alias) {
  vector<unique_ptr<ParsedExpression>> arguments;
  for (auto &value : values) {
    arguments.push_back(Constant(std::move(value)));
  }
  auto result = make_uniq<TableFunctionRef>();
  result->function = make_uniq<FunctionExpression>(name, std::move(arguments));
  result->alias = alias;
  return std::move(result);
}

static unique_ptr<TableRef> MutationTarget(const GqlBoundMutation &mutation,
                                           const string &purpose,
                                           const string &alias) {
  return FunctionTable(
      "gql_mutation_target",
      {Value(ElementKind(mutation)), Value(purpose), Value(mutation.name)},
      alias);
}

static unique_ptr<TableRef> MutationEdgeTable(const string &alias) {
  return FunctionTable("gql_mutation_target",
                       {Value("EDGE"), Value("DELETE"), Value("")}, alias);
}

static unique_ptr<TableRef> MutationGraph(const string &alias) {
  return FunctionTable("gql_mutation_graph", {}, alias);
}

static unique_ptr<TableRef> Snapshot(const string &snapshot_name,
                                     const string &alias) {
  auto result = make_uniq<BaseTableRef>();
  result->table_name = snapshot_name;
  result->alias = alias;
  return std::move(result);
}

static unique_ptr<SQLStatement> ControlStatement(const string &command_id,
                                                 bool begin) {
  auto function = FunctionTable(
      "gql_mutation_control", {Value(command_id), Value(begin)}, "gql_control");
  auto select = make_uniq<SelectNode>();
  select->from_table = std::move(function);
  select->select_list.push_back(make_uniq<StarExpression>());
  auto statement = make_uniq<SelectStatement>();
  statement->node = std::move(select);
  return std::move(statement);
}

static unique_ptr<SQLStatement> ResultStatement() {
  auto select = make_uniq<SelectNode>();
  select->from_table = make_uniq<EmptyTableRef>();
  auto success = Constant(Value(true));
  success->SetAlias("success");
  select->select_list.push_back(std::move(success));
  auto statement = make_uniq<SelectStatement>();
  statement->node = std::move(select);
  return std::move(statement);
}

static unique_ptr<SQLStatement>
CreateSnapshot(const vector<GqlLogicalPlan> &plans,
               const string &snapshot_name) {
  auto create = make_uniq<CreateStatement>();
  auto info =
      make_uniq<CreateTableInfo>(TEMP_CATALOG, DEFAULT_SCHEMA, snapshot_name);
  info->temporary = true;
  info->query =
      unique_ptr_cast<SQLStatement, SelectStatement>(GqlLowerSelect(plans));
  create->info = std::move(info);
  return std::move(create);
}

static unique_ptr<SQLStatement> DropSnapshot(const string &snapshot_name) {
  auto drop = make_uniq<DropStatement>();
  auto info = make_uniq<DropInfo>();
  info->type = CatalogType::TABLE_ENTRY;
  info->catalog = TEMP_CATALOG;
  info->schema = DEFAULT_SCHEMA;
  info->name = snapshot_name;
  drop->info = std::move(info);
  return std::move(drop);
}

static unique_ptr<ParsedExpression> NonNullSnapshotValue(idx_t mutation_index) {
  auto value = Column("gql_mutation_match", ValueColumn(mutation_index));
  auto result = make_uniq<CaseExpression>();
  CaseCheck check;
  check.when_expr = make_uniq<OperatorExpression>(
      ExpressionType::OPERATOR_IS_NULL, value->Copy());
  vector<unique_ptr<ParsedExpression>> arguments;
  arguments.push_back(
      Constant(Value("GQL SET property expression produced NULL")));
  check.then_expr =
      make_uniq<FunctionExpression>("error", std::move(arguments));
  result->case_checks.push_back(std::move(check));
  result->else_expr = std::move(value);
  return std::move(result);
}

static unique_ptr<SQLStatement> SetProperty(const string &snapshot_name,
                                            idx_t mutation_index,
                                            const GqlBoundMutation &mutation) {
  auto update = make_uniq<UpdateStatement>();
  update->table = MutationTarget(mutation, "PROPERTY", "gql_mutation_target");
  update->from_table = Snapshot(snapshot_name, "gql_mutation_match");
  update->set_info = make_uniq<UpdateSetInfo>();
  update->set_info->columns.push_back(mutation.name);
  update->set_info->expressions.push_back(NonNullSnapshotValue(mutation_index));
  update->set_info->condition =
      Equal(Column("gql_mutation_target", KeyColumn(mutation)),
            Column("gql_mutation_match", TargetColumn(mutation_index)));
  return std::move(update);
}

static unique_ptr<SQLStatement> SetLabel(const string &snapshot_name,
                                         idx_t mutation_index,
                                         const GqlBoundMutation &mutation) {
  auto update = make_uniq<UpdateStatement>();
  update->table = MutationTarget(mutation, "LABEL", "gql_mutation_target");
  update->from_table = Snapshot(snapshot_name, "gql_mutation_match");
  update->set_info = make_uniq<UpdateSetInfo>();
  update->set_info->columns.push_back(LabelColumn(mutation));
  update->set_info->expressions.push_back(
      AppendLabel(Column("gql_mutation_target", LabelColumn(mutation)),
                  mutation.name));
  update->set_info->condition =
      Equal(Column("gql_mutation_target", KeyColumn(mutation)),
            Column("gql_mutation_match", TargetColumn(mutation_index)));
  return std::move(update);
}

static unique_ptr<SQLStatement>
RemoveProperty(const string &snapshot_name, idx_t mutation_index,
               const GqlBoundMutation &mutation) {
  auto update = make_uniq<UpdateStatement>();
  update->table = MutationTarget(mutation, "PROPERTY", "gql_mutation_target");
  update->from_table = Snapshot(snapshot_name, "gql_mutation_match");
  update->set_info = make_uniq<UpdateSetInfo>();
  update->set_info->columns.push_back(mutation.name);
  update->set_info->expressions.push_back(Constant(Value()));
  update->set_info->condition =
      Equal(Column("gql_mutation_target", KeyColumn(mutation)),
            Column("gql_mutation_match", TargetColumn(mutation_index)));
  return std::move(update);
}

static unique_ptr<SQLStatement> RemoveLabel(const string &snapshot_name,
                                            idx_t mutation_index,
                                            const GqlBoundMutation &mutation) {
  auto update = make_uniq<UpdateStatement>();
  update->table = MutationTarget(mutation, "LABEL", "gql_mutation_target");
  update->from_table = Snapshot(snapshot_name, "gql_mutation_match");
  update->set_info = make_uniq<UpdateSetInfo>();
  update->set_info->columns.push_back(LabelColumn(mutation));
  update->set_info->expressions.push_back(
      EraseLabel(Column("gql_mutation_target", LabelColumn(mutation)),
                 mutation.name));
  auto target =
      Equal(Column("gql_mutation_target", KeyColumn(mutation)),
            Column("gql_mutation_match", TargetColumn(mutation_index)));
  auto has_label =
      HasLabel(Column("gql_mutation_target", LabelColumn(mutation)),
               mutation.name);
  update->set_info->condition = And(std::move(target), std::move(has_label));
  return std::move(update);
}

static unique_ptr<SQLStatement>
RejectAttachedNodeDelete(const string &snapshot_name, idx_t mutation_index) {
  auto select = make_uniq<SelectNode>();
  unique_ptr<TableRef> from = MutationEdgeTable("gql_mutation_edge");
  auto snapshot = Snapshot(snapshot_name, "gql_mutation_match");
  auto join = make_uniq<JoinRef>(JoinRefType::REGULAR);
  join->left = std::move(from);
  join->right = std::move(snapshot);
  join->type = JoinType::INNER;
  join->condition =
      Or(Equal(Column("gql_mutation_edge", "__gql_source_id"),
               Column("gql_mutation_match", TargetColumn(mutation_index))),
         Equal(Column("gql_mutation_edge", "__gql_target_id"),
               Column("gql_mutation_match", TargetColumn(mutation_index))));
  select->from_table = std::move(join);
  vector<unique_ptr<ParsedExpression>> arguments;
  arguments.push_back(Constant(Value("GQL DELETE cannot remove a node with "
                                     "incident edges; use DETACH DELETE")));
  select->select_list.push_back(
      make_uniq<FunctionExpression>("error", std::move(arguments)));
  auto limit = make_uniq<LimitModifier>();
  limit->limit = Constant(Value::UBIGINT(1));
  select->modifiers.push_back(std::move(limit));
  auto statement = make_uniq<SelectStatement>();
  statement->node = std::move(select);
  return std::move(statement);
}

static unique_ptr<SQLStatement> DeleteIncidentEdges(const string &snapshot_name,
                                                    idx_t mutation_index) {
  auto deletion = make_uniq<DeleteStatement>();
  deletion->table = MutationEdgeTable("gql_mutation_edge");
  deletion->using_clauses.push_back(
      Snapshot(snapshot_name, "gql_mutation_match"));
  deletion->condition =
      Or(Equal(Column("gql_mutation_edge", "__gql_source_id"),
               Column("gql_mutation_match", TargetColumn(mutation_index))),
         Equal(Column("gql_mutation_edge", "__gql_target_id"),
               Column("gql_mutation_match", TargetColumn(mutation_index))));
  return std::move(deletion);
}

static unique_ptr<SQLStatement>
DeleteElement(const string &snapshot_name, idx_t mutation_index,
              const GqlBoundMutation &mutation) {
  auto deletion = make_uniq<DeleteStatement>();
  deletion->table = MutationTarget(mutation, "DELETE", "gql_mutation_target");
  deletion->using_clauses.push_back(
      Snapshot(snapshot_name, "gql_mutation_match"));
  deletion->condition =
      Equal(Column("gql_mutation_target", KeyColumn(mutation)),
            Column("gql_mutation_match", TargetColumn(mutation_index)));
  return std::move(deletion);
}

static unique_ptr<SQLStatement>
UpdateGraphVersion(const string &snapshot_name) {
  auto update = make_uniq<UpdateStatement>();
  auto graph_table = make_uniq<BaseTableRef>();
  graph_table->schema_name = "gql_internal";
  graph_table->table_name = "graphs";
  graph_table->alias = "gql_mutation_graph_table";
  update->table = std::move(graph_table);
  update->from_table = MutationGraph("gql_mutation_graph");
  update->set_info = make_uniq<UpdateSetInfo>();
  update->set_info->columns.push_back("graph_version");
  vector<unique_ptr<ParsedExpression>> addition;
  addition.push_back(Column("gql_mutation_graph_table", "graph_version"));
  addition.push_back(Constant(Value::UBIGINT(1)));
  auto increment = make_uniq<FunctionExpression>("+", std::move(addition));
  increment->is_operator = true;
  update->set_info->expressions.push_back(std::move(increment));
  auto graph_matches = Equal(Column("gql_mutation_graph_table", "graph_id"),
                             Column("gql_mutation_graph", "graph_id"));
  auto exists_select = make_uniq<SelectNode>();
  exists_select->from_table = Snapshot(snapshot_name, "gql_mutation_match");
  exists_select->select_list.push_back(Constant(Value::INTEGER(1)));
  auto exists_statement = make_uniq<SelectStatement>();
  exists_statement->node = std::move(exists_select);
  auto exists = make_uniq<SubqueryExpression>();
  exists->subquery_type = SubqueryType::EXISTS;
  exists->subquery = std::move(exists_statement);
  update->set_info->condition =
      And(std::move(graph_matches), std::move(exists));
  return std::move(update);
}

static unique_ptr<SQLStatement> UpdateGraphVersionAlways() {
  auto update = make_uniq<UpdateStatement>();
  auto graph_table = make_uniq<BaseTableRef>();
  graph_table->schema_name = "gql_internal";
  graph_table->table_name = "graphs";
  graph_table->alias = "gql_mutation_graph_table";
  update->table = std::move(graph_table);
  update->from_table = MutationGraph("gql_mutation_graph");
  update->set_info = make_uniq<UpdateSetInfo>();
  update->set_info->columns.push_back("graph_version");
  vector<unique_ptr<ParsedExpression>> addition;
  addition.push_back(Column("gql_mutation_graph_table", "graph_version"));
  addition.push_back(Constant(Value::UBIGINT(1)));
  auto increment = make_uniq<FunctionExpression>("+", std::move(addition));
  increment->is_operator = true;
  update->set_info->expressions.push_back(std::move(increment));
  update->set_info->condition =
      Equal(Column("gql_mutation_graph_table", "graph_id"),
            Column("gql_mutation_graph", "graph_id"));
  return std::move(update);
}

static const string &MappedProperty(const GqlElementTableBinding &table,
                                    const string &property) {
  for (const auto &entry : table.property_columns) {
    if (StringUtil::CIEquals(entry.first, property)) {
      return entry.second;
    }
  }
  throw BinderException(
      "Property '%s' is not mapped for managed graph table %s.%s", property,
      table.schema_name, table.table_name);
}

static GqlTableGraphBinding LoadSelectedGraph(ClientContext &context) {
  auto graph_name = GqlGetSelectedGraph(context);
  if (graph_name.empty()) {
    throw InvalidInputException(
        "No graph selected; use SESSION SET GRAPH before mutation");
  }
  GqlTableGraphBinding graph;
  if (!GqlTryLoadTableGraph(context, graph_name, graph)) {
    throw InvalidInputException(
        "Graph '%s' has no managed native tables; load it with COPY GRAPH",
        graph_name);
  }
  return graph;
}

static unique_ptr<TableRef>
MutationTargetBindReplace(ClientContext &context,
                          TableFunctionBindInput &input) {
  if (input.inputs.size() != 3 || input.inputs[0].IsNull() ||
      input.inputs[1].IsNull() || input.inputs[2].IsNull()) {
    throw BinderException(
        "GQL mutation target requires element kind, purpose, and property");
  }
  auto graph = LoadSelectedGraph(context);
  auto kind = input.inputs[0].GetValue<string>();
  auto purpose = input.inputs[1].GetValue<string>();
  auto property = input.inputs[2].GetValue<string>();
  const auto &table = kind == "EDGE" ? graph.edge
                      : kind == "VERTEX"
                          ? graph.vertex
                          : throw BinderException("Invalid GQL element kind");
  const auto expected_key = kind == "EDGE" ? "__gql_edge_id" : "__gql_id";
  if (!StringUtil::CIEquals(table.key_column, expected_key)) {
    throw NotImplementedException(
        "Managed GQL mutation requires the canonical %s key column",
        expected_key);
  }
  if (purpose == "PROPERTY") {
    auto &column = MappedProperty(table, property);
    if (!StringUtil::CIEquals(column, property)) {
      throw NotImplementedException("Property '%s' maps to physical column "
                                    "'%s'; aliased mutation columns are not "
                                    "implemented",
                                    property, column);
    }
  } else if (purpose == "LABEL") {
    auto expected_label = kind == "EDGE" ? "__gql_type" : "__gql_label";
    if (!StringUtil::CIEquals(table.label_column, expected_label)) {
      throw NotImplementedException(
          "Managed GQL mutation requires the canonical %s label column",
          expected_label);
    }
  } else if (purpose != "DELETE") {
    throw BinderException("Invalid GQL mutation purpose '%s'", purpose);
  }
  auto result = make_uniq<BaseTableRef>();
  result->catalog_name = table.catalog_name;
  result->schema_name = table.schema_name;
  result->table_name = table.table_name;
  return std::move(result);
}

static unique_ptr<TableRef> MutationGraphBindReplace(ClientContext &context,
                                                     TableFunctionBindInput &) {
  auto graph = LoadSelectedGraph(context);
  auto select = make_uniq<SelectNode>();
  select->from_table = make_uniq<EmptyTableRef>();
  auto id = Constant(Value::UBIGINT(graph.graph_id));
  id->SetAlias("graph_id");
  select->select_list.push_back(std::move(id));
  auto statement = make_uniq<SelectStatement>();
  statement->node = std::move(select);
  return make_uniq<SubqueryRef>(std::move(statement));
}

TableFunction GqlMutationTargetFunction() {
  TableFunction function(
      "gql_mutation_target",
      {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
      nullptr, nullptr);
  function.bind_replace = MutationTargetBindReplace;
  return function;
}

TableFunction GqlMutationGraphFunction() {
  TableFunction function("gql_mutation_graph", {}, nullptr, nullptr);
  function.bind_replace = MutationGraphBindReplace;
  return function;
}

static unique_ptr<TableRef>
MergeTargetBindReplace(ClientContext &context, TableFunctionBindInput &input) {
  if (input.inputs.size() != 1 || input.inputs[0].IsNull()) {
    throw BinderException("GQL MERGE target requires a property-name list");
  }
  auto graph = LoadSelectedGraph(context);
  if (!StringUtil::CIEquals(graph.vertex.key_column, "__gql_id") ||
      !StringUtil::CIEquals(graph.vertex.label_column, "__gql_label")) {
    throw NotImplementedException(
        "GQL MERGE requires canonical __gql_id and __gql_label columns");
  }
  for (const auto &entry : ListValue::GetChildren(input.inputs[0])) {
    auto property = entry.GetValue<string>();
    auto &column = MappedProperty(graph.vertex, property);
    if (!StringUtil::CIEquals(column, property)) {
      throw NotImplementedException(
          "MERGE property '%s' maps to physical column '%s'; aliased columns "
          "are not implemented",
          property, column);
    }
  }
  auto result = make_uniq<BaseTableRef>();
  result->catalog_name = graph.vertex.catalog_name;
  result->schema_name = graph.vertex.schema_name;
  result->table_name = graph.vertex.table_name;
  return std::move(result);
}

static unique_ptr<TableRef> MergeIdBindReplace(ClientContext &context,
                                               TableFunctionBindInput &) {
  auto graph = LoadSelectedGraph(context);
  vector<unique_ptr<ParsedExpression>> arguments;
  arguments.push_back(Constant(Value(
      "gql_internal.graph_" + to_string(graph.graph_id) + "_vertex_id_seq")));
  auto next_id = make_uniq<FunctionExpression>("nextval", std::move(arguments));
  next_id->SetAlias("__gql_new_id");
  auto select = make_uniq<SelectNode>();
  select->from_table = make_uniq<EmptyTableRef>();
  select->select_list.push_back(std::move(next_id));
  auto statement = make_uniq<SelectStatement>();
  statement->node = std::move(select);
  return make_uniq<SubqueryRef>(std::move(statement));
}

TableFunction GqlMergeTargetFunction() {
  TableFunction function("gql_merge_target",
                         {LogicalType::LIST(LogicalType::VARCHAR)}, nullptr,
                         nullptr);
  function.bind_replace = MergeTargetBindReplace;
  return function;
}

TableFunction GqlMergeIdFunction() {
  TableFunction function("gql_merge_id", {}, nullptr, nullptr);
  function.bind_replace = MergeIdBindReplace;
  return function;
}

static Value MergeLiteralValue(const GqlLiteral &literal) {
  switch (literal.type) {
  case GqlLiteralType::BOOLEAN:
    return Value::BOOLEAN(literal.value == "true");
  case GqlLiteralType::INTEGER:
    return Value::BIGINT(std::stoll(literal.value));
  case GqlLiteralType::DECIMAL:
    return Value(literal.value).DefaultCastAs(LogicalType::DECIMAL(38, 18));
  case GqlLiteralType::DOUBLE:
    return Value::DOUBLE(std::stod(literal.value));
  case GqlLiteralType::STRING:
    return Value(literal.value);
  case GqlLiteralType::NULL_VALUE:
    throw BinderException(
        "NULL properties are not supported in MERGE patterns");
  }
  throw InternalException("Unknown MERGE literal type");
}

static unique_ptr<SQLStatement>
LowerMergeStatement(const GqlMergeStatement &merge) {
  vector<Value> property_names;
  for (const auto &property : merge.vertex.properties) {
    property_names.emplace_back(property.name.value);
  }

  auto statement = make_uniq<MergeIntoStatement>();
  statement->target =
      FunctionTable("gql_merge_target",
                    {Value::LIST(LogicalType::VARCHAR, property_names)},
                    "gql_merge_target");
  statement->source = FunctionTable("gql_merge_id", {}, "gql_merge_source");

  unique_ptr<ParsedExpression> condition;
  for (const auto &label : merge.vertex.labels) {
    auto comparison =
        HasLabel(Column("gql_merge_target", "__gql_label"), label.value);
    condition = condition ? And(std::move(condition), std::move(comparison))
                          : std::move(comparison);
  }
  for (const auto &property : merge.vertex.properties) {
    auto comparison =
        NotDistinct(Column("gql_merge_target", property.name.value),
                    Constant(MergeLiteralValue(property.value)));
    condition = condition ? And(std::move(condition), std::move(comparison))
                          : std::move(comparison);
  }
  if (!condition) {
    throw InternalException("MERGE lowering requires a match condition");
  }
  statement->join_condition = std::move(condition);

  auto insert = make_uniq<MergeIntoAction>();
  insert->action_type = MergeActionType::MERGE_INSERT;
  insert->insert_columns = {"__gql_id", "__gql_external_id", "__gql_label"};
  insert->expressions.push_back(Column("gql_merge_source", "__gql_new_id"));
  insert->expressions.push_back(make_uniq<CastExpression>(
      LogicalType::VARCHAR,
      Column("gql_merge_source", "__gql_new_id")));
  vector<string> labels;
  for (const auto &label : merge.vertex.labels) {
    labels.push_back(label.value);
  }
  insert->expressions.push_back(Constant(
      labels.empty() ? Value() : Value(StringUtil::Join(labels, ";"))));
  for (const auto &property : merge.vertex.properties) {
    insert->insert_columns.push_back(property.name.value);
    insert->expressions.push_back(Constant(MergeLiteralValue(property.value)));
  }
  statement->actions[MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET]
      .push_back(std::move(insert));
  return std::move(statement);
}

vector<unique_ptr<SQLStatement>>
GqlLowerMerge(const GqlMergeStatement &merge) {
  static atomic<uint64_t> next_merge_command_id(0);
  auto command_id = "gql_merge_" +
                    to_string(next_merge_command_id.fetch_add(
                        1, std::memory_order_relaxed));
  vector<unique_ptr<SQLStatement>> statements;
  statements.push_back(ControlStatement(command_id, true));
  statements.push_back(LowerMergeStatement(merge));
  statements.push_back(UpdateGraphVersionAlways());
  statements.push_back(ControlStatement(command_id, false));
  statements.push_back(ResultStatement());
  return statements;
}

vector<unique_ptr<SQLStatement>>
GqlLowerMutation(const vector<GqlLogicalPlan> &plans) {
  if (plans.empty() || plans[0].mutations.empty()) {
    throw InternalException("GQL mutation lowering requires a bound mutation");
  }
  static atomic<uint64_t> next_command_id(0);
  auto command_id =
      "gql_mutation_" +
      to_string(next_command_id.fetch_add(1, std::memory_order_relaxed));
  auto snapshot_name = "_" + command_id;
  vector<unique_ptr<SQLStatement>> statements;
  statements.push_back(ControlStatement(command_id, true));
  statements.push_back(CreateSnapshot(plans, snapshot_name));
  for (idx_t mutation_index = 0; mutation_index < plans[0].mutations.size();
       mutation_index++) {
    const auto &mutation = plans[0].mutations[mutation_index];
    switch (mutation.type) {
    case GqlMutationType::SET_PROPERTY:
      statements.push_back(
          SetProperty(snapshot_name, mutation_index, mutation));
      break;
    case GqlMutationType::SET_LABEL:
      statements.push_back(SetLabel(snapshot_name, mutation_index, mutation));
      break;
    case GqlMutationType::REMOVE_PROPERTY:
      statements.push_back(
          RemoveProperty(snapshot_name, mutation_index, mutation));
      break;
    case GqlMutationType::REMOVE_LABEL:
      statements.push_back(
          RemoveLabel(snapshot_name, mutation_index, mutation));
      break;
    case GqlMutationType::DELETE_ELEMENT: {
      auto node = mutation.binding_type.id == GqlTypeId::NODE;
      if (node && mutation.detach) {
        statements.push_back(
            DeleteIncidentEdges(snapshot_name, mutation_index));
      } else if (node) {
        statements.push_back(
            RejectAttachedNodeDelete(snapshot_name, mutation_index));
      }
      statements.push_back(
          DeleteElement(snapshot_name, mutation_index, mutation));
      break;
    }
    case GqlMutationType::CLEAR_PROPERTIES:
      throw NotImplementedException(
          "SET variable = {...} over managed wide tables");
    }
  }
  statements.push_back(UpdateGraphVersion(snapshot_name));
  statements.push_back(DropSnapshot(snapshot_name));
  statements.push_back(ControlStatement(command_id, false));
  statements.push_back(ResultStatement());
  return statements;
}

} // namespace duckdb
