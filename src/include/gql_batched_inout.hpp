#pragma once

#include "duckdb/optimizer/optimizer_extension.hpp"

namespace duckdb {

//! Post-optimizer rule giving lateral CSR expansion and element fetch whole input chunks.
void GqlBatchInOutFunctions(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);

//! Post-optimizer rules for statements that contain a MATCH: removes lowering's fences, then batches.
void GqlOptimizeMatchPlan(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);

} // namespace duckdb
