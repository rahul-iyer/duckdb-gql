#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

TableFunction GqlRelationalMatchFunction();
TableFunction GqlRecursiveMatchFunction();

} // namespace duckdb
