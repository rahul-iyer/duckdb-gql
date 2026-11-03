#define DUCKDB_EXTENSION_MAIN

#include "duckgql_extension.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/main/extension_callback_manager.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/extension_callback.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_execute.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_merge_into.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/planner/planner_extension.hpp"
#include "gql_algorithms.hpp"
#include "gql_catalog.hpp"
#include "gql_batched_inout.hpp"
#include "gql_csr.hpp"
#include "gql_import.hpp"
#include "gql_mutation.hpp"
#include "gql_optimizer.hpp"
#include "gql_parser.hpp"
#include "gql_plan_reuse.hpp"
#include "gql_relational.hpp"
#include "gql_referenced.hpp"
#include "gql_storage.hpp"

namespace duckdb {

static void ObserveTableWrite(ClientContext &context, TableCatalogEntry &table, bool inserts_only) {
	GqlNotifyCsrTableWritePlanned(context, table.catalog.GetName().GetIdentifierName(),
	                              table.schema.name.GetIdentifierName(), table.name.GetIdentifierName(), inserts_only);
}

static bool MergeOnlyInserts(const LogicalMergeInto &merge) {
	for (const auto &condition : merge.actions) {
		for (const auto &action : condition.second) {
			if (action->action_type == MergeActionType::MERGE_UPDATE ||
			    action->action_type == MergeActionType::MERGE_DELETE) {
				return false;
			}
		}
	}
	return true;
}

static void ObserveWrites(ClientContext &context, const LogicalOperator &operation) {
	switch (operation.type) {
	case LogicalOperatorType::LOGICAL_INSERT: {
		const auto &insert = operation.Cast<LogicalInsert>();
		const auto conflict = insert.on_conflict_info.action_type;
		ObserveTableWrite(context, insert.table,
		                  conflict == OnConflictAction::THROW || conflict == OnConflictAction::NOTHING);
		break;
	}
	case LogicalOperatorType::LOGICAL_DELETE:
		ObserveTableWrite(context, operation.Cast<LogicalDelete>().table, false);
		break;
	case LogicalOperatorType::LOGICAL_UPDATE:
		ObserveTableWrite(context, operation.Cast<LogicalUpdate>().table, false);
		break;
	case LogicalOperatorType::LOGICAL_MERGE_INTO: {
		const auto &merge = operation.Cast<LogicalMergeInto>();
		ObserveTableWrite(context, merge.table, MergeOnlyInserts(merge));
		break;
	}
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
	GqlPlanReuseObserveBind(input, statement);
	// Before the commit too, so no reader can pair the new data with an old plan.
	if (!input.binder.GetStatementProperties().IsReadOnly()) {
		GqlInvalidateTableGraphCache(input.context);
	}
	if (statement.plan) {
		ObserveWrites(input.context, *statement.plan);
	}
}

// Writers that never bind a statement, such as appenders, must still reach the observer.
struct GqlConnectionObserver : ExtensionCallback {
	void OnConnectionOpened(ClientContext &context) override {
		GqlRegisterCsrWriteObserver(context);
	}
};

static void RegisterAlgorithmFunctions(ExtensionLoader &loader) {
	auto &database = loader.GetDatabaseInstance();
	auto &catalog = Catalog::GetSystemCatalog(database);
	auto transaction = CatalogTransaction::GetSystemTransaction(database);
	CreateSchemaInfo schema;
	schema.SetSchema("algo");
	schema.internal = true;
	schema.on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
	catalog.CreateSchema(transaction, schema);

	auto register_function = [&](TableFunction function) {
		CreateTableFunctionInfo info(std::move(function));
		info.SetSchema("algo");
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		loader.RegisterFunction(std::move(info));
	};
	register_function(GqlBfsFunction());
	register_function(GqlDfsFunction());
	register_function(GqlSsspFunction());
	register_function(GqlWeightedSsspFunction());
	register_function(GqlPageRankFunction());
	register_function(GqlWccFunction());
	register_function(GqlSccFunction());
	register_function(GqlTriangleCountFunction());
	register_function(GqlLccFunction());
	register_function(GqlLouvainFunction());
	register_function(GqlDegreeFunction());
	register_function(GqlClosenessFunction());
}

static void SetGqlOptimizer(ClientContext &, SetScope, Value &parameter) {
	if (parameter.IsNull()) {
		throw InvalidInputException("gql_optimizer must be 'legacy' or 'cost'");
	}
	auto mode = StringUtil::Lower(parameter.GetValue<string>());
	if (mode != "legacy" && mode != "cost") {
		throw InvalidInputException("gql_optimizer must be 'legacy' or 'cost'");
	}
	parameter = Value(mode);
}

static void LoadInternal(ExtensionLoader &loader) {
	loader.RegisterFunction(GqlGraphsFunction());
	loader.RegisterFunction(GqlReferencedKeysFunction());
	loader.RegisterFunction(GqlReferencedCacheStatsFunction());
	loader.RegisterFunction(GqlNeighborsFunction());
	loader.RegisterFunction(GqlCsrVerticesFunction());
	loader.RegisterFunction(GqlCsrExpandFunctions());
	loader.RegisterFunction(GqlCsrPathExpandFunction());
	loader.RegisterFunction(GqlVertexFetchFunction());
	loader.RegisterFunction(GqlVertexLookupFunction());
	loader.RegisterFunction(GqlCsrShortestExpandFunction());
	loader.RegisterFunction(GqlCsrAllShortestFunction());
	loader.RegisterFunction(GqlProjectedVertexFetchFunction());
	loader.RegisterFunction(GqlEdgeFetchFunction());
	loader.RegisterFunction(GqlBuildCsrFunction());
	loader.RegisterFunction(GqlCsrStatsFunction());
	loader.RegisterFunction(GqlCsrEdgeStatsFunction());
	loader.RegisterFunction(GqlCsrEndpointStatsFunction());
	loader.RegisterFunction(GqlAlgorithmStatsFunction());
	loader.RegisterFunction(GqlCreatePropertyIndexFunction());
	loader.RegisterFunction(GqlDropPropertyIndexFunction());
	loader.RegisterFunction(GqlPropertyIndexesFunction());
	RegisterAlgorithmFunctions(loader);
	loader.RegisterFunction(GqlRelationalMatchFunction());
	loader.RegisterFunction(GqlOptimizerPlanFunction());
	loader.RegisterFunction(GqlTrailUniqueFunction());
	loader.RegisterFunction(GqlFloatCastFunction());
	loader.RegisterFunction(GqlRecursiveMatchFunction());
	loader.RegisterFunction(GqlAlgorithmCallFunction());
	loader.RegisterFunction(GqlAlgorithmResultFunction());
	loader.RegisterFunction(GqlMutationTargetFunction());
	loader.RegisterFunction(GqlClearPropertiesSourceFunction());
	loader.RegisterFunction(GqlMutationGraphFunction());
	loader.RegisterFunction(GqlInsertTargetFunction());
	loader.RegisterFunction(GqlInsertResultFunction());
	loader.RegisterFunction(GqlInsertIdsFunction());
	loader.RegisterFunction(GqlMatchInsertIdsFunction());
	loader.RegisterFunction(GqlMergeTargetFunction());
	loader.RegisterFunction(GqlMergeIdFunction());
	loader.RegisterFunction(GqlMutationControlFunction());
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption("gql_optimizer",
	                          "Graph access planner: legacy (default) or experimental cost-based search",
	                          LogicalType::VARCHAR, Value("legacy"), SetGqlOptimizer);
	auto parser_override_option = DBConfig::GetOptionByName("allow_parser_override_extension");
	if (parser_override_option && parser_override_option->setting_idx.IsValid() &&
	    !config.user_settings.IsSet(parser_override_option->setting_idx.GetIndex())) {
		config.SetOption(*parser_override_option, Value("fallback"));
	}
	auto &callbacks = ExtensionCallbackManager::Get(loader.GetDatabaseInstance());
	callbacks.Register(GqlParserExtension());
	callbacks.Register(make_shared_ptr<GqlConnectionObserver>());
	for (auto &connection : ConnectionManager::Get(loader.GetDatabaseInstance()).GetConnectionList()) {
		GqlRegisterCsrWriteObserver(*connection);
	}
	PlannerExtension planner_extension;
	planner_extension.post_bind_function = RegisterCsrWriteObserver;
	callbacks.Register(std::move(planner_extension));
	OptimizerExtension batched_in_out;
	batched_in_out.optimize_function = GqlOptimizeMatchPlan;
	callbacks.Register(std::move(batched_in_out));
	OptimizerExtension plan_reuse;
	plan_reuse.optimize_function = GqlPlanReuseOptimize;
	callbacks.Register(std::move(plan_reuse));
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
	return "0.2.0";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(duckgql, loader) {
	duckdb::LoadInternal(loader);
}
}
