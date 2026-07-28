#define DUCKDB_EXTENSION_MAIN

#include "duckgql_extension.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension_callback_manager.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_execute.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_merge_into.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/planner/planner_extension.hpp"
#include "gql_algorithms.hpp"
#include "gql_catalog.hpp"
#include "gql_csr.hpp"
#include "gql_import.hpp"
#include "gql_mutation.hpp"
#include "gql_parser.hpp"
#include "gql_relational.hpp"
#include "gql_storage.hpp"

namespace duckdb {

static void ObserveTableWrite(ClientContext &context, TableCatalogEntry &table) {
	GqlNotifyCsrTableWritePlanned(context, table.catalog.GetName(), table.schema.name, table.name);
}

static void ObserveWrites(ClientContext &context, const LogicalOperator &operation) {
	switch (operation.type) {
	case LogicalOperatorType::LOGICAL_INSERT:
		ObserveTableWrite(context, operation.Cast<LogicalInsert>().table);
		break;
	case LogicalOperatorType::LOGICAL_DELETE:
		ObserveTableWrite(context, operation.Cast<LogicalDelete>().table);
		break;
	case LogicalOperatorType::LOGICAL_UPDATE:
		ObserveTableWrite(context, operation.Cast<LogicalUpdate>().table);
		break;
	case LogicalOperatorType::LOGICAL_MERGE_INTO:
		ObserveTableWrite(context, operation.Cast<LogicalMergeInto>().table);
		break;
	case LogicalOperatorType::LOGICAL_EXECUTE:
		if (!operation.Cast<LogicalExecute>().prepared->properties.IsReadOnly()) {
			GqlNotifyCsrPreparedWriteExecution(context);
		}
		break;
	default:
		break;
	}
	for (const auto &child : operation.children) {
		ObserveWrites(context, *child);
	}
}

static void RegisterCsrWriteObserver(PlannerExtensionInput &input, BoundStatement &statement) {
	GqlRegisterCsrWriteObserver(input.context);
	if (statement.plan) {
		ObserveWrites(input.context, *statement.plan);
	}
}

static void RegisterAlgorithmFunctions(ExtensionLoader &loader) {
	auto &database = loader.GetDatabaseInstance();
	auto &catalog = Catalog::GetSystemCatalog(database);
	auto transaction = CatalogTransaction::GetSystemTransaction(database);
	CreateSchemaInfo schema;
	schema.schema = "algo";
	schema.internal = true;
	schema.on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
	catalog.CreateSchema(transaction, schema);

	auto register_function = [&](TableFunction function) {
		CreateTableFunctionInfo info(std::move(function));
		info.schema = "algo";
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		loader.RegisterFunction(std::move(info));
	};
	register_function(GqlBfsFunction());
	register_function(GqlDfsFunction());
	register_function(GqlSsspFunction());
	register_function(GqlPageRankFunction());
	register_function(GqlWccFunction());
	register_function(GqlSccFunction());
	register_function(GqlTriangleCountFunction());
	register_function(GqlLccFunction());
	register_function(GqlDegreeFunction());
	register_function(GqlClosenessFunction());
}

static void LoadInternal(ExtensionLoader &loader) {
	loader.RegisterFunction(GqlGraphsFunction());
	loader.RegisterFunction(GqlNeighborsFunction());
	loader.RegisterFunction(GqlCsrVerticesFunction());
	loader.RegisterFunction(GqlCsrExpandFunction());
	loader.RegisterFunction(GqlCsrPathExpandFunction());
	loader.RegisterFunction(GqlVertexFetchFunction());
	loader.RegisterFunction(GqlEdgeFetchFunction());
	loader.RegisterFunction(GqlBuildCsrFunction());
	loader.RegisterFunction(GqlCsrStatsFunction());
	loader.RegisterFunction(GqlCsrEdgeStatsFunction());
	loader.RegisterFunction(GqlCreatePropertyIndexFunction());
	loader.RegisterFunction(GqlDropPropertyIndexFunction());
	loader.RegisterFunction(GqlPropertyIndexesFunction());
	RegisterAlgorithmFunctions(loader);
	loader.RegisterFunction(GqlRelationalMatchFunction());
	loader.RegisterFunction(GqlRecursiveMatchFunction());
	loader.RegisterFunction(GqlAlgorithmCallFunction());
	loader.RegisterFunction(GqlAlgorithmResultFunction());
	loader.RegisterFunction(GqlMutationTargetFunction());
	loader.RegisterFunction(GqlClearPropertiesSourceFunction());
	loader.RegisterFunction(GqlMutationGraphFunction());
	loader.RegisterFunction(GqlInsertTargetFunction());
	loader.RegisterFunction(GqlInsertIdsFunction());
	loader.RegisterFunction(GqlMatchInsertIdsFunction());
	loader.RegisterFunction(GqlMergeTargetFunction());
	loader.RegisterFunction(GqlMergeIdFunction());
	loader.RegisterFunction(GqlMutationControlFunction());
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	auto parser_override_option = DBConfig::GetOptionByName("allow_parser_override_extension");
	if (parser_override_option && parser_override_option->setting_idx.IsValid() &&
	    !config.user_settings.IsSet(parser_override_option->setting_idx.GetIndex())) {
		config.SetOption(*parser_override_option, Value("fallback"));
	}
	auto &callbacks = ExtensionCallbackManager::Get(loader.GetDatabaseInstance());
	callbacks.Register(GqlParserExtension());
	PlannerExtension planner_extension;
	planner_extension.post_bind_function = RegisterCsrWriteObserver;
	callbacks.Register(std::move(planner_extension));
}

void DuckgqlExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string DuckgqlExtension::Name() {
	return "duckgql";
}

std::string DuckgqlExtension::Version() const {
#ifdef EXT_VERSION_DUCKGQL
	return EXT_VERSION_DUCKGQL;
#else
	return "0.1.0";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(duckgql, loader) {
	duckdb::LoadInternal(loader);
}
}
