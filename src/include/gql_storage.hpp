#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

class ClientContext;
class Connection;

void GqlEnsureStorage(Connection &connection);
TableFunction GqlCreateGraphFunction();
TableFunction GqlDropGraphFunction();
TableFunction GqlSetGraphFunction();
TableFunction GqlGraphsFunction();
TableFunction GqlCreatePropertyIndexFunction();
TableFunction GqlDropPropertyIndexFunction();
TableFunction GqlPropertyIndexesFunction();
TableFunction GqlMutationControlFunction();
string GqlGetSelectedGraph(ClientContext &context);

} // namespace duckdb
