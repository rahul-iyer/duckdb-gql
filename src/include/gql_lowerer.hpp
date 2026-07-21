#pragma once

#include "gql_ir.hpp"

#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/parser/sql_statement.hpp"

namespace duckdb {

ParserExtensionPlanResult GqlLower(const GqlLogicalPlan &plan);
unique_ptr<SQLStatement> GqlLowerSelect(vector<GqlLogicalPlan> plans);

} // namespace duckdb
