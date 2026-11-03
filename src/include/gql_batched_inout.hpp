#pragma once

#include "duckdb/optimizer/optimizer_extension.hpp"

namespace duckdb {

//! Post-optimizer rule giving lateral CSR expansion and element fetch whole input chunks.
void GqlBatchInOutFunctions(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);

} // namespace duckdb
