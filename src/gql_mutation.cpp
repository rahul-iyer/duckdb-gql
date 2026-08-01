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
#include "duckdb/parser/expression/lambda_expression.hpp"
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

#include <algorithm>
#include <atomic>

namespace duckdb {

static unique_ptr<ParsedExpression> Column(const string &table, const string &column) {
	return make_uniq<ColumnRefExpression>(column, table);
}

static unique_ptr<ParsedExpression> Constant(Value value) {
	return make_uniq<ConstantExpression>(std::move(value));
}

static unique_ptr<ParsedExpression> Equal(unique_ptr<ParsedExpression> left, unique_ptr<ParsedExpression> right) {
	return make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(left), std::move(right));
}

static unique_ptr<ParsedExpression> NotDistinct(unique_ptr<ParsedExpression> left, unique_ptr<ParsedExpression> right) {
	return make_uniq<ComparisonExpression>(ExpressionType::COMPARE_NOT_DISTINCT_FROM, std::move(left),
	                                       std::move(right));
}

static unique_ptr<ParsedExpression> And(unique_ptr<ParsedExpression> left, unique_ptr<ParsedExpression> right) {
	return make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(left), std::move(right));
}

static unique_ptr<ParsedExpression> Or(unique_ptr<ParsedExpression> left, unique_ptr<ParsedExpression> right) {
	return make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_OR, std::move(left), std::move(right));
}

static unique_ptr<ParsedExpression> Function(const string &name, vector<unique_ptr<ParsedExpression>> arguments) {
	return make_uniq<FunctionExpression>(name, std::move(arguments));
}

static unique_ptr<ParsedExpression> Subtract(unique_ptr<ParsedExpression> left, unique_ptr<ParsedExpression> right) {
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(std::move(left));
	arguments.push_back(std::move(right));
	auto result = make_uniq<FunctionExpression>("-", std::move(arguments));
	result->is_operator = true;
	return std::move(result);
}

static void AppendStructField(vector<unique_ptr<ParsedExpression>> &fields, unique_ptr<ParsedExpression> expression,
                              const string &name) {
	expression->SetAlias(name);
	fields.push_back(std::move(expression));
}

static Value LabelListValue(const vector<string> &labels) {
	vector<Value> values;
	values.reserve(labels.size());
	for (const auto &label : labels) {
		values.emplace_back(label);
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(values));
}

static unique_ptr<ParsedExpression> HasLabel(unique_ptr<ParsedExpression> labels, const string &label, bool is_list) {
	if (is_list) {
		vector<unique_ptr<ParsedExpression>> contains_arguments;
		contains_arguments.push_back(std::move(labels));
		contains_arguments.push_back(Constant(Value(label)));
		return Function("list_contains", std::move(contains_arguments));
	}
	return make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(labels), Constant(Value(label)));
}

static unique_ptr<ParsedExpression> AppendLabel(unique_ptr<ParsedExpression> labels, const string &label,
                                                bool is_list) {
	auto result = make_uniq<CaseExpression>();

	CaseCheck missing;
	missing.when_expr = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NULL, labels->Copy());
	missing.then_expr = Constant(is_list ? LabelListValue({label}) : Value(label));
	result->case_checks.push_back(std::move(missing));

	CaseCheck present;
	present.when_expr = HasLabel(labels->Copy(), label, is_list);
	present.then_expr = labels->Copy();
	result->case_checks.push_back(std::move(present));

	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(std::move(labels));
	arguments.push_back(Constant(Value(label)));
	if (is_list) {
		result->else_expr = Function("list_append", std::move(arguments));
	} else {
		arguments.insert(arguments.begin() + 1, Constant(Value(";")));
		result->else_expr = Function("concat", std::move(arguments));
	}
	return std::move(result);
}

static unique_ptr<ParsedExpression> EraseLabel(unique_ptr<ParsedExpression> labels, const string &label, bool is_list) {
	if (is_list) {
		auto item = make_uniq<ColumnRefExpression>("gql_label_item");
		auto predicate =
		    make_uniq<ComparisonExpression>(ExpressionType::COMPARE_NOTEQUAL, std::move(item), Constant(Value(label)));
		vector<unique_ptr<ParsedExpression>> arguments;
		arguments.push_back(std::move(labels));
		arguments.push_back(make_uniq<LambdaExpression>(vector<string> {"gql_label_item"}, std::move(predicate)));
		return Function("list_filter", std::move(arguments));
	}
	vector<unique_ptr<ParsedExpression>> padded_arguments;
	padded_arguments.push_back(Constant(Value(";")));
	padded_arguments.push_back(std::move(labels));
	padded_arguments.push_back(Constant(Value(";")));

	vector<unique_ptr<ParsedExpression>> needle_arguments;
	needle_arguments.push_back(Constant(Value(";")));
	needle_arguments.push_back(Constant(Value(label)));
	needle_arguments.push_back(Constant(Value(";")));

	vector<unique_ptr<ParsedExpression>> replace_arguments;
	replace_arguments.push_back(Function("concat", std::move(padded_arguments)));
	replace_arguments.push_back(Function("concat", std::move(needle_arguments)));
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
	return mutation.binding_type.id == GqlTypeId::EDGE ? "__gql_edge_id" : "__gql_id";
}

static const char *LabelColumn(const GqlBoundMutation &mutation) {
	return mutation.binding_type.id == GqlTypeId::EDGE ? "__gql_type" : "__gql_label";
}

static unique_ptr<TableRef> FunctionTable(const string &name, vector<Value> values, const string &alias) {
	vector<unique_ptr<ParsedExpression>> arguments;
	for (auto &value : values) {
		arguments.push_back(Constant(std::move(value)));
	}
	auto result = make_uniq<TableFunctionRef>();
	result->function = make_uniq<FunctionExpression>(name, std::move(arguments));
	result->alias = alias;
	return std::move(result);
}

static unique_ptr<TableRef> MutationTarget(const GqlBoundMutation &mutation, const string &purpose,
                                           const string &alias) {
	return FunctionTable("gql_mutation_target", {Value(ElementKind(mutation)), Value(purpose), Value(mutation.name)},
	                     alias);
}

static unique_ptr<TableRef> MutationEdgeTable(const string &alias) {
	return FunctionTable("gql_mutation_target", {Value("EDGE"), Value("DELETE"), Value("")}, alias);
}

static unique_ptr<TableRef> MutationGraph(const string &alias) {
	return FunctionTable("gql_mutation_graph", {}, alias);
}

static unique_ptr<TableRef> Snapshot(const string &snapshot_name, const string &alias) {
	auto result = make_uniq<BaseTableRef>();
	result->table_name = snapshot_name;
	result->alias = alias;
	return std::move(result);
}

static unique_ptr<SQLStatement> ControlStatement(const string &command_id, bool begin) {
	auto function = FunctionTable("gql_mutation_control", {Value(command_id), Value(begin)}, "gql_control");
	auto select = make_uniq<SelectNode>();
	select->from_table = std::move(function);
	select->select_list.push_back(make_uniq<StarExpression>());
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(select);
	return std::move(statement);
}

static string ControlSnapshotName(const string &command_id) {
	return "_" + command_id + "_control";
}

static unique_ptr<SQLStatement> QuietBeginControlStatement(const string &command_id) {
	auto create = make_uniq<CreateStatement>();
	auto info = make_uniq<CreateTableInfo>(TEMP_CATALOG, DEFAULT_SCHEMA, ControlSnapshotName(command_id));
	info->temporary = true;
	auto select = make_uniq<SelectNode>();
	select->from_table = FunctionTable("gql_mutation_control", {Value(command_id), Value(true)}, "gql_control");
	select->select_list.push_back(make_uniq<StarExpression>());
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(select);
	info->query = std::move(statement);
	create->info = std::move(info);
	return std::move(create);
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

static unique_ptr<SQLStatement> InsertNodeResultStatement(const string &command_id, idx_t vertex_count,
                                                          idx_t vertex_index, const string &return_name) {
	auto select = make_uniq<SelectNode>();
	select->from_table = FunctionTable(
	    "gql_insert_result",
	    {Value(command_id), Value::UBIGINT(vertex_count), Value::UBIGINT(vertex_index), Value(return_name)},
	    "gql_insert_result");
	select->select_list.push_back(make_uniq<StarExpression>());
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(select);
	return std::move(statement);
}

static unique_ptr<ParsedExpression> VertexNode(const GqlTableGraphBinding &graph, const string &vertex_alias,
                                               const string &return_name) {
	vector<unique_ptr<ParsedExpression>> fields;
	AppendStructField(fields, Column(vertex_alias, graph.vertex.key_column), "vertex_id");
	vector<unique_ptr<ParsedExpression>> label_arguments;
	label_arguments.push_back(Column(vertex_alias, graph.vertex.label_column));
	label_arguments.push_back(Constant(Value(";")));
	AppendStructField(fields, Function("array_to_string", std::move(label_arguments)), "__gql_labels");
	vector<pair<string, string>> properties(graph.vertex.property_columns.begin(), graph.vertex.property_columns.end());
	std::sort(properties.begin(), properties.end(),
	          [](const auto &left, const auto &right) { return StringUtil::CILessThan(left.first, right.first); });
	for (const auto &property : properties) {
		AppendStructField(fields, Column(vertex_alias, property.second), property.first);
	}
	auto node = Function("struct_pack", std::move(fields));
	node->SetAlias(return_name);
	return std::move(node);
}

static unique_ptr<SQLStatement> CreateSnapshot(const vector<GqlLogicalPlan> &plans, const string &snapshot_name) {
	auto create = make_uniq<CreateStatement>();
	auto info = make_uniq<CreateTableInfo>(TEMP_CATALOG, DEFAULT_SCHEMA, snapshot_name);
	info->temporary = true;
	info->query = unique_ptr_cast<SQLStatement, SelectStatement>(GqlLowerSelect(plans));
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

static unique_ptr<SQLStatement> SetProperty(const string &snapshot_name, idx_t mutation_index,
                                            const GqlBoundMutation &mutation) {
	auto update = make_uniq<UpdateStatement>();
	update->table = MutationTarget(mutation, "PROPERTY", "gql_mutation_target");
	update->from_table = Snapshot(snapshot_name, "gql_mutation_match");
	update->set_info = make_uniq<UpdateSetInfo>();
	update->set_info->columns.push_back(mutation.name);
	update->set_info->expressions.push_back(Column("gql_mutation_match", ValueColumn(mutation_index)));
	update->set_info->condition = Equal(Column("gql_mutation_target", KeyColumn(mutation)),
	                                    Column("gql_mutation_match", TargetColumn(mutation_index)));
	return std::move(update);
}

static unique_ptr<SQLStatement> SetLabel(const string &snapshot_name, idx_t mutation_index,
                                         const GqlBoundMutation &mutation) {
	auto is_list = mutation.binding_type.id != GqlTypeId::EDGE;
	auto update = make_uniq<UpdateStatement>();
	update->table = MutationTarget(mutation, "LABEL", "gql_mutation_target");
	update->from_table = Snapshot(snapshot_name, "gql_mutation_match");
	update->set_info = make_uniq<UpdateSetInfo>();
	update->set_info->columns.push_back(LabelColumn(mutation));
	update->set_info->expressions.push_back(
	    AppendLabel(Column("gql_mutation_target", LabelColumn(mutation)), mutation.name, is_list));
	update->set_info->condition = Equal(Column("gql_mutation_target", KeyColumn(mutation)),
	                                    Column("gql_mutation_match", TargetColumn(mutation_index)));
	return std::move(update);
}

static unique_ptr<SQLStatement> ClearProperties(const string &snapshot_name, idx_t mutation_index,
                                                const GqlBoundMutation &mutation) {
	auto statement = make_uniq<MergeIntoStatement>();
	statement->target = MutationTarget(mutation, "DELETE", "gql_clear_target");
	statement->source = FunctionTable(
	    "gql_clear_properties_source",
	    {Value(ElementKind(mutation)), Value(snapshot_name), Value(TargetColumn(mutation_index))}, "gql_clear_source");
	statement->join_condition =
	    Equal(Column("gql_clear_target", KeyColumn(mutation)), Column("gql_clear_source", KeyColumn(mutation)));

	// The bind replacement exposes the key unchanged and every mapped property
	// as a typed NULL. UPDATE BY NAME therefore adapts to the managed wide-table
	// schema without making the parser-side mutation plan depend on catalog
	// metadata. Including the unchanged key also keeps an empty property map a
	// valid no-op update for element tables with no mapped properties.
	auto action = make_uniq<MergeIntoAction>();
	action->action_type = MergeActionType::MERGE_UPDATE;
	action->column_order = InsertColumnOrder::INSERT_BY_NAME;
	statement->actions[MergeActionCondition::WHEN_MATCHED].push_back(std::move(action));
	return std::move(statement);
}

static unique_ptr<SQLStatement> RemoveProperty(const string &snapshot_name, idx_t mutation_index,
                                               const GqlBoundMutation &mutation) {
	auto update = make_uniq<UpdateStatement>();
	update->table = MutationTarget(mutation, "PROPERTY", "gql_mutation_target");
	update->from_table = Snapshot(snapshot_name, "gql_mutation_match");
	update->set_info = make_uniq<UpdateSetInfo>();
	update->set_info->columns.push_back(mutation.name);
	update->set_info->expressions.push_back(Constant(Value()));
	update->set_info->condition = Equal(Column("gql_mutation_target", KeyColumn(mutation)),
	                                    Column("gql_mutation_match", TargetColumn(mutation_index)));
	return std::move(update);
}

static unique_ptr<SQLStatement> RemoveLabel(const string &snapshot_name, idx_t mutation_index,
                                            const GqlBoundMutation &mutation) {
	auto is_list = mutation.binding_type.id != GqlTypeId::EDGE;
	auto update = make_uniq<UpdateStatement>();
	update->table = MutationTarget(mutation, "LABEL", "gql_mutation_target");
	update->from_table = Snapshot(snapshot_name, "gql_mutation_match");
	update->set_info = make_uniq<UpdateSetInfo>();
	update->set_info->columns.push_back(LabelColumn(mutation));
	update->set_info->expressions.push_back(
	    EraseLabel(Column("gql_mutation_target", LabelColumn(mutation)), mutation.name, is_list));
	auto target = Equal(Column("gql_mutation_target", KeyColumn(mutation)),
	                    Column("gql_mutation_match", TargetColumn(mutation_index)));
	auto has_label = HasLabel(Column("gql_mutation_target", LabelColumn(mutation)), mutation.name, is_list);
	update->set_info->condition = And(std::move(target), std::move(has_label));
	return std::move(update);
}

static unique_ptr<SQLStatement> RejectAttachedNodeDelete(const string &snapshot_name, idx_t mutation_index) {
	auto select = make_uniq<SelectNode>();
	unique_ptr<TableRef> from = MutationEdgeTable("gql_mutation_edge");
	auto snapshot = Snapshot(snapshot_name, "gql_mutation_match");
	auto join = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join->left = std::move(from);
	join->right = std::move(snapshot);
	join->type = JoinType::INNER;
	join->condition = Or(Equal(Column("gql_mutation_edge", "__gql_source_id"),
	                           Column("gql_mutation_match", TargetColumn(mutation_index))),
	                     Equal(Column("gql_mutation_edge", "__gql_target_id"),
	                           Column("gql_mutation_match", TargetColumn(mutation_index))));
	select->from_table = std::move(join);
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(Constant(Value("GQL DELETE cannot remove a node with "
	                                   "incident edges; use DETACH DELETE")));
	select->select_list.push_back(make_uniq<FunctionExpression>("error", std::move(arguments)));
	auto limit = make_uniq<LimitModifier>();
	limit->limit = Constant(Value::UBIGINT(1));
	select->modifiers.push_back(std::move(limit));
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(select);
	return std::move(statement);
}

static unique_ptr<SQLStatement> DeleteIncidentEdges(const string &snapshot_name, idx_t mutation_index) {
	auto deletion = make_uniq<DeleteStatement>();
	deletion->table = MutationEdgeTable("gql_mutation_edge");
	deletion->using_clauses.push_back(Snapshot(snapshot_name, "gql_mutation_match"));
	deletion->condition = Or(Equal(Column("gql_mutation_edge", "__gql_source_id"),
	                               Column("gql_mutation_match", TargetColumn(mutation_index))),
	                         Equal(Column("gql_mutation_edge", "__gql_target_id"),
	                               Column("gql_mutation_match", TargetColumn(mutation_index))));
	return std::move(deletion);
}

static unique_ptr<SQLStatement> DeleteElement(const string &snapshot_name, idx_t mutation_index,
                                              const GqlBoundMutation &mutation) {
	auto deletion = make_uniq<DeleteStatement>();
	deletion->table = MutationTarget(mutation, "DELETE", "gql_mutation_target");
	deletion->using_clauses.push_back(Snapshot(snapshot_name, "gql_mutation_match"));
	deletion->condition = Equal(Column("gql_mutation_target", KeyColumn(mutation)),
	                            Column("gql_mutation_match", TargetColumn(mutation_index)));
	return std::move(deletion);
}

static unique_ptr<SQLStatement> UpdateGraphVersion(const string &snapshot_name, idx_t mutation_count = 0) {
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
	auto graph_matches =
	    Equal(Column("gql_mutation_graph_table", "graph_id"), Column("gql_mutation_graph", "graph_id"));
	auto exists_select = make_uniq<SelectNode>();
	exists_select->from_table = Snapshot(snapshot_name, "gql_mutation_match");
	exists_select->select_list.push_back(Constant(Value::INTEGER(1)));
	if (mutation_count > 0) {
		unique_ptr<ParsedExpression> has_target;
		for (idx_t mutation_index = 0; mutation_index < mutation_count; mutation_index++) {
			auto present = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL,
			                                             Column("gql_mutation_match", TargetColumn(mutation_index)));
			has_target = has_target ? Or(std::move(has_target), std::move(present)) : std::move(present);
		}
		exists_select->where_clause = std::move(has_target);
	}
	auto exists_statement = make_uniq<SelectStatement>();
	exists_statement->node = std::move(exists_select);
	auto exists = make_uniq<SubqueryExpression>();
	exists->subquery_type = SubqueryType::EXISTS;
	exists->subquery = std::move(exists_statement);
	update->set_info->condition = And(std::move(graph_matches), std::move(exists));
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
	    Equal(Column("gql_mutation_graph_table", "graph_id"), Column("gql_mutation_graph", "graph_id"));
	return std::move(update);
}

static const string &MappedProperty(const GqlElementTableBinding &table, const string &property) {
	for (const auto &entry : table.property_columns) {
		if (StringUtil::CIEquals(entry.first, property)) {
			return entry.second;
		}
	}
	throw BinderException("Property '%s' is not mapped for managed graph table %s.%s", property, table.schema_name,
	                      table.table_name);
}

static GqlTableGraphBinding LoadSelectedGraph(ClientContext &context) {
	auto graph_name = GqlGetSelectedGraph(context);
	if (graph_name.empty()) {
		throw InvalidInputException("No graph selected; use SESSION SET GRAPH before mutation");
	}
	GqlTableGraphBinding graph;
	if (!GqlTryLoadTableGraph(context, graph_name, graph)) {
		throw InvalidInputException("Graph '%s' has no managed native tables; load it with COPY GRAPH", graph_name);
	}
	return graph;
}

static unique_ptr<TableRef> MutationTargetBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	if (input.inputs.size() != 3 || input.inputs[0].IsNull() || input.inputs[1].IsNull() || input.inputs[2].IsNull()) {
		throw BinderException("GQL mutation target requires element kind, purpose, and property");
	}
	auto graph = LoadSelectedGraph(context);
	auto kind = input.inputs[0].GetValue<string>();
	auto purpose = input.inputs[1].GetValue<string>();
	auto property = input.inputs[2].GetValue<string>();
	const auto &table = kind == "EDGE"     ? graph.edge
	                    : kind == "VERTEX" ? graph.vertex
	                                       : throw BinderException("Invalid GQL element kind");
	const auto expected_key = kind == "EDGE" ? "__gql_edge_id" : "__gql_id";
	if (!StringUtil::CIEquals(table.key_column, expected_key)) {
		throw NotImplementedException("Managed GQL mutation requires the canonical %s key column", expected_key);
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
		if (kind == "EDGE") {
			throw NotImplementedException(
			    "DuckGQL edges have exactly one immutable type; edge label SET/REMOVE is not supported");
		}
		auto expected_label = kind == "EDGE" ? "__gql_type" : "__gql_label";
		if (!StringUtil::CIEquals(table.label_column, expected_label)) {
			throw NotImplementedException("Managed GQL mutation requires the canonical %s label column",
			                              expected_label);
		}
		if (kind == "VERTEX" && !table.label_is_list) {
			throw NotImplementedException("Managed vertex label mutation requires native LIST_COLUMN storage; "
			                              "recreate and reload the graph with COPY GRAPH");
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

static unique_ptr<TableRef> MutationGraphBindReplace(ClientContext &context, TableFunctionBindInput &) {
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
	TableFunction function("gql_mutation_target", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                       nullptr, nullptr);
	function.bind_replace = MutationTargetBindReplace;
	return function;
}

static unique_ptr<TableRef> ClearPropertiesSourceBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	if (input.inputs.size() != 3 || input.inputs[0].IsNull() || input.inputs[1].IsNull() || input.inputs[2].IsNull()) {
		throw BinderException("GQL property-map replacement requires element kind, "
		                      "snapshot, and target column");
	}
	auto graph = LoadSelectedGraph(context);
	auto kind = input.inputs[0].GetValue<string>();
	auto snapshot_name = input.inputs[1].GetValue<string>();
	auto target_column = input.inputs[2].GetValue<string>();
	const auto &table = kind == "EDGE"     ? graph.edge
	                    : kind == "VERTEX" ? graph.vertex
	                                       : throw BinderException("Invalid GQL element kind");
	auto expected_key = kind == "EDGE" ? "__gql_edge_id" : "__gql_id";
	if (!StringUtil::CIEquals(table.key_column, expected_key)) {
		throw NotImplementedException("Managed GQL property-map replacement requires canonical %s keys", expected_key);
	}

	auto target = make_uniq<BaseTableRef>();
	target->catalog_name = table.catalog_name;
	target->schema_name = table.schema_name;
	target->table_name = table.table_name;
	target->alias = "gql_clear_input";
	auto snapshot = Snapshot(snapshot_name, "gql_clear_snapshot");
	auto join = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join->left = std::move(target);
	join->right = std::move(snapshot);
	join->type = JoinType::INNER;
	join->condition = Equal(Column("gql_clear_input", table.key_column), Column("gql_clear_snapshot", target_column));

	auto select = make_uniq<SelectNode>();
	select->from_table = std::move(join);
	select->modifiers.push_back(make_uniq<DistinctModifier>());
	auto key = Column("gql_clear_input", table.key_column);
	key->SetAlias(table.key_column);
	select->select_list.push_back(std::move(key));
	case_insensitive_set_t physical_properties;
	for (const auto &entry : table.property_columns) {
		if (!physical_properties.insert(entry.second).second) {
			continue;
		}
		auto value = Constant(Value());
		value->SetAlias(entry.second);
		select->select_list.push_back(std::move(value));
	}
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(select);
	return make_uniq<SubqueryRef>(std::move(statement));
}

TableFunction GqlClearPropertiesSourceFunction() {
	TableFunction function("gql_clear_properties_source",
	                       {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr, nullptr);
	function.bind_replace = ClearPropertiesSourceBindReplace;
	return function;
}

TableFunction GqlMutationGraphFunction() {
	TableFunction function("gql_mutation_graph", {}, nullptr, nullptr);
	function.bind_replace = MutationGraphBindReplace;
	return function;
}

static unique_ptr<TableRef> InsertTargetBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	if (input.inputs.size() != 2 || input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("GQL INSERT target requires an element kind and property-name list");
	}
	auto graph = LoadSelectedGraph(context);
	auto kind = input.inputs[0].GetValue<string>();
	const auto &table = kind == "EDGE"     ? graph.edge
	                    : kind == "VERTEX" ? graph.vertex
	                                       : throw BinderException("Invalid GQL element kind");
	auto expected_key = kind == "EDGE" ? "__gql_edge_id" : "__gql_id";
	auto expected_label = kind == "EDGE" ? "__gql_type" : "__gql_label";
	if (!StringUtil::CIEquals(table.key_column, expected_key) ||
	    !StringUtil::CIEquals(table.label_column, expected_label)) {
		throw NotImplementedException("GQL INSERT requires canonical %s and %s columns", expected_key, expected_label);
	}
	if (kind == "VERTEX" && !table.label_is_list) {
		throw NotImplementedException("GQL INSERT requires native vertex LIST_COLUMN storage; "
		                              "recreate and reload the graph with COPY GRAPH");
	}
	if (kind == "EDGE" && (!StringUtil::CIEquals(graph.edge_source_column, "__gql_source_id") ||
	                       !StringUtil::CIEquals(graph.edge_target_column, "__gql_target_id"))) {
		throw NotImplementedException("GQL INSERT requires canonical edge endpoint columns");
	}
	case_insensitive_set_t properties;
	for (const auto &entry : ListValue::GetChildren(input.inputs[1])) {
		auto property = entry.GetValue<string>();
		if (!properties.insert(property).second) {
			throw BinderException("Duplicate GQL INSERT property '%s'", property);
		}
		auto &column = MappedProperty(table, property);
		if (!StringUtil::CIEquals(column, property)) {
			throw NotImplementedException("INSERT property '%s' maps to physical column '%s'; aliased "
			                              "columns are not implemented",
			                              property, column);
		}
	}
	auto result = make_uniq<BaseTableRef>();
	result->catalog_name = table.catalog_name;
	result->schema_name = table.schema_name;
	result->table_name = table.table_name;
	return std::move(result);
}

static unique_ptr<TableRef> InsertResultBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	if (input.inputs.size() != 4 || input.inputs[0].IsNull() || input.inputs[1].IsNull() || input.inputs[2].IsNull() ||
	    input.inputs[3].IsNull()) {
		throw BinderException("GQL INSERT result requires a command id, vertex count, vertex index, and result name");
	}
	auto command_id = input.inputs[0].GetValue<string>();
	auto vertex_count = input.inputs[1].GetValue<uint64_t>();
	auto vertex_index = input.inputs[2].GetValue<uint64_t>();
	auto return_name = input.inputs[3].GetValue<string>();
	if (command_id.empty() || vertex_count == 0 || vertex_index >= vertex_count || return_name.empty()) {
		throw BinderException("Invalid GQL INSERT node result specification");
	}
	auto graph = LoadSelectedGraph(context);
	if (!StringUtil::CIEquals(graph.vertex.key_column, "__gql_id") ||
	    !StringUtil::CIEquals(graph.vertex.label_column, "__gql_label") || !graph.vertex.label_is_list) {
		throw NotImplementedException("GQL INSERT RETURN requires canonical managed vertex storage");
	}

	auto vertex_alias = "gql_insert_return_vertex";
	auto vertex = make_uniq<BaseTableRef>();
	vertex->catalog_name = graph.vertex.catalog_name;
	vertex->schema_name = graph.vertex.schema_name;
	vertex->table_name = graph.vertex.table_name;
	vertex->alias = vertex_alias;
	auto control =
	    FunctionTable("gql_mutation_control", {Value(command_id), Value(false)}, "gql_insert_return_control");
	auto join = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join->left = std::move(vertex);
	join->right = std::move(control);
	join->type = JoinType::INNER;
	join->condition = Constant(Value(true));

	vector<unique_ptr<ParsedExpression>> sequence_arguments;
	sequence_arguments.push_back(Constant(Value("gql_internal.graph_" + to_string(graph.graph_id) + "_vertex_id_seq")));
	unique_ptr<ParsedExpression> inserted_id = Function("currval", std::move(sequence_arguments));
	auto offset = vertex_count - vertex_index - 1;
	if (offset != 0) {
		inserted_id = Subtract(std::move(inserted_id), Constant(Value::UBIGINT(offset)));
	}

	auto select = make_uniq<SelectNode>();
	select->from_table = std::move(join);
	select->where_clause = Equal(Column(vertex_alias, graph.vertex.key_column), std::move(inserted_id));
	select->select_list.push_back(VertexNode(graph, vertex_alias, return_name));
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(select);
	return make_uniq<SubqueryRef>(std::move(statement));
}

static unique_ptr<TableRef> InsertIdsBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	if (input.inputs.size() != 2 || input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("GQL INSERT id allocation requires vertex and edge counts");
	}
	auto graph = LoadSelectedGraph(context);
	auto vertex_count = input.inputs[0].GetValue<uint64_t>();
	auto edge_count = input.inputs[1].GetValue<uint64_t>();
	auto select = make_uniq<SelectNode>();
	select->from_table = make_uniq<EmptyTableRef>();
	auto add_ids = [&](const string &sequence_suffix, const string &column_prefix, uint64_t count) {
		for (uint64_t index = 0; index < count; index++) {
			vector<unique_ptr<ParsedExpression>> arguments;
			arguments.push_back(Constant(Value("gql_internal.graph_" + to_string(graph.graph_id) + sequence_suffix)));
			auto next_id = make_uniq<FunctionExpression>("nextval", std::move(arguments));
			next_id->SetAlias(column_prefix + to_string(index));
			select->select_list.push_back(std::move(next_id));
		}
	};
	add_ids("_vertex_id_seq", "vertex_id_", vertex_count);
	add_ids("_edge_id_seq", "edge_id_", edge_count);
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(select);
	return make_uniq<SubqueryRef>(std::move(statement));
}

TableFunction GqlInsertTargetFunction() {
	TableFunction function("gql_insert_target", {LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR)},
	                       nullptr, nullptr);
	function.bind_replace = InsertTargetBindReplace;
	return function;
}

TableFunction GqlInsertResultFunction() {
	TableFunction function("gql_insert_result",
	                       {LogicalType::VARCHAR, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::VARCHAR},
	                       nullptr, nullptr);
	function.bind_replace = InsertResultBindReplace;
	return function;
}

TableFunction GqlInsertIdsFunction() {
	TableFunction function("gql_insert_ids", {LogicalType::UBIGINT, LogicalType::UBIGINT}, nullptr, nullptr);
	function.bind_replace = InsertIdsBindReplace;
	return function;
}

static unique_ptr<TableRef> MatchInsertIdsBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	if (input.inputs.size() != 3 || input.inputs[0].IsNull() || input.inputs[1].IsNull() || input.inputs[2].IsNull()) {
		throw BinderException("GQL MATCH INSERT id allocation requires a source "
		                      "snapshot and vertex/edge counts");
	}
	auto graph = LoadSelectedGraph(context);
	auto snapshot_name = input.inputs[0].GetValue<string>();
	auto vertex_count = input.inputs[1].GetValue<uint64_t>();
	auto edge_count = input.inputs[2].GetValue<uint64_t>();
	auto select = make_uniq<SelectNode>();
	auto source = make_uniq<BaseTableRef>();
	source->table_name = snapshot_name;
	source->alias = "gql_match_insert_input";
	select->from_table = std::move(source);
	select->select_list.push_back(make_uniq<StarExpression>());
	auto add_ids = [&](const string &sequence_suffix, const string &column_prefix, uint64_t count) {
		for (uint64_t index = 0; index < count; index++) {
			vector<unique_ptr<ParsedExpression>> arguments;
			arguments.push_back(Constant(Value("gql_internal.graph_" + to_string(graph.graph_id) + sequence_suffix)));
			auto next_id = make_uniq<FunctionExpression>("nextval", std::move(arguments));
			next_id->SetAlias(column_prefix + to_string(index));
			select->select_list.push_back(std::move(next_id));
		}
	};
	add_ids("_vertex_id_seq", "vertex_id_", vertex_count);
	add_ids("_edge_id_seq", "edge_id_", edge_count);
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(select);
	return make_uniq<SubqueryRef>(std::move(statement));
}

TableFunction GqlMatchInsertIdsFunction() {
	TableFunction function("gql_match_insert_ids", {LogicalType::VARCHAR, LogicalType::UBIGINT, LogicalType::UBIGINT},
	                       nullptr, nullptr);
	function.bind_replace = MatchInsertIdsBindReplace;
	return function;
}

static unique_ptr<TableRef> MergeTargetBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	if (input.inputs.size() != 1 || input.inputs[0].IsNull()) {
		throw BinderException("GQL MERGE target requires a property-name list");
	}
	auto graph = LoadSelectedGraph(context);
	if (!StringUtil::CIEquals(graph.vertex.key_column, "__gql_id") ||
	    !StringUtil::CIEquals(graph.vertex.label_column, "__gql_label")) {
		throw NotImplementedException("GQL MERGE requires canonical __gql_id and __gql_label columns");
	}
	if (!graph.vertex.label_is_list) {
		throw NotImplementedException("GQL MERGE requires native vertex LIST_COLUMN storage; "
		                              "recreate and reload the graph with COPY GRAPH");
	}
	for (const auto &entry : ListValue::GetChildren(input.inputs[0])) {
		auto property = entry.GetValue<string>();
		auto &column = MappedProperty(graph.vertex, property);
		if (!StringUtil::CIEquals(column, property)) {
			throw NotImplementedException("MERGE property '%s' maps to physical column '%s'; aliased columns "
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

static unique_ptr<TableRef> MergeIdBindReplace(ClientContext &context, TableFunctionBindInput &) {
	auto graph = LoadSelectedGraph(context);
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(Constant(Value("gql_internal.graph_" + to_string(graph.graph_id) + "_vertex_id_seq")));
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
	TableFunction function("gql_merge_target", {LogicalType::LIST(LogicalType::VARCHAR)}, nullptr, nullptr);
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
		throw BinderException("NULL properties are not supported in MERGE patterns");
	}
	throw InternalException("Unknown MERGE literal type");
}

static Value InsertLiteralValue(const GqlLiteral &literal) {
	if (literal.type == GqlLiteralType::NULL_VALUE) {
		return Value();
	}
	return MergeLiteralValue(literal);
}

static vector<Value> InsertPropertyNames(const GqlInsertElement &element) {
	vector<Value> result;
	for (const auto &property : element.properties) {
		result.emplace_back(property.name.value);
	}
	return result;
}

static unique_ptr<TableRef> InsertTarget(const char *kind, const GqlInsertElement &element, const string &alias) {
	return FunctionTable("gql_insert_target",
	                     {Value(kind), Value::LIST(LogicalType::VARCHAR, InsertPropertyNames(element))}, alias);
}

static unique_ptr<SQLStatement> CreateInsertIds(const string &snapshot_name, idx_t vertex_count, idx_t edge_count) {
	auto create = make_uniq<CreateStatement>();
	auto info = make_uniq<CreateTableInfo>(TEMP_CATALOG, DEFAULT_SCHEMA, snapshot_name);
	info->temporary = true;
	auto select = make_uniq<SelectNode>();
	select->from_table =
	    FunctionTable("gql_insert_ids", {Value::UBIGINT(vertex_count), Value::UBIGINT(edge_count)}, "gql_insert_ids");
	select->select_list.push_back(make_uniq<StarExpression>());
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(select);
	info->query = std::move(statement);
	create->info = std::move(info);
	return std::move(create);
}

static unique_ptr<ParsedExpression> InsertLabels(const GqlInsertElement &element) {
	vector<string> labels;
	case_insensitive_set_t seen;
	for (const auto &label : element.labels) {
		if (seen.insert(label.value).second) {
			labels.push_back(label.value);
		}
	}
	return Constant(LabelListValue(labels));
}

static unique_ptr<ParsedExpression> InsertScalarLabels(const GqlInsertElement &element) {
	vector<string> labels;
	case_insensitive_set_t seen;
	for (const auto &label : element.labels) {
		if (seen.insert(label.value).second) {
			labels.push_back(label.value);
		}
	}
	if (labels.size() != 1) {
		throw BinderException("DuckGQL edge insertion requires exactly one edge type");
	}
	return Constant(Value(labels[0]));
}

static void AddInsertProperties(MergeIntoAction &action, const GqlInsertElement &element) {
	for (const auto &property : element.properties) {
		action.insert_columns.push_back(property.name.value);
		action.expressions.push_back(Constant(InsertLiteralValue(property.value)));
	}
}

static unique_ptr<SQLStatement> LowerInsertVertex(const string &snapshot_name, idx_t vertex_index,
                                                  const GqlInsertElement &vertex) {
	auto statement = make_uniq<MergeIntoStatement>();
	statement->target = InsertTarget("VERTEX", vertex, "gql_insert_target");
	statement->source = Snapshot(snapshot_name, "gql_insert_source");
	statement->join_condition = Constant(Value(false));

	auto action = make_uniq<MergeIntoAction>();
	action->action_type = MergeActionType::MERGE_INSERT;
	action->insert_columns = {"__gql_id", "__gql_label"};
	auto id_column = "vertex_id_" + to_string(vertex_index);
	action->expressions.push_back(Column("gql_insert_source", id_column));
	action->expressions.push_back(InsertLabels(vertex));
	AddInsertProperties(*action, vertex);
	statement->actions[MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET].push_back(std::move(action));
	return std::move(statement);
}

static unique_ptr<SQLStatement> LowerInsertEdge(const string &snapshot_name, idx_t edge_index,
                                                const GqlInsertEdge &edge) {
	auto statement = make_uniq<MergeIntoStatement>();
	statement->target = InsertTarget("EDGE", edge, "gql_insert_target");
	statement->source = Snapshot(snapshot_name, "gql_insert_source");
	statement->join_condition = Constant(Value(false));

	auto action = make_uniq<MergeIntoAction>();
	action->action_type = MergeActionType::MERGE_INSERT;
	action->insert_columns = {"__gql_edge_id", "__gql_source_id", "__gql_target_id", "__gql_type"};
	action->expressions.push_back(Column("gql_insert_source", "edge_id_" + to_string(edge_index)));
	action->expressions.push_back(Column("gql_insert_source", "vertex_id_" + to_string(edge.source_vertex)));
	action->expressions.push_back(Column("gql_insert_source", "vertex_id_" + to_string(edge.target_vertex)));
	action->expressions.push_back(InsertScalarLabels(edge));
	AddInsertProperties(*action, edge);
	statement->actions[MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET].push_back(std::move(action));
	return std::move(statement);
}

vector<unique_ptr<SQLStatement>> GqlLowerInsert(const GqlInsertStatement &insert) {
	if (insert.vertices.empty()) {
		throw BinderException("GQL INSERT requires at least one vertex");
	}
	static atomic<uint64_t> next_insert_command_id(0);
	auto command_id = "gql_insert_" + to_string(next_insert_command_id.fetch_add(1, std::memory_order_relaxed));
	auto snapshot_name = "_" + command_id;
	vector<unique_ptr<SQLStatement>> statements;
	statements.push_back(QuietBeginControlStatement(command_id));
	statements.push_back(DropSnapshot(ControlSnapshotName(command_id)));
	statements.push_back(CreateInsertIds(snapshot_name, insert.vertices.size(), insert.edges.size()));
	for (idx_t index = 0; index < insert.vertices.size(); index++) {
		statements.push_back(LowerInsertVertex(snapshot_name, index, insert.vertices[index]));
	}
	for (idx_t index = 0; index < insert.edges.size(); index++) {
		statements.push_back(LowerInsertEdge(snapshot_name, index, insert.edges[index]));
	}
	statements.push_back(UpdateGraphVersionAlways());
	statements.push_back(DropSnapshot(snapshot_name));
	if (insert.return_vertex_index != DConstants::INVALID_INDEX) {
		statements.push_back(InsertNodeResultStatement(command_id, insert.vertices.size(), insert.return_vertex_index,
		                                               insert.return_name));
	} else {
		statements.push_back(ControlStatement(command_id, false));
		statements.push_back(ResultStatement());
	}
	return statements;
}

static vector<Value> MatchInsertPropertyNames(const vector<GqlBoundInsertProperty> &properties) {
	vector<Value> result;
	for (const auto &property : properties) {
		result.emplace_back(property.name);
	}
	return result;
}

static unique_ptr<TableRef> MatchInsertTarget(const char *kind, const vector<GqlBoundInsertProperty> &properties,
                                              const string &alias) {
	return FunctionTable("gql_insert_target",
	                     {Value(kind), Value::LIST(LogicalType::VARCHAR, MatchInsertPropertyNames(properties))}, alias);
}

static unique_ptr<SQLStatement> CreateMatchInsertIds(const string &source_snapshot, const string &ids_snapshot,
                                                     idx_t vertex_count, idx_t edge_count) {
	auto create = make_uniq<CreateStatement>();
	auto info = make_uniq<CreateTableInfo>(TEMP_CATALOG, DEFAULT_SCHEMA, ids_snapshot);
	info->temporary = true;
	auto select = make_uniq<SelectNode>();
	select->from_table = FunctionTable(
	    "gql_match_insert_ids", {Value(source_snapshot), Value::UBIGINT(vertex_count), Value::UBIGINT(edge_count)},
	    "gql_match_insert_ids");
	select->select_list.push_back(make_uniq<StarExpression>());
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(select);
	info->query = std::move(statement);
	create->info = std::move(info);
	return std::move(create);
}

static unique_ptr<ParsedExpression> MatchInsertLabels(const vector<string> &input) {
	vector<string> labels;
	case_insensitive_set_t seen;
	for (const auto &label : input) {
		if (seen.insert(label).second) {
			labels.push_back(label);
		}
	}
	return Constant(LabelListValue(labels));
}

static unique_ptr<ParsedExpression> MatchInsertScalarLabels(const vector<string> &input) {
	vector<string> labels;
	case_insensitive_set_t seen;
	for (const auto &label : input) {
		if (seen.insert(label).second) {
			labels.push_back(label);
		}
	}
	if (labels.size() != 1) {
		throw BinderException("DuckGQL edge insertion requires exactly one edge type");
	}
	return Constant(Value(labels[0]));
}

static void AddMatchInsertProperties(MergeIntoAction &action, const vector<GqlBoundInsertProperty> &properties) {
	for (const auto &property : properties) {
		action.insert_columns.push_back(property.name);
		action.expressions.push_back(Column("gql_match_insert_source", property.value_column));
	}
}

static string MatchInsertVertexIdColumn(const GqlBoundInsertVertex &vertex) {
	return vertex.existing ? vertex.existing_id_column : "vertex_id_" + to_string(vertex.allocation_index);
}

static unique_ptr<SQLStatement> LowerMatchInsertVertex(const string &ids_snapshot, const GqlBoundInsertVertex &vertex) {
	auto statement = make_uniq<MergeIntoStatement>();
	statement->target = MatchInsertTarget("VERTEX", vertex.properties, "gql_insert_target");
	statement->source = Snapshot(ids_snapshot, "gql_match_insert_source");
	statement->join_condition = Constant(Value(false));

	auto action = make_uniq<MergeIntoAction>();
	action->action_type = MergeActionType::MERGE_INSERT;
	action->insert_columns = {"__gql_id", "__gql_label"};
	auto id_column = MatchInsertVertexIdColumn(vertex);
	action->expressions.push_back(Column("gql_match_insert_source", id_column));
	action->expressions.push_back(MatchInsertLabels(vertex.labels));
	AddMatchInsertProperties(*action, vertex.properties);
	statement->actions[MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET].push_back(std::move(action));
	return std::move(statement);
}

static unique_ptr<SQLStatement> LowerMatchInsertEdge(const string &ids_snapshot, const GqlBoundInsert &insert,
                                                     const GqlBoundInsertEdge &edge) {
	auto statement = make_uniq<MergeIntoStatement>();
	statement->target = MatchInsertTarget("EDGE", edge.properties, "gql_insert_target");
	statement->source = Snapshot(ids_snapshot, "gql_match_insert_source");
	statement->join_condition = Constant(Value(false));

	auto action = make_uniq<MergeIntoAction>();
	action->action_type = MergeActionType::MERGE_INSERT;
	action->insert_columns = {"__gql_edge_id", "__gql_source_id", "__gql_target_id", "__gql_type"};
	action->expressions.push_back(Column("gql_match_insert_source", "edge_id_" + to_string(edge.allocation_index)));
	action->expressions.push_back(
	    Column("gql_match_insert_source", MatchInsertVertexIdColumn(insert.vertices[edge.source_vertex])));
	action->expressions.push_back(
	    Column("gql_match_insert_source", MatchInsertVertexIdColumn(insert.vertices[edge.target_vertex])));
	action->expressions.push_back(MatchInsertScalarLabels(edge.labels));
	AddMatchInsertProperties(*action, edge.properties);
	statement->actions[MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET].push_back(std::move(action));
	return std::move(statement);
}

vector<unique_ptr<SQLStatement>> GqlLowerMatchInsert(const vector<GqlLogicalPlan> &plans) {
	if (plans.empty() || !plans[0].insertion) {
		throw InternalException("GQL MATCH INSERT lowering requires a bound insertion");
	}
	const auto &insert = *plans[0].insertion;
	static atomic<uint64_t> next_match_insert_command_id(0);
	auto command_id =
	    "gql_match_insert_" + to_string(next_match_insert_command_id.fetch_add(1, std::memory_order_relaxed));
	auto match_snapshot = "_" + command_id + "_matches";
	auto ids_snapshot = "_" + command_id + "_ids";
	vector<unique_ptr<SQLStatement>> statements;
	statements.push_back(ControlStatement(command_id, true));
	statements.push_back(CreateSnapshot(plans, match_snapshot));
	statements.push_back(
	    CreateMatchInsertIds(match_snapshot, ids_snapshot, insert.new_vertex_count, insert.edges.size()));
	for (const auto &vertex : insert.vertices) {
		if (vertex.create) {
			statements.push_back(LowerMatchInsertVertex(ids_snapshot, vertex));
		}
	}
	for (const auto &edge : insert.edges) {
		statements.push_back(LowerMatchInsertEdge(ids_snapshot, insert, edge));
	}
	statements.push_back(UpdateGraphVersion(ids_snapshot));
	statements.push_back(DropSnapshot(ids_snapshot));
	statements.push_back(DropSnapshot(match_snapshot));
	statements.push_back(ControlStatement(command_id, false));
	statements.push_back(ResultStatement());
	return statements;
}

static unique_ptr<SQLStatement> LowerMergeStatement(const GqlMergeStatement &merge) {
	vector<Value> property_names;
	for (const auto &property : merge.vertex.properties) {
		property_names.emplace_back(property.name.value);
	}

	auto statement = make_uniq<MergeIntoStatement>();
	statement->target =
	    FunctionTable("gql_merge_target", {Value::LIST(LogicalType::VARCHAR, property_names)}, "gql_merge_target");
	statement->source = FunctionTable("gql_merge_id", {}, "gql_merge_source");

	unique_ptr<ParsedExpression> condition;
	for (const auto &label : merge.vertex.labels) {
		auto comparison = HasLabel(Column("gql_merge_target", "__gql_label"), label.value, true);
		condition = condition ? And(std::move(condition), std::move(comparison)) : std::move(comparison);
	}
	for (const auto &property : merge.vertex.properties) {
		auto comparison =
		    NotDistinct(Column("gql_merge_target", property.name.value), Constant(MergeLiteralValue(property.value)));
		condition = condition ? And(std::move(condition), std::move(comparison)) : std::move(comparison);
	}
	if (!condition) {
		throw InternalException("MERGE lowering requires a match condition");
	}
	statement->join_condition = std::move(condition);

	auto insert = make_uniq<MergeIntoAction>();
	insert->action_type = MergeActionType::MERGE_INSERT;
	insert->insert_columns = {"__gql_id", "__gql_label"};
	insert->expressions.push_back(Column("gql_merge_source", "__gql_new_id"));
	vector<string> labels;
	for (const auto &label : merge.vertex.labels) {
		labels.push_back(label.value);
	}
	insert->expressions.push_back(Constant(LabelListValue(labels)));
	for (const auto &property : merge.vertex.properties) {
		insert->insert_columns.push_back(property.name.value);
		insert->expressions.push_back(Constant(MergeLiteralValue(property.value)));
	}
	statement->actions[MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET].push_back(std::move(insert));
	return std::move(statement);
}

vector<unique_ptr<SQLStatement>> GqlLowerMerge(const GqlMergeStatement &merge) {
	static atomic<uint64_t> next_merge_command_id(0);
	auto command_id = "gql_merge_" + to_string(next_merge_command_id.fetch_add(1, std::memory_order_relaxed));
	vector<unique_ptr<SQLStatement>> statements;
	statements.push_back(ControlStatement(command_id, true));
	statements.push_back(LowerMergeStatement(merge));
	statements.push_back(UpdateGraphVersionAlways());
	statements.push_back(ControlStatement(command_id, false));
	statements.push_back(ResultStatement());
	return statements;
}

vector<unique_ptr<SQLStatement>> GqlLowerMutation(const vector<GqlLogicalPlan> &plans) {
	if (plans.empty() || plans[0].mutations.empty()) {
		throw InternalException("GQL mutation lowering requires a bound mutation");
	}
	static atomic<uint64_t> next_command_id(0);
	auto command_id = "gql_mutation_" + to_string(next_command_id.fetch_add(1, std::memory_order_relaxed));
	auto snapshot_name = "_" + command_id;
	vector<unique_ptr<SQLStatement>> statements;
	statements.push_back(ControlStatement(command_id, true));
	statements.push_back(CreateSnapshot(plans, snapshot_name));
	auto delete_program = plans[0].mutations[0].type == GqlMutationType::DELETE_ELEMENT;
	bool changes_graph = false;
	if (delete_program) {
		// DELETE item order is not execution order. Remove every explicitly
		// targeted edge first, then evaluate node constraints against the
		// post-edge-delete topology, and only then remove nodes. A later
		// constraint failure rolls the earlier edge deletes back with the command.
		for (idx_t mutation_index = 0; mutation_index < plans[0].mutations.size(); mutation_index++) {
			const auto &mutation = plans[0].mutations[mutation_index];
			if (mutation.type != GqlMutationType::DELETE_ELEMENT) {
				throw InternalException("GQL DELETE program contains a non-delete mutation");
			}
			if (mutation.binding_type.id == GqlTypeId::EDGE) {
				statements.push_back(DeleteElement(snapshot_name, mutation_index, mutation));
			}
		}
		for (idx_t mutation_index = 0; mutation_index < plans[0].mutations.size(); mutation_index++) {
			const auto &mutation = plans[0].mutations[mutation_index];
			if (mutation.binding_type.id != GqlTypeId::NODE) {
				continue;
			}
			statements.push_back(mutation.detach ? DeleteIncidentEdges(snapshot_name, mutation_index)
			                                     : RejectAttachedNodeDelete(snapshot_name, mutation_index));
		}
		for (idx_t mutation_index = 0; mutation_index < plans[0].mutations.size(); mutation_index++) {
			const auto &mutation = plans[0].mutations[mutation_index];
			if (mutation.binding_type.id == GqlTypeId::NODE) {
				statements.push_back(DeleteElement(snapshot_name, mutation_index, mutation));
			}
		}
		changes_graph = true;
	} else {
		for (idx_t mutation_index = 0; mutation_index < plans[0].mutations.size(); mutation_index++) {
			const auto &mutation = plans[0].mutations[mutation_index];
			switch (mutation.type) {
			case GqlMutationType::SET_PROPERTY:
				statements.push_back(SetProperty(snapshot_name, mutation_index, mutation));
				changes_graph = true;
				break;
			case GqlMutationType::SET_PROPERTIES:
				throw InternalException("Unexpanded GQL property-map mutation reached physical lowering");
			case GqlMutationType::SET_LABEL:
				statements.push_back(SetLabel(snapshot_name, mutation_index, mutation));
				changes_graph = true;
				break;
			case GqlMutationType::CLEAR_PROPERTIES:
				statements.push_back(ClearProperties(snapshot_name, mutation_index, mutation));
				changes_graph = true;
				break;
			case GqlMutationType::MERGE_PROPERTIES:
				break;
			case GqlMutationType::REMOVE_PROPERTY:
				statements.push_back(RemoveProperty(snapshot_name, mutation_index, mutation));
				changes_graph = true;
				break;
			case GqlMutationType::REMOVE_LABEL:
				statements.push_back(RemoveLabel(snapshot_name, mutation_index, mutation));
				changes_graph = true;
				break;
			case GqlMutationType::DELETE_ELEMENT:
				throw InternalException("GQL non-delete program contains a delete mutation");
			}
		}
	}
	if (changes_graph) {
		statements.push_back(UpdateGraphVersion(snapshot_name, plans[0].mutations.size()));
	}
	statements.push_back(DropSnapshot(snapshot_name));
	statements.push_back(ControlStatement(command_id, false));
	statements.push_back(ResultStatement());
	return statements;
}

} // namespace duckdb
