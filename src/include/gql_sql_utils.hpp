#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

class BaseQueryResult;
class Connection;
class MaterializedQueryResult;

string GqlQuoteLiteral(const string &value);
string GqlQuoteIdentifier(const string &value);
void GqlThrowOnError(const BaseQueryResult &result, const string &context = "DuckGQL internal query failed");
unique_ptr<MaterializedQueryResult> GqlQuery(Connection &connection, const string &sql,
                                             const string &context = "DuckGQL internal query failed");

} // namespace duckdb
