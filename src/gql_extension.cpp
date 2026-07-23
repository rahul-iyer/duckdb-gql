#define DUCKDB_EXTENSION_MAIN

#include "gql_extension.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension_callback_manager.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "gql_algorithms.hpp"
#include "gql_catalog.hpp"
#include "gql_csr.hpp"
#include "gql_import.hpp"
#include "gql_mutation.hpp"
#include "gql_parser.hpp"
#include "gql_relational.hpp"
#include "gql_storage.hpp"

namespace duckdb {

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
  loader.RegisterFunction(GqlBuildCsrFunction());
  loader.RegisterFunction(GqlCsrStatsFunction());
  RegisterAlgorithmFunctions(loader);
  loader.RegisterFunction(GqlRelationalMatchFunction());
  loader.RegisterFunction(GqlRecursiveMatchFunction());
  loader.RegisterFunction(GqlAlgorithmCallFunction());
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
  auto parser_override_option =
      DBConfig::GetOptionByName("allow_parser_override_extension");
  if (parser_override_option && parser_override_option->setting_idx.IsValid() &&
      !config.user_settings.IsSet(
          parser_override_option->setting_idx.GetIndex())) {
    config.SetOption(*parser_override_option, Value("fallback"));
  }
  auto &callbacks = ExtensionCallbackManager::Get(loader.GetDatabaseInstance());
  callbacks.Register(GqlParserExtension());
}

void GqlExtension::Load(ExtensionLoader &loader) { LoadInternal(loader); }

std::string GqlExtension::Name() { return "gql"; }

std::string GqlExtension::Version() const {
#ifdef EXT_VERSION_GQL
  return EXT_VERSION_GQL;
#else
  return "0.1.0-dev";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(gql, loader) { duckdb::LoadInternal(loader); }
}
