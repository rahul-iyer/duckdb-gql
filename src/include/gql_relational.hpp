#pragma once

#include "duckdb/function/table_function.hpp"
#include "duckdb/function/scalar_function.hpp"

namespace duckdb {

struct GqlBoundExpression;
class ParsedExpression;

//! A column of a MATCH's rows that says how many matches each row stands for. COUNT(*) over
//! the rows is the sum of this column, which lets a MATCH return fewer rows than it has matches.
constexpr const char *GQL_MULTIPLICITY_COLUMN = "__gql_aggregate_multiplicity";

// Lower scalar/aggregate expressions whose variable references name projected columns.
unique_ptr<ParsedExpression> GqlLowerProjectedExpression(const GqlBoundExpression &expression,
                                                       const vector<string> &columns, const string &table_alias);

TableFunction GqlRelationalMatchFunction();
TableFunction GqlRecursiveMatchFunction();
ScalarFunction GqlTrailUniqueFunction();
ScalarFunction GqlFloatCastFunction();

} // namespace duckdb
