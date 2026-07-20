#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

class ClientContext;

TableFunction GqlCreateGraphFunction();
TableFunction GqlDropGraphFunction();
TableFunction GqlSetGraphFunction();
TableFunction GqlInsertVertexFunction();
TableFunction GqlInsertPathFunction();
TableFunction GqlGraphsFunction();
TableFunction GqlVerticesFunction();
TableFunction GqlEdgesFunction();
TableFunction GqlPropertiesFunction();
TableFunction GqlMutationControlFunction();
string GqlGetSelectedGraph(ClientContext &context);

} // namespace duckdb
