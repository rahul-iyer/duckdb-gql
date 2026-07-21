#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

TableFunction GqlNeighborsFunction();
TableFunction GqlBuildCsrFunction();
TableFunction GqlCsrStatsFunction();
TableFunction GqlCsrPathFunction();

} // namespace duckdb
