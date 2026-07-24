#pragma once

#include "duckdb/parser/parser_extension.hpp"
#include "gql_ast.hpp"

namespace duckdb {

struct GqlParseData : ParserExtensionParseData {
	string query;
	shared_ptr<GqlStatement> statement;

	unique_ptr<ParserExtensionParseData> Copy() const override;
	string ToString() const override;
};

struct GqlParserExtensionInfo : ParserExtensionInfo {};

ParserExtensionParseResult GqlParse(ParserExtensionInfo *info, const string &query);
ParserExtensionPlanResult GqlPlan(ParserExtensionInfo *info, ClientContext &context,
                                  unique_ptr<ParserExtensionParseData> parse_data);
ParserOverrideResult GqlParserOverride(ParserExtensionInfo *info, const string &query, ParserOptions &options);

struct GqlParserExtension : ParserExtension {
	GqlParserExtension();
};

} // namespace duckdb
