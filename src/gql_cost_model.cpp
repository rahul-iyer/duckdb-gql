#include "gql_cost_model.hpp"

#include <algorithm>
#include <cmath>

namespace duckdb {

// Measured on one thread over graphs larger than the CPU caches, in units of one sequential tuple.
// Sequential: a scanned row, a walked CSR edge, an emitted row. A seed's vertex lookup is random.
// A neighbor label check follows the CSR's edge order, which bulk loads write sorted by endpoint,
// so it costs about one sequential read. A row fetch reconstructs every requested column.
static constexpr double RANDOM_VERTEX_ACCESS = 30;
static constexpr double LABEL_CHECK = 1;
static constexpr double WALKED_EDGE = 0.5;
static constexpr double EMITTED_ROW = 0.6;
static constexpr double JOIN_INPUT_ROW = 2.5;
static constexpr double FETCH_ROW = 128;
static constexpr double FETCH_BYTE = 4;
//! Binding graph metadata and initializing a correlated table function, even for one row.
static constexpr double FUNCTION_STARTUP = 8192;

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
	// A soft spill-work penalty for writing and rereading the excess, not a memory reservation.
	spill = peak_memory_bytes > bytes ? static_cast<double>(peak_memory_bytes - bytes) / 4 : 0;
}

GqlPlanCost GqlCostModel::Scan(idx_t table_rows, idx_t output_rows, idx_t width) const {
	GqlPlanCost result;
	result.scan = 8 + WorkRows(table_rows) * (1 + static_cast<double>(width) / 32);
	// Scans stream; do not confuse total bytes read with retained memory.
	result.peak_memory_bytes = MemoryBytes(std::min(WorkRows(output_rows), 2048.0), width);
	return result;
}

GqlPlanCost GqlCostModel::Lookup(idx_t table_rows, idx_t output_rows, idx_t width) const {
	auto result = Fetch(output_rows, width);
	// A native table predicate (ART probe or scan), not a separate graph table-function bind.
	result.fetch -= FUNCTION_STARTUP;
	result.scan = RANDOM_VERTEX_ACCESS * std::log2(2 + WorkRows(table_rows));
	return result;
}

GqlPlanCost GqlCostModel::Fetch(idx_t rows, idx_t width) const {
	GqlPlanCost result;
	result.fetch = FUNCTION_STARTUP + WorkRows(rows) * (FETCH_ROW + FETCH_BYTE * static_cast<double>(width));
	result.peak_memory_bytes = MemoryBytes(std::min(WorkRows(rows), 2048.0), width);
	return result;
}

GqlPlanCost GqlCostModel::Expand(idx_t seed_rows, double walked_edges, double checked_edges, idx_t output_rows) const {
	GqlPlanCost result;
	result.traversal = 8 + WorkRows(seed_rows) * RANDOM_VERTEX_ACCESS + walked_edges * WALKED_EDGE +
	                   checked_edges * LABEL_CHECK + WorkRows(output_rows) * EMITTED_ROW;
	result.peak_memory_bytes = MemoryBytes(2048, 32);
	return result;
}

GqlPlanCost GqlCostModel::Join(idx_t left_rows, idx_t right_rows, idx_t output_rows, idx_t width) const {
	GqlPlanCost result;
	auto build_rows = std::min(WorkRows(left_rows), WorkRows(right_rows));
	result.join = (WorkRows(left_rows) + WorkRows(right_rows)) * JOIN_INPUT_ROW + WorkRows(output_rows) * EMITTED_ROW;
	result.peak_memory_bytes = MemoryBytes(build_rows, width + 16);
	return result;
}

} // namespace duckdb
