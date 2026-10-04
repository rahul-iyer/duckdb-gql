#pragma once

#include "duckdb/function/table_function.hpp"
#include "duckdb/function/scalar_function.hpp"

namespace duckdb {

struct GqlBoundExpression;
class ParsedExpression;

// Lower scalar/aggregate expressions whose variable references name projected columns.
unique_ptr<ParsedExpression> GqlLowerProjectedExpression(const GqlBoundExpression &expression,
                                                       const vector<string> &columns, const string &table_alias);

TableFunction GqlRelationalMatchFunction();
TableFunction GqlRecursiveMatchFunction();
ScalarFunction GqlTrailUniqueFunction();

} // namespace duckdb
