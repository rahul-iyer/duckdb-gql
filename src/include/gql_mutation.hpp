#pragma once

#include "gql_ir.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/sql_statement.hpp"

namespace duckdb {

class ClientContext;
struct GqlTableGraphBinding;

vector<unique_ptr<SQLStatement>> GqlLowerMutation(const vector<GqlLogicalPlan> &plans);
vector<unique_ptr<SQLStatement>> GqlLowerInsert(const GqlInsertStatement &insert);
vector<unique_ptr<SQLStatement>> GqlLowerMatchInsert(const vector<GqlLogicalPlan> &plans);
//! Loads the selected graph and returns true when writes go to referenced source tables.
bool GqlLoadReferencedWriteGraph(ClientContext &context, GqlTableGraphBinding &graph);
vector<unique_ptr<SQLStatement>> GqlLowerReferencedInsert(const GqlTableGraphBinding &graph,
                                                          const GqlInsertStatement &insert);
vector<unique_ptr<SQLStatement>> GqlLowerReferencedMutation(const GqlTableGraphBinding &graph,
                                                            const vector<GqlLogicalPlan> &plans);
vector<unique_ptr<SQLStatement>> GqlLowerReferencedMatchInsert(const GqlTableGraphBinding &graph,
                                                               const vector<GqlLogicalPlan> &plans);
vector<unique_ptr<SQLStatement>> GqlLowerMerge(const GqlMergeStatement &merge);
TableFunction GqlMutationTargetFunction();
TableFunction GqlClearPropertiesSourceFunction();
TableFunction GqlMutationGraphFunction();
TableFunction GqlInsertTargetFunction();
TableFunction GqlInsertResultFunction();
TableFunction GqlInsertIdsFunction();
TableFunction GqlMatchInsertIdsFunction();
TableFunction GqlMergeTargetFunction();
TableFunction GqlMergeIdFunction();

} // namespace duckdb
