#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

// Relative work units, not milliseconds. No cost or cardinality is a semantic
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

// Shared by access-path enumeration and diagnostics. These deliberately simple
// coefficients are independent of query names and scale factors. They model
// sequential column work versus random row fetches and per-seed lateral work.
struct GqlCostModel {
	explicit GqlCostModel(idx_t worker_count_p = 1) : worker_count(worker_count_p ? worker_count_p : 1) {
	}
	GqlPlanCost Scan(idx_t table_rows, idx_t output_rows, idx_t width) const;
	GqlPlanCost Lookup(idx_t table_rows, idx_t output_rows, idx_t width) const;
	GqlPlanCost Fetch(idx_t rows, idx_t width) const;
	GqlPlanCost Expand(idx_t seed_rows, idx_t output_rows) const;
	GqlPlanCost Join(idx_t left_rows, idx_t right_rows, idx_t output_rows, idx_t width) const;

private:
	idx_t worker_count;
};

} // namespace duckdb
