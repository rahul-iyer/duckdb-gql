#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/storage/buffer/buffer_handle.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/function/table_function.hpp"

#include <functional>
#include <limits>

namespace duckdb {

class ClientContext;

using GqlCsrCapabilities = uint32_t;

enum GqlCsrCapability : GqlCsrCapabilities {
	GQL_CSR_OUTGOING = 1U << 0U,
	GQL_CSR_INCOMING = 1U << 1U,
	GQL_CSR_EDGE_IDS = 1U << 2U,
	GQL_CSR_EDGE_LABELS = 1U << 3U,
	GQL_CSR_VERTEX_LABELS = 1U << 4U,
	GQL_CSR_VERTEX_LABEL_POSTINGS = 1U << 5U,
	GQL_CSR_EDGE_STATS = 1U << 6U,
	GQL_CSR_OUT_DEGREES = 1U << 7U,
	GQL_CSR_ENDPOINT_STATS = 1U << 8U,
	GQL_CSR_VERTEX_ROW_IDS = 1U << 9U,
};

static constexpr GqlCsrCapabilities GQL_CSR_FULL = GQL_CSR_OUTGOING | GQL_CSR_INCOMING | GQL_CSR_EDGE_IDS |
                                                   GQL_CSR_EDGE_LABELS | GQL_CSR_VERTEX_LABELS |
                                                   GQL_CSR_VERTEX_LABEL_POSTINGS | GQL_CSR_EDGE_STATS |
                                                   GQL_CSR_VERTEX_ROW_IDS;

//! Reads a saved page back. A page with a source can be dropped under memory pressure, not spilled.
class GqlCsrPageSource {
public:
	virtual ~GqlCsrPageSource() = default;
	virtual void Read(idx_t array_id, idx_t page, data_ptr_t target, idx_t bytes) = 0;
};

//! Writes metadata in order, and gives each paged array an ID for its pages.
class GqlCsrWriter {
public:
	template <class T>
	void Write(const T &value) {
		data.append(reinterpret_cast<const char *>(&value), sizeof(T));
	}
	void WriteBytes(const void *bytes, idx_t size) {
		Write<idx_t>(size);
		data.append(static_cast<const char *>(bytes), size);
	}
	void WriteString(const string &value) {
		WriteBytes(value.data(), value.size());
	}
	string data;
	idx_t next_array = 0;
	std::function<void(idx_t array_id, idx_t page, const_data_ptr_t bytes)> write_page;
};

class GqlCsrReader {
public:
	GqlCsrReader(const string &data_p, BufferManager &manager_p, shared_ptr<GqlCsrPageSource> source_p)
	    : data(data_p), manager(manager_p), source(std::move(source_p)) {
	}
	template <class T>
	T Read() {
		T value;
		ReadInto(&value, sizeof(T));
		return value;
	}
	void ReadInto(void *target, idx_t size) {
		if (position + size > data.size()) {
			throw InvalidInputException("Saved GQL CSR metadata is truncated");
		}
		memcpy(target, data.data() + position, size);
		position += size;
	}
	idx_t ReadSize() {
		return Read<idx_t>();
	}
	string ReadString() {
		string value(ReadSize(), '\0');
		ReadInto(&value[0], value.size());
		return value;
	}
	const string &data;
	idx_t position = 0;
	BufferManager &manager;
	shared_ptr<GqlCsrPageSource> source;
	idx_t next_array = 0;
};

// Values on evictable pages of the buffer manager, so they count towards the memory limit. A page
// stays pinned from its first read until ReleasePages, which callers run when no query can be reading.
class GqlCsrPages {
public:
	GqlCsrPages(BufferManager &manager, idx_t byte_count);
	//! Pages read from a source when first used.
	GqlCsrPages(BufferManager &manager, idx_t byte_count, shared_ptr<GqlCsrPageSource> source, idx_t array_id);

	//! The page's data, pinning it if needed.
	data_ptr_t Page(idx_t page) const {
		// Reads through the pointer depend on its value, which CPUs order without a barrier. A
		// barrier on this path slows every read of a graph-wide search.
		auto data = pointers[page].load(std::memory_order_relaxed);
		return data ? data : Pin(page);
	}

	void ReleasePages() const;

	idx_t AllocatedBytes() const {
		return blocks.size() * PAGE_BYTES;
	}

	idx_t PageCount() const {
		return blocks.size();
	}

	static constexpr idx_t PAGE_SHIFT = 18;
	static constexpr idx_t PAGE_BYTES = idx_t(1) << PAGE_SHIFT;

private:
	data_ptr_t Pin(idx_t page) const;

	BufferManager &manager;
	shared_ptr<GqlCsrPageSource> source;
	idx_t array_id = 0;
	mutable mutex lock;
	mutable vector<shared_ptr<BlockHandle>> blocks;
	mutable vector<BufferHandle> pins;
	mutable unique_ptr<atomic<data_ptr_t>[]> pointers;
};

// An array built in memory, then moved to buffer-managed pages. Reads look the same either way.
template <class T>
class GqlCsrArray {
public:
	void assign(idx_t count, const T &value) {
		values.assign(count, value);
	}
	void resize(idx_t count) {
		values.resize(count);
	}
	void reserve(idx_t count) {
		values.reserve(count);
	}
	void push_back(const T &value) {
		values.push_back(value);
	}
	T &back() {
		return values.back();
	}
	typename vector<T>::iterator begin() {
		return values.begin();
	}
	typename vector<T>::iterator end() {
		return values.end();
	}
	T &operator[](idx_t index) {
		D_ASSERT(!pages);
		return values[index];
	}
	const T &operator[](idx_t index) const {
		if (pages) {
			return reinterpret_cast<const T *>(pages->Page(index >> PAGE_VALUE_SHIFT))[index & PAGE_VALUE_MASK];
		}
		return values[index];
	}
	idx_t size() const {
		return pages ? logical_size : values.size();
	}
	bool empty() const {
		return size() == 0;
	}
	idx_t AllocatedBytes() const {
		return pages ? pages->AllocatedBytes() : values.capacity() * sizeof(T);
	}
	void Save(GqlCsrWriter &writer) const {
		writer.Write<bool>(pages != nullptr);
		writer.Write<idx_t>(size());
		if (!pages) {
			writer.WriteBytes(values.data(), values.size() * sizeof(T));
			return;
		}
		const auto array_id = writer.next_array++;
		for (idx_t page = 0; page < pages->PageCount(); page++) {
			writer.write_page(array_id, page, pages->Page(page));
		}
	}
	void Load(GqlCsrReader &reader) {
		const auto paged = reader.Read<bool>();
		logical_size = reader.Read<idx_t>();
		if (paged) {
			pages = make_uniq<GqlCsrPages>(reader.manager, logical_size * sizeof(T), reader.source, reader.next_array++);
			return;
		}
		values.resize(reader.ReadSize() / sizeof(T));
		reader.ReadInto(values.data(), values.size() * sizeof(T));
	}
	//! A copy of the values, while they are still in memory.
	vector<T> Values() const {
		D_ASSERT(!pages);
		return values;
	}
	void ReleasePages() const {
		if (pages) {
			pages->ReleasePages();
		}
	}
	//! Moves the values to pages, when there are enough to fill one.
	void Freeze(BufferManager &manager) {
		if (values.size() * sizeof(T) < GqlCsrPages::PAGE_BYTES) {
			return;
		}
		logical_size = values.size();
		pages = make_uniq<GqlCsrPages>(manager, logical_size * sizeof(T));
		for (idx_t start = 0; start < logical_size; start += PAGE_VALUE_MASK + 1) {
			const auto count = MinValue<idx_t>(PAGE_VALUE_MASK + 1, logical_size - start);
			memcpy(pages->Page(start >> PAGE_VALUE_SHIFT), values.data() + start, count * sizeof(T));
		}
		vector<T>().swap(values);
	}

private:
	static constexpr idx_t PAGE_VALUE_SHIFT = sizeof(T) == 4 ? GqlCsrPages::PAGE_SHIFT - 2 : GqlCsrPages::PAGE_SHIFT - 3;
	static constexpr idx_t PAGE_VALUE_MASK = (idx_t(1) << PAGE_VALUE_SHIFT) - 1;

	vector<T> values;
	idx_t logical_size = 0;
	unique_ptr<GqlCsrPages> pages;
};

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
		return explicit_ids[count - 1];
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
		explicit_ids = GqlCsrArray<uint64_t>();
		implicit_dense = true;
	}

	bool IsImplicitDense() const {
		return implicit_dense;
	}

	idx_t AllocatedBytes() const {
		return explicit_ids.AllocatedBytes();
	}

	void Freeze(BufferManager &manager) {
		explicit_ids.Freeze(manager);
	}

	void Save(GqlCsrWriter &writer) const {
		writer.Write<bool>(implicit_dense);
		writer.Write<idx_t>(count);
		explicit_ids.Save(writer);
	}

	void Load(GqlCsrReader &reader) {
		implicit_dense = reader.Read<bool>();
		count = reader.Read<idx_t>();
		explicit_ids.Load(reader);
	}

	void ReleasePages() const {
		explicit_ids.ReleasePages();
	}

private:
	bool implicit_dense = false;
	idx_t count = 0;
	GqlCsrArray<uint64_t> explicit_ids;
};

class GqlCsrOrdinals {
public:
	//! With a manager, values large enough for a page live in buffer-managed pages.
	void Resize(idx_t count, bool use_compact, BufferManager *manager = nullptr) {
		compact = use_compact;
		logical_size = count;
		vector<uint32_t>().swap(compact_values);
		vector<idx_t>().swap(wide_values);
		pages.reset();
		if (manager && count * WidthBytes() >= GqlCsrPages::PAGE_BYTES) {
			pages = make_uniq<GqlCsrPages>(*manager, count * WidthBytes());
		} else if (compact) {
			compact_values.resize(count);
		} else {
			wide_values.resize(count);
		}
	}

	void Set(idx_t index, idx_t ordinal) {
		if (compact) {
			// Only builds write here, so this costs reads nothing. A silent cut would corrupt the CSR.
			if (ordinal > std::numeric_limits<uint32_t>::max()) {
				throw InternalException("GQL CSR value %llu does not fit the width chosen for it",
				                        static_cast<unsigned long long>(ordinal));
			}
			if (pages) {
				reinterpret_cast<uint32_t *>(pages->Page(index >> (GqlCsrPages::PAGE_SHIFT - 2)))[index & COMPACT_MASK] =
				    static_cast<uint32_t>(ordinal);
			} else {
				compact_values[index] = static_cast<uint32_t>(ordinal);
			}
		} else if (pages) {
			reinterpret_cast<idx_t *>(pages->Page(index >> (GqlCsrPages::PAGE_SHIFT - 3)))[index & WIDE_MASK] = ordinal;
		} else {
			wide_values[index] = ordinal;
		}
	}

	idx_t operator[](idx_t index) const {
		if (pages) {
			if (compact) {
				return reinterpret_cast<const uint32_t *>(pages->Page(index >> (GqlCsrPages::PAGE_SHIFT - 2)))[index & COMPACT_MASK];
			}
			return reinterpret_cast<const idx_t *>(pages->Page(index >> (GqlCsrPages::PAGE_SHIFT - 3)))[index & WIDE_MASK];
		}
		return compact ? static_cast<idx_t>(compact_values[index]) : wide_values[index];
	}

	//! Calls function(index, value) over a range, looking up each page once.
	template <class FUNCTION>
	void ForEach(idx_t start, idx_t end, FUNCTION &&function) const {
		if (!pages) {
			for (auto index = start; index < end; index++) {
				function(index, (*this)[index]);
			}
		} else if (compact) {
			ForEachOnPages<uint32_t>(start, end, function);
		} else {
			ForEachOnPages<idx_t>(start, end, function);
		}
	}

	idx_t size() const {
		return logical_size;
	}

	bool empty() const {
		return size() == 0;
	}

	uint8_t WidthBytes() const {
		return compact ? sizeof(uint32_t) : sizeof(idx_t);
	}

	idx_t AllocatedBytes() const {
		if (pages) {
			return pages->AllocatedBytes();
		}
		return compact ? compact_values.capacity() * sizeof(uint32_t) : wide_values.capacity() * sizeof(idx_t);
	}

	void ReleasePages() const {
		if (pages) {
			pages->ReleasePages();
		}
	}

	void Save(GqlCsrWriter &writer) const {
		writer.Write<bool>(compact);
		writer.Write<idx_t>(logical_size);
		writer.Write<bool>(pages != nullptr);
		if (pages) {
			const auto array_id = writer.next_array++;
			for (idx_t page = 0; page < pages->PageCount(); page++) {
				writer.write_page(array_id, page, pages->Page(page));
			}
		} else if (compact) {
			writer.WriteBytes(compact_values.data(), compact_values.size() * sizeof(uint32_t));
		} else {
			writer.WriteBytes(wide_values.data(), wide_values.size() * sizeof(idx_t));
		}
	}

	void Load(GqlCsrReader &reader) {
		compact = reader.Read<bool>();
		logical_size = reader.Read<idx_t>();
		if (reader.Read<bool>()) {
			pages = make_uniq<GqlCsrPages>(reader.manager, logical_size * WidthBytes(), reader.source, reader.next_array++);
		} else if (compact) {
			compact_values.resize(reader.ReadSize() / sizeof(uint32_t));
			reader.ReadInto(compact_values.data(), compact_values.size() * sizeof(uint32_t));
		} else {
			wide_values.resize(reader.ReadSize() / sizeof(idx_t));
			reader.ReadInto(wide_values.data(), wide_values.size() * sizeof(idx_t));
		}
	}

private:
	template <class VALUE, class FUNCTION>
	void ForEachOnPages(idx_t start, idx_t end, FUNCTION &function) const {
		const idx_t per_page = GqlCsrPages::PAGE_BYTES / sizeof(VALUE);
		while (start < end) {
			const auto page = start / per_page;
			const auto stop = MinValue<idx_t>(end, (page + 1) * per_page);
			const auto *data = reinterpret_cast<const VALUE *>(pages->Page(page));
			for (auto index = start; index < stop; index++) {
				function(index, static_cast<idx_t>(data[index % per_page]));
			}
			start = stop;
		}
	}

	static constexpr idx_t COMPACT_MASK = (GqlCsrPages::PAGE_BYTES >> 2) - 1;
	static constexpr idx_t WIDE_MASK = (GqlCsrPages::PAGE_BYTES >> 3) - 1;

	bool compact = false;
	idx_t logical_size = 0;
	vector<uint32_t> compact_values;
	vector<idx_t> wide_values;
	unique_ptr<GqlCsrPages> pages;
};

class GqlCsrEdgeLabels {
public:
	void Reset(idx_t count, bool use_uniform, uint32_t uniform_label) {
		logical_size = count;
		uniform = use_uniform;
		uniform_label_id = uniform_label;
		if (uniform) {
			explicit_values = GqlCsrArray<uint32_t>();
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
		return explicit_values.AllocatedBytes();
	}

	void Freeze(BufferManager &manager) {
		explicit_values.Freeze(manager);
	}

	void Save(GqlCsrWriter &writer) const {
		writer.Write<idx_t>(logical_size);
		writer.Write<bool>(uniform);
		writer.Write<uint32_t>(uniform_label_id);
		explicit_values.Save(writer);
	}

	void Load(GqlCsrReader &reader) {
		logical_size = reader.Read<idx_t>();
		uniform = reader.Read<bool>();
		uniform_label_id = reader.Read<uint32_t>();
		explicit_values.Load(reader);
	}

	void ReleasePages() const {
		explicit_values.ReleasePages();
	}

private:
	idx_t logical_size = 0;
	bool uniform = true;
	uint32_t uniform_label_id = 0;
	GqlCsrArray<uint32_t> explicit_values;
};

struct GqlCsrEdgeLabelStats {
	uint64_t edge_count = 0;
	uint64_t outgoing_vertex_count = 0;
	uint64_t incoming_vertex_count = 0;
	uint64_t max_outgoing_degree = 0;
	uint64_t max_incoming_degree = 0;
	//! Sums of squared degree. A vertex reached over an edge is degree-biased, with mean degree squares/edges.
	double outgoing_degree_squares = 0;
	double incoming_degree_squares = 0;
};

// Population totals include vertices with zero matching degree. Sparse pairs
// avoid a vertex-label by relationship-type matrix. Absence means zero only
// when the snapshot marks this complete, bounded statistic as available.
struct GqlCsrLabelFanoutStats {
	uint64_t outgoing_edges = 0;
	uint64_t incoming_edges = 0;
	uint64_t outgoing_vertices = 0;
	uint64_t incoming_vertices = 0;
	// Populated by explicit endpoint analysis, not by the cheap marginal pass.
	double outgoing_degree_squares = 0;
	double incoming_degree_squares = 0;
};

inline uint64_t GqlCsrLabelFanoutKey(uint32_t vertex_label, uint32_t edge_label) {
	return (static_cast<uint64_t>(vertex_label) << 32) | edge_label;
}

struct GqlEndpointGroupCounts {
	uint64_t edges = 0;
	uint64_t source_vertices = 0;
	uint64_t target_vertices = 0;
};

// Label sets partition vertices: overlapping individual labels cannot count an
// edge twice. Packed keys contain a 32-bit edge label and two 8-bit group IDs.
struct GqlEndpointStatistics {
	vector<vector<uint32_t>> groups;
	vector<uint64_t> populations;
	unordered_map<uint64_t, GqlEndpointGroupCounts> counts;
	unordered_map<uint64_t, GqlCsrLabelFanoutStats> fanout_by_group;
	idx_t AllocatedBytes() const;
};

struct GqlEndpointSummary {
	uint64_t edges = 0;
	uint64_t source_edges = 0;
	uint64_t target_edges = 0;
	uint64_t source_population = 0;
	uint64_t target_population = 0;
	uint64_t source_active = 0;
	uint64_t target_active = 0;
	double source_degree_squares = 0;
	double target_degree_squares = 0;
};

inline uint64_t GqlEndpointGroupKey(uint32_t edge_label, uint8_t source, uint8_t target) {
	return (static_cast<uint64_t>(edge_label) << 16) | (static_cast<uint64_t>(source) << 8) | target;
}

struct GqlCsrDeltaEdge {
	idx_t neighbor;
	uint64_t edge_id;
	uint32_t label_id;
};

//! Rows inserted since a full snapshot was built, which stays immutable underneath. New
//! vertices take ordinals past the snapshot's own. A catch-up publishes a new delta, so a
//! statement keeps the one it leased. Labels are the snapshot's, or the rows force a rebuild.
struct GqlCsrDelta {
	vector<uint64_t> vertex_ids;
	vector<uint64_t> vertex_row_ids;
	vector<vector<uint32_t>> vertex_labels;
	unordered_map<uint64_t, idx_t> ordinal_by_id;
	unordered_map<uint32_t, vector<uint64_t>> vertex_ids_by_label;
	unordered_map<idx_t, vector<GqlCsrDeltaEdge>> outgoing;
	unordered_map<idx_t, vector<GqlCsrDeltaEdge>> incoming;
	idx_t edge_count = 0;
	uint64_t vertex_row_watermark = 0;
	uint64_t edge_row_watermark = 0;
};

struct GqlCsrSnapshot {
	GqlCsrCapabilities capabilities = 0;
	//! Optional numeric property projected as DOUBLE, aligned with adjacency.
	string weight_property;
	unordered_map<string, uint64_t> source_write_generations;
	vector<double> outgoing_weights;
	vector<double> incoming_weights;
	uint64_t graph_id;
	uint64_t graph_version;
	uint64_t write_generation;
	uint64_t vertex_write_generation;
	uint64_t edge_write_generation;
	//! Writes that inserts alone cannot explain, which a delta can't follow.
	uint64_t vertex_destructive_generation = 0;
	uint64_t edge_destructive_generation = 0;
	uint64_t schema_version = 0;
	//! Rows from these physical row IDs on were not in the build.
	uint64_t vertex_row_watermark = 0;
	uint64_t edge_row_watermark = 0;
	//! Whether inserts can be followed, decided at build. The next two change after
	//! publication and are only accessed under the storage lock.
	bool supports_delta = false;
	uint64_t synced_generation = 0;
	shared_ptr<const GqlCsrDelta> delta;
	//! Read from the database file rather than built in this process.
	bool loaded_from_disk = false;
	bool has_source_snapshot = false;
	uint64_t source_snapshot_id = 0;
	string source_catalog;
	string vertex_table_key;
	string edge_table_key;
	bool dense_vertex_ids = false;
	bool vertex_ids_match_rowids = false;
	bool has_vertex_row_ids = false;
	//! Native table identities captured with the versioned managed snapshot.
	string vertex_catalog_name, vertex_schema_name, vertex_table_name;
	string edge_catalog_name, edge_schema_name, edge_table_name;
	//! Physical row IDs by CSR ordinal, independent of parallel import order.
	//! Empty when canonical IDs already encode physical row IDs.
	GqlCsrOrdinals vertex_row_ids;
	//! The reverse, for the same snapshots, indexed by row ID. Gaps hold a stale ordinal, so
	//! a read checks it against vertex_row_ids.
	GqlCsrOrdinals vertex_ordinal_by_row_id;
	bool edge_ids_match_rowids = false;
	idx_t edge_count = 0;
	GqlCsrVertexIds vertex_ids;
	unordered_map<uint64_t, idx_t> ordinal_by_id;
	GqlCsrArray<idx_t> vertex_label_offsets;
	GqlCsrArray<uint32_t> vertex_label_ids;
	vector<uint64_t> vertex_label_posting_offsets;
	GqlCsrOrdinals vertex_label_postings;
	GqlCsrArray<uint64_t> outgoing_offsets;
	GqlCsrOrdinals outgoing_degrees;
	GqlCsrOrdinals outgoing_neighbors;
	GqlCsrOrdinals outgoing_edge_ids;
	GqlCsrEdgeLabels outgoing_label_ids;
	GqlCsrArray<uint64_t> incoming_offsets;
	GqlCsrOrdinals incoming_neighbors;
	GqlCsrOrdinals incoming_edge_ids;
	GqlCsrEdgeLabels incoming_label_ids;
	unordered_map<string, uint32_t> label_ids;
	vector<GqlCsrEdgeLabelStats> edge_label_stats;
	unordered_map<uint64_t, GqlCsrLabelFanoutStats> label_fanout_stats;
	bool has_label_fanout_stats = false;
	unique_ptr<GqlEndpointStatistics> endpoint_statistics;
	string endpoint_statistics_status = "not_requested";
	bool edge_labels_single = true;
	idx_t topology_bytes = 0;
	idx_t identity_bytes = 0;
	idx_t label_bytes = 0;
	idx_t auxiliary_bytes = 0;
	idx_t build_auxiliary_bytes = 0;
	idx_t memory_bytes = 0;

	bool HasVertexRowIds() const {
		return vertex_ids_match_rowids || has_vertex_row_ids;
	}

	//! Unpins what no query is reading, so the buffer manager can evict it.
	void ReleasePages() const {
		for (const auto *values : {&vertex_row_ids, &vertex_ordinal_by_row_id, &vertex_label_postings, &outgoing_degrees,
		                           &outgoing_neighbors, &outgoing_edge_ids, &incoming_neighbors, &incoming_edge_ids}) {
			values->ReleasePages();
		}
		vertex_ids.ReleasePages();
		vertex_label_offsets.ReleasePages();
		vertex_label_ids.ReleasePages();
		outgoing_offsets.ReleasePages();
		incoming_offsets.ReleasePages();
		outgoing_label_ids.ReleasePages();
		incoming_label_ids.ReleasePages();
	}

	void SaveArrays(GqlCsrWriter &writer) const {
		vertex_ids.Save(writer);
		vertex_row_ids.Save(writer);
		vertex_ordinal_by_row_id.Save(writer);
		vertex_label_offsets.Save(writer);
		vertex_label_ids.Save(writer);
		vertex_label_postings.Save(writer);
		outgoing_offsets.Save(writer);
		outgoing_neighbors.Save(writer);
		outgoing_edge_ids.Save(writer);
		outgoing_label_ids.Save(writer);
		incoming_offsets.Save(writer);
		incoming_neighbors.Save(writer);
		incoming_edge_ids.Save(writer);
		incoming_label_ids.Save(writer);
	}

	void LoadArrays(GqlCsrReader &reader) {
		vertex_ids.Load(reader);
		vertex_row_ids.Load(reader);
		vertex_ordinal_by_row_id.Load(reader);
		vertex_label_offsets.Load(reader);
		vertex_label_ids.Load(reader);
		vertex_label_postings.Load(reader);
		outgoing_offsets.Load(reader);
		outgoing_neighbors.Load(reader);
		outgoing_edge_ids.Load(reader);
		outgoing_label_ids.Load(reader);
		incoming_offsets.Load(reader);
		incoming_neighbors.Load(reader);
		incoming_edge_ids.Load(reader);
		incoming_label_ids.Load(reader);
	}

	//! Moves what was built in memory to pages. Later reads go through the const accessors.
	void Freeze(BufferManager &manager) {
		vertex_ids.Freeze(manager);
		vertex_label_offsets.Freeze(manager);
		vertex_label_ids.Freeze(manager);
		outgoing_offsets.Freeze(manager);
		incoming_offsets.Freeze(manager);
		outgoing_label_ids.Freeze(manager);
		incoming_label_ids.Freeze(manager);
	}
};

unique_ptr<GqlEndpointStatistics> GqlCollectEndpointStatistics(ClientContext &context, const GqlCsrSnapshot &snapshot,
                                                               string &status, idx_t &workspace_bytes);
bool GqlSummarizeEndpointStatistics(const GqlCsrSnapshot &snapshot, const vector<uint32_t> &source_labels,
                                    uint32_t edge_label, const vector<uint32_t> &target_labels,
                                    GqlEndpointSummary &result);

//! Bind data of in-out functions that can consume whole input chunks in a lateral
//! join. DuckDB drives projected inputs row by row; GqlBatchInOutFunctions replaces
//! that and has the function emit these child columns, in order, after its own.
struct GqlBatchedInOutBindData : TableFunctionData {
	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<GqlBatchedInOutBindData>();
		result->passthrough_input = passthrough_input;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		return passthrough_input == other_p.Cast<GqlBatchedInOutBindData>().passthrough_input;
	}

	vector<column_t> passthrough_input;
};

bool GqlTryGetCsrOrdinal(const GqlCsrSnapshot &snapshot, uint64_t vertex_id, idx_t &ordinal);
//! The ordinal of a physical row, which is how property indexes name vertices.
bool GqlTryGetCsrOrdinalByRowId(const GqlCsrSnapshot &snapshot, uint64_t row_id, idx_t &ordinal);

shared_ptr<const GqlCsrSnapshot> GqlGetCsrSnapshot(ClientContext &context, const string &graph_name);

//! A snapshot no write has followed, for consumers of what a delta doesn't extend.
shared_ptr<const GqlCsrSnapshot> GqlGetUnchangedCsrSnapshot(ClientContext &context, const string &graph_name);

//! Return a current capability-compatible snapshot, building it automatically
//! when the database instance has not prepared one yet. Weight properties have
//! separate cache entries; an empty property requests topology without weights.
shared_ptr<const GqlCsrSnapshot> GqlGetOrBuildCsrSnapshot(ClientContext &context, const string &graph_name,
                                                          GqlCsrCapabilities capabilities, bool *built = nullptr,
                                                          const string &weight_property = string());

//! Returns a current database-scoped snapshot when one is available and valid, whose
//! statistics may precede a delta. Unlike GqlGetCsrSnapshot, this is a non-throwing
//! optimizer probe.
shared_ptr<const GqlCsrSnapshot> GqlTryGetCsrSnapshot(ClientContext &context, const string &graph_name);

//! Register a lightweight observer that invalidates snapshots when a prepared
//! write executes.
void GqlRegisterCsrWriteObserver(ClientContext &context);

//! Advance the generation for a table when a write is planned and again at its commit.
void GqlNotifyCsrTableWritePlanned(ClientContext &context, const string &catalog_name, const string &schema_name,
                                   const string &table_name, bool inserts_only);

//! Conservatively invalidate snapshots when a prepared write executes.
void GqlNotifyCsrPreparedWriteExecution(ClientContext &context);

TableFunction GqlNeighborsFunction();
TableFunction GqlCsrVerticesFunction();
TableFunctionSet GqlCsrExpandFunctions();
TableFunction GqlVertexLookupFunction();
TableFunction GqlCsrShortestExpandFunction();
TableFunction GqlCsrAllShortestFunction();
//! Snapshots built in this database instance so far.
uint64_t GqlCsrBuildCount(ClientContext &context);
//! Whether expansion can test neighbor labels, which it does for its neighbor_label argument.
bool GqlCsrHasVertexLabels(const GqlCsrSnapshot &snapshot);
//! Vertices carrying a label in this snapshot.
idx_t GqlLabelPostingSize(const GqlCsrSnapshot &snapshot, const string &label);
//! Whether reading a label's posting list is cheaper than scanning the vertex table for it.
bool GqlLabelPostingBeatsScan(const GqlCsrSnapshot &snapshot, const string &label);
//! Whether filtering an expansion by the far endpoint's label removes enough rows.
bool GqlNeighborLabelIsSelective(const GqlCsrSnapshot &snapshot, const string &edge_label, const string &direction,
                                 const string &neighbor_label);
TableFunction GqlCsrPathExpandFunction();
TableFunction GqlVertexFetchFunction();
TableFunction GqlProjectedVertexFetchFunction();
TableFunction GqlEdgeFetchFunction();
TableFunction GqlBuildCsrFunction();
TableFunction GqlCsrStatsFunction();
TableFunction GqlCsrEdgeStatsFunction();
TableFunction GqlCsrEndpointStatsFunction();

} // namespace duckdb
