#include "gql_cost_model.hpp"
#include "duckdb/storage/storage_info.hpp"

#include <algorithm>
#include <cmath>

namespace duckdb {

static double WorkRows(idx_t rows) {
	// Unknown is expensive, but finite so plans can still be compared and
	// explained. It must never turn into an integer overflow or a cheap zero.
	return rows == DConstants::INVALID_INDEX ? 1e12 : static_cast<double>(rows);
}

static idx_t MemoryBytes(double rows, idx_t width) {
	return static_cast<idx_t>(std::min(rows * width, static_cast<double>(DConstants::INVALID_INDEX / 2)));
}

double GqlPlanCost::Total() const {
	return scan + traversal + fetch + join + spill;
}

void GqlPlanCost::Add(const GqlPlanCost &other) {
	scan += other.scan;
	traversal += other.traversal;
	fetch += other.fetch;
	join += other.join;
	spill += other.spill;
	peak_memory_bytes = std::max(peak_memory_bytes, other.peak_memory_bytes);
}

void GqlPlanCost::ApplyMemoryBudget(idx_t bytes) {
	// A soft spill-work penalty, not an execution memory reservation or limit.
	spill = peak_memory_bytes > bytes ? static_cast<double>(peak_memory_bytes - bytes) / 8 : 0;
}

GqlPlanCost GqlCostModel::Scan(idx_t table_rows, idx_t output_rows, idx_t width) const {
	GqlPlanCost result;
	// A small table cannot use all workers: native scan parallelism is limited
	// by its row groups. This is a cost estimate, not a storage-layout assertion.
	auto groups = std::max(1.0, std::ceil(WorkRows(table_rows) / DEFAULT_ROW_GROUP_SIZE));
	auto parallelism = std::min(static_cast<double>(worker_count), groups);
	result.scan = 8 + WorkRows(table_rows) * (0.08 + static_cast<double>(width) / 256) / parallelism;
	// Scans stream; do not confuse total bytes read with retained memory.
	result.peak_memory_bytes = MemoryBytes(std::min(WorkRows(output_rows), 2048.0), width);
	return result;
}

GqlPlanCost GqlCostModel::Lookup(idx_t table_rows, idx_t output_rows, idx_t width) const {
	auto result = Fetch(output_rows, width);
	// This is a native table predicate (ART or scan), not a separate graph
	// table-function bind. Do not charge element-fetch metadata startup here.
	result.fetch = 8 + WorkRows(output_rows) * (128 + static_cast<double>(width) * 2);
	result.scan = 4 * std::log2(2 + WorkRows(table_rows));
	return result;
}

GqlPlanCost GqlCostModel::Fetch(idx_t rows, idx_t width) const {
	GqlPlanCost result;
	// Element-fetch functions bind graph metadata and initialize a correlated
	// source even for one row. Keep that fixed work separate from per-row work;
	// otherwise tiny frontiers appear almost free compared with native joins.
	// Scattered projected row fetches cross many storage vectors. Calibration
	// must charge that work even when only a few columns are requested.
	auto per_row = analyzed_domains ? 512 + static_cast<double>(width) * 8 : 128 + static_cast<double>(width) * 2;
	result.fetch = 32776 + WorkRows(rows) * per_row;
	result.peak_memory_bytes = MemoryBytes(std::min(WorkRows(rows), 2048.0), width);
	return result;
}

GqlPlanCost GqlCostModel::Expand(idx_t seed_rows, idx_t output_rows) const {
	GqlPlanCost result;
	result.traversal = 8 + WorkRows(seed_rows) * 32 + WorkRows(output_rows) * 1.5;
	result.peak_memory_bytes = MemoryBytes(2048, 32);
	return result;
}

GqlPlanCost GqlCostModel::Join(idx_t left_rows, idx_t right_rows, idx_t output_rows, idx_t width) const {
	GqlPlanCost result;
	auto build_rows = std::min(WorkRows(left_rows), WorkRows(right_rows));
	result.join = (WorkRows(left_rows) + WorkRows(right_rows)) * 0.2 + WorkRows(output_rows) * 0.05;
	result.peak_memory_bytes = MemoryBytes(build_rows, width + 16);
	return result;
}

} // namespace duckdb
