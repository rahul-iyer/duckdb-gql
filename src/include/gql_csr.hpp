#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/function/table_function.hpp"

#include <limits>

namespace duckdb {

class ClientContext;

class GqlCsrVertexIds {
public:
	void reserve(idx_t count) {
		explicit_ids.reserve(count);
	}

	void push_back(uint64_t vertex_id) {
		explicit_ids.push_back(vertex_id);
		count = explicit_ids.size();
	}

	bool empty() const {
		return count == 0;
	}

	uint64_t back() const {
		return explicit_ids.back();
	}

	idx_t size() const {
		return count;
	}

	uint64_t operator[](idx_t ordinal) const {
		D_ASSERT(ordinal < count);
		return implicit_dense ? ordinal + 1 : explicit_ids[ordinal];
	}

	void MakeImplicitDense() {
		count = explicit_ids.size();
		vector<uint64_t>().swap(explicit_ids);
		implicit_dense = true;
	}

	bool IsImplicitDense() const {
		return implicit_dense;
	}

	idx_t AllocatedBytes() const {
		return explicit_ids.capacity() * sizeof(uint64_t);
	}

private:
	bool implicit_dense = false;
	idx_t count = 0;
	vector<uint64_t> explicit_ids;
};

class GqlCsrOrdinals {
public:
	void Resize(idx_t count, bool use_compact) {
		compact = use_compact;
		if (compact) {
			compact_values.resize(count);
			vector<idx_t>().swap(wide_values);
		} else {
			wide_values.resize(count);
			vector<uint32_t>().swap(compact_values);
		}
	}

	void Set(idx_t index, idx_t ordinal) {
		if (compact) {
			D_ASSERT(ordinal <= std::numeric_limits<uint32_t>::max());
			compact_values[index] = static_cast<uint32_t>(ordinal);
		} else {
			wide_values[index] = ordinal;
		}
	}

	idx_t operator[](idx_t index) const {
		return compact ? static_cast<idx_t>(compact_values[index]) : wide_values[index];
	}

	idx_t size() const {
		return compact ? compact_values.size() : wide_values.size();
	}

	uint8_t WidthBytes() const {
		return compact ? sizeof(uint32_t) : sizeof(idx_t);
	}

	idx_t AllocatedBytes() const {
		return compact ? compact_values.capacity() * sizeof(uint32_t) : wide_values.capacity() * sizeof(idx_t);
	}

private:
	bool compact = false;
	vector<uint32_t> compact_values;
	vector<idx_t> wide_values;
};

class GqlCsrEdgeLabels {
public:
	void Reset(idx_t count, bool use_uniform, uint32_t uniform_label) {
		logical_size = count;
		uniform = use_uniform;
		uniform_label_id = uniform_label;
		if (uniform) {
			vector<uint32_t>().swap(explicit_values);
		} else {
			explicit_values.resize(count);
		}
	}

	void Set(idx_t index, uint32_t label_id) {
		D_ASSERT(!uniform);
		explicit_values[index] = label_id;
	}

	void MaterializeUniform() {
		if (!uniform) {
			return;
		}
		explicit_values.assign(logical_size, uniform_label_id);
		uniform = false;
	}

	uint32_t operator[](idx_t index) const {
		D_ASSERT(index < logical_size);
		return uniform ? uniform_label_id : explicit_values[index];
	}

	idx_t size() const {
		return logical_size;
	}

	bool IsUniform() const {
		return uniform;
	}

	uint32_t UniformLabelId() const {
		return uniform_label_id;
	}

	idx_t AllocatedBytes() const {
		return explicit_values.capacity() * sizeof(uint32_t);
	}

private:
	idx_t logical_size = 0;
	bool uniform = true;
	uint32_t uniform_label_id = 0;
	vector<uint32_t> explicit_values;
};

struct GqlCsrSnapshot {
	uint64_t graph_id;
	uint64_t graph_version;
	uint64_t write_generation;
	uint64_t vertex_write_generation;
	uint64_t edge_write_generation;
	string vertex_table_key;
	string edge_table_key;
	bool dense_vertex_ids = false;
	GqlCsrVertexIds vertex_ids;
	unordered_map<uint64_t, idx_t> ordinal_by_id;
	vector<idx_t> vertex_label_offsets;
	vector<uint32_t> vertex_label_ids;
	vector<uint64_t> vertex_label_posting_offsets;
	GqlCsrOrdinals vertex_label_postings;
	vector<uint64_t> outgoing_offsets;
	GqlCsrOrdinals outgoing_neighbors;
	vector<uint64_t> outgoing_edge_ids;
	GqlCsrEdgeLabels outgoing_label_ids;
	vector<uint64_t> incoming_offsets;
	GqlCsrOrdinals incoming_neighbors;
	vector<uint64_t> incoming_edge_ids;
	GqlCsrEdgeLabels incoming_label_ids;
	unordered_map<string, uint32_t> label_ids;
	bool edge_labels_single = true;
	idx_t topology_bytes = 0;
	idx_t identity_bytes = 0;
	idx_t label_bytes = 0;
	idx_t auxiliary_bytes = 0;
	idx_t build_auxiliary_bytes = 0;
	idx_t memory_bytes = 0;
};

bool GqlTryGetCsrOrdinal(const GqlCsrSnapshot &snapshot, uint64_t vertex_id, idx_t &ordinal);

shared_ptr<const GqlCsrSnapshot> GqlGetCsrSnapshot(ClientContext &context, const string &graph_name);

//! Returns a current connection-local snapshot when one is available and
//! valid. Unlike GqlGetCsrSnapshot, this is a non-throwing optimizer probe.
shared_ptr<const GqlCsrSnapshot> GqlTryGetCsrSnapshot(ClientContext &context, const string &graph_name);

//! Register a lightweight observer that invalidates snapshots when a prepared
//! write executes.
void GqlRegisterCsrWriteObserver(ClientContext &context);

//! Advance the generation for a table when a write is planned.
void GqlNotifyCsrTableWritePlanned(ClientContext &context, const string &catalog_name, const string &schema_name,
                                   const string &table_name);

//! Conservatively invalidate snapshots when a prepared write executes.
void GqlNotifyCsrPreparedWriteExecution(ClientContext &context);

TableFunction GqlNeighborsFunction();
TableFunction GqlCsrVerticesFunction();
TableFunction GqlCsrExpandFunction();
TableFunction GqlBuildCsrFunction();
TableFunction GqlCsrStatsFunction();

} // namespace duckdb
