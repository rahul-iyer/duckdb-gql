#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

// Total single-threaded work, in units of one sequential tuple (about 4ns), not wall time.
// Every operator parallelizes, so no operator gets a parallelism discount. No cost or cardinality is a semantic
// bound: duplicate bindings can produce more rows than there are graph edges.
struct GqlCardinalityEstimate {
	idx_t rows = DConstants::INVALID_INDEX;
	idx_t distinct_ids = DConstants::INVALID_INDEX;
	idx_t row_width = 8;
	bool from_statistics = false;
};

struct GqlPlanCost {
	double scan = 0;
	double traversal = 0;
	double fetch = 0;
	double join = 0;
	double spill = 0;
	idx_t peak_memory_bytes = 0;

	double Total() const;
	void Add(const GqlPlanCost &other);
	void ApplyMemoryBudget(idx_t bytes);
};

// Shared by access-path enumeration and diagnostics. Each operator costs the tuples it touches:
// sequential work counts one per tuple, a random vertex or row access costs more. The constants
// were measured once on synthetic graphs, independent of any benchmark query.
struct GqlCostModel {
	GqlPlanCost Scan(idx_t table_rows, idx_t output_rows, idx_t width) const;
	GqlPlanCost Lookup(idx_t table_rows, idx_t output_rows, idx_t width) const;
	GqlPlanCost Fetch(idx_t rows, idx_t width) const;
	//! A CSR expansion walks every edge of each seed in its direction, checks the neighbor's
	//! label on edges of the requested type when the pattern requires one, and emits output_rows.
	GqlPlanCost Expand(idx_t seed_rows, double walked_edges, double checked_edges, idx_t output_rows) const;
	GqlPlanCost Join(idx_t left_rows, idx_t right_rows, idx_t output_rows, idx_t width) const;
};

} // namespace duckdb
