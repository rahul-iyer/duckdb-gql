#include "gql_lowerer.hpp"

#include "duckdb/common/constants.hpp"
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
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

#include <atomic>

namespace duckdb {

static unique_ptr<ParsedExpression> MutationColumn(const string &table, const string &column) {
	return make_uniq<ColumnRefExpression>(column, table);
}

static unique_ptr<ParsedExpression> MutationConstant(Value value) {
	return make_uniq<ConstantExpression>(std::move(value));
}

static unique_ptr<ParsedExpression> MutationEqual(unique_ptr<ParsedExpression> left,
                                                  unique_ptr<ParsedExpression> right) {
	return make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(left), std::move(right));
}

static unique_ptr<ParsedExpression> MutationOr(unique_ptr<ParsedExpression> left, unique_ptr<ParsedExpression> right) {
	return make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_OR, std::move(left), std::move(right));
}

static unique_ptr<TableRef> MutationTable(const string &table, const string &alias) {
	auto result = make_uniq<BaseTableRef>();
	result->schema_name = "gql_internal";
	result->table_name = table;
	result->alias = alias;
	return std::move(result);
}

static string TargetColumn(idx_t mutation_index) {
	return "gql_target_id_" + to_string(mutation_index);
}

static string ValueColumn(idx_t mutation_index) {
	return "gql_mutation_value_" + to_string(mutation_index);
}

static unique_ptr<TableRef> MutationSnapshot(const string &snapshot_name, const string &alias) {
	auto result = make_uniq<BaseTableRef>();
	result->table_name = snapshot_name;
	result->alias = alias;
	return std::move(result);
}

static void MutationJoin(unique_ptr<TableRef> &left, unique_ptr<TableRef> right,
                         unique_ptr<ParsedExpression> condition) {
	auto join = make_uniq<JoinRef>();
	join->left = std::move(left);
	join->right = std::move(right);
	join->type = JoinType::INNER;
	join->condition = std::move(condition);
	left = std::move(join);
}

static unique_ptr<TableRef> MatchedObjects(const string &snapshot_name, idx_t mutation_index, const string &match_alias,
                                           const string &object_alias) {
	auto from = MutationSnapshot(snapshot_name, match_alias);
	MutationJoin(from, MutationTable("objects", object_alias),
	             MutationEqual(MutationColumn(object_alias, "object_id"),
	                           MutationColumn(match_alias, TargetColumn(mutation_index))));
	return from;
}

static unique_ptr<SelectStatement> GraphIds(const string &snapshot_name) {
	auto select = make_uniq<SelectNode>();
	select->from_table = MatchedObjects(snapshot_name, 0, "gql_mutation_match", "gql_mutation_object");
	select->select_list.push_back(MutationColumn("gql_mutation_object", "graph_id"));
	select->modifiers.push_back(make_uniq<DistinctModifier>());
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(select);
	return statement;
}

static unique_ptr<SQLStatement> UpdateGraphVersion(const string &snapshot_name) {
	auto update = make_uniq<UpdateStatement>();
	update->table = MutationTable("graphs", "gql_mutation_graph");
	update->from_table = make_uniq<SubqueryRef>(GraphIds(snapshot_name), "gql_mutation_graph_ids");
	update->set_info = make_uniq<UpdateSetInfo>();
	update->set_info->columns.push_back("graph_version");
	vector<unique_ptr<ParsedExpression>> addition;
	addition.push_back(MutationColumn("gql_mutation_graph", "graph_version"));
	addition.push_back(MutationConstant(Value::UBIGINT(1)));
	auto increment = make_uniq<FunctionExpression>("+", std::move(addition));
	increment->is_operator = true;
	update->set_info->expressions.push_back(std::move(increment));
	update->set_info->condition = MutationEqual(MutationColumn("gql_mutation_graph", "graph_id"),
	                                            MutationColumn("gql_mutation_graph_ids", "graph_id"));
	return std::move(update);
}

static unique_ptr<InsertStatement> InsertFrom(const string &table, vector<string> columns,
                                              unique_ptr<SelectStatement> source) {
	auto insert = make_uniq<InsertStatement>();
	insert->schema = "gql_internal";
	insert->table = table;
	insert->table_ref = MutationTable(table, table);
	insert->columns = std::move(columns);
	insert->select_statement = std::move(source);
	return insert;
}

static void IgnoreConflict(InsertStatement &insert, vector<string> indexed_columns) {
	insert.on_conflict_info = make_uniq<OnConflictInfo>();
	insert.on_conflict_info->action_type = OnConflictAction::NOTHING;
	insert.on_conflict_info->indexed_columns = std::move(indexed_columns);
}

static unique_ptr<SQLStatement> InsertDictionaryValue(const string &snapshot_name, const string &table,
                                                      const string &name_column, const string &name) {
	auto select = GraphIds(snapshot_name);
	auto &node = select->node->Cast<SelectNode>();
	node.select_list.push_back(MutationConstant(Value(name)));
	auto insert = InsertFrom(table, {"graph_id", name_column}, std::move(select));
	IgnoreConflict(*insert, {"graph_id", name_column});
	return std::move(insert);
}

static pair<string, LogicalType> PropertyMember(const GqlType &type) {
	switch (type.id) {
	case GqlTypeId::BOOLEAN:
		return {"bool_value", LogicalType::BOOLEAN};
	case GqlTypeId::INTEGER:
	case GqlTypeId::ELEMENT_ID:
		return {"int_value", LogicalType::BIGINT};
	case GqlTypeId::DECIMAL:
		return {"decimal_value", LogicalType::DECIMAL(38, 18)};
	case GqlTypeId::DOUBLE:
		return {"double_value", LogicalType::DOUBLE};
	case GqlTypeId::STRING:
		return {"string_value", LogicalType::VARCHAR};
	case GqlTypeId::UNKNOWN:
	case GqlTypeId::NULL_VALUE:
	case GqlTypeId::NODE:
	case GqlTypeId::EDGE:
	case GqlTypeId::PATH:
	case GqlTypeId::PROPERTY_VALUE:
		break;
	}
	throw InternalException("Unsupported bound GQL SET property type");
}

static unique_ptr<ParsedExpression> MutationFunction(const string &name,
                                                     vector<unique_ptr<ParsedExpression>> arguments) {
	return make_uniq<FunctionExpression>(name, std::move(arguments));
}

static unique_ptr<ParsedExpression> DynamicStoredPropertyBranch(const ParsedExpression &variant, const string &member,
                                                                const LogicalType &type) {
	auto value = make_uniq<CastExpression>(type, variant.Copy());
	value->SetAlias(member);
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(std::move(value));
	return make_uniq<CastExpression>(GqlDuckType({GqlTypeId::PROPERTY_VALUE, false}),
	                                 MutationFunction("union_value", std::move(arguments)));
}

static unique_ptr<ParsedExpression> VariantType(const ParsedExpression &variant) {
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(variant.Copy());
	return MutationFunction("variant_typeof", std::move(arguments));
}

static unique_ptr<ParsedExpression> DynamicStoredPropertyValue(idx_t mutation_index) {
	auto variant = make_uniq<CastExpression>(LogicalType::VARIANT(),
	                                         MutationColumn("gql_mutation_match", ValueColumn(mutation_index)));
	auto result = make_uniq<CaseExpression>();
	auto add_branch = [&](const vector<string> &tags, const string &member, const LogicalType &type) {
		unique_ptr<ParsedExpression> condition;
		for (const auto &tag : tags) {
			auto equal = MutationEqual(VariantType(*variant), MutationConstant(Value(tag)));
			condition = condition ? MutationOr(std::move(condition), std::move(equal)) : std::move(equal);
		}
		CaseCheck check;
		check.when_expr = std::move(condition);
		check.then_expr = DynamicStoredPropertyBranch(*variant, member, type);
		result->case_checks.push_back(std::move(check));
	};
	auto add_prefix_branch = [&](const string &prefix, const string &member, const LogicalType &type) {
		vector<unique_ptr<ParsedExpression>> arguments;
		arguments.push_back(VariantType(*variant));
		arguments.push_back(MutationConstant(Value(prefix)));
		CaseCheck check;
		check.when_expr = MutationFunction("starts_with", std::move(arguments));
		check.then_expr = DynamicStoredPropertyBranch(*variant, member, type);
		result->case_checks.push_back(std::move(check));
	};
	add_branch({"BOOLEAN"}, "bool_value", LogicalType::BOOLEAN);
	add_branch({"INT8", "INT16", "INT32", "INT64", "INT128"}, "int_value", LogicalType::BIGINT);
	add_branch({"UINT8", "UINT16", "UINT32", "UINT64", "UINT128"}, "uint_value", LogicalType::UBIGINT);
	add_prefix_branch("DECIMAL", "decimal_value", LogicalType::DECIMAL(38, 18));
	add_branch({"FLOAT", "DOUBLE"}, "double_value", LogicalType::DOUBLE);
	add_branch({"VARCHAR"}, "string_value", LogicalType::VARCHAR);
	add_branch({"BLOB"}, "blob_value", LogicalType::BLOB);
	add_branch({"DATE"}, "date_value", LogicalType::DATE);
	add_branch({"TIME"}, "time_value", LogicalType::TIME);
	add_branch({"TIMESTAMP", "TIMESTAMP_MS", "TIMESTAMP_NS", "TIMESTAMP_S"}, "timestamp_value", LogicalType::TIMESTAMP);
	add_branch({"TIMESTAMP WITH TIME ZONE"}, "timestamptz_value", LogicalType::TIMESTAMP_TZ);
	add_branch({"INTERVAL"}, "interval_value", LogicalType::INTERVAL);
	vector<unique_ptr<ParsedExpression>> error_arguments;
	error_arguments.push_back(MutationConstant(Value("GQL SET property expression produced NULL or an "
	                                                 "unsupported value type")));
	result->else_expr = MutationFunction("error", std::move(error_arguments));
	return std::move(result);
}

static unique_ptr<ParsedExpression> StoredPropertyValue(const GqlBoundMutation &mutation, idx_t mutation_index) {
	if (mutation.value->result_type.id == GqlTypeId::PROPERTY_VALUE) {
		return DynamicStoredPropertyValue(mutation_index);
	}
	auto member = PropertyMember(mutation.value->result_type);
	auto value =
	    make_uniq<CastExpression>(member.second, MutationColumn("gql_mutation_match", ValueColumn(mutation_index)));
	value->SetAlias(member.first);
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(std::move(value));
	return make_uniq<FunctionExpression>("union_value", std::move(arguments));
}

static unique_ptr<SQLStatement> SetPropertyValue(const string &snapshot_name, idx_t mutation_index,
                                                 const GqlBoundMutation &mutation) {
	auto select = make_uniq<SelectNode>();
	auto from = MatchedObjects(snapshot_name, mutation_index, "gql_mutation_match", "gql_mutation_object");
	auto key_condition = MutationEqual(MutationColumn("gql_mutation_key", "graph_id"),
	                                   MutationColumn("gql_mutation_object", "graph_id"));
	key_condition = make_uniq<ConjunctionExpression>(
	    ExpressionType::CONJUNCTION_AND, std::move(key_condition),
	    MutationEqual(MutationColumn("gql_mutation_key", "key_name"), MutationConstant(Value(mutation.name))));
	MutationJoin(from, MutationTable("property_keys", "gql_mutation_key"), std::move(key_condition));
	select->from_table = std::move(from);
	select->select_list.push_back(MutationColumn("gql_mutation_object", "graph_id"));
	select->select_list.push_back(MutationColumn("gql_mutation_match", TargetColumn(mutation_index)));
	select->select_list.push_back(MutationColumn("gql_mutation_key", "key_id"));
	select->select_list.push_back(StoredPropertyValue(mutation, mutation_index));
	auto source = make_uniq<SelectStatement>();
	source->node = std::move(select);
	auto insert = InsertFrom("object_properties", {"graph_id", "object_id", "key_id", "value"}, std::move(source));
	insert->on_conflict_info = make_uniq<OnConflictInfo>();
	insert->on_conflict_info->action_type = OnConflictAction::UPDATE;
	insert->on_conflict_info->indexed_columns = {"object_id", "key_id"};
	insert->on_conflict_info->set_info = make_uniq<UpdateSetInfo>();
	insert->on_conflict_info->set_info->columns.push_back("value");
	insert->on_conflict_info->set_info->expressions.push_back(MutationColumn("excluded", "value"));
	return std::move(insert);
}

static unique_ptr<SQLStatement> SetLabelValue(const string &snapshot_name, idx_t mutation_index,
                                              const GqlBoundMutation &mutation) {
	auto select = make_uniq<SelectNode>();
	auto from = MatchedObjects(snapshot_name, mutation_index, "gql_mutation_match", "gql_mutation_object");
	auto label_condition = MutationEqual(MutationColumn("gql_mutation_label", "graph_id"),
	                                     MutationColumn("gql_mutation_object", "graph_id"));
	label_condition = make_uniq<ConjunctionExpression>(
	    ExpressionType::CONJUNCTION_AND, std::move(label_condition),
	    MutationEqual(MutationColumn("gql_mutation_label", "label_name"), MutationConstant(Value(mutation.name))));
	MutationJoin(from, MutationTable("labels", "gql_mutation_label"), std::move(label_condition));
	select->from_table = std::move(from);
	select->select_list.push_back(MutationColumn("gql_mutation_object", "graph_id"));
	select->select_list.push_back(MutationColumn("gql_mutation_match", TargetColumn(mutation_index)));
	select->select_list.push_back(MutationColumn("gql_mutation_label", "label_id"));
	auto source = make_uniq<SelectStatement>();
	source->node = std::move(select);
	auto insert = InsertFrom("object_labels", {"graph_id", "object_id", "label_id"}, std::move(source));
	IgnoreConflict(*insert, {"object_id", "label_id"});
	return std::move(insert);
}

static unique_ptr<SQLStatement> RemoveAssignment(const string &snapshot_name, idx_t mutation_index,
                                                 const GqlBoundMutation &mutation) {
	auto property = mutation.type == GqlMutationType::REMOVE_PROPERTY;
	auto assignment_alias = property ? "gql_mutation_property" : "gql_mutation_object_label";
	auto dictionary_alias = property ? "gql_mutation_key" : "gql_mutation_label";
	auto assignment_table = property ? "object_properties" : "object_labels";
	auto dictionary_table = property ? "property_keys" : "labels";
	auto id_column = property ? "key_id" : "label_id";
	auto name_column = property ? "key_name" : "label_name";
	auto deletion = make_uniq<DeleteStatement>();
	deletion->table = MutationTable(assignment_table, assignment_alias);
	deletion->using_clauses.push_back(MutationSnapshot(snapshot_name, "gql_mutation_match"));
	deletion->using_clauses.push_back(MutationTable(dictionary_table, dictionary_alias));
	auto condition = MutationEqual(MutationColumn(assignment_alias, "object_id"),
	                               MutationColumn("gql_mutation_match", TargetColumn(mutation_index)));
	condition = make_uniq<ConjunctionExpression>(
	    ExpressionType::CONJUNCTION_AND, std::move(condition),
	    MutationEqual(MutationColumn(assignment_alias, id_column), MutationColumn(dictionary_alias, id_column)));
	condition = make_uniq<ConjunctionExpression>(
	    ExpressionType::CONJUNCTION_AND, std::move(condition),
	    MutationEqual(MutationColumn(assignment_alias, "graph_id"), MutationColumn(dictionary_alias, "graph_id")));
	condition = make_uniq<ConjunctionExpression>(
	    ExpressionType::CONJUNCTION_AND, std::move(condition),
	    MutationEqual(MutationColumn(dictionary_alias, name_column), MutationConstant(Value(mutation.name))));
	deletion->condition = std::move(condition);
	return std::move(deletion);
}

static unique_ptr<SQLStatement> ClearProperties(const string &snapshot_name, idx_t mutation_index) {
	auto deletion = make_uniq<DeleteStatement>();
	deletion->table = MutationTable("object_properties", "gql_mutation_property");
	deletion->using_clauses.push_back(MutationSnapshot(snapshot_name, "gql_mutation_match"));
	deletion->condition = MutationEqual(MutationColumn("gql_mutation_property", "object_id"),
	                                    MutationColumn("gql_mutation_match", TargetColumn(mutation_index)));
	return std::move(deletion);
}

static unique_ptr<SQLStatement> RejectAttachedNodeDelete(const string &snapshot_name, idx_t mutation_index) {
	auto select = make_uniq<SelectNode>();
	auto from = MutationSnapshot(snapshot_name, "gql_mutation_match");
	auto endpoint = MutationOr(MutationEqual(MutationColumn("gql_mutation_edge", "source_id"),
	                                         MutationColumn("gql_mutation_match", TargetColumn(mutation_index))),
	                           MutationEqual(MutationColumn("gql_mutation_edge", "target_id"),
	                                         MutationColumn("gql_mutation_match", TargetColumn(mutation_index))));
	endpoint = make_uniq<ConjunctionExpression>(
	    ExpressionType::CONJUNCTION_AND,
	    MutationEqual(MutationColumn("gql_mutation_edge", "kind"), MutationConstant(Value::UTINYINT(1))),
	    std::move(endpoint));
	MutationJoin(from, MutationTable("objects", "gql_mutation_edge"), std::move(endpoint));
	select->from_table = std::move(from);
	vector<unique_ptr<ParsedExpression>> error_arguments;
	error_arguments.push_back(MutationConstant(Value("GQL DELETE cannot remove a node with incident "
	                                                 "edges; use DETACH DELETE")));
	select->select_list.push_back(make_uniq<FunctionExpression>("error", std::move(error_arguments)));
	auto limit = make_uniq<LimitModifier>();
	limit->limit = MutationConstant(Value::UBIGINT(1));
	select->modifiers.push_back(std::move(limit));
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(select);
	return std::move(statement);
}

static unique_ptr<SQLStatement> DeleteObjects(const string &snapshot_name, idx_t mutation_index, bool detach_node) {
	auto deletion = make_uniq<DeleteStatement>();
	deletion->table = MutationTable("objects", "gql_mutation_delete_object");
	deletion->using_clauses.push_back(MutationSnapshot(snapshot_name, "gql_mutation_match"));
	auto condition = MutationEqual(MutationColumn("gql_mutation_delete_object", "object_id"),
	                               MutationColumn("gql_mutation_match", TargetColumn(mutation_index)));
	if (detach_node) {
		condition =
		    MutationOr(std::move(condition),
		               MutationOr(MutationEqual(MutationColumn("gql_mutation_delete_object", "source_id"),
		                                        MutationColumn("gql_mutation_match", TargetColumn(mutation_index))),
		                          MutationEqual(MutationColumn("gql_mutation_delete_object", "target_id"),
		                                        MutationColumn("gql_mutation_match", TargetColumn(mutation_index)))));
	}
	deletion->condition = std::move(condition);
	return std::move(deletion);
}

static unique_ptr<SQLStatement> DeleteOrphanAssignments(const string &table, const string &alias) {
	auto exists_select = make_uniq<SelectNode>();
	exists_select->from_table = MutationTable("objects", "gql_mutation_existing_object");
	exists_select->select_list.push_back(MutationConstant(Value::INTEGER(1)));
	exists_select->where_clause =
	    MutationEqual(MutationColumn("gql_mutation_existing_object", "object_id"), MutationColumn(alias, "object_id"));
	auto exists_statement = make_uniq<SelectStatement>();
	exists_statement->node = std::move(exists_select);
	auto exists = make_uniq<SubqueryExpression>();
	exists->subquery_type = SubqueryType::EXISTS;
	exists->subquery = std::move(exists_statement);
	auto deletion = make_uniq<DeleteStatement>();
	deletion->table = MutationTable(table, alias);
	deletion->condition = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(exists));
	return std::move(deletion);
}

static unique_ptr<SQLStatement> MutationControlStatement(const string &command_id, bool begin) {
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(MutationConstant(Value(command_id)));
	arguments.push_back(MutationConstant(Value::BOOLEAN(begin)));
	auto function_ref = make_uniq<TableFunctionRef>();
	function_ref->function = make_uniq<FunctionExpression>("gql_mutation_control", std::move(arguments));
	auto select = make_uniq<SelectNode>();
	select->from_table = std::move(function_ref);
	select->select_list.push_back(make_uniq<StarExpression>());
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(select);
	return std::move(statement);
}

static unique_ptr<SQLStatement> MutationResultStatement() {
	auto select = make_uniq<SelectNode>();
	select->from_table = make_uniq<EmptyTableRef>();
	auto success = MutationConstant(Value::BOOLEAN(true));
	success->SetAlias("success");
	select->select_list.push_back(std::move(success));
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(select);
	return std::move(statement);
}

static unique_ptr<SQLStatement> CreateMutationSnapshot(const vector<GqlLogicalPlan> &plans,
                                                       const string &snapshot_name) {
	auto create = make_uniq<CreateStatement>();
	auto info = make_uniq<CreateTableInfo>(TEMP_CATALOG, DEFAULT_SCHEMA, snapshot_name);
	info->temporary = true;
	info->query = unique_ptr_cast<SQLStatement, SelectStatement>(GqlLowerSelect(plans));
	create->info = std::move(info);
	return std::move(create);
}

static unique_ptr<SQLStatement> DropMutationSnapshot(const string &snapshot_name) {
	auto drop = make_uniq<DropStatement>();
	auto info = make_uniq<DropInfo>();
	info->type = CatalogType::TABLE_ENTRY;
	info->catalog = TEMP_CATALOG;
	info->schema = DEFAULT_SCHEMA;
	info->name = snapshot_name;
	drop->info = std::move(info);
	return std::move(drop);
}

vector<unique_ptr<SQLStatement>> GqlLowerMutation(const vector<GqlLogicalPlan> &plans) {
	if (plans.empty() || plans[0].mutations.empty()) {
		throw InternalException("GQL mutation lowering requires a bound mutation");
	}
	static atomic<uint64_t> next_command_id(0);
	auto command_number = next_command_id.fetch_add(1, std::memory_order_relaxed);
	auto command_id = "gql_mutation_" + to_string(command_number);
	auto snapshot_name = "_" + command_id;
	vector<unique_ptr<SQLStatement>> statements;
	statements.push_back(MutationControlStatement(command_id, true));
	statements.push_back(CreateMutationSnapshot(plans, snapshot_name));
	statements.push_back(UpdateGraphVersion(snapshot_name));
	for (idx_t mutation_index = 0; mutation_index < plans[0].mutations.size(); mutation_index++) {
		const auto &mutation = plans[0].mutations[mutation_index];
		switch (mutation.type) {
		case GqlMutationType::SET_PROPERTY:
			statements.push_back(InsertDictionaryValue(snapshot_name, "property_keys", "key_name", mutation.name));
			statements.push_back(SetPropertyValue(snapshot_name, mutation_index, mutation));
			break;
		case GqlMutationType::SET_LABEL:
			statements.push_back(InsertDictionaryValue(snapshot_name, "labels", "label_name", mutation.name));
			statements.push_back(SetLabelValue(snapshot_name, mutation_index, mutation));
			break;
		case GqlMutationType::CLEAR_PROPERTIES:
			statements.push_back(ClearProperties(snapshot_name, mutation_index));
			break;
		case GqlMutationType::REMOVE_PROPERTY:
		case GqlMutationType::REMOVE_LABEL:
			statements.push_back(RemoveAssignment(snapshot_name, mutation_index, mutation));
			break;
		case GqlMutationType::DELETE_ELEMENT: {
			auto node = mutation.binding_type.id == GqlTypeId::NODE;
			if (node && !mutation.detach) {
				statements.push_back(RejectAttachedNodeDelete(snapshot_name, mutation_index));
			}
			statements.push_back(DeleteObjects(snapshot_name, mutation_index, node && mutation.detach));
			break;
		}
		}
	}
	for (const auto &mutation : plans[0].mutations) {
		if (mutation.type != GqlMutationType::DELETE_ELEMENT) {
			continue;
		}
		statements.push_back(DeleteOrphanAssignments("object_properties", "gql_mutation_orphan_property"));
		statements.push_back(DeleteOrphanAssignments("object_labels", "gql_mutation_orphan_label"));
		break;
	}
	statements.push_back(DropMutationSnapshot(snapshot_name));
	statements.push_back(MutationControlStatement(command_id, false));
	// DuckDB may leave the final SELECT streaming. Keep the transaction-control
	// statement eager by returning a side-effect-free result after it.
	statements.push_back(MutationResultStatement());
	return statements;
}

} // namespace duckdb
