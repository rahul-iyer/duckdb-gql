#define DUCKDB_EXTENSION_MAIN

#include "gql_extension.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/main/extension_callback_manager.hpp"
#include "gql_catalog.hpp"
#include "gql_csr.hpp"
#include "gql_import.hpp"
#include "gql_mutation.hpp"
#include "gql_parser.hpp"
#include "gql_relational.hpp"
#include "gql_storage.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	loader.RegisterFunction(GqlGraphsFunction());
	loader.RegisterFunction(GqlNeighborsFunction());
	loader.RegisterFunction(GqlBuildCsrFunction());
	loader.RegisterFunction(GqlCsrStatsFunction());
	loader.RegisterFunction(GqlCsrPathFunction());
	loader.RegisterFunction(GqlRelationalMatchFunction());
	loader.RegisterFunction(GqlRecursiveMatchFunction());
	loader.RegisterFunction(GqlMutationTargetFunction());
	loader.RegisterFunction(GqlMutationGraphFunction());
	loader.RegisterFunction(GqlInsertTargetFunction());
	loader.RegisterFunction(GqlInsertIdsFunction());
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
}

void GqlExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string GqlExtension::Name() {
	return "gql";
}

std::string GqlExtension::Version() const {
#ifdef EXT_VERSION_GQL
	return EXT_VERSION_GQL;
#else
	return "0.1.0-dev";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(gql, loader) {
	duckdb::LoadInternal(loader);
}
}
