#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

TableFunction GqlNeighborsFunction();
TableFunction GqlCsrStatsFunction();
TableFunction GqlMatchFunction();

} // namespace duckdb
