#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

// Snapshot-scoped, buffer-managed key maps. Properties are read by DuckLake's
// native scans and joins; this module neither copies them nor reads Parquet.
TableFunction GqlReferencedKeysFunction();
TableFunction GqlReferencedCacheStatsFunction();

} // namespace duckdb
