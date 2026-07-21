#pragma once

#include "gql_ir.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/sql_statement.hpp"

namespace duckdb {

vector<unique_ptr<SQLStatement>>
GqlLowerMutation(const vector<GqlLogicalPlan> &plans);
vector<unique_ptr<SQLStatement>> GqlLowerMerge(const GqlMergeStatement &merge);
TableFunction GqlMutationTargetFunction();
TableFunction GqlMutationGraphFunction();
TableFunction GqlMergeTargetFunction();
TableFunction GqlMergeIdFunction();

} // namespace duckdb
