#pragma once

#include "duckdb/function/table_function.hpp"
#include "gql_csr.hpp"

namespace duckdb {

TableFunction GqlCopyGraphFunction();
TableFunction GqlAnalyzeGraphFunction();

using GqlFanoutStatistics = unordered_map<string, GqlCsrEdgeLabelStats>;
GqlFanoutStatistics GqlLoadFanoutStatistics(ClientContext &context, uint64_t graph_id);

} // namespace duckdb
