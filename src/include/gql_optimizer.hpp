#pragma once

#include "gql_ir.hpp"

namespace duckdb {

// Applies semantics-preserving logical rewrites and annotates every operator
// with binding, correlation, nullability, and cardinality properties.
void GqlOptimize(GqlLogicalPlan &plan);

} // namespace duckdb
