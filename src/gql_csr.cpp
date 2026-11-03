#include "gql_csr.hpp"

#include "gql_catalog.hpp"
#include "gql_optimizer.hpp"
#include "gql_sql_utils.hpp"
#include "gql_storage.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/execution_context.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/main/query_result_stream.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {

static unique_ptr<DataChunk> FetchCsrRows(QueryResultStream<> &stream) {
	auto chunk = stream.Fetch();
	if (stream.HasError()) {
		throw InvalidInputException("DuckGQL CSR query failed: %s", stream.GetError());
	}
	return chunk;
}

static constexpr const char *GQL_CSR_WRITE_OBSERVER_STATE_KEY = "gql_csr_write_observer_state";
static constexpr const char *GQL_CSR_GENERATION_CACHE_KEY = "gql_csr_generation_state";
static constexpr const char *GQL_DERIVED_GRAPH_STORAGE_CACHE_KEY = "gql_derived_graph_storage_state";

struct GqlCsrGenerationState : ObjectCacheEntry {
	static string ObjectType() {
		return "gql_csr_generation_state";
	}

	string GetObjectType() override {
		return ObjectType();
	}

	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx {};
	}

	mutex lock;
	uint64_t write_generation = 0;
	unordered_map<string, uint64_t> table_write_generations;
	//! The subset of those writes that may do more than insert rows.
	unordered_map<string, uint64_t> table_destructive_generations;
};

static string CsrTableKey(const string &catalog_name, const string &schema_name, const string &table_name) {
	return StringUtil::Lower(catalog_name) + "." + StringUtil::Lower(schema_name) + "." + StringUtil::Lower(table_name);
}

static shared_ptr<GqlCsrGenerationState> GetCsrGenerationState(ClientContext &context) {
	return ObjectCache::GetObjectCache(context).GetOrCreate<GqlCsrGenerationState>(GQL_CSR_GENERATION_CACHE_KEY);
}

static uint64_t ReadCsrWriteGeneration(ClientContext &context) {
	auto state = GetCsrGenerationState(context);
	lock_guard<mutex> guard(state->lock);
	return state->write_generation;
}

static void AdvanceCsrWriteGeneration(ClientContext &context) {
	auto state = GetCsrGenerationState(context);
	lock_guard<mutex> guard(state->lock);
	state->write_generation++;
}

static uint64_t ReadCsrTableWriteGeneration(ClientContext &context, const string &table_key) {
	auto state = GetCsrGenerationState(context);
	lock_guard<mutex> guard(state->lock);
	return state->table_write_generations[table_key];
}

static uint64_t ReadCsrTableDestructiveGeneration(ClientContext &context, const string &table_key) {
	auto state = GetCsrGenerationState(context);
	lock_guard<mutex> guard(state->lock);
	return state->table_destructive_generations[table_key];
}

static void AdvanceCsrTableWriteGeneration(ClientContext &context, const string &table_key, bool inserts_only) {
	auto state = GetCsrGenerationState(context);
	lock_guard<mutex> guard(state->lock);
	state->table_write_generations[table_key]++;
	if (!inserts_only) {
		state->table_destructive_generations[table_key]++;
	}
}

// Generations also advance after the commit. Otherwise a snapshot built between the plan
// and the commit would record the new generation and read the old rows.
struct GqlCsrWriteObserverState : ClientContextState {
	void TransactionCommit(MetaTransaction &transaction, ClientContext &context) override {
		for (const auto &table : planned_table_writes) {
			AdvanceCsrTableWriteGeneration(context, table.first, table.second);
		}
		if (planned_prepared_write) {
			AdvanceCsrWriteGeneration(context);
		}
		ClearPlannedWrites();
		if (transaction.ModifiedDatabase()) {
			GqlInvalidateTableGraphCache(context);
		}
	}

	void TransactionRollback(MetaTransaction &, ClientContext &) override {
		ClearPlannedWrites();
	}

	RebindQueryInfo OnRebindPreparedStatement(ClientContext &context, BindPreparedStatementCallbackInfo &info,
	                                          RebindQueryInfo current_rebind) override {
		if (!info.prepared_statement.properties.IsReadOnly()) {
			AdvanceCsrWriteGeneration(context);
			planned_prepared_write = true;
			GqlInvalidateTableGraphCache(context);
		}
		return current_rebind;
	}

	void ClearPlannedWrites() {
		planned_table_writes.clear();
		planned_prepared_write = false;
	}

	//! Table to whether its writes in this transaction only inserted.
	unordered_map<string, bool> planned_table_writes;
	bool planned_prepared_write = false;
};

void GqlRegisterCsrWriteObserver(ClientContext &context) {
	context.registered_state->GetOrCreate<GqlCsrWriteObserverState>(GQL_CSR_WRITE_OBSERVER_STATE_KEY);
}

void GqlNotifyCsrTableWritePlanned(ClientContext &context, const string &catalog_name, const string &schema_name,
                                   const string &table_name, bool inserts_only) {
	auto observer = context.registered_state->GetOrCreate<GqlCsrWriteObserverState>(GQL_CSR_WRITE_OBSERVER_STATE_KEY);
	auto key = CsrTableKey(catalog_name, schema_name, table_name);
	AdvanceCsrTableWriteGeneration(context, key, inserts_only);
	auto planned = observer->planned_table_writes.emplace(key, inserts_only);
	planned.first->second = planned.first->second && inserts_only;
}

void GqlNotifyCsrPreparedWriteExecution(ClientContext &context) {
	auto observer = context.registered_state->GetOrCreate<GqlCsrWriteObserverState>(GQL_CSR_WRITE_OBSERVER_STATE_KEY);
	AdvanceCsrWriteGeneration(context);
	observer->planned_prepared_write = true;
}

static string QualifiedTable(const GqlElementTableBinding &table) {
	if (!table.relation_sql.empty()) {
		return "(" + table.relation_sql + ")";
	}
	return GqlQuoteIdentifier(table.catalog_name) + "." + GqlQuoteIdentifier(table.schema_name) + "." +
	       GqlQuoteIdentifier(table.table_name);
}

// Database-instance owner for immutable graph projections. The canonical graph
// remains in DuckDB tables; this manager publishes versioned derived layouts
// that every connection in the same DatabaseInstance can lease and reuse.
struct GqlDerivedGraphStorageState : ObjectCacheEntry {
	static string ObjectType() {
		return "gql_derived_graph_storage_state";
	}

	string GetObjectType() override {
		return ObjectType();
	}

	optional_idx GetEstimatedCacheMemory() const override {
		// The manager is intentionally non-evictable in this first slice. It
		// eagerly removes stale and dominated projections below; bounded,
		// reservation-backed LRU eviction is the next storage slice.
		return optional_idx {};
	}

	mutex lock;
	//! Held across a delta catch-up's queries, so readers don't repeat each other's scan.
	mutex delta_catch_up_lock;
	std::condition_variable build_finished;
	unordered_map<uint64_t, vector<shared_ptr<GqlCsrSnapshot>>> snapshots;
	unordered_set<uint64_t> builds_in_progress;
	uint64_t build_count = 0;
	atomic<uint64_t> acquisition_count {0};
};

static shared_ptr<GqlDerivedGraphStorageState> GetDerivedGraphStorageState(ClientContext &context) {
	return ObjectCache::GetObjectCache(context).GetOrCreate<GqlDerivedGraphStorageState>(
	    GQL_DERIVED_GRAPH_STORAGE_CACHE_KEY);
}

struct GraphVersion {
	uint64_t graph_id = 0;
	uint64_t graph_version = 0;
	bool has_source_snapshot = false;
	uint64_t source_snapshot_id = 0;
	string source_catalog;
};

static uint64_t ReadDuckLakeSnapshot(Connection &connection, const string &catalog) {
	auto result =
	    GqlQuery(connection, "SELECT id::UBIGINT FROM " + GqlQuoteIdentifier(catalog) + ".current_snapshot()");
	if (result->RowCount() != 1 || result->GetValue(0, 0).IsNull()) {
		throw InvalidInputException("DuckLake catalog '%s' did not expose a current snapshot", catalog);
	}
	return result->GetValue(0, 0).GetValue<uint64_t>();
}

static GraphVersion ReadGraphVersion(Connection &connection, const string &graph_name) {
	auto result = GqlQuery(connection, "SELECT g.graph_id, g.graph_version, s.source_kind, s.source_catalog, "
	                                   "s.snapshot_policy, s.pinned_snapshot_id FROM gql_internal.graphs g LEFT JOIN "
	                                   "gql_internal.graph_sources s USING (graph_id) WHERE g.graph_name = " +
	                                       GqlQuoteLiteral(graph_name));
	if (result->RowCount() == 0) {
		throw InvalidInputException("Graph '%s' does not exist", graph_name);
	}
	GraphVersion version;
	version.graph_id = result->GetValue(0, 0).GetValue<uint64_t>();
	version.graph_version = result->GetValue(1, 0).GetValue<uint64_t>();
	if (!result->GetValue(2, 0).IsNull() &&
	    StringUtil::CIEquals(result->GetValue(2, 0).GetValue<string>(), "DUCKLAKE")) {
		if (result->GetValue(3, 0).IsNull() || result->GetValue(4, 0).IsNull()) {
			throw InvalidInputException("DuckLake graph '%s' has incomplete snapshot metadata", graph_name);
		}
		version.source_catalog = result->GetValue(3, 0).GetValue<string>();
		auto policy = result->GetValue(4, 0).GetValue<string>();
		auto observed_snapshot = ReadDuckLakeSnapshot(connection, version.source_catalog);
		if (StringUtil::CIEquals(policy, "LIVE")) {
			version.source_snapshot_id = observed_snapshot;
		} else if (StringUtil::CIEquals(policy, "PINNED") && !result->GetValue(5, 0).IsNull()) {
			version.source_snapshot_id = result->GetValue(5, 0).GetValue<uint64_t>();
			if (observed_snapshot != version.source_snapshot_id) {
				throw InvalidInputException(
				    "Pinned graph '%s' requires DuckLake catalog '%s' attached with SNAPSHOT_VERSION %llu; "
				    "observed snapshot %llu",
				    graph_name, version.source_catalog, version.source_snapshot_id, observed_snapshot);
			}
		} else {
			throw InvalidInputException("DuckLake graph '%s' has invalid snapshot policy metadata", graph_name);
		}
		version.has_source_snapshot = true;
	}
	return version;
}

static bool SameGraphVersion(const GraphVersion &left, const GraphVersion &right) {
	return left.graph_id == right.graph_id && left.graph_version == right.graph_version &&
	       left.has_source_snapshot == right.has_source_snapshot &&
	       (!left.has_source_snapshot || (left.source_snapshot_id == right.source_snapshot_id &&
	                                      StringUtil::CIEquals(left.source_catalog, right.source_catalog)));
}

static void FinalizeOffsets(vector<uint64_t> &offsets) {
	for (idx_t index = 1; index < offsets.size(); index++) {
		offsets[index] += offsets[index - 1];
	}
}

bool GqlTryGetCsrOrdinal(const GqlCsrSnapshot &snapshot, uint64_t vertex_id, idx_t &ordinal) {
	if (snapshot.dense_vertex_ids) {
		if (vertex_id == 0 || vertex_id > snapshot.vertex_ids.size()) {
			return false;
		}
		ordinal = NumericCast<idx_t>(vertex_id - 1);
		return true;
	}
	auto entry = snapshot.ordinal_by_id.find(vertex_id);
	if (entry == snapshot.ordinal_by_id.end()) {
		return false;
	}
	ordinal = entry->second;
	return true;
}

bool GqlTryGetCsrOrdinalByRowId(const GqlCsrSnapshot &snapshot, uint64_t row_id, idx_t &ordinal) {
	if (snapshot.vertex_ids_match_rowids) {
		return GqlTryGetCsrOrdinal(snapshot, row_id + 1, ordinal);
	}
	auto entry = snapshot.vertex_ordinal_by_row_id.find(row_id);
	if (entry == snapshot.vertex_ordinal_by_row_id.end()) {
		return false;
	}
	ordinal = entry->second;
	return true;
}

static bool CsrTryGetOrdinal(const GqlCsrSnapshot &snapshot, const GqlCsrDelta *delta, uint64_t vertex_id,
                             idx_t &ordinal) {
	if (GqlTryGetCsrOrdinal(snapshot, vertex_id, ordinal)) {
		return true;
	}
	if (!delta) {
		return false;
	}
	auto entry = delta->ordinal_by_id.find(vertex_id);
	if (entry == delta->ordinal_by_id.end()) {
		return false;
	}
	ordinal = entry->second;
	return true;
}

static uint64_t CsrVertexId(const GqlCsrSnapshot &snapshot, const GqlCsrDelta *delta, idx_t ordinal) {
	const auto base_vertices = snapshot.vertex_ids.size();
	return ordinal < base_vertices ? snapshot.vertex_ids[ordinal] : delta->vertex_ids[ordinal - base_vertices];
}

static uint64_t CsrVertexRowId(const GqlCsrSnapshot &snapshot, const GqlCsrDelta *delta, idx_t ordinal) {
	const auto base_vertices = snapshot.vertex_ids.size();
	if (ordinal >= base_vertices) {
		return delta->vertex_row_ids[ordinal - base_vertices];
	}
	return snapshot.vertex_ids_match_rowids ? snapshot.vertex_ids[ordinal] - 1 : snapshot.vertex_row_ids[ordinal];
}

static const vector<GqlCsrDeltaEdge> *CsrDeltaEdges(const GqlCsrDelta *delta, idx_t ordinal, bool outgoing) {
	if (!delta) {
		return nullptr;
	}
	const auto &edges = outgoing ? delta->outgoing : delta->incoming;
	if (edges.empty()) {
		return nullptr;
	}
	auto entry = edges.find(ordinal);
	return entry == edges.end() ? nullptr : &entry->second;
}

static bool CsrVertexHasLabels(const GqlCsrSnapshot &snapshot, const GqlCsrDelta *delta, idx_t ordinal,
                               const vector<uint32_t> &required) {
	const auto base_vertices = snapshot.vertex_ids.size();
	const uint32_t *begin;
	const uint32_t *end;
	if (ordinal < base_vertices) {
		begin = snapshot.vertex_label_ids.data() + snapshot.vertex_label_offsets[ordinal];
		end = snapshot.vertex_label_ids.data() + snapshot.vertex_label_offsets[ordinal + 1];
	} else {
		const auto &labels = delta->vertex_labels[ordinal - base_vertices];
		begin = labels.data();
		end = labels.data() + labels.size();
	}
	for (const auto label : required) {
		if (std::find(begin, end, label) == end) {
			return false;
		}
	}
	return true;
}

// Calls function(neighbor, edge_id) for each edge of one label, in the CSR and then the delta.
template <class FUNCTION>
static void CsrForEachEdge(const GqlCsrSnapshot &snapshot, const GqlCsrDelta *delta, idx_t vertex, bool outgoing,
                           uint32_t label_id, FUNCTION &&function) {
	if (vertex < snapshot.vertex_ids.size()) {
		const auto &offsets = outgoing ? snapshot.outgoing_offsets : snapshot.incoming_offsets;
		const auto &neighbors = outgoing ? snapshot.outgoing_neighbors : snapshot.incoming_neighbors;
		const auto &labels = outgoing ? snapshot.outgoing_label_ids : snapshot.incoming_label_ids;
		const auto &edge_ids = outgoing ? snapshot.outgoing_edge_ids : snapshot.incoming_edge_ids;
		for (auto index = offsets[vertex]; index < offsets[vertex + 1]; index++) {
			if (labels[index] == label_id) {
				function(neighbors[index], edge_ids[index]);
			}
		}
	}
	if (const auto *edges = CsrDeltaEdges(delta, vertex, outgoing)) {
		for (const auto &edge : *edges) {
			if (edge.label_id == label_id) {
				function(edge.neighbor, edge.edge_id);
			}
		}
	}
}

static uint32_t CsrLabelId(GqlCsrSnapshot &snapshot, const string &label) {
	if (label.empty()) {
		return 0;
	}
	auto normalized = StringUtil::Lower(label);
	auto entry = snapshot.label_ids.find(normalized);
	if (entry != snapshot.label_ids.end()) {
		return entry->second;
	}
	auto identifier = NumericCast<uint32_t>(snapshot.label_ids.size() + 1);
	snapshot.label_ids.emplace(std::move(normalized), identifier);
	return identifier;
}

template <class HASH_CONTAINER>
static idx_t HashContainerStorageBytes(const HASH_CONTAINER &container) {
	// std::unordered_* does not expose node allocation sizes. Count its bucket
	// array plus one value and two link-sized words per node as a stable,
	// allocator-independent estimate.
	return container.bucket_count() * sizeof(void *) +
	       container.size() * (sizeof(typename HASH_CONTAINER::value_type) + 2 * sizeof(void *));
}

static idx_t LabelDictionaryStorageBytes(const unordered_map<string, uint32_t> &labels) {
	idx_t bytes = HashContainerStorageBytes(labels);
	for (const auto &entry : labels) {
		bytes += entry.first.capacity() + 1;
	}
	return bytes;
}

static bool CsrHasCapabilities(GqlCsrCapabilities available, GqlCsrCapabilities required) {
	// A materialized outgoing CSR already contains the exact out-degree in its
	// offset differences, so it can satisfy an algorithm asking only for the
	// compact degree sidecar.
	if (available & GQL_CSR_OUTGOING) {
		required &= ~GQL_CSR_OUT_DEGREES;
	}
	return (available & required) == required;
}

static GqlCsrCapabilities NormalizeCsrCapabilities(GqlCsrCapabilities capabilities) {
	if (capabilities & GQL_CSR_ENDPOINT_STATS)
		capabilities |= GQL_CSR_FULL;
	if (capabilities & GQL_CSR_VERTEX_LABEL_POSTINGS) {
		capabilities |= GQL_CSR_VERTEX_LABELS;
	}
	if (capabilities & GQL_CSR_EDGE_STATS) {
		capabilities |= GQL_CSR_OUTGOING | GQL_CSR_INCOMING | GQL_CSR_EDGE_LABELS;
	}
	if ((capabilities & (GQL_CSR_OUTGOING | GQL_CSR_INCOMING)) == 0) {
		throw InternalException("GQL CSR snapshot requires outgoing or incoming topology");
	}
	return capabilities;
}

static string CsrVertexLabelProjection(const GqlElementTableBinding &vertex) {
	if (!vertex.static_labels.empty()) {
		string projection = "[";
		for (idx_t index = 0; index < vertex.static_labels.size(); index++) {
			if (index > 0) {
				projection += ", ";
			}
			projection += GqlQuoteLiteral(vertex.static_labels[index]);
		}
		return projection + "]::VARCHAR[]";
	}
	if (!vertex.label_column.empty()) {
		return vertex.label_is_list ? GqlQuoteIdentifier(vertex.label_column)
		                            : "string_split(CAST(" + GqlQuoteIdentifier(vertex.label_column) + " AS VARCHAR), ';')";
	}
	return "CAST(NULL AS VARCHAR[])";
}

static string CsrEdgeLabelProjection(const GqlElementTableBinding &edge) {
	if (!edge.static_labels.empty()) {
		return GqlQuoteLiteral(edge.static_labels[0]);
	}
	if (!edge.label_column.empty()) {
		return "CAST(" + GqlQuoteIdentifier(edge.label_column) + " AS VARCHAR)";
	}
	return "CAST(NULL AS VARCHAR)";
}

static uint64_t ReadCsrSchemaVersion(Connection &connection, uint64_t graph_id) {
	auto result = GqlQuery(connection, "SELECT schema_version FROM gql_internal.graph_storage WHERE graph_id = " +
	                                       to_string(graph_id));
	return result->RowCount() == 1 ? result->GetValue(0, 0).GetValue<uint64_t>() : 0;
}

static uint64_t ReadCsrRowWatermark(Connection &connection, const string &table) {
	return GqlQuery(connection, "SELECT coalesce(max(rowid) + 1, 0)::UBIGINT FROM " + table)
	    ->GetValue(0, 0)
	    .GetValue<uint64_t>();
}

static shared_ptr<GqlCsrSnapshot> BuildTableSnapshot(ClientContext &context, const string &graph_name,
                                                     GqlCsrCapabilities requested_capabilities,
                                                     const string &weight_property = string()) {
	auto capabilities = NormalizeCsrCapabilities(requested_capabilities);
	const bool build_outgoing = capabilities & GQL_CSR_OUTGOING;
	const bool build_incoming = capabilities & GQL_CSR_INCOMING;
	const bool build_edge_ids = capabilities & GQL_CSR_EDGE_IDS;
	const bool build_edge_labels = capabilities & GQL_CSR_EDGE_LABELS;
	const bool build_vertex_labels = capabilities & GQL_CSR_VERTEX_LABELS;
	const bool build_vertex_label_postings = capabilities & GQL_CSR_VERTEX_LABEL_POSTINGS;
	const bool build_edge_stats = capabilities & GQL_CSR_EDGE_STATS;
	const bool build_out_degrees = capabilities & GQL_CSR_OUT_DEGREES;
	GqlTableGraphBinding binding;
	if (!GqlTryLoadTableGraph(context, graph_name, binding)) {
		throw InvalidInputException("CSR algorithms require a table-backed graph; "
		                            "load graph '%s' with COPY GRAPH first",
		                            graph_name);
	}
	if (binding.cached_reference_ids) {
		binding.vertex.relation_sql = binding.vertex.referenced_fetch_sql;
	}
	Connection connection(*context.db);
	auto snapshot = make_shared_ptr<GqlCsrSnapshot>();
	snapshot->capabilities = capabilities;
	snapshot->weight_property = StringUtil::Lower(weight_property);
	snapshot->write_generation = ReadCsrWriteGeneration(context);
	snapshot->vertex_table_key =
	    CsrTableKey(binding.vertex.catalog_name, binding.vertex.schema_name, binding.vertex.table_name);
	if (StringUtil::CIEquals(binding.vertex.ownership, "MANAGED")) {
		snapshot->vertex_catalog_name = binding.vertex.catalog_name;
		snapshot->vertex_schema_name = binding.vertex.schema_name;
		snapshot->vertex_table_name = binding.vertex.table_name;
	}
	if (StringUtil::CIEquals(binding.edge.ownership, "MANAGED")) {
		snapshot->edge_catalog_name = binding.edge.catalog_name;
		snapshot->edge_schema_name = binding.edge.schema_name;
		snapshot->edge_table_name = binding.edge.table_name;
	}
	snapshot->edge_table_key =
	    CsrTableKey(binding.edge.catalog_name, binding.edge.schema_name, binding.edge.table_name);
	snapshot->vertex_write_generation = ReadCsrTableWriteGeneration(context, snapshot->vertex_table_key);
	snapshot->edge_write_generation = ReadCsrTableWriteGeneration(context, snapshot->edge_table_key);
	snapshot->vertex_destructive_generation = ReadCsrTableDestructiveGeneration(context, snapshot->vertex_table_key);
	snapshot->edge_destructive_generation = ReadCsrTableDestructiveGeneration(context, snapshot->edge_table_key);
	snapshot->synced_generation = GqlDatabaseWriteGeneration(context);
	// Referenced unions depend on every physical node and edge table, rather
	// than the synthetic union name used by the relational binding.
	for (const auto *table : {&binding.vertex, &binding.edge}) {
		for (const auto &source : table->source_tables) {
			auto key = CsrTableKey(source.catalog_name, source.schema_name, source.table_name);
			snapshot->source_write_generations.emplace(key, ReadCsrTableWriteGeneration(context, key));
		}
	}

	connection.BeginTransaction();
	auto graph = ReadGraphVersion(connection, graph_name);
	if (binding.cached_reference_ids && graph.source_snapshot_id != binding.source_snapshot_id) {
		throw InvalidInputException("Graph '%s' changed before its CSR source snapshot was read; retry the operation",
		                            graph_name);
	}
	snapshot->graph_id = graph.graph_id;
	snapshot->graph_version = graph.graph_version;
	snapshot->has_source_snapshot = graph.has_source_snapshot;
	snapshot->source_snapshot_id = graph.source_snapshot_id;
	snapshot->source_catalog = graph.source_catalog;
	snapshot->supports_delta = (capabilities & GQL_CSR_FULL) == GQL_CSR_FULL && weight_property.empty() &&
	                           !snapshot->vertex_table_name.empty() && !snapshot->edge_table_name.empty() &&
	                           !graph.has_source_snapshot && binding.vertex.source_tables.empty() &&
	                           binding.edge.source_tables.empty();
	if (snapshot->supports_delta) {
		snapshot->schema_version = ReadCsrSchemaVersion(connection, graph.graph_id);
		snapshot->vertex_row_watermark = ReadCsrRowWatermark(connection, QualifiedTable(binding.vertex));
		snapshot->edge_row_watermark = ReadCsrRowWatermark(connection, QualifiedTable(binding.edge));
	}

	const bool managed_vertices = StringUtil::CIEquals(binding.vertex.ownership, "MANAGED");
	const bool build_vertex_row_ids = managed_vertices && (capabilities & GQL_CSR_VERTEX_ROW_IDS);
	auto vertex_count = GqlQuery(connection, "SELECT count(*)::UBIGINT" +
	                                            string(build_vertex_row_ids ? ", coalesce(max(rowid), 0)::UBIGINT" : "") +
	                                            " FROM " + QualifiedTable(binding.vertex));
	snapshot->vertex_ids.reserve(NumericCast<idx_t>(vertex_count->GetValue(0, 0).GetValue<uint64_t>()));
	if (build_vertex_row_ids) {
		snapshot->has_vertex_row_ids = true;
		snapshot->vertex_row_ids.Resize(NumericCast<idx_t>(vertex_count->GetValue(0, 0).GetValue<uint64_t>()),
		                                vertex_count->GetValue(1, 0).GetValue<uint64_t>() <=
		                                    std::numeric_limits<uint32_t>::max());
	}
	auto vertex_label_projection =
	    build_vertex_labels ? CsrVertexLabelProjection(binding.vertex) : string("CAST(NULL AS VARCHAR[])");
	auto vertex_rowid_projection =
	    StringUtil::CIEquals(binding.vertex.ownership, "MANAGED") ? "CAST(rowid AS UBIGINT)" : "CAST(0 AS UBIGINT)";
	auto vertices = make_uniq<QueryResultStream<>>(
	    connection.Submit("SELECT CAST(" + GqlQuoteIdentifier(binding.vertex.key_column) +
	                      " AS UBIGINT) AS vertex_id, " + vertex_label_projection + ", " + vertex_rowid_projection +
	                      " AS physical_rowid FROM " + QualifiedTable(binding.vertex) + " ORDER BY vertex_id"));

	snapshot->vertex_ids_match_rowids = StringUtil::CIEquals(binding.vertex.ownership, "MANAGED");
	if (build_vertex_labels) {
		snapshot->vertex_label_offsets.push_back(0);
	}
	while (auto chunk = FetchCsrRows(*vertices)) {
		UnifiedVectorFormat vertex_data;
		UnifiedVectorFormat label_data;
		UnifiedVectorFormat label_child_data;
		UnifiedVectorFormat row_id_data;
		chunk->data[0].ToUnifiedFormat(chunk->size(), vertex_data);
		chunk->data[1].ToUnifiedFormat(chunk->size(), label_data);
		chunk->data[2].ToUnifiedFormat(chunk->size(), row_id_data);
		auto &label_child = ListVector::GetEntry(chunk->data[1]);
		label_child.ToUnifiedFormat(ListVector::GetListSize(chunk->data[1]), label_child_data);
		auto ids = UnifiedVectorFormat::GetData<uint64_t>(vertex_data);
		auto label_lists = UnifiedVectorFormat::GetData<list_entry_t>(label_data);
		auto labels = UnifiedVectorFormat::GetData<string_t>(label_child_data);
		auto row_ids = UnifiedVectorFormat::GetData<uint64_t>(row_id_data);
		for (idx_t row = 0; row < chunk->size(); row++) {
			auto index = vertex_data.sel->get_index(row);
			auto row_id_index = row_id_data.sel->get_index(row);
			if (!vertex_data.validity.RowIsValid(index) || !row_id_data.validity.RowIsValid(row_id_index)) {
				throw InvalidInputException("Table-backed CSR vertex keys must not contain NULL values");
			}
			auto vertex_id = ids[index];
			if (build_vertex_row_ids) {
				snapshot->vertex_row_ids.Set(snapshot->vertex_ids.size(), row_ids[row_id_index]);
			}
			snapshot->vertex_ids_match_rowids =
			    snapshot->vertex_ids_match_rowids && vertex_id > 0 && row_ids[row_id_index] == vertex_id - 1;
			if (!snapshot->vertex_ids.empty() && snapshot->vertex_ids.back() == vertex_id) {
				throw InvalidInputException("Table-backed CSR requires unique vertex keys; duplicate key %llu",
				                            static_cast<unsigned long long>(vertex_id));
			}
			snapshot->vertex_ids.push_back(vertex_id);
			auto label_index = label_data.sel->get_index(row);
			if (build_vertex_labels && label_data.validity.RowIsValid(label_index)) {
				auto label_list = label_lists[label_index];
				auto label_start = snapshot->vertex_label_ids.size();
				for (idx_t offset = 0; offset < label_list.length; offset++) {
					auto child_index = label_child_data.sel->get_index(label_list.offset + offset);
					if (label_child_data.validity.RowIsValid(child_index)) {
						auto label = labels[child_index].GetString();
						if (!label.empty()) {
							auto label_id = CsrLabelId(*snapshot, label);
							if (std::find(snapshot->vertex_label_ids.begin() + label_start,
							              snapshot->vertex_label_ids.end(),
							              label_id) == snapshot->vertex_label_ids.end()) {
								snapshot->vertex_label_ids.push_back(label_id);
							}
						}
					}
				}
			}
			if (build_vertex_labels) {
				snapshot->vertex_label_offsets.push_back(snapshot->vertex_label_ids.size());
			}
		}
	}
	if (snapshot->vertex_ids_match_rowids) {
		// Preserve the zero-allocation shortcut for ordered managed tables.
		snapshot->vertex_row_ids = GqlCsrOrdinals();
	}
	if (build_vertex_row_ids && !snapshot->vertex_ids_match_rowids) {
		snapshot->vertex_ordinal_by_row_id.reserve(snapshot->vertex_ids.size());
		for (idx_t ordinal = 0; ordinal < snapshot->vertex_ids.size(); ordinal++) {
			snapshot->vertex_ordinal_by_row_id.emplace(snapshot->vertex_row_ids[ordinal], ordinal);
		}
	}
	snapshot->dense_vertex_ids = true;
	for (idx_t ordinal = 0; ordinal < snapshot->vertex_ids.size(); ordinal++) {
		if (snapshot->vertex_ids[ordinal] != ordinal + 1) {
			snapshot->dense_vertex_ids = false;
			break;
		}
	}
	idx_t transient_vertex_id_bytes = 0;
	if (!snapshot->dense_vertex_ids) {
		snapshot->ordinal_by_id.reserve(snapshot->vertex_ids.size());
		for (idx_t ordinal = 0; ordinal < snapshot->vertex_ids.size(); ordinal++) {
			snapshot->ordinal_by_id.emplace(snapshot->vertex_ids[ordinal], ordinal);
		}
	} else {
		transient_vertex_id_bytes = snapshot->vertex_ids.AllocatedBytes();
		snapshot->vertex_ids.MakeImplicitDense();
	}

	const bool compact_vertex_ordinals =
	    snapshot->vertex_ids.empty() || snapshot->vertex_ids.size() - 1 <= std::numeric_limits<uint32_t>::max();
	if (build_vertex_label_postings) {
		// Invert per-vertex labels only for optimizer/frontier consumers. Pure
		// algorithms filter directly over the compact per-vertex label arrays.
		const auto vertex_label_count = snapshot->label_ids.size();
		snapshot->vertex_label_posting_offsets.assign(vertex_label_count + 2, 0);
		for (const auto label_id : snapshot->vertex_label_ids) {
			snapshot->vertex_label_posting_offsets[label_id + 1]++;
		}
		FinalizeOffsets(snapshot->vertex_label_posting_offsets);
		snapshot->vertex_label_postings.Resize(snapshot->vertex_label_ids.size(), compact_vertex_ordinals);
		auto vertex_label_cursor = snapshot->vertex_label_posting_offsets;
		for (idx_t vertex = 0; vertex < snapshot->vertex_ids.size(); vertex++) {
			for (idx_t offset = snapshot->vertex_label_offsets[vertex];
			     offset < snapshot->vertex_label_offsets[vertex + 1]; offset++) {
				auto label_id = snapshot->vertex_label_ids[offset];
				snapshot->vertex_label_postings.Set(vertex_label_cursor[label_id]++, vertex);
			}
		}
	}

	auto label_projection = build_edge_labels ? CsrEdgeLabelProjection(binding.edge) : string("CAST(NULL AS VARCHAR)");
	auto edge_count = GqlQuery(connection, "SELECT count(*)::UBIGINT FROM " + QualifiedTable(binding.edge));
	auto expected_edges = NumericCast<idx_t>(edge_count->GetValue(0, 0).GetValue<uint64_t>());
	snapshot->edge_count = expected_edges;
	auto edge_key = "CAST(" + GqlQuoteIdentifier(binding.edge.key_column) + " AS UBIGINT)";
	auto edge_source = "CAST(" + GqlQuoteIdentifier(binding.edge_source_column) + " AS UBIGINT)";
	auto edge_target = "CAST(" + GqlQuoteIdentifier(binding.edge_target_column) + " AS UBIGINT)";
	auto edge_table = QualifiedTable(binding.edge);
	auto projected_edge_key = build_edge_ids ? edge_key : "CAST(0 AS UBIGINT)";
	auto projected_row_id = build_edge_ids && StringUtil::CIEquals(binding.edge.ownership, "MANAGED")
	                            ? "CAST(rowid AS UBIGINT)"
	                            : "CAST(0 AS UBIGINT)";
	auto endpoint_projection = "SELECT " + projected_edge_key + ", " + edge_source + ", " + edge_target + ", " +
	                           projected_row_id + " FROM " + edge_table;
	string weight_projection;
	if (!weight_property.empty()) {
		auto property = binding.edge.property_columns.find(StringUtil::Lower(weight_property));
		if (property == binding.edge.property_columns.end()) {
			throw BinderException("GQL edge weight property '%s' is not mapped", weight_property);
		}
		auto column = GqlQuoteIdentifier(property->second);
		auto type_result = GqlQuery(connection, "SELECT " + column + " FROM " + edge_table + " LIMIT 0");
		if (!type_result->GetTypes()[0].IsNumeric()) {
			throw BinderException("GQL edge weight property '%s' must have a numeric type", weight_property);
		}
		weight_projection = ", CAST(" + column + " AS DOUBLE)";
	}
	auto edge_projection = "SELECT " + projected_edge_key + ", " + edge_source + ", " + edge_target + ", " +
	                       label_projection + weight_projection + " FROM " + edge_table;
	// COPY GRAPH owns these tables and generates monotonically unique IDs. Keep
	// duplicate validation for any future non-managed/table-attachment path,
	// but do not build an O(E) hash set for the managed fast path.
	auto validate_edge_ids =
	    build_edge_ids && (binding.edge.schema_name != "gql_data" || binding.edge.key_column != "__gql_edge_id");
	unordered_set<uint64_t> edge_ids;
	if (validate_edge_ids) {
		edge_ids.reserve(expected_edges);
	}
	if (build_outgoing) {
		snapshot->outgoing_offsets.assign(snapshot->vertex_ids.size() + 1, 0);
	}
	if (build_incoming) {
		snapshot->incoming_offsets.assign(snapshot->vertex_ids.size() + 1, 0);
	}
	if (build_out_degrees) {
		snapshot->outgoing_degrees.Resize(snapshot->vertex_ids.size(),
		                                  expected_edges <= std::numeric_limits<uint32_t>::max());
	}
	auto degree_rows = make_uniq<QueryResultStream<>>(connection.Submit(endpoint_projection));

	idx_t counted_edges = 0;
	snapshot->edge_ids_match_rowids = build_edge_ids && StringUtil::CIEquals(binding.edge.ownership, "MANAGED");
	while (auto chunk = FetchCsrRows(*degree_rows)) {
		UnifiedVectorFormat edge_id_data;
		UnifiedVectorFormat source_data;
		UnifiedVectorFormat target_data;
		UnifiedVectorFormat row_id_data;
		chunk->data[0].ToUnifiedFormat(chunk->size(), edge_id_data);
		chunk->data[1].ToUnifiedFormat(chunk->size(), source_data);
		chunk->data[2].ToUnifiedFormat(chunk->size(), target_data);
		chunk->data[3].ToUnifiedFormat(chunk->size(), row_id_data);
		auto edge_id_values = UnifiedVectorFormat::GetData<uint64_t>(edge_id_data);
		auto source_values = UnifiedVectorFormat::GetData<uint64_t>(source_data);
		auto target_values = UnifiedVectorFormat::GetData<uint64_t>(target_data);
		auto row_id_values = UnifiedVectorFormat::GetData<uint64_t>(row_id_data);
		for (idx_t row = 0; row < chunk->size(); row++) {
			auto edge_index = edge_id_data.sel->get_index(row);
			auto source_index = source_data.sel->get_index(row);
			auto target_index = target_data.sel->get_index(row);
			auto row_id_index = row_id_data.sel->get_index(row);
			if ((build_edge_ids &&
			     (!edge_id_data.validity.RowIsValid(edge_index) || !row_id_data.validity.RowIsValid(row_id_index))) ||
			    !source_data.validity.RowIsValid(source_index) || !target_data.validity.RowIsValid(target_index)) {
				throw InvalidInputException("Table-backed CSR edge keys and endpoints "
				                            "must not contain NULL values");
			}
			auto edge_id = build_edge_ids ? edge_id_values[edge_index] : NumericCast<uint64_t>(counted_edges + 1);
			if (build_edge_ids) {
				snapshot->edge_ids_match_rowids =
				    snapshot->edge_ids_match_rowids && edge_id > 0 && row_id_values[row_id_index] == edge_id - 1;
			}
			idx_t source;
			idx_t target;
			if (!GqlTryGetCsrOrdinal(*snapshot, source_values[source_index], source) ||
			    !GqlTryGetCsrOrdinal(*snapshot, target_values[target_index], target)) {
				throw InvalidInputException("Graph contains edge %llu with an invalid endpoint",
				                            static_cast<unsigned long long>(edge_id));
			}
			if (build_outgoing) {
				snapshot->outgoing_offsets[source + 1]++;
			}
			if (build_incoming) {
				snapshot->incoming_offsets[target + 1]++;
			}
			if (build_out_degrees) {
				snapshot->outgoing_degrees.Set(source, snapshot->outgoing_degrees[source] + 1);
			}
			counted_edges++;
		}
	}
	if (counted_edges != expected_edges) {
		throw InternalException("GQL CSR edge scan returned an unexpected row count");
	}
	degree_rows.reset();

	if (build_outgoing) {
		FinalizeOffsets(snapshot->outgoing_offsets);
	}
	if (build_incoming) {
		FinalizeOffsets(snapshot->incoming_offsets);
	}
	const bool compact_neighbors =
	    snapshot->vertex_ids.empty() || snapshot->vertex_ids.size() - 1 <= std::numeric_limits<uint32_t>::max();
	if (build_outgoing) {
		snapshot->outgoing_neighbors.Resize(expected_edges, compact_neighbors);
		if (!weight_property.empty()) {
			snapshot->outgoing_weights.resize(expected_edges);
		}
		if (build_edge_ids) {
			snapshot->outgoing_edge_ids.resize(expected_edges);
		}
		if (build_edge_labels) {
			snapshot->outgoing_label_ids.Reset(expected_edges, true, 0);
		}
	}
	if (build_incoming) {
		snapshot->incoming_neighbors.Resize(expected_edges, compact_neighbors);
		if (!weight_property.empty()) {
			snapshot->incoming_weights.resize(expected_edges);
		}
		if (build_edge_ids) {
			snapshot->incoming_edge_ids.resize(expected_edges);
		}
		if (build_edge_labels) {
			snapshot->incoming_label_ids.Reset(expected_edges, true, 0);
		}
	}
	auto outgoing_cursor = snapshot->outgoing_offsets;
	auto incoming_cursor = snapshot->incoming_offsets;
	auto edge_rows = make_uniq<QueryResultStream<>>(connection.Submit(edge_projection));

	idx_t scattered_edges = 0;
	bool edge_labels_uniform = true;
	bool saw_edge_label = false;
	uint32_t uniform_edge_label_id = 0;
	while (auto chunk = FetchCsrRows(*edge_rows)) {
		UnifiedVectorFormat edge_id_data;
		UnifiedVectorFormat source_data;
		UnifiedVectorFormat target_data;
		UnifiedVectorFormat label_data;
		UnifiedVectorFormat weight_data;
		chunk->data[0].ToUnifiedFormat(chunk->size(), edge_id_data);
		chunk->data[1].ToUnifiedFormat(chunk->size(), source_data);
		chunk->data[2].ToUnifiedFormat(chunk->size(), target_data);
		chunk->data[3].ToUnifiedFormat(chunk->size(), label_data);
		auto edge_id_values = UnifiedVectorFormat::GetData<uint64_t>(edge_id_data);
		auto source_values = UnifiedVectorFormat::GetData<uint64_t>(source_data);
		auto target_values = UnifiedVectorFormat::GetData<uint64_t>(target_data);
		if (!weight_property.empty()) {
			chunk->data[4].ToUnifiedFormat(chunk->size(), weight_data);
		}
		auto label_values = UnifiedVectorFormat::GetData<string_t>(label_data);
		for (idx_t row = 0; row < chunk->size(); row++) {
			auto edge_index = edge_id_data.sel->get_index(row);
			auto source_index = source_data.sel->get_index(row);
			auto target_index = target_data.sel->get_index(row);
			auto edge_id = edge_id_values[edge_index];
			if (validate_edge_ids && !edge_ids.insert(edge_id).second) {
				throw InvalidInputException("Table-backed CSR requires unique edge keys; duplicate key %llu",
				                            static_cast<unsigned long long>(edge_id));
			}
			idx_t source;
			idx_t target;
			if (!GqlTryGetCsrOrdinal(*snapshot, source_values[source_index], source) ||
			    !GqlTryGetCsrOrdinal(*snapshot, target_values[target_index], target)) {
				throw InternalException("GQL CSR endpoints changed within a read transaction");
			}
			// NULL and NaN both remain invalid weights for algorithms to validate
			// after applying their vertex and edge projections.
			double weight = 0;
			if (!weight_property.empty()) {
				auto weight_index = weight_data.sel->get_index(row);
				weight = weight_data.validity.RowIsValid(weight_index)
				             ? UnifiedVectorFormat::GetData<double>(weight_data)[weight_index]
				             : std::numeric_limits<double>::quiet_NaN();
			}
			uint32_t label_id = 0;
			if (build_edge_labels) {
				auto label_index = label_data.sel->get_index(row);
				auto label =
				    label_data.validity.RowIsValid(label_index) ? label_values[label_index].GetString() : string();
				if (label.empty() || label.find(';') != string::npos) {
					throw InvalidInputException("Table-backed CSR requires every edge to have exactly one type");
				}
				label_id = CsrLabelId(*snapshot, label);
				if (build_edge_stats && label_id >= snapshot->edge_label_stats.size()) {
					snapshot->edge_label_stats.resize(label_id + 1);
				}
				if (build_edge_stats) {
					snapshot->edge_label_stats[label_id].edge_count++;
				}
				if (!saw_edge_label) {
					uniform_edge_label_id = label_id;
					saw_edge_label = true;
					if (build_outgoing) {
						snapshot->outgoing_label_ids.Reset(expected_edges, true, label_id);
					}
					if (build_incoming) {
						snapshot->incoming_label_ids.Reset(expected_edges, true, label_id);
					}
				} else if (edge_labels_uniform && label_id != uniform_edge_label_id) {
					edge_labels_uniform = false;
					if (build_outgoing) {
						snapshot->outgoing_label_ids.MaterializeUniform();
					}
					if (build_incoming) {
						snapshot->incoming_label_ids.MaterializeUniform();
					}
				}
			}
			if (build_outgoing) {
				auto outgoing = outgoing_cursor[source]++;
				snapshot->outgoing_neighbors.Set(outgoing, target);
				if (!weight_property.empty()) {
					snapshot->outgoing_weights[outgoing] = weight;
				}
				if (build_edge_ids) {
					snapshot->outgoing_edge_ids[outgoing] = edge_id;
				}
				if (build_edge_labels && !edge_labels_uniform) {
					snapshot->outgoing_label_ids.Set(outgoing, label_id);
				}
			}
			if (build_incoming) {
				auto incoming = incoming_cursor[target]++;
				snapshot->incoming_neighbors.Set(incoming, source);
				if (!weight_property.empty()) {
					snapshot->incoming_weights[incoming] = weight;
				}
				if (build_edge_ids) {
					snapshot->incoming_edge_ids[incoming] = edge_id;
				}
				if (build_edge_labels && !edge_labels_uniform) {
					snapshot->incoming_label_ids.Set(incoming, label_id);
				}
			}
			scattered_edges++;
		}
	}
	if (scattered_edges != expected_edges) {
		throw InternalException("GQL CSR edge scan changed within a read transaction");
	}
	vector<uint64_t> label_degrees(snapshot->edge_label_stats.size(), 0);
	vector<uint32_t> touched_labels;
	// Reuse the existing directional degree pass. Optional statistics have a
	// fixed entry budget and bounded work per touched relationship type. If
	// either budget is exceeded, discard all pairs rather than treating a missing
	// pair in a partial map as zero degree.
	constexpr idx_t LABEL_FANOUT_PAIRS = 4096;
	constexpr idx_t LABEL_FANOUT_LABELS_PER_VERTEX = 16;
	snapshot->has_label_fanout_stats = build_edge_stats && build_vertex_labels && build_vertex_label_postings;
	auto discard_label_fanout = [&]() {
		snapshot->has_label_fanout_stats = false;
		decltype(snapshot->label_fanout_stats)().swap(snapshot->label_fanout_stats);
	};
	auto collect_degree_stats = [&](const vector<uint64_t> &offsets, const GqlCsrEdgeLabels &labels, bool outgoing) {
		for (idx_t vertex = 0; vertex + 1 < offsets.size(); vertex++) {
			touched_labels.clear();
			for (idx_t edge = offsets[vertex]; edge < offsets[vertex + 1]; edge++) {
				auto label_id = labels[edge];
				if (label_degrees[label_id]++ == 0) {
					touched_labels.push_back(label_id);
				}
			}
			for (const auto label_id : touched_labels) {
				auto degree = label_degrees[label_id];
				auto &stats = snapshot->edge_label_stats[label_id];
				if (outgoing) {
					stats.outgoing_vertex_count++;
					stats.max_outgoing_degree = MaxValue<uint64_t>(stats.max_outgoing_degree, degree);
					stats.outgoing_degree_squares += static_cast<double>(degree) * static_cast<double>(degree);
				} else {
					stats.incoming_vertex_count++;
					stats.max_incoming_degree = MaxValue<uint64_t>(stats.max_incoming_degree, degree);
					stats.incoming_degree_squares += static_cast<double>(degree) * static_cast<double>(degree);
				}
				if (snapshot->has_label_fanout_stats) {
					auto start = snapshot->vertex_label_offsets[vertex];
					auto end = snapshot->vertex_label_offsets[vertex + 1];
					if (end - start > LABEL_FANOUT_LABELS_PER_VERTEX) {
						discard_label_fanout();
					} else {
						for (auto position = start; position < end; position++) {
							auto key = GqlCsrLabelFanoutKey(snapshot->vertex_label_ids[position], label_id);
							auto entry = snapshot->label_fanout_stats.find(key);
							if (entry == snapshot->label_fanout_stats.end()) {
								if (snapshot->label_fanout_stats.size() >= LABEL_FANOUT_PAIRS) {
									discard_label_fanout();
									break;
								}
								entry = snapshot->label_fanout_stats.emplace(key, GqlCsrLabelFanoutStats()).first;
							}
							(outgoing ? entry->second.outgoing_edges : entry->second.incoming_edges) += degree;
							(outgoing ? entry->second.outgoing_vertices : entry->second.incoming_vertices)++;
						}
					}
				}
				label_degrees[label_id] = 0;
			}
		}
	};
	if (build_edge_stats) {
		collect_degree_stats(snapshot->outgoing_offsets, snapshot->outgoing_label_ids, true);
		collect_degree_stats(snapshot->incoming_offsets, snapshot->incoming_label_ids, false);
	}
	idx_t endpoint_workspace_bytes = 0;
	if (capabilities & GQL_CSR_ENDPOINT_STATS) {
		snapshot->endpoint_statistics = GqlCollectEndpointStatistics(
		    context, *snapshot, snapshot->endpoint_statistics_status, endpoint_workspace_bytes);
		if (!snapshot->endpoint_statistics) {
			// Capabilities describe available resources, not attempted analysis.
			snapshot->capabilities &= ~GQL_CSR_ENDPOINT_STATS;
		}
	}

	snapshot->topology_bytes =
	    snapshot->outgoing_offsets.capacity() * sizeof(uint64_t) + snapshot->outgoing_neighbors.AllocatedBytes() +
	    snapshot->incoming_offsets.capacity() * sizeof(uint64_t) + snapshot->incoming_neighbors.AllocatedBytes() +
	    snapshot->outgoing_degrees.AllocatedBytes();
	snapshot->identity_bytes = snapshot->vertex_ids.AllocatedBytes() +
	                           snapshot->vertex_row_ids.AllocatedBytes() +
	                           snapshot->outgoing_edge_ids.capacity() * sizeof(uint64_t) +
	                           snapshot->incoming_edge_ids.capacity() * sizeof(uint64_t);
	snapshot->label_bytes =
	    snapshot->vertex_label_offsets.capacity() * sizeof(idx_t) +
	    snapshot->vertex_label_ids.capacity() * sizeof(uint32_t) +
	    snapshot->vertex_label_posting_offsets.capacity() * sizeof(uint64_t) +
	    snapshot->vertex_label_postings.AllocatedBytes() + snapshot->outgoing_label_ids.AllocatedBytes() +
	    snapshot->incoming_label_ids.AllocatedBytes() + LabelDictionaryStorageBytes(snapshot->label_ids);
	snapshot->auxiliary_bytes =
	    snapshot->weight_property.capacity() +
	    (snapshot->outgoing_weights.capacity() + snapshot->incoming_weights.capacity()) * sizeof(double) +
	    sizeof(GqlCsrSnapshot) + HashContainerStorageBytes(snapshot->ordinal_by_id) +
	    HashContainerStorageBytes(snapshot->vertex_ordinal_by_row_id) +
	    snapshot->source_catalog.capacity() + snapshot->vertex_table_key.capacity() +
	    snapshot->edge_table_key.capacity() + snapshot->edge_label_stats.capacity() * sizeof(GqlCsrEdgeLabelStats);
	snapshot->auxiliary_bytes += HashContainerStorageBytes(snapshot->source_write_generations);
	snapshot->auxiliary_bytes += HashContainerStorageBytes(snapshot->label_fanout_stats);
	snapshot->auxiliary_bytes += snapshot->endpoint_statistics_status.capacity();
	snapshot->auxiliary_bytes += snapshot->vertex_catalog_name.capacity() + snapshot->vertex_schema_name.capacity() +
	                             snapshot->vertex_table_name.capacity() + snapshot->edge_catalog_name.capacity() +
	                             snapshot->edge_schema_name.capacity() + snapshot->edge_table_name.capacity();
	if (snapshot->endpoint_statistics)
		snapshot->auxiliary_bytes += snapshot->endpoint_statistics->AllocatedBytes();
	for (const auto &source : snapshot->source_write_generations) {
		snapshot->auxiliary_bytes += source.first.capacity();
	}
	snapshot->build_auxiliary_bytes =
	    endpoint_workspace_bytes + transient_vertex_id_bytes + outgoing_cursor.capacity() * sizeof(uint64_t) +
	    incoming_cursor.capacity() * sizeof(uint64_t) + HashContainerStorageBytes(edge_ids) +
	    label_degrees.capacity() * sizeof(uint64_t) + touched_labels.capacity() * sizeof(uint32_t);
	snapshot->memory_bytes =
	    snapshot->topology_bytes + snapshot->identity_bytes + snapshot->label_bytes + snapshot->auxiliary_bytes;
	connection.Commit();
	Connection validation_connection(*context.db);
	auto current_graph = ReadGraphVersion(validation_connection, graph_name);
	// Inserts that raced construction are the delta's to follow, and generations catch the rest.
	if (!snapshot->supports_delta && !SameGraphVersion(graph, current_graph)) {
		throw InvalidInputException("Graph '%s' changed while its CSR snapshot was being built; retry the operation",
		                            graph_name);
	}
	return snapshot;
}

static bool CsrSnapshotSourcesAreCurrent(ClientContext &context, const GqlCsrSnapshot &snapshot) {
	for (const auto &source : snapshot.source_write_generations) {
		if (source.second != ReadCsrTableWriteGeneration(context, source.first)) {
			return false;
		}
	}

	return snapshot.write_generation == ReadCsrWriteGeneration(context) &&
	       snapshot.vertex_write_generation == ReadCsrTableWriteGeneration(context, snapshot.vertex_table_key) &&
	       snapshot.edge_write_generation == ReadCsrTableWriteGeneration(context, snapshot.edge_table_key);
}

static bool CsrSnapshotIsCurrent(ClientContext &context, const GqlCsrSnapshot &snapshot, const GraphVersion &graph) {
	return CsrSnapshotSourcesAreCurrent(context, snapshot) && snapshot.graph_id == graph.graph_id &&
	       snapshot.graph_version == graph.graph_version &&
	       snapshot.has_source_snapshot == graph.has_source_snapshot &&
	       (!snapshot.has_source_snapshot || (snapshot.source_snapshot_id == graph.source_snapshot_id &&
	                                          StringUtil::CIEquals(snapshot.source_catalog, graph.source_catalog)));
}

// Whether every change since the build can only have been an insert, so a delta can follow.
static bool CsrSnapshotCanCatchUp(ClientContext &context, const GqlCsrSnapshot &snapshot, const GraphVersion &graph) {
	return snapshot.supports_delta && snapshot.graph_id == graph.graph_id && !graph.has_source_snapshot &&
	       snapshot.write_generation == ReadCsrWriteGeneration(context) &&
	       snapshot.vertex_destructive_generation ==
	           ReadCsrTableDestructiveGeneration(context, snapshot.vertex_table_key) &&
	       snapshot.edge_destructive_generation == ReadCsrTableDestructiveGeneration(context, snapshot.edge_table_key);
}

static shared_ptr<GqlCsrSnapshot>
FindPreparedTableSnapshotLocked(ClientContext &context, GqlDerivedGraphStorageState &storage, const GraphVersion &graph,
                                GqlCsrCapabilities required_capabilities, const string &weight_property = string()) {
	auto entry = storage.snapshots.find(graph.graph_id);
	if (entry == storage.snapshots.end()) {
		return nullptr;
	}
	auto &snapshots = entry->second;
	snapshots.erase(std::remove_if(snapshots.begin(), snapshots.end(),
	                               [&](const shared_ptr<GqlCsrSnapshot> &snapshot) {
		                               return !CsrSnapshotIsCurrent(context, *snapshot, graph) &&
		                                      !CsrSnapshotCanCatchUp(context, *snapshot, graph);
	                               }),
	                snapshots.end());
	shared_ptr<GqlCsrSnapshot> best;
	for (const auto &snapshot : snapshots) {
		if (snapshot->weight_property != weight_property ||
		    !CsrHasCapabilities(snapshot->capabilities, required_capabilities) ||
		    !CsrSnapshotIsCurrent(context, *snapshot, graph)) {
			continue;
		}
		if (!best || snapshot->memory_bytes < best->memory_bytes) {
			best = snapshot;
		}
	}
	if (snapshots.empty()) {
		storage.snapshots.erase(entry);
	}
	return best;
}

static shared_ptr<GqlCsrSnapshot> GetPreparedTableSnapshot(ClientContext &context, const string &graph_name,
                                                           GqlDerivedGraphStorageState &storage,
                                                           GqlCsrCapabilities required_capabilities,
                                                           const string &weight_property = string()) {
	// A cached binding implies a managed graph, which has no source snapshot.
	GraphVersion graph;
	if (!GqlTryGetCachedGraphVersion(context, graph_name, graph.graph_id, graph.graph_version)) {
		Connection connection(*context.db);
		graph = ReadGraphVersion(connection, graph_name);
	}
	lock_guard<mutex> guard(storage.lock);
	auto snapshot = FindPreparedTableSnapshotLocked(context, storage, graph, required_capabilities, weight_property);
	if (snapshot) {
		return snapshot;
	}
	throw InvalidInputException("CSR for table-backed graph '%s' has not been built in this database instance; "
	                            "run CALL gql_build_csr('%s') first",
	                            graph_name, graph_name);
}

static shared_ptr<GqlCsrSnapshot> PublishTableSnapshot(GqlDerivedGraphStorageState &storage,
                                                       shared_ptr<GqlCsrSnapshot> snapshot) {
	lock_guard<mutex> guard(storage.lock);
	auto &snapshots = storage.snapshots[snapshot->graph_id];
	auto same_generations = [&](const GqlCsrSnapshot &entry) {
		return entry.graph_version == snapshot->graph_version &&
		       entry.has_source_snapshot == snapshot->has_source_snapshot &&
		       (!entry.has_source_snapshot || (entry.source_snapshot_id == snapshot->source_snapshot_id &&
		                                       StringUtil::CIEquals(entry.source_catalog, snapshot->source_catalog))) &&
		       entry.write_generation == snapshot->write_generation &&
		       entry.vertex_write_generation == snapshot->vertex_write_generation &&
		       entry.edge_write_generation == snapshot->edge_write_generation &&
		       entry.source_write_generations == snapshot->source_write_generations;
	};
	snapshots.erase(std::remove_if(snapshots.begin(), snapshots.end(),
	                               [&](const shared_ptr<GqlCsrSnapshot> &entry) {
		                               // A snapshot a delta can still follow stays until a full one replaces it.
		                               const bool can_catch_up =
		                                   entry->supports_delta && entry->write_generation == snapshot->write_generation &&
		                                   entry->vertex_destructive_generation == snapshot->vertex_destructive_generation &&
		                                   entry->edge_destructive_generation == snapshot->edge_destructive_generation;
		                               return (!can_catch_up && !same_generations(*entry)) ||
		                                      (entry->weight_property == snapshot->weight_property &&
		                                       CsrHasCapabilities(snapshot->capabilities, entry->capabilities));
	                               }),
	                snapshots.end());
	storage.build_count++;
	shared_ptr<GqlCsrSnapshot> best;
	for (const auto &entry : snapshots) {
		if (entry->weight_property == snapshot->weight_property && same_generations(*entry) &&
		    CsrHasCapabilities(entry->capabilities, snapshot->capabilities) &&
		    (!best || entry->memory_bytes < best->memory_bytes)) {
			best = entry;
		}
	}
	if (best) {
		return best;
	}
	snapshots.push_back(snapshot);
	return snapshot;
}

static uint64_t ReadCsrBuildCount(GqlDerivedGraphStorageState &storage) {
	lock_guard<mutex> guard(storage.lock);
	return storage.build_count;
}

uint64_t GqlCsrBuildCount(ClientContext &context) {
	return ReadCsrBuildCount(*GetDerivedGraphStorageState(context));
}

struct GqlCsrLease {
	shared_ptr<const GqlCsrSnapshot> snapshot;
	shared_ptr<const GqlCsrDelta> delta;
};

struct GqlCsrQueryLeases : ClientContextState {
	mutex lock;
	unordered_map<string, GqlCsrLease> leases;

	void QueryEnd() override {
		lock_guard<mutex> guard(lock);
		leases.clear();
	}
};

// Builds and publishes one snapshot, unless another build of this graph is running, in
// which case it waits for that one. Returns whether it built. Callers look the result up,
// so that a write that raced construction invalidates it before anyone can lease it.
static bool BuildAndPublishTableSnapshot(ClientContext &context, GqlDerivedGraphStorageState &storage, uint64_t graph_id,
                                         const string &graph_name, GqlCsrCapabilities capabilities,
                                         const string &weight_property = string()) {
	{
		unique_lock<mutex> guard(storage.lock);
		if (storage.builds_in_progress.find(graph_id) != storage.builds_in_progress.end()) {
			storage.build_finished.wait(guard, [&] {
				return storage.builds_in_progress.find(graph_id) == storage.builds_in_progress.end();
			});
			return false;
		}
		storage.builds_in_progress.insert(graph_id);
	}
	try {
		PublishTableSnapshot(storage, BuildTableSnapshot(context, graph_name, capabilities, weight_property));
	} catch (...) {
		{
			lock_guard<mutex> guard(storage.lock);
			storage.builds_in_progress.erase(graph_id);
		}
		storage.build_finished.notify_all();
		throw;
	}
	{
		lock_guard<mutex> guard(storage.lock);
		storage.builds_in_progress.erase(graph_id);
	}
	storage.build_finished.notify_all();
	return true;
}

static idx_t CsrDeltaRowLimit(const GqlCsrSnapshot &snapshot) {
	return MaxValue<idx_t>(4096, snapshot.edge_count / 1024);
}

// Reads the rows past the watermarks. Every check that fails means rows a delta can't
// express, or that the table isn't laid out as assumed, and returns false for a rebuild.
// Comparing row counts covers anything the watermarks would miss: rows of a transaction
// that commits after one with higher row IDs, row IDs moved by a checkpoint, or a table
// replaced outside of this extension.
static bool ScanCsrDelta(ClientContext &context, const string &graph_name, const GqlCsrSnapshot &snapshot,
                         const shared_ptr<const GqlCsrDelta> &previous, shared_ptr<const GqlCsrDelta> &result) {
	GqlTableGraphBinding binding;
	if (!GqlTryLoadTableGraph(context, graph_name, binding) ||
	    !StringUtil::CIEquals(binding.vertex.table_name, snapshot.vertex_table_name) ||
	    !StringUtil::CIEquals(binding.edge.table_name, snapshot.edge_table_name)) {
		return false;
	}
	const auto vertex_table = QualifiedTable(binding.vertex);
	const auto edge_table = QualifiedTable(binding.edge);
	const auto previous_vertices = previous ? previous->vertex_ids.size() : 0;
	const auto previous_edges = previous ? previous->edge_count : 0;
	const auto vertex_watermark = previous ? previous->vertex_row_watermark : snapshot.vertex_row_watermark;
	const auto edge_watermark = previous ? previous->edge_row_watermark : snapshot.edge_row_watermark;
	const auto base_vertices = snapshot.vertex_ids.size();

	Connection connection(*context.db);
	connection.BeginTransaction();
	if (ReadCsrSchemaVersion(connection, snapshot.graph_id) != snapshot.schema_version) {
		connection.Rollback();
		return false;
	}
	const auto vertex_total = GqlQuery(connection, "SELECT count(*)::UBIGINT FROM " + vertex_table)
	                              ->GetValue(0, 0)
	                              .GetValue<uint64_t>();
	const auto edge_total =
	    GqlQuery(connection, "SELECT count(*)::UBIGINT FROM " + edge_table)->GetValue(0, 0).GetValue<uint64_t>();
	const auto known_vertices = base_vertices + previous_vertices;
	const auto known_edges = snapshot.edge_count + previous_edges;
	if (vertex_total < known_vertices || edge_total < known_edges ||
	    vertex_total - base_vertices + edge_total - snapshot.edge_count > CsrDeltaRowLimit(snapshot)) {
		connection.Rollback();
		return false;
	}
	unique_ptr<GqlQueryResult> new_vertices;
	unique_ptr<GqlQueryResult> new_edges;
	if (vertex_total > known_vertices) {
		new_vertices = GqlQuery(connection, "SELECT CAST(" + GqlQuoteIdentifier(binding.vertex.key_column) +
		                                        " AS UBIGINT), " + CsrVertexLabelProjection(binding.vertex) +
		                                        ", CAST(rowid AS UBIGINT) FROM " + vertex_table +
		                                        " WHERE rowid >= " + to_string(vertex_watermark) + " ORDER BY rowid");
	}
	if (edge_total > known_edges) {
		new_edges = GqlQuery(connection,
		                     "SELECT CAST(" + GqlQuoteIdentifier(binding.edge.key_column) + " AS UBIGINT), CAST(" +
		                         GqlQuoteIdentifier(binding.edge_source_column) + " AS UBIGINT), CAST(" +
		                         GqlQuoteIdentifier(binding.edge_target_column) + " AS UBIGINT), " +
		                         CsrEdgeLabelProjection(binding.edge) + ", CAST(rowid AS UBIGINT) FROM " + edge_table +
		                         " WHERE rowid >= " + to_string(edge_watermark) + " ORDER BY rowid");
	}
	connection.Commit();
	const auto new_vertex_count = new_vertices ? new_vertices->RowCount() : 0;
	const auto new_edge_count = new_edges ? new_edges->RowCount() : 0;
	if (known_vertices + new_vertex_count != vertex_total || known_edges + new_edge_count != edge_total) {
		return false;
	}
	if (new_vertex_count == 0 && new_edge_count == 0) {
		result = previous;
		return true;
	}

	auto delta = previous ? make_shared_ptr<GqlCsrDelta>(*previous) : make_shared_ptr<GqlCsrDelta>();
	if (!previous) {
		delta->vertex_row_watermark = snapshot.vertex_row_watermark;
		delta->edge_row_watermark = snapshot.edge_row_watermark;
	}
	for (idx_t row = 0; row < new_vertex_count; row++) {
		const auto vertex_id = new_vertices->GetValue(0, row);
		const auto labels = new_vertices->GetValue(1, row);
		if (vertex_id.IsNull()) {
			return false;
		}
		const auto id = vertex_id.GetValue<uint64_t>();
		idx_t existing;
		if (CsrTryGetOrdinal(snapshot, delta.get(), id, existing)) {
			return false;
		}
		vector<uint32_t> label_ids;
		if (!labels.IsNull()) {
			for (const auto &label : ListValue::GetChildren(labels)) {
				if (label.IsNull() || label.GetValue<string>().empty()) {
					continue;
				}
				// A label with no vertices may be planned as an empty scan, so it needs new statistics.
				auto entry = snapshot.label_ids.find(StringUtil::Lower(label.GetValue<string>()));
				if (entry == snapshot.label_ids.end() ||
				    entry->second + 1 >= snapshot.vertex_label_posting_offsets.size() ||
				    snapshot.vertex_label_posting_offsets[entry->second] ==
				        snapshot.vertex_label_posting_offsets[entry->second + 1]) {
					return false;
				}
				if (std::find(label_ids.begin(), label_ids.end(), entry->second) == label_ids.end()) {
					label_ids.push_back(entry->second);
				}
			}
		}
		for (const auto label : label_ids) {
			delta->vertex_ids_by_label[label].push_back(id);
		}
		delta->ordinal_by_id.emplace(id, base_vertices + delta->vertex_ids.size());
		delta->vertex_ids.push_back(id);
		delta->vertex_row_ids.push_back(new_vertices->GetValue(2, row).GetValue<uint64_t>());
		delta->vertex_labels.push_back(std::move(label_ids));
		delta->vertex_row_watermark = delta->vertex_row_ids.back() + 1;
	}
	for (idx_t row = 0; row < new_edge_count; row++) {
		const auto edge_id = new_edges->GetValue(0, row);
		const auto source_id = new_edges->GetValue(1, row);
		const auto target_id = new_edges->GetValue(2, row);
		const auto label = new_edges->GetValue(3, row);
		const auto row_id = new_edges->GetValue(4, row).GetValue<uint64_t>();
		if (edge_id.IsNull() || source_id.IsNull() || target_id.IsNull() || label.IsNull()) {
			return false;
		}
		// Plans fetch edge rows by ID when the snapshot says they match row IDs.
		if (snapshot.edge_ids_match_rowids && edge_id.GetValue<uint64_t>() != row_id + 1) {
			return false;
		}
		auto entry = snapshot.label_ids.find(StringUtil::Lower(label.GetValue<string>()));
		idx_t source;
		idx_t target;
		if (entry == snapshot.label_ids.end() || entry->second >= snapshot.edge_label_stats.size() ||
		    snapshot.edge_label_stats[entry->second].edge_count == 0 ||
		    !CsrTryGetOrdinal(snapshot, delta.get(), source_id.GetValue<uint64_t>(), source) ||
		    !CsrTryGetOrdinal(snapshot, delta.get(), target_id.GetValue<uint64_t>(), target)) {
			return false;
		}
		delta->outgoing[source].push_back({target, edge_id.GetValue<uint64_t>(), entry->second});
		delta->incoming[target].push_back({source, edge_id.GetValue<uint64_t>(), entry->second});
		delta->edge_count++;
		delta->edge_row_watermark = row_id + 1;
	}
	result = std::move(delta);
	return true;
}

static GqlCsrLease ResolveFullLease(ClientContext &context, const string &graph_name,
                                    GqlDerivedGraphStorageState &storage) {
	bool rebuilt = false;
	while (true) {
		// A cached binding implies a managed graph, which has no source snapshot.
		GraphVersion graph;
		if (!GqlTryGetCachedGraphVersion(context, graph_name, graph.graph_id, graph.graph_version)) {
			Connection connection(*context.db);
			graph = ReadGraphVersion(connection, graph_name);
		}
		shared_ptr<GqlCsrSnapshot> snapshot;
		{
			lock_guard<mutex> guard(storage.lock);
			snapshot = FindPreparedTableSnapshotLocked(context, storage, graph, GQL_CSR_FULL);
			if (!snapshot) {
				auto entry = storage.snapshots.find(graph.graph_id);
				if (entry != storage.snapshots.end()) {
					for (const auto &candidate : entry->second) {
						if (CsrSnapshotCanCatchUp(context, *candidate, graph)) {
							snapshot = candidate;
							break;
						}
					}
				}
			}
		}
		if (!snapshot) {
			throw InvalidInputException("CSR for table-backed graph '%s' has not been built in this database instance; "
			                            "run CALL gql_build_csr('%s') first",
			                            graph_name, graph_name);
		}
		if (!snapshot->supports_delta) {
			return {snapshot, nullptr};
		}
		bool caught_up = false;
		{
			lock_guard<mutex> catch_up(storage.delta_catch_up_lock);
			// Read before the scan: a write that commits during it advances this again,
			// so the next reader scans once more instead of missing the write's rows.
			const auto generation = GqlDatabaseWriteGeneration(context);
			shared_ptr<const GqlCsrDelta> previous;
			{
				lock_guard<mutex> guard(storage.lock);
				if (snapshot->synced_generation == generation) {
					return {snapshot, snapshot->delta};
				}
				previous = snapshot->delta;
			}
			shared_ptr<const GqlCsrDelta> delta;
			caught_up = ScanCsrDelta(context, graph_name, *snapshot, previous, delta);
			if (caught_up) {
				lock_guard<mutex> guard(storage.lock);
				snapshot->delta = delta;
				snapshot->synced_generation = generation;
				return {snapshot, delta};
			}
		}
		// Without progress after one rebuild, other writers are outrunning it.
		if (rebuilt) {
			throw InvalidInputException("Graph '%s' changed while its CSR snapshot was being built; retry the operation",
			                            graph_name);
		}
		BuildAndPublishTableSnapshot(context, storage, graph.graph_id, graph_name, snapshot->capabilities);
		rebuilt = true;
	}
}

static GqlCsrLease AcquireFullLease(ClientContext &context, const string &graph_name,
                                    GqlDerivedGraphStorageState &storage) {
	auto leases = context.registered_state->GetOrCreate<GqlCsrQueryLeases>("gql_csr_query_leases");
	lock_guard<mutex> guard(leases->lock);
	auto entry = leases->leases.find(graph_name);
	// Inserts committed during a statement don't end its lease, so it reads one version.
	if (entry != leases->leases.end() &&
	    entry->second.snapshot->write_generation == ReadCsrWriteGeneration(context) &&
	    entry->second.snapshot->vertex_destructive_generation ==
	        ReadCsrTableDestructiveGeneration(context, entry->second.snapshot->vertex_table_key) &&
	    entry->second.snapshot->edge_destructive_generation ==
	        ReadCsrTableDestructiveGeneration(context, entry->second.snapshot->edge_table_key)) {
		return entry->second;
	}
	auto lease = ResolveFullLease(context, graph_name, storage);
	// A managed graph's lease is shared by binding and physical operators only
	// within this statement. Recheck write generations on reuse and clear it at
	// QueryEnd. External LIVE snapshots retain their version checks on every
	// acquisition, since their changes need not pass through our write observer.
	if (lease.snapshot->supports_delta) {
		leases->leases[graph_name] = lease;
	}
	return lease;
}

static GqlCsrLease GqlGetCsrLease(ClientContext &context, const string &graph_name) {
	if (!context.transaction.IsAutoCommit()) {
		throw NotImplementedException("CSR algorithms are not eligible inside an explicit transaction");
	}
	auto storage = GetDerivedGraphStorageState(context);
	storage->acquisition_count++;
	return AcquireFullLease(context, graph_name, *storage);
}

shared_ptr<const GqlCsrSnapshot> GqlGetCsrSnapshot(ClientContext &context, const string &graph_name) {
	return GqlGetCsrLease(context, graph_name).snapshot;
}

shared_ptr<const GqlCsrSnapshot> GqlGetUnchangedCsrSnapshot(ClientContext &context, const string &graph_name) {
	if (!context.transaction.IsAutoCommit()) {
		throw NotImplementedException("CSR algorithms are not eligible inside an explicit transaction");
	}
	auto storage = GetDerivedGraphStorageState(context);
	storage->acquisition_count++;
	return GetPreparedTableSnapshot(context, graph_name, *storage, GQL_CSR_FULL);
}

shared_ptr<const GqlCsrSnapshot> GqlGetOrBuildCsrSnapshot(ClientContext &context, const string &graph_name,
                                                          GqlCsrCapabilities capabilities, bool *built,
                                                          const string &weight_property) {
	if (!context.transaction.IsAutoCommit()) {
		throw NotImplementedException("CSR algorithms are not eligible inside an explicit transaction");
	}
	capabilities = NormalizeCsrCapabilities(capabilities);
	if (built) {
		*built = false;
	}
	auto storage = GetDerivedGraphStorageState(context);
	storage->acquisition_count++;
	bool performed_build = false;
	while (true) {
		Connection connection(*context.db);
		auto graph = ReadGraphVersion(connection, graph_name);
		{
			lock_guard<mutex> guard(storage->lock);
			auto snapshot = FindPreparedTableSnapshotLocked(context, *storage, graph, capabilities,
			                                                StringUtil::Lower(weight_property));
			if (snapshot) {
				if (built) {
					*built = performed_build;
				}
				return snapshot;
			}
		}
		performed_build |=
		    BuildAndPublishTableSnapshot(context, *storage, graph.graph_id, graph_name, capabilities, weight_property);
	}
}

shared_ptr<const GqlCsrSnapshot> GqlTryGetCsrSnapshot(ClientContext &context, const string &graph_name) {
	if (!context.transaction.IsAutoCommit()) {
		return nullptr;
	}
	auto storage = GetDerivedGraphStorageState(context);
	try {
		return AcquireFullLease(context, graph_name, *storage).snapshot;
	} catch (const InvalidInputException &) {
		return nullptr;
	}
}

struct CsrBindData : TableFunctionData {
	bool endpoint_statistics = false;
	string graph_name;
	string weight_property;
	uint64_t vertex_id = 0;
	string direction;
};

struct CsrVerticesBindData : TableFunctionData {
	string graph_name;
	string label;
	idx_t estimated_count = 0;
};

static bool TryCsrVertexLabelRange(const GqlCsrSnapshot &snapshot, const string &label, idx_t &start, idx_t &end) {
	auto entry = snapshot.label_ids.find(StringUtil::Lower(label));
	if (entry == snapshot.label_ids.end() || entry->second + 1 >= snapshot.vertex_label_posting_offsets.size()) {
		start = 0;
		end = 0;
		return false;
	}
	start = NumericCast<idx_t>(snapshot.vertex_label_posting_offsets[entry->second]);
	end = NumericCast<idx_t>(snapshot.vertex_label_posting_offsets[entry->second + 1]);
	return true;
}

static unique_ptr<FunctionData> CsrVerticesBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<CsrVerticesBindData>();
	result->graph_name = input.inputs[0].GetValue<string>();
	result->label = StringUtil::Lower(input.inputs[1].GetValue<string>());
	if (result->label.empty()) {
		throw BinderException("GQL CSR vertex lookup requires one node label");
	}
	auto snapshot = GqlGetCsrSnapshot(context, result->graph_name);
	idx_t start;
	idx_t end;
	TryCsrVertexLabelRange(*snapshot, result->label, start, end);
	result->estimated_count = end - start;
	names = {"__gql_id"};
	return_types = {LogicalType::UBIGINT};
	return std::move(result);
}

struct CsrVerticesState : GlobalTableFunctionState {
	shared_ptr<const GqlCsrSnapshot> snapshot;
	shared_ptr<const GqlCsrDelta> delta;
	const vector<uint64_t> *delta_vertices = nullptr;
	idx_t cursor = 0;
	idx_t end = 0;
	idx_t delta_cursor = 0;
};

static unique_ptr<GlobalTableFunctionState> CsrVerticesInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &data = input.bind_data->Cast<CsrVerticesBindData>();
	auto result = make_uniq<CsrVerticesState>();
	auto lease = GqlGetCsrLease(context, data.graph_name);
	result->snapshot = lease.snapshot;
	result->delta = lease.delta;
	TryCsrVertexLabelRange(*result->snapshot, data.label, result->cursor, result->end);
	auto label = result->snapshot->label_ids.find(data.label);
	if (result->delta && label != result->snapshot->label_ids.end()) {
		auto vertices = result->delta->vertex_ids_by_label.find(label->second);
		if (vertices != result->delta->vertex_ids_by_label.end()) {
			result->delta_vertices = &vertices->second;
		}
	}
	return std::move(result);
}

static void CsrVerticesFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<CsrVerticesState>();
	auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.end - state.cursor);
	for (idx_t index = 0; index < count; index++) {
		auto vertex = state.snapshot->vertex_label_postings[state.cursor + index];
		output.SetValue(0, index, Value::UBIGINT(state.snapshot->vertex_ids[vertex]));
	}
	state.cursor += count;
	if (state.delta_vertices) {
		const auto delta_count =
		    MinValue<idx_t>(STANDARD_VECTOR_SIZE - count, state.delta_vertices->size() - state.delta_cursor);
		for (idx_t index = 0; index < delta_count; index++) {
			output.SetValue(0, count + index, Value::UBIGINT((*state.delta_vertices)[state.delta_cursor + index]));
		}
		state.delta_cursor += delta_count;
		count += delta_count;
	}
	output.SetChildCardinality(count);
}

static unique_ptr<NodeStatistics> CsrVerticesCardinality(ClientContext &, const FunctionData *bind_data) {
	auto &data = bind_data->Cast<CsrVerticesBindData>();
	return make_uniq<NodeStatistics>(data.estimated_count, data.estimated_count);
}

static unique_ptr<FunctionData> NeighborsBind(ClientContext &, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<CsrBindData>();
	result->graph_name = input.inputs[0].GetValue<string>();
	auto vertex_id = input.inputs[1].GetValue<int64_t>();
	if (vertex_id < 0) {
		throw BinderException("GQL vertex ID must be non-negative");
	}
	result->vertex_id = NumericCast<uint64_t>(vertex_id);
	result->direction = StringUtil::Lower(input.inputs[2].GetValue<string>());
	if (result->direction != "out" && result->direction != "in") {
		throw BinderException("GQL CSR direction must be 'out' or 'in'");
	}
	names = {"neighbor_id", "edge_id"};
	return_types = {LogicalType::UBIGINT, LogicalType::UBIGINT};
	return std::move(result);
}

struct NeighborRowsState : GlobalTableFunctionState {
	bool initialized = false;
	idx_t offset = 0;
	vector<pair<uint64_t, uint64_t>> rows;
};

static unique_ptr<GlobalTableFunctionState> NeighborsInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<NeighborRowsState>();
}

static void NeighborsFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<NeighborRowsState>();
	if (!state.initialized) {
		auto &data = input.bind_data->Cast<CsrBindData>();
		auto lease = GqlGetCsrLease(context, data.graph_name);
		const auto &snapshot = lease.snapshot;
		const auto *delta = lease.delta.get();
		idx_t vertex;
		if (CsrTryGetOrdinal(*snapshot, delta, data.vertex_id, vertex)) {
			const bool outgoing = data.direction == "out";
			if (vertex < snapshot->vertex_ids.size()) {
				const auto &offsets = outgoing ? snapshot->outgoing_offsets : snapshot->incoming_offsets;
				const auto &neighbors = outgoing ? snapshot->outgoing_neighbors : snapshot->incoming_neighbors;
				const auto &edge_ids = outgoing ? snapshot->outgoing_edge_ids : snapshot->incoming_edge_ids;
				for (idx_t index = offsets[vertex]; index < offsets[vertex + 1]; index++) {
					state.rows.emplace_back(snapshot->vertex_ids[neighbors[index]], edge_ids[index]);
				}
			}
			if (const auto *edges = CsrDeltaEdges(delta, vertex, outgoing)) {
				for (const auto &edge : *edges) {
					state.rows.emplace_back(CsrVertexId(*snapshot, delta, edge.neighbor), edge.edge_id);
				}
			}
		}
		state.initialized = true;
	}
	auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	for (idx_t index = 0; index < count; index++) {
		output.SetValue(0, index, Value::UBIGINT(state.rows[state.offset + index].first));
		output.SetValue(1, index, Value::UBIGINT(state.rows[state.offset + index].second));
	}
	state.offset += count;
	output.SetChildCardinality(count);
}

struct CsrExpandLocalState : LocalTableFunctionState {
	CsrExpandLocalState() : selection(STANDARD_VECTOR_SIZE) {
	}

	shared_ptr<const GqlCsrSnapshot> snapshot;
	shared_ptr<const GqlCsrDelta> delta;
	//! Delta edges of the current row, emitted after its adjacency range.
	const vector<GqlCsrDeltaEdge> *delta_edges = nullptr;
	idx_t delta_cursor = 0;
	string graph_name;
	string direction;
	string edge_label;
	string raw_edge_label;
	string raw_neighbor_label;
	//! Input row whose adjacency range is being emitted, and the next one to start.
	idx_t current_row = 0;
	idx_t next_row = 0;
	idx_t cursor = 0;
	idx_t end = 0;
	bool input_ready = false;
	bool active = false;
	bool outgoing = true;
	bool label_exists = true;
	uint32_t label_id = 0;
	bool filter_neighbors = false;
	bool neighbor_label_exists = true;
	vector<uint32_t> neighbor_label_ids;
	uint64_t vertex_id = 0;
	SelectionVector selection;
};

static unique_ptr<FunctionData> CsrExpandBind(ClientContext &, TableFunctionBindInput &,
                                              vector<LogicalType> &return_types, vector<Identifier> &names) {
	names = {"__gql_edge_id", "__gql_source_id", "__gql_target_id", "__gql_type"};
	return_types = {LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::VARCHAR};
	return make_uniq<GqlBatchedInOutBindData>();
}

struct CsrExpandGlobalState : GlobalTableFunctionState {
	// A physical expansion operator may receive many one-row lateral inputs and
	// may create several local states. Pin one immutable snapshot per graph for
	// the operator lifetime so seed processing never repeats catalog/version
	// validation.
	mutex lock;
	unordered_map<string, GqlCsrLease> leases;
};

static unique_ptr<GlobalTableFunctionState> CsrExpandGlobalInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<CsrExpandGlobalState>();
}

static unique_ptr<LocalTableFunctionState> CsrExpandLocalInit(ExecutionContext &, TableFunctionInitInput &,
                                                              GlobalTableFunctionState *) {
	return make_uniq<CsrExpandLocalState>();
}

static GqlCsrLease AcquireCsrOperatorLease(ExecutionContext &context, CsrExpandGlobalState &global,
                                           const string &graph_name) {
	lock_guard<mutex> guard(global.lock);
	auto entry = global.leases.find(graph_name);
	if (entry != global.leases.end()) {
		return entry->second;
	}
	auto lease = GqlGetCsrLease(context.client, graph_name);
	global.leases.emplace(graph_name, lease);
	return lease;
}

static bool SameString(const string_t &value, const string &other) {
	return value.GetSize() == other.size() && memcmp(value.GetData(), other.data(), other.size()) == 0;
}

// Emits the input columns a lateral join expects back, aligned with the output.
static void EmitBatchedPassthrough(const GqlBatchedInOutBindData &bind, DataChunk &input, DataChunk &output,
                                   const SelectionVector &selection, idx_t count) {
	const auto base = output.ColumnCount() - bind.passthrough_input.size();
	for (idx_t index = 0; index < bind.passthrough_input.size(); index++) {
		output.data[base + index].Slice(input.data[bind.passthrough_input[index]], selection, count);
	}
}

//! Starts the adjacency range of one input row. Returns false when the row
//! changes the edge label, so the caller can flush its constant type column.
static bool BeginCsrExpansionRow(ExecutionContext &context, const DataChunk &input, idx_t row, idx_t emitted,
                                 CsrExpandGlobalState &global, CsrExpandLocalState &state) {
	state.active = false;
	if (!FlatVector::Validity(input.data[0]).RowIsValid(row)) {
		return true;
	}
	auto &fields = StructVector::GetEntries(input.data[0]);
	if (fields.size() < 4 || !FlatVector::Validity(fields[0]).RowIsValid(row) ||
	    !FlatVector::Validity(fields[2]).RowIsValid(row) || !FlatVector::Validity(fields[3]).RowIsValid(row)) {
		throw InvalidInputException("GQL CSR expansion requires graph, direction, and edge label");
	}
	const auto &raw_label = FlatVector::GetData<string_t>(fields[3])[row];
	if (emitted > 0 && !SameString(raw_label, state.raw_edge_label)) {
		return false;
	}
	const auto &raw_direction = FlatVector::GetData<string_t>(fields[2])[row];
	if (!SameString(raw_direction, state.direction)) {
		state.direction = raw_direction.GetString();
		auto direction = StringUtil::Lower(state.direction);
		if (direction != "out" && direction != "in") {
			throw InvalidInputException("GQL CSR expansion direction must be 'out' or 'in'");
		}
		state.outgoing = direction == "out";
	}
	const auto &raw_graph = FlatVector::GetData<string_t>(fields[0])[row];
	if (!state.snapshot || !SameString(raw_graph, state.graph_name)) {
		state.graph_name = raw_graph.GetString();
		auto lease = AcquireCsrOperatorLease(context, global, state.graph_name);
		state.snapshot = lease.snapshot;
		state.delta = lease.delta;
		state.raw_edge_label.clear();
		state.edge_label.clear();
		state.raw_neighbor_label.clear();
		state.filter_neighbors = false;
		state.neighbor_label_exists = true;
	}
	if (state.edge_label.empty() || !SameString(raw_label, state.raw_edge_label)) {
		state.raw_edge_label = raw_label.GetString();
		state.edge_label = StringUtil::Lower(state.raw_edge_label);
		if (state.edge_label.empty()) {
			throw InvalidInputException("GQL CSR expansion requires one edge label");
		}
		auto label = state.snapshot->label_ids.find(state.edge_label);
		state.label_exists = label != state.snapshot->label_ids.end();
		state.label_id = state.label_exists ? label->second : 0;
	}
	if (fields.size() == 5 && !FlatVector::Validity(fields[4]).RowIsValid(row)) {
		state.raw_neighbor_label.clear();
		state.filter_neighbors = false;
		state.neighbor_label_exists = true;
	} else if (fields.size() == 5) {
		const auto &raw_neighbor = FlatVector::GetData<string_t>(fields[4])[row];
		if (!SameString(raw_neighbor, state.raw_neighbor_label)) {
			state.raw_neighbor_label = raw_neighbor.GetString();
			const auto &snapshot = *state.snapshot;
			state.neighbor_label_exists = true;
			state.neighbor_label_ids.clear();
			for (const auto &name : StringUtil::Split(state.raw_neighbor_label, ';')) {
				auto label = snapshot.label_ids.find(StringUtil::Lower(name));
				state.neighbor_label_exists &= label != snapshot.label_ids.end();
				if (label != snapshot.label_ids.end()) {
					state.neighbor_label_ids.push_back(label->second);
				}
			}
			state.filter_neighbors = GqlCsrHasVertexLabels(snapshot);
		}
	}
	if (!FlatVector::Validity(fields[1]).RowIsValid(row)) {
		return true;
	}
	state.vertex_id = FlatVector::GetData<uint64_t>(fields[1])[row];
	idx_t vertex;
	if (!CsrTryGetOrdinal(*state.snapshot, state.delta.get(), state.vertex_id, vertex)) {
		return true;
	}
	const auto &offsets = state.outgoing ? state.snapshot->outgoing_offsets : state.snapshot->incoming_offsets;
	const bool in_base = vertex < state.snapshot->vertex_ids.size();
	state.cursor = in_base ? offsets[vertex] : 0;
	state.end = in_base ? offsets[vertex + 1] : 0;
	state.delta_edges = CsrDeltaEdges(state.delta.get(), vertex, state.outgoing);
	state.delta_cursor = 0;
	state.active = state.label_exists && (!state.filter_neighbors || state.neighbor_label_exists);
	state.current_row = row;
	return true;
}

// Emits whole vectors across input rows. Stopping after each seed would make all
// downstream operators run a vectorized loop per seed instead of per 2048 rows.
static OperatorResultType CsrExpandFunction(ExecutionContext &context, TableFunctionInput &data_p, DataChunk &input,
                                            DataChunk &output) {
	auto &state = data_p.local_state->Cast<CsrExpandLocalState>();
	auto &global = data_p.global_state->Cast<CsrExpandGlobalState>();
	auto &bind = data_p.bind_data->Cast<GqlBatchedInOutBindData>();
	if (input.ColumnCount() < 1) {
		throw InternalException("GQL CSR expansion requires a struct input column");
	}
	if (!state.input_ready) {
		input.data[0].Flatten();
		state.next_row = 0;
		state.active = false;
		state.input_ready = true;
	}
	auto edge_output = FlatVector::GetDataMutable<uint64_t>(output.data[0]);
	auto source_output = FlatVector::GetDataMutable<uint64_t>(output.data[1]);
	auto target_output = FlatVector::GetDataMutable<uint64_t>(output.data[2]);
	idx_t count = 0;
	bool label_changed = false;
	while (count < STANDARD_VECTOR_SIZE) {
		if (!state.active) {
			if (state.next_row >= input.size()) {
				break;
			}
			if (!BeginCsrExpansionRow(context, input, state.next_row, count, global, state)) {
				label_changed = true;
				break;
			}
			state.next_row++;
			if (!state.active) {
				continue;
			}
		}
		const auto &snapshot = *state.snapshot;
		const auto &neighbors = state.outgoing ? snapshot.outgoing_neighbors : snapshot.incoming_neighbors;
		const auto &edge_ids = state.outgoing ? snapshot.outgoing_edge_ids : snapshot.incoming_edge_ids;
		const auto &label_ids = state.outgoing ? snapshot.outgoing_label_ids : snapshot.incoming_label_ids;
		while (state.cursor < state.end && count < STANDARD_VECTOR_SIZE) {
			auto index = state.cursor++;
			if (label_ids[index] != state.label_id) {
				continue;
			}
			if (state.filter_neighbors) {
				const auto neighbor = neighbors[index];
				const auto labels_begin = snapshot.vertex_label_ids.begin() + snapshot.vertex_label_offsets[neighbor];
				const auto labels_end = snapshot.vertex_label_ids.begin() + snapshot.vertex_label_offsets[neighbor + 1];
				bool matches = true;
				for (const auto required : state.neighbor_label_ids) {
					matches = matches && std::find(labels_begin, labels_end, required) != labels_end;
				}
				if (!matches) {
					continue;
				}
			}
			auto neighbor_id = snapshot.vertex_ids[neighbors[index]];
			edge_output[count] = edge_ids[index];
			source_output[count] = state.outgoing ? state.vertex_id : neighbor_id;
			target_output[count] = state.outgoing ? neighbor_id : state.vertex_id;
			state.selection.set_index(count, state.current_row);
			count++;
		}
		if (state.delta_edges && state.cursor >= state.end) {
			const auto *delta = state.delta.get();
			while (state.delta_cursor < state.delta_edges->size() && count < STANDARD_VECTOR_SIZE) {
				const auto &edge = (*state.delta_edges)[state.delta_cursor++];
				if (edge.label_id != state.label_id ||
				    (state.filter_neighbors &&
				     !CsrVertexHasLabels(snapshot, delta, edge.neighbor, state.neighbor_label_ids))) {
					continue;
				}
				auto neighbor_id = CsrVertexId(snapshot, delta, edge.neighbor);
				edge_output[count] = edge.edge_id;
				source_output[count] = state.outgoing ? state.vertex_id : neighbor_id;
				target_output[count] = state.outgoing ? neighbor_id : state.vertex_id;
				state.selection.set_index(count, state.current_row);
				count++;
			}
		}
		state.active = state.cursor < state.end || (state.delta_edges && state.delta_cursor < state.delta_edges->size());
	}
	if (count > 0) {
		output.data[3].Reference(Value(state.edge_label), count_t(count));
		EmitBatchedPassthrough(bind, input, output, state.selection, count);
	}
	output.SetChildCardinality(count);
	if (label_changed || state.active || state.next_row < input.size()) {
		return OperatorResultType::HAVE_MORE_OUTPUT;
	}
	state.input_ready = false;
	return OperatorResultType::NEED_MORE_INPUT;
}

struct ElementFetchBindData : GqlBatchedInOutBindData {
	ElementFetchBindData(string graph_name_p, string element_kind_p, TableCatalogEntry &table_p)
	    : graph_name(std::move(graph_name_p)), element_kind(std::move(element_kind_p)), table(table_p) {
	}

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<ElementFetchBindData>(graph_name, element_kind, table);
		result->column_ids = column_ids;
		result->projected = projected;
		result->passthrough_input = passthrough_input;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ElementFetchBindData>();
		return graph_name == other.graph_name && element_kind == other.element_kind && &table == &other.table &&
		       column_ids == other.column_ids && projected == other.projected &&
		       passthrough_input == other.passthrough_input;
	}

	string graph_name;
	string element_kind;
	TableCatalogEntry &table;
	vector<idx_t> column_ids;
	bool projected = false;
};

static unique_ptr<FunctionData> ElementFetchBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto graph_name = GqlGetSelectedGraph(context);
	const bool projected = input.table_function.GetName() == "gql_vertex_fetch_projected";
	string element_kind = input.table_function.GetName() == "gql_vertex_fetch" || projected ? "vertex" : "edge";

	// The prepared snapshot already records the native managed table. Reloading
	// all graph metadata here repeats catalog SQL for every projected fetch.
	auto snapshot = GqlGetCsrSnapshot(context, graph_name);
	const bool vertex = element_kind == "vertex";
	const auto &catalog_name = vertex ? snapshot->vertex_catalog_name : snapshot->edge_catalog_name;
	const auto &schema_name = vertex ? snapshot->vertex_schema_name : snapshot->edge_schema_name;
	const auto &table_name = vertex ? snapshot->vertex_table_name : snapshot->edge_table_name;
	auto has_row_ids = element_kind == "vertex" ? snapshot->HasVertexRowIds() : snapshot->edge_ids_match_rowids;
	if (table_name.empty() || !has_row_ids) {
		throw BinderException("GQL element fetch requires a managed graph with a current physical row mapping");
	}

	auto &catalog = Catalog::GetCatalog(context, Identifier(catalog_name));
	auto table_entry = catalog.GetEntry(context, CatalogType::TABLE_ENTRY, Identifier(schema_name),
	                                    Identifier(table_name), OnEntryNotFound::THROW_EXCEPTION);
	auto &table = table_entry->Cast<TableCatalogEntry>();
	if (!table.IsDuckTable()) {
		throw BinderException("GQL element fetch requires native DuckDB table storage");
	}
	auto result = make_uniq<ElementFetchBindData>(std::move(graph_name), std::move(element_kind), table);
	result->projected = projected;
	unordered_set<string> requested;
	if (projected) {
		if (input.input_table_types.size() != 1 || input.input_table_types[0].id() != LogicalTypeId::STRUCT) {
			throw BinderException("Projected vertex fetch requires an ID and column-schema struct");
		}
		const auto &fields = StructType::GetChildTypes(input.input_table_types[0]);
		if (fields.size() != 2 || fields[0].first != "vertex_id" || fields[0].second != LogicalType::UBIGINT ||
		    fields[1].first != "columns" || fields[1].second.id() != LogicalTypeId::STRUCT) {
			throw BinderException("Projected vertex fetch requires vertex_id UBIGINT and columns STRUCT");
		}
		for (const auto &field : StructType::GetChildTypes(fields[1].second)) {
			requested.insert(StringUtil::Lower(field.first.GetIdentifierName()));
		}
	}
	for (const auto &column : table.GetColumns().Logical()) {
		if (projected && requested.erase(StringUtil::Lower(column.Name().GetIdentifierName())) == 0) {
			continue;
		}
		names.push_back(column.Name());
		return_types.push_back(column.Type());
		result->column_ids.push_back(column.Logical().index);
	}
	if (!requested.empty()) {
		throw BinderException("Unknown projected vertex column '%s'", *requested.begin());
	}
	return std::move(result);
}

struct ElementFetchGlobalState : GlobalTableFunctionState {
	explicit ElementFetchGlobalState(TableCatalogEntry &table_p) : table(table_p) {
	}

	TableCatalogEntry &table;
	vector<StorageIndex> column_ids;
	shared_ptr<const GqlCsrSnapshot> snapshot;
	shared_ptr<const GqlCsrDelta> delta;
};

static unique_ptr<GlobalTableFunctionState> ElementFetchGlobalInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &data = input.bind_data->Cast<ElementFetchBindData>();
	auto result = make_uniq<ElementFetchGlobalState>(data.table);
	// Validate once per execution, then pin the immutable identity mapping for
	// all lateral batches. Writes invalidate the snapshot before the next query.
	auto lease = GqlGetCsrLease(context, data.graph_name);
	result->snapshot = lease.snapshot;
	result->delta = lease.delta;
	for (const auto &column : input.column_indexes) {
		if (column.IsVirtualColumn()) {
			throw NotImplementedException("GQL element fetch does not expose virtual columns");
		}
		auto mapped = ColumnIndex(data.column_ids.at(column.GetPrimaryIndex()), column.GetChildIndexes());
		result->column_ids.push_back(data.table.GetStorageIndex(mapped));
	}
	return std::move(result);
}

struct ElementFetchLocalState : LocalTableFunctionState {
	ElementFetchLocalState() : selection(STANDARD_VECTOR_SIZE) {
	}

	ColumnFetchState fetch_state;
	vector<pair<row_t, sel_t>> requests;
	vector<row_t> row_ids;
	SelectionVector selection;
	bool emitted = false;
};

static unique_ptr<LocalTableFunctionState> ElementFetchLocalInit(ExecutionContext &, TableFunctionInitInput &,
                                                                 GlobalTableFunctionState *) {
	return make_uniq<ElementFetchLocalState>();
}

// Storage merges consecutive row IDs of one row group, so a sorted batch decodes
// each segment once. A chunk stays bounded by the vector size of its input.
static OperatorResultType ElementFetchFunction(ExecutionContext &context, TableFunctionInput &data_p, DataChunk &input,
                                               DataChunk &output) {
	auto &local = data_p.local_state->Cast<ElementFetchLocalState>();
	auto &bind = data_p.bind_data->Cast<ElementFetchBindData>();
	auto &global = data_p.global_state->Cast<ElementFetchGlobalState>();
	// A constant-argument call repeats on the same input until a call returns no
	// rows, so every chunk of rows must be followed by an empty NEED_MORE_INPUT.
	if (local.emitted) {
		local.emitted = false;
		output.SetChildCardinality(0);
		return OperatorResultType::NEED_MORE_INPUT;
	}
	input.data[0].Flatten();
	const auto &id_vector = bind.projected ? StructVector::GetEntries(input.data[0])[0] : input.data[0];
	const auto ids = FlatVector::GetData<uint64_t>(id_vector);
	const auto &id_validity = FlatVector::Validity(id_vector);
	const auto &input_validity = FlatVector::Validity(input.data[0]);
	const bool vertex = bind.element_kind == "vertex";
	local.requests.clear();
	for (idx_t row = 0; row < input.size(); row++) {
		if (!input_validity.RowIsValid(row) || !id_validity.RowIsValid(row)) {
			continue;
		}
		const auto canonical_id = ids[row];
		if (vertex) {
			idx_t ordinal;
			if (!CsrTryGetOrdinal(*global.snapshot, global.delta.get(), canonical_id, ordinal)) {
				continue;
			}
			auto row_id = CsrVertexRowId(*global.snapshot, global.delta.get(), ordinal);
			local.requests.emplace_back(NumericCast<row_t>(row_id), NumericCast<sel_t>(row));
		} else if (canonical_id != 0) {
			local.requests.emplace_back(NumericCast<row_t>(canonical_id - 1), NumericCast<sel_t>(row));
		}
	}
	if (local.requests.empty()) {
		output.SetChildCardinality(0);
		return OperatorResultType::NEED_MORE_INPUT;
	}
	std::sort(local.requests.begin(), local.requests.end());
	local.row_ids.resize(local.requests.size());
	for (idx_t index = 0; index < local.requests.size(); index++) {
		local.row_ids[index] = local.requests[index].first;
		local.selection.set_index(index, local.requests[index].second);
	}

	auto &table = global.table.Cast<DuckTableEntry>();
	auto &storage = table.GetStorage();
	auto &transaction = DuckTransaction::Get(context.client, table.catalog);
	Vector row_ids(LogicalType::ROW_TYPE, data_ptr_cast(local.row_ids.data()), local.row_ids.size());
	storage.Fetch(transaction, output, global.column_ids, row_ids, local.row_ids.size(), local.fetch_state);
	auto count = output.size();
	if (count != local.row_ids.size() && !bind.passthrough_input.empty()) {
		// Fetch packs its result when a row is invisible to this transaction, which
		// loses the row-to-seed alignment. Recover it by fetching rows individually.
		vector<LogicalType> fetched_types;
		for (idx_t column = 0; column < global.column_ids.size(); column++) {
			fetched_types.push_back(output.data[column].GetType());
		}
		DataChunk single;
		single.Initialize(Allocator::Get(context.client), fetched_types);
		count = 0;
		for (idx_t index = 0; index < local.row_ids.size(); index++) {
			single.Reset();
			Vector single_row_id(LogicalType::ROW_TYPE, data_ptr_cast(&local.row_ids[index]), 1);
			storage.Fetch(transaction, single, global.column_ids, single_row_id, 1, local.fetch_state);
			if (single.size() == 0) {
				continue;
			}
			for (idx_t column = 0; column < fetched_types.size(); column++) {
				VectorOperations::Copy(single.data[column], output.data[column], 1, 0, count);
			}
			local.selection.set_index(count, local.requests[index].second);
			count++;
		}
	}
	output.SetChildCardinality(count);
	if (count == 0) {
		return OperatorResultType::NEED_MORE_INPUT;
	}
	EmitBatchedPassthrough(bind, input, output, local.selection, count);
	output.SetChildCardinality(count);
	local.emitted = true;
	return OperatorResultType::HAVE_MORE_OUTPUT;
}

// ANY SHORTEST: one row per reached vertex with its distance, instead of every path.
// A breadth-first search is exact here, because all edges cost one hop. Below the
// minimum length a vertex may be revisited, since a longer walk can still qualify.
struct CsrShortestLocalState : LocalTableFunctionState {
	shared_ptr<const GqlCsrSnapshot> snapshot;
	shared_ptr<const GqlCsrDelta> delta;
	string edge_label;
	bool outgoing = true;
	uint64_t start_id = 0;
	bool searched = false;
	vector<pair<idx_t, uint32_t>> reached;
	idx_t cursor = 0;
};

static unique_ptr<FunctionData> CsrShortestBind(ClientContext &, TableFunctionBindInput &,
                                                vector<LogicalType> &return_types, vector<Identifier> &names) {
	names = {"__gql_edge_id", "__gql_source_id", "__gql_target_id", "__gql_type", "__gql_length"};
	return_types = {LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::VARCHAR,
	                LogicalType::UBIGINT};
	return make_uniq<TableFunctionData>();
}

static unique_ptr<LocalTableFunctionState> CsrShortestLocalInit(ExecutionContext &, TableFunctionInitInput &,
                                                                GlobalTableFunctionState *) {
	return make_uniq<CsrShortestLocalState>();
}

static void SearchCsrShortest(ExecutionContext &context, DataChunk &input, CsrExpandGlobalState &global,
                              CsrShortestLocalState &state) {
	state.reached.clear();
	state.cursor = 0;
	if (input.size() != 1 || input.ColumnCount() < 1) {
		throw InternalException("GQL shortest expansion requires one lateral input row");
	}
	auto packed = input.data[0].GetValue(0);
	if (packed.IsNull()) {
		return;
	}
	auto &fields = StructValue::GetChildren(packed);
	if (fields.size() != 7 || fields[0].IsNull() || fields[2].IsNull() || fields[3].IsNull() || fields[4].IsNull() ||
	    fields[5].IsNull() || fields[6].IsNull()) {
		throw InvalidInputException("GQL shortest expansion requires a graph, direction, label, and bounds");
	}
	if (fields[1].IsNull()) {
		return;
	}
	const auto direction = StringUtil::Lower(fields[2].GetValue<string>());
	if (direction != "out" && direction != "in") {
		throw InvalidInputException("GQL shortest expansion direction must be 'out' or 'in'");
	}
	state.outgoing = direction == "out";
	state.edge_label = StringUtil::Lower(fields[3].GetValue<string>());
	auto lease = AcquireCsrOperatorLease(context, global, fields[0].GetValue<string>());
	state.snapshot = lease.snapshot;
	state.delta = lease.delta;
	state.start_id = fields[1].GetValue<uint64_t>();
	const auto &snapshot = *state.snapshot;
	const auto *delta = state.delta.get();
	const auto label = snapshot.label_ids.find(state.edge_label);
	idx_t start;
	if (label == snapshot.label_ids.end() || !CsrTryGetOrdinal(snapshot, delta, state.start_id, start)) {
		return;
	}
	const auto minimum = fields[4].GetValue<uint64_t>();
	const auto maximum = fields[5].GetValue<uint64_t>();
	const auto unbounded = fields[6].GetValue<bool>();
	unordered_set<idx_t> settled;
	vector<idx_t> frontier {start};
	for (uint64_t depth = 1; !frontier.empty() && (unbounded || depth <= maximum); depth++) {
		context.client.InterruptCheck();
		vector<idx_t> next;
		unordered_set<idx_t> level;
		for (const auto vertex : frontier) {
			CsrForEachEdge(snapshot, delta, vertex, state.outgoing, label->second, [&](idx_t neighbor, uint64_t) {
				if (settled.count(neighbor) != 0 || !level.insert(neighbor).second) {
					return;
				}
				next.push_back(neighbor);
				if (depth >= minimum) {
					state.reached.emplace_back(neighbor, NumericCast<uint32_t>(depth));
				}
			});
		}
		if (depth >= minimum) {
			settled.insert(next.begin(), next.end());
		}
		frontier = std::move(next);
	}
}

static OperatorResultType CsrShortestFunction(ExecutionContext &context, TableFunctionInput &data_p, DataChunk &input,
                                              DataChunk &output) {
	auto &state = data_p.local_state->Cast<CsrShortestLocalState>();
	if (!state.searched) {
		SearchCsrShortest(context, input, data_p.global_state->Cast<CsrExpandGlobalState>(), state);
		state.searched = true;
	}
	const auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.reached.size() - state.cursor);
	if (count == 0) {
		state.searched = false;
		output.SetChildCardinality(0);
		return OperatorResultType::NEED_MORE_INPUT;
	}
	auto edges = FlatVector::GetDataMutable<uint64_t>(output.data[0]);
	auto sources = FlatVector::GetDataMutable<uint64_t>(output.data[1]);
	auto targets = FlatVector::GetDataMutable<uint64_t>(output.data[2]);
	auto lengths = FlatVector::GetDataMutable<uint64_t>(output.data[4]);
	output.data[3].Reference(Value(state.edge_label), count_t(count));
	for (idx_t index = 0; index < count; index++) {
		const auto &entry = state.reached[state.cursor + index];
		const auto end_id = CsrVertexId(*state.snapshot, state.delta.get(), entry.first);
		edges[index] = 0;
		sources[index] = state.outgoing ? state.start_id : end_id;
		targets[index] = state.outgoing ? end_id : state.start_id;
		lengths[index] = entry.second;
	}
	state.cursor += count;
	output.SetChildCardinality(count);
	return OperatorResultType::HAVE_MORE_OUTPUT;
}

// ALL SHORTEST between two given vertices. A breadth-first search records each vertex's
// predecessors up to the target's level, and only paths back from the target are built,
// so the cost follows the paths between the endpoints and not all paths from the source.
struct CsrAllShortestBindData : TableFunctionData {
	explicit CsrAllShortestBindData(TableCatalogEntry *table_p) : table(table_p) {
	}

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<CsrAllShortestBindData>(table);
		result->value_column = value_column;
		result->value_type = value_type;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<CsrAllShortestBindData>();
		return table == other.table && value_column == other.value_column;
	}

	TableCatalogEntry *table;
	optional_idx value_column;
	LogicalType value_type;
};

struct CsrAllShortestStep {
	idx_t vertex;
	idx_t neighbor;
	uint64_t edge_id;
};

struct CsrAllShortestRow {
	uint64_t edge_id;
	uint64_t source_id;
	uint64_t target_id;
	idx_t path;
};

struct CsrAllShortestLocalState : LocalTableFunctionState {
	shared_ptr<const GqlCsrSnapshot> snapshot;
	shared_ptr<const GqlCsrDelta> delta;
	string edge_label;
	bool searched = false;
	uint64_t length = 0;
	vector<CsrAllShortestRow> rows;
	vector<Value> values;
	idx_t cursor = 0;
	ColumnFetchState fetch_state;
};

static unique_ptr<FunctionData> CsrAllShortestBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	names = {"__gql_edge_id", "__gql_source_id", "__gql_target_id", "__gql_type", "__gql_length"};
	return_types = {LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::VARCHAR,
	                LogicalType::UBIGINT};
	if (input.input_table_types.size() != 1 || input.input_table_types[0].id() != LogicalTypeId::STRUCT) {
		throw BinderException("GQL ALL SHORTEST requires a struct argument");
	}
	const auto &fields = StructType::GetChildTypes(input.input_table_types[0]);
	if (fields.size() != 9 && fields.size() != 10) {
		throw BinderException("GQL ALL SHORTEST received an invalid argument");
	}
	if (fields.size() == 9) {
		return make_uniq<CsrAllShortestBindData>(nullptr);
	}
	GqlTableGraphBinding binding;
	if (!GqlTryLoadTableGraph(context, GqlGetSelectedGraph(context), binding) ||
	    !StringUtil::CIEquals(binding.vertex.ownership, "MANAGED")) {
		throw NotImplementedException("GQL path node values require a managed graph");
	}
	auto &catalog = Catalog::GetCatalog(context, Identifier(binding.vertex.catalog_name));
	auto entry = catalog.GetEntry(context, CatalogType::TABLE_ENTRY, Identifier(binding.vertex.schema_name),
	                              Identifier(binding.vertex.table_name), OnEntryNotFound::THROW_EXCEPTION);
	auto &table = entry->Cast<TableCatalogEntry>();
	auto result = make_uniq<CsrAllShortestBindData>(&table);
	const auto column_name = StructType::GetChildTypes(fields[9].second)[0].first;
	auto &column = table.GetColumn(column_name);
	result->value_column = column.Logical().index;
	result->value_type = column.Type();
	names.emplace_back("__gql_node_values");
	return_types.push_back(LogicalType::LIST(column.Type()));
	return std::move(result);
}

static unique_ptr<LocalTableFunctionState> CsrAllShortestLocalInit(ExecutionContext &, TableFunctionInitInput &,
                                                                   GlobalTableFunctionState *) {
	return make_uniq<CsrAllShortestLocalState>();
}

static void SearchCsrAllShortest(ExecutionContext &context, DataChunk &input, const CsrAllShortestBindData &bind,
                                 CsrExpandGlobalState &global, CsrAllShortestLocalState &state) {
	state.rows.clear();
	state.values.clear();
	state.cursor = 0;
	if (input.size() != 1 || input.ColumnCount() < 1) {
		throw InternalException("GQL ALL SHORTEST requires one lateral input row");
	}
	auto packed = input.data[0].GetValue(0);
	if (packed.IsNull()) {
		return;
	}
	auto &fields = StructValue::GetChildren(packed);
	if (fields[1].IsNull() || fields[2].IsNull()) {
		return;
	}
	const auto outgoing = StringUtil::CIEquals(fields[3].GetValue<string>(), "out");
	state.edge_label = StringUtil::Lower(fields[4].GetValue<string>());
	const auto maximum = fields[5].GetValue<uint64_t>();
	const auto unbounded = fields[6].GetValue<bool>();
	const auto per_step = fields[7].GetValue<bool>();
	const auto from_left = fields[8].GetValue<bool>();
	auto lease = AcquireCsrOperatorLease(context, global, fields[0].GetValue<string>());
	state.snapshot = lease.snapshot;
	state.delta = lease.delta;
	const auto &snapshot = *state.snapshot;
	const auto *delta = state.delta.get();
	const auto label = snapshot.label_ids.find(state.edge_label);
	idx_t start, target;
	if (label == snapshot.label_ids.end() || !CsrTryGetOrdinal(snapshot, delta, fields[1].GetValue<uint64_t>(), start) ||
	    !CsrTryGetOrdinal(snapshot, delta, fields[2].GetValue<uint64_t>(), target)) {
		return;
	}
	// The start is not settled, so that it can be its own target through a cycle.
	unordered_map<idx_t, vector<CsrAllShortestStep>> predecessors;
	vector<idx_t> frontier {start};
	uint64_t depth = 0;
	bool found = false;
	while (!found && !frontier.empty() && (unbounded || depth < maximum)) {
		context.client.InterruptCheck();
		depth++;
		unordered_map<idx_t, vector<CsrAllShortestStep>> level;
		for (const auto vertex : frontier) {
			CsrForEachEdge(snapshot, delta, vertex, outgoing, label->second, [&](idx_t neighbor, uint64_t edge_id) {
				if (predecessors.count(neighbor) == 0) {
					level[neighbor].push_back({vertex, neighbor, edge_id});
				}
			});
		}
		frontier.clear();
		for (auto &entry : level) {
			found = found || entry.first == target;
			frontier.push_back(entry.first);
			predecessors.emplace(entry.first, std::move(entry.second));
		}
	}
	if (!found) {
		return;
	}
	state.length = depth;
	// Walks back from the target. A start reached again above level zero has no paths
	// through it, because anything it could reach was settled at level one.
	vector<CsrAllShortestStep> path(depth);
	vector<vector<idx_t>> node_paths;
	idx_t path_index = 0;
	std::function<void(idx_t, uint64_t)> walk = [&](idx_t vertex, uint64_t level) {
		if ((path_index & 1023) == 0) {
			context.client.InterruptCheck();
		}
		if (level == 0) {
			if (vertex != start) {
				return;
			}
			vector<idx_t> nodes {start};
			for (const auto &step : path) {
				nodes.push_back(step.neighbor);
			}
			if (!from_left) {
				std::reverse(nodes.begin(), nodes.end());
			}
			if (bind.value_column.IsValid()) {
				node_paths.push_back(std::move(nodes));
			}
			if (!per_step) {
				state.rows.push_back(
				    {0, CsrVertexId(snapshot, delta, start), CsrVertexId(snapshot, delta, target), path_index});
			} else {
				for (const auto &step : path) {
					const auto from = CsrVertexId(snapshot, delta, step.vertex);
					const auto to = CsrVertexId(snapshot, delta, step.neighbor);
					state.rows.push_back({step.edge_id, outgoing ? from : to, outgoing ? to : from, path_index});
				}
			}
			path_index++;
			return;
		}
		auto entry = predecessors.find(vertex);
		if (entry == predecessors.end()) {
			return;
		}
		for (const auto &step : entry->second) {
			path[level - 1] = step;
			walk(step.vertex, level - 1);
		}
	};
	walk(target, depth);
	if (!bind.value_column.IsValid()) {
		return;
	}
	// Each vertex's value is fetched once, however many paths it lies on.
	vector<pair<row_t, idx_t>> requests;
	unordered_map<idx_t, Value> value_by_vertex;
	for (const auto &nodes : node_paths) {
		for (const auto node : nodes) {
			if (value_by_vertex.emplace(node, Value(bind.value_type)).second) {
				const auto row_id = CsrVertexRowId(snapshot, delta, node);
				requests.emplace_back(NumericCast<row_t>(row_id), node);
			}
		}
	}
	std::sort(requests.begin(), requests.end());
	auto &table = bind.table->Cast<DuckTableEntry>();
	auto &transaction = DuckTransaction::Get(context.client, table.catalog);
	vector<StorageIndex> column_ids {table.GetStorageIndex(ColumnIndex(bind.value_column.GetIndex()))};
	DataChunk fetched;
	fetched.Initialize(Allocator::Get(context.client), {bind.value_type});
	for (idx_t index = 0; index < requests.size(); index++) {
		fetched.Reset();
		auto row_id = requests[index].first;
		Vector row_ids(LogicalType::ROW_TYPE, data_ptr_cast(&row_id), 1);
		table.GetStorage().Fetch(transaction, fetched, column_ids, row_ids, 1, state.fetch_state);
		if (fetched.size() == 1) {
			value_by_vertex[requests[index].second] = fetched.GetValue(0, 0);
		}
	}
	for (const auto &nodes : node_paths) {
		vector<Value> values;
		for (const auto node : nodes) {
			values.push_back(value_by_vertex.at(node));
		}
		state.values.push_back(Value::LIST(bind.value_type, std::move(values)));
	}
}

static OperatorResultType CsrAllShortestFunction(ExecutionContext &context, TableFunctionInput &data_p, DataChunk &input,
                                                 DataChunk &output) {
	auto &state = data_p.local_state->Cast<CsrAllShortestLocalState>();
	auto &bind = data_p.bind_data->Cast<CsrAllShortestBindData>();
	if (!state.searched) {
		SearchCsrAllShortest(context, input, bind, data_p.global_state->Cast<CsrExpandGlobalState>(), state);
		state.searched = true;
	}
	const auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.cursor);
	if (count == 0) {
		state.searched = false;
		output.SetChildCardinality(0);
		return OperatorResultType::NEED_MORE_INPUT;
	}
	auto edges = FlatVector::GetDataMutable<uint64_t>(output.data[0]);
	auto sources = FlatVector::GetDataMutable<uint64_t>(output.data[1]);
	auto targets = FlatVector::GetDataMutable<uint64_t>(output.data[2]);
	auto lengths = FlatVector::GetDataMutable<uint64_t>(output.data[4]);
	output.data[3].Reference(Value(state.edge_label), count_t(count));
	for (idx_t index = 0; index < count; index++) {
		const auto &row = state.rows[state.cursor + index];
		edges[index] = row.edge_id;
		sources[index] = row.source_id;
		targets[index] = row.target_id;
		lengths[index] = state.length;
		if (bind.value_column.IsValid()) {
			output.data[5].SetValue(index, state.values[row.path]);
		}
	}
	state.cursor += count;
	output.SetChildCardinality(count);
	return OperatorResultType::HAVE_MORE_OUTPUT;
}

struct VertexLookupBindData : ElementFetchBindData {
	using ElementFetchBindData::ElementFetchBindData;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<VertexLookupBindData>(graph_name, element_kind, table);
		result->column_ids = column_ids;
		result->projected = projected;
		result->index_column = index_column;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		return ElementFetchBindData::Equals(other_p) &&
		       index_column == other_p.Cast<VertexLookupBindData>().index_column;
	}

	string index_column;
};

static unique_ptr<FunctionData> VertexLookupBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto graph_name = GqlGetSelectedGraph(context);
	GqlTableGraphBinding binding;
	if (!GqlTryLoadTableGraph(context, graph_name, binding) ||
	    !StringUtil::CIEquals(binding.vertex.ownership, "MANAGED")) {
		throw BinderException("GQL vertex lookup requires a managed graph");
	}
	auto &catalog = Catalog::GetCatalog(context, Identifier(binding.vertex.catalog_name));
	auto entry = catalog.GetEntry(context, CatalogType::TABLE_ENTRY, Identifier(binding.vertex.schema_name),
	                              Identifier(binding.vertex.table_name), OnEntryNotFound::THROW_EXCEPTION);
	auto &table = entry->Cast<TableCatalogEntry>();
	if (!table.IsDuckTable()) {
		throw BinderException("GQL vertex lookup requires native DuckDB table storage");
	}
	if (input.input_table_types.size() != 1 || input.input_table_types[0].id() != LogicalTypeId::STRUCT) {
		throw BinderException("GQL vertex lookup requires a value, index, and column-schema struct");
	}
	const auto &fields = StructType::GetChildTypes(input.input_table_types[0]);
	if (fields.size() != 3 || fields[0].first != "value" || fields[1].first != "index" ||
	    fields[1].second.id() != LogicalTypeId::STRUCT || fields[2].first != "columns" ||
	    fields[2].second.id() != LogicalTypeId::STRUCT) {
		throw BinderException("GQL vertex lookup requires value, index STRUCT, and columns STRUCT");
	}
	auto result = make_uniq<VertexLookupBindData>(std::move(graph_name), "vertex", table);
	result->projected = true;
	result->index_column = StructType::GetChildTypes(fields[1].second)[0].first.GetIdentifierName();
	unordered_set<string> requested;
	for (const auto &field : StructType::GetChildTypes(fields[2].second)) {
		requested.insert(StringUtil::Lower(field.first.GetIdentifierName()));
	}
	for (const auto &column : table.GetColumns().Logical()) {
		if (requested.erase(StringUtil::Lower(column.Name().GetIdentifierName())) == 0) {
			continue;
		}
		names.push_back(column.Name());
		return_types.push_back(column.Type());
		result->column_ids.push_back(column.Logical().index);
	}
	if (!requested.empty()) {
		throw BinderException("Unknown GQL vertex lookup column '%s'", *requested.begin());
	}
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> VertexLookupGlobalInit(ClientContext &, TableFunctionInitInput &input) {
	auto &data = input.bind_data->Cast<VertexLookupBindData>();
	auto result = make_uniq<ElementFetchGlobalState>(data.table);
	for (const auto &column : input.column_indexes) {
		if (column.IsVirtualColumn()) {
			throw NotImplementedException("GQL vertex lookup does not expose virtual columns");
		}
		auto mapped = ColumnIndex(data.column_ids.at(column.GetPrimaryIndex()), column.GetChildIndexes());
		result->column_ids.push_back(data.table.GetStorageIndex(mapped));
	}
	return std::move(result);
}

struct VertexLookupLocalState : LocalTableFunctionState {
	ColumnFetchState fetch_state;
	vector<row_t> row_ids;
	idx_t cursor = 0;
	bool probed = false;
};

static unique_ptr<LocalTableFunctionState> VertexLookupLocalInit(ExecutionContext &, TableFunctionInitInput &,
                                                                 GlobalTableFunctionState *) {
	return make_uniq<VertexLookupLocalState>();
}

// Candidates only: Fetch applies visibility and the MATCH predicate stays above this.
static OperatorResultType VertexLookupFunction(ExecutionContext &context, TableFunctionInput &data_p, DataChunk &input,
                                               DataChunk &output) {
	auto &local = data_p.local_state->Cast<VertexLookupLocalState>();
	auto &bind = data_p.bind_data->Cast<VertexLookupBindData>();
	auto &global = data_p.global_state->Cast<ElementFetchGlobalState>();
	auto &table = global.table.Cast<DuckTableEntry>();
	if (!local.probed) {
		local.probed = true;
		local.row_ids.clear();
		local.cursor = 0;
		input.data[0].Flatten();
		const auto &values = StructVector::GetEntries(input.data[0])[0];
		vector<idx_t> matches;
		for (idx_t row = 0; row < input.size(); row++) {
			auto value = values.GetValue(row);
			if (value.IsNull()) {
				continue;
			}
			matches.clear();
			if (!GqlProbePropertyIndex(context.client, table, bind.index_column, value,
			                           NumericLimits<idx_t>::Maximum(), matches)) {
				throw InvalidInputException("GQL property index on '%s' is unavailable; prepare the statement again",
				                            bind.index_column);
			}
			for (const auto match : matches) {
				local.row_ids.push_back(NumericCast<row_t>(match));
			}
		}
		std::sort(local.row_ids.begin(), local.row_ids.end());
	}
	// Each call that emits rows is followed by another, which ends this input when empty.
	while (local.cursor < local.row_ids.size()) {
		const auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, local.row_ids.size() - local.cursor);
		Vector row_ids(LogicalType::ROW_TYPE, data_ptr_cast(local.row_ids.data() + local.cursor), count);
		auto &transaction = DuckTransaction::Get(context.client, table.catalog);
		table.GetStorage().Fetch(transaction, output, global.column_ids, row_ids, count, local.fetch_state);
		local.cursor += count;
		if (output.size() > 0) {
			return OperatorResultType::HAVE_MORE_OUTPUT;
		}
	}
	local.probed = false;
	output.SetChildCardinality(0);
	return OperatorResultType::NEED_MORE_INPUT;
}

// A cursor at or past base_end indexes delta_edges.
struct CsrPathFrame {
	idx_t vertex = 0;
	idx_t cursor = 0;
	idx_t end = 0;
	idx_t base_end = 0;
	const vector<GqlCsrDeltaEdge> *delta_edges = nullptr;
};

struct CsrPathExpandLocalState : LocalTableFunctionState {
	shared_ptr<const GqlCsrSnapshot> snapshot;
	shared_ptr<const GqlCsrDelta> delta;
	string graph_name;
	string edge_label;
	vector<CsrPathFrame> frames;
	vector<uint64_t> edge_ids;
	// Short trails avoid hash-table overhead. Once a trail reaches the threshold,
	// keep membership in sync through backtracking until this input is exhausted.
	unordered_set<uint64_t> active_edge_ids;
	bool indexed_edges = false;
	bool initialized = false;
	bool active = false;
	bool outgoing = true;
	bool label_exists = true;
	bool unbounded = false;
	bool zero_pending = false;
	uint32_t label_id = 0;
	idx_t minimum_repetitions = 1;
	idx_t maximum_repetitions = 1;
	uint64_t start_id = 0;
	uint64_t current_end_id = 0;
};

static unique_ptr<FunctionData> CsrPathExpandBind(ClientContext &, TableFunctionBindInput &,
                                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	names = {"__gql_edge_id", "__gql_source_id", "__gql_target_id", "__gql_type"};
	return_types = {LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::VARCHAR};
	return make_uniq<TableFunctionData>();
}

static unique_ptr<LocalTableFunctionState> CsrPathExpandLocalInit(ExecutionContext &, TableFunctionInitInput &,
                                                                  GlobalTableFunctionState *) {
	return make_uniq<CsrPathExpandLocalState>();
}

static void ResetCsrPathExpansionInput(CsrPathExpandLocalState &state) {
	state.frames.clear();
	state.edge_ids.clear();
	state.active_edge_ids.clear();
	state.indexed_edges = false;
	state.initialized = false;
	state.active = false;
	state.outgoing = true;
	state.unbounded = false;
	state.zero_pending = false;
	state.minimum_repetitions = 1;
	state.maximum_repetitions = 1;
	state.start_id = 0;
	state.current_end_id = 0;
}

static CsrPathFrame MakeCsrPathFrame(const CsrPathExpandLocalState &state, idx_t vertex) {
	CsrPathFrame frame;
	frame.vertex = vertex;
	if (vertex < state.snapshot->vertex_ids.size()) {
		const auto &offsets = state.outgoing ? state.snapshot->outgoing_offsets : state.snapshot->incoming_offsets;
		frame.cursor = NumericCast<idx_t>(offsets[vertex]);
		frame.base_end = NumericCast<idx_t>(offsets[vertex + 1]);
	}
	frame.delta_edges = CsrDeltaEdges(state.delta.get(), vertex, state.outgoing);
	frame.end = frame.base_end + (frame.delta_edges ? frame.delta_edges->size() : 0);
	return frame;
}

static void InitializeCsrPathExpansion(ExecutionContext &context, DataChunk &input, CsrExpandGlobalState &global,
                                       CsrPathExpandLocalState &state) {
	if (input.size() != 1 || input.ColumnCount() != 1) {
		throw InternalException("GQL CSR path expansion requires one lateral input row");
	}
	auto packed = input.data[0].GetValue(0);
	if (packed.IsNull()) {
		state.initialized = true;
		return;
	}
	auto &fields = StructValue::GetChildren(packed);
	if (fields.size() != 7 || fields[0].IsNull() || fields[2].IsNull() || fields[3].IsNull() || fields[4].IsNull() ||
	    fields[5].IsNull() || fields[6].IsNull()) {
		throw InvalidInputException("GQL CSR path expansion requires graph, direction, label, and bounds");
	}
	auto graph_name = fields[0].GetValue<string>();
	auto direction = StringUtil::Lower(fields[2].GetValue<string>());
	auto edge_label = StringUtil::Lower(fields[3].GetValue<string>());
	state.minimum_repetitions = NumericCast<idx_t>(fields[4].GetValue<uint64_t>());
	state.maximum_repetitions = NumericCast<idx_t>(fields[5].GetValue<uint64_t>());
	state.unbounded = fields[6].GetValue<bool>();
	if (direction != "out" && direction != "in") {
		throw InvalidInputException("GQL CSR path expansion direction must be 'out' or 'in'");
	}
	if (edge_label.empty() || (!state.unbounded && state.minimum_repetitions == 0) ||
	    (!state.unbounded && state.minimum_repetitions > state.maximum_repetitions)) {
		throw InvalidInputException("GQL CSR path expansion has an invalid label or repetition range");
	}
	state.outgoing = direction == "out";
	if (!state.snapshot || state.graph_name != graph_name) {
		auto lease = AcquireCsrOperatorLease(context, global, graph_name);
		state.snapshot = lease.snapshot;
		state.delta = lease.delta;
		state.graph_name = std::move(graph_name);
		state.edge_label.clear();
	}
	if (state.edge_label != edge_label) {
		state.edge_label = std::move(edge_label);
		auto label = state.snapshot->label_ids.find(state.edge_label);
		state.label_exists = label != state.snapshot->label_ids.end();
		state.label_id = state.label_exists ? label->second : 0;
	}
	if (fields[1].IsNull()) {
		state.initialized = true;
		return;
	}
	state.start_id = fields[1].GetValue<uint64_t>();
	state.zero_pending = state.minimum_repetitions == 0;
	idx_t vertex;
	if (!CsrTryGetOrdinal(*state.snapshot, state.delta.get(), state.start_id, vertex)) {
		state.initialized = true;
		return;
	}
	state.frames.push_back(MakeCsrPathFrame(state, vertex));
	state.active = state.label_exists;
	state.initialized = true;
}

static bool NextCsrPath(CsrPathExpandLocalState &state) {
	if (state.zero_pending) {
		state.zero_pending = false;
		state.current_end_id = state.start_id;
		return true;
	}
	const auto &neighbors = state.outgoing ? state.snapshot->outgoing_neighbors : state.snapshot->incoming_neighbors;
	const auto &edge_ids = state.outgoing ? state.snapshot->outgoing_edge_ids : state.snapshot->incoming_edge_ids;
	const auto &label_ids = state.outgoing ? state.snapshot->outgoing_label_ids : state.snapshot->incoming_label_ids;
	while (!state.frames.empty()) {
		auto &frame = state.frames.back();
		if (frame.cursor >= frame.end) {
			state.frames.pop_back();
			if (!state.edge_ids.empty() && state.edge_ids.size() >= state.frames.size()) {
				if (state.indexed_edges) {
					state.active_edge_ids.erase(state.edge_ids.back());
				}
				state.edge_ids.pop_back();
			}
			continue;
		}
		auto edge_offset = frame.cursor++;
		const bool in_base = edge_offset < frame.base_end;
		const auto *delta_edge = in_base ? nullptr : &(*frame.delta_edges)[edge_offset - frame.base_end];
		if ((in_base ? label_ids[edge_offset] : delta_edge->label_id) != state.label_id) {
			continue;
		}
		auto edge_id = in_base ? edge_ids[edge_offset] : delta_edge->edge_id;
		static constexpr idx_t EDGE_MEMBERSHIP_THRESHOLD = 64;
		if (!state.indexed_edges && state.edge_ids.size() >= EDGE_MEMBERSHIP_THRESHOLD) {
			state.active_edge_ids.insert(state.edge_ids.begin(), state.edge_ids.end());
			state.indexed_edges = true;
		}
		if (state.indexed_edges) {
			if (!state.active_edge_ids.insert(edge_id).second) {
				continue;
			}
		} else {
			if (std::find(state.edge_ids.begin(), state.edge_ids.end(), edge_id) != state.edge_ids.end()) {
				continue;
			}
		}
		idx_t neighbor = in_base ? neighbors[edge_offset] : delta_edge->neighbor;
		state.edge_ids.push_back(edge_id);
		auto depth = state.edge_ids.size();
		auto descend = state.unbounded || depth < state.maximum_repetitions;
		CsrPathFrame next;
		next.vertex = neighbor;
		state.frames.push_back(descend ? MakeCsrPathFrame(state, neighbor) : next);
		state.current_end_id = CsrVertexId(*state.snapshot, state.delta.get(), neighbor);
		if (depth >= state.minimum_repetitions) {
			return true;
		}
	}
	return false;
}

static OperatorResultType CsrPathExpandFunction(ExecutionContext &context, TableFunctionInput &data_p, DataChunk &input,
                                                DataChunk &output) {
	auto &state = data_p.local_state->Cast<CsrPathExpandLocalState>();
	if (!state.initialized) {
		auto &global = data_p.global_state->Cast<CsrExpandGlobalState>();
		InitializeCsrPathExpansion(context, input, global, state);
	}
	if (!state.active) {
		ResetCsrPathExpansionInput(state);
		return OperatorResultType::NEED_MORE_INPUT;
	}

	auto result_edges = FlatVector::GetDataMutable<uint64_t>(output.data[0]);
	auto result_sources = FlatVector::GetDataMutable<uint64_t>(output.data[1]);
	auto result_targets = FlatVector::GetDataMutable<uint64_t>(output.data[2]);
	output.data[3].Reference(Value(state.edge_label), count_t(STANDARD_VECTOR_SIZE));
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && NextCsrPath(state)) {
		auto edge_id = state.edge_ids.empty() ? 0 : state.edge_ids.back();
		result_edges[count] = edge_id;
		result_sources[count] = state.outgoing ? state.start_id : state.current_end_id;
		result_targets[count] = state.outgoing ? state.current_end_id : state.start_id;
		count++;
	}
	output.SetChildCardinality(count);
	// A standalone table scan consumes this in/out function until it emits an
	// empty chunk (it does not honor NEED_MORE_INPUT). Preserve the exhausted
	// state after the last nonempty chunk so that call cannot restart the seed.
	if (count > 0) {
		return OperatorResultType::HAVE_MORE_OUTPUT;
	}
	ResetCsrPathExpansionInput(state);
	return OperatorResultType::NEED_MORE_INPUT;
}

static unique_ptr<FunctionData> CsrStatsBind(ClientContext &, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<CsrBindData>();
	result->graph_name = input.inputs[0].GetValue<string>();
	auto weight = input.named_parameters.find("weight_property");
	if (weight != input.named_parameters.end() && !weight->second.IsNull()) {
		result->weight_property = StringUtil::Lower(weight->second.GetValue<string>());
	}
	names = {"graph_name",
	         "graph_version",
	         "vertex_count",
	         "edge_count",
	         "memory_bytes",
	         "build_count",
	         "cached",
	         "neighbor_width_bytes",
	         "vertex_ids_explicit",
	         "edge_labels_uniform",
	         "topology_bytes",
	         "identity_bytes",
	         "label_bytes",
	         "auxiliary_bytes",
	         "build_auxiliary_bytes",
	         "capability_mask",
	         "has_outgoing",
	         "has_incoming",
	         "has_edge_ids",
	         "has_edge_labels",
	         "has_vertex_labels",
	         "has_vertex_label_postings",
	         "has_edge_stats",
	         "has_out_degrees",
	         "snapshot_acquisition_count",
	         "source_catalog",
	         "source_snapshot_id",
	         "has_label_fanout_stats",
	         "label_fanout_pairs",
	         "endpoint_statistics_status",
	         "endpoint_label_groups",
	         "endpoint_combinations",
	         "endpoint_statistics_bytes",
	         "delta_vertex_count",
	         "delta_edge_count"};
	return_types = {
	    LogicalType::VARCHAR, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT,
	    LogicalType::UBIGINT, LogicalType::BOOLEAN, LogicalType::UBIGINT, LogicalType::BOOLEAN, LogicalType::BOOLEAN,
	    LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT,
	    LogicalType::UBIGINT, LogicalType::BOOLEAN, LogicalType::BOOLEAN, LogicalType::BOOLEAN, LogicalType::BOOLEAN,
	    LogicalType::BOOLEAN, LogicalType::BOOLEAN, LogicalType::BOOLEAN, LogicalType::BOOLEAN, LogicalType::UBIGINT,
	    LogicalType::VARCHAR, LogicalType::UBIGINT, LogicalType::BOOLEAN, LogicalType::UBIGINT, LogicalType::VARCHAR,
	    LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT};
	return std::move(result);
}

struct SingleRowState : GlobalTableFunctionState {
	bool done = false;
};

static unique_ptr<GlobalTableFunctionState> SingleRowInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<SingleRowState>();
}

static void BuildCsrFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<SingleRowState>();
	if (state.done) {
		return;
	}
	if (!context.transaction.IsAutoCommit()) {
		throw NotImplementedException("CSR construction is not eligible inside an explicit transaction");
	}
	auto &data = input.bind_data->Cast<CsrBindData>();
	{
		// Migrate catalog-only additions before loading an existing graph. This
		// keeps CALL gql_build_csr usable as the first command after opening a
		// database created by an older DuckGQL build.
		Connection storage_connection(*context.db);
		GqlEnsureStorage(storage_connection);
	}
	auto storage = GetDerivedGraphStorageState(context);
	GqlTableGraphBinding binding;
	if (!GqlTryLoadTableGraph(context, data.graph_name, binding)) {
		throw InvalidInputException("Graph '%s' has no native tables; load it with "
		                            "COPY GRAPH before building CSR",
		                            data.graph_name);
	}
	PublishTableSnapshot(*storage,
	                     BuildTableSnapshot(context, data.graph_name,
	                                        GQL_CSR_FULL | (data.endpoint_statistics ? GQL_CSR_ENDPOINT_STATS : 0)));
	// Publication is followed by the same acquisition used by consumers. If a write
	// raced construction, CALL must not report a stale projection as successfully
	// prepared, unless a delta follows it.
	auto snapshot = AcquireFullLease(context, data.graph_name, *storage).snapshot;
	auto build_count = ReadCsrBuildCount(*storage);

	output.SetChildCardinality(1);
	output.SetValue(0, 0, Value(data.graph_name));
	output.SetValue(1, 0, Value::UBIGINT(snapshot->graph_version));
	output.SetValue(2, 0, Value::UBIGINT(snapshot->vertex_ids.size()));
	output.SetValue(3, 0, Value::UBIGINT(snapshot->edge_count));
	output.SetValue(4, 0, Value::UBIGINT(snapshot->memory_bytes));
	output.SetValue(5, 0, Value::UBIGINT(build_count));
	state.done = true;
}

static void CsrStatsFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<SingleRowState>();
	if (state.done) {
		return;
	}
	auto &data = input.bind_data->Cast<CsrBindData>();
	auto storage = GetDerivedGraphStorageState(context);
	shared_ptr<const GqlCsrSnapshot> snapshot;
	shared_ptr<const GqlCsrDelta> delta;
	try {
		snapshot = GetPreparedTableSnapshot(context, data.graph_name, *storage, 0, data.weight_property);
	} catch (const InvalidInputException &) {
		// The only CSR left may be one that a delta extends.
		if (!data.weight_property.empty()) {
			throw;
		}
		auto lease = AcquireFullLease(context, data.graph_name, *storage);
		snapshot = lease.snapshot;
		delta = lease.delta;
	}
	auto build_count = ReadCsrBuildCount(*storage);
	const idx_t delta_vertices = delta ? delta->vertex_ids.size() : 0;
	const idx_t delta_edges = delta ? delta->edge_count : 0;
	output.SetChildCardinality(1);
	output.SetValue(0, 0, Value(data.graph_name));
	output.SetValue(1, 0, Value::UBIGINT(snapshot->graph_version));
	output.SetValue(2, 0, Value::UBIGINT(snapshot->vertex_ids.size() + delta_vertices));
	output.SetValue(3, 0, Value::UBIGINT(snapshot->edge_count + delta_edges));
	output.SetValue(4, 0, Value::UBIGINT(snapshot->memory_bytes));
	output.SetValue(5, 0, Value::UBIGINT(build_count));
	output.SetValue(6, 0, Value(true));
	output.SetValue(7, 0,
	                Value::UBIGINT((snapshot->capabilities & GQL_CSR_OUTGOING)
	                                   ? snapshot->outgoing_neighbors.WidthBytes()
	                                   : snapshot->incoming_neighbors.WidthBytes()));
	output.SetValue(8, 0, Value(!snapshot->vertex_ids.IsImplicitDense()));
	output.SetValue(9, 0, Value(snapshot->outgoing_label_ids.IsUniform()));
	output.SetValue(10, 0, Value::UBIGINT(snapshot->topology_bytes));
	output.SetValue(11, 0, Value::UBIGINT(snapshot->identity_bytes));
	output.SetValue(12, 0, Value::UBIGINT(snapshot->label_bytes));
	output.SetValue(13, 0, Value::UBIGINT(snapshot->auxiliary_bytes));
	output.SetValue(14, 0, Value::UBIGINT(snapshot->build_auxiliary_bytes));
	output.SetValue(15, 0, Value::UBIGINT(snapshot->capabilities));
	output.SetValue(16, 0, Value::BOOLEAN(snapshot->capabilities & GQL_CSR_OUTGOING));
	output.SetValue(17, 0, Value::BOOLEAN(snapshot->capabilities & GQL_CSR_INCOMING));
	output.SetValue(18, 0, Value::BOOLEAN(snapshot->capabilities & GQL_CSR_EDGE_IDS));
	output.SetValue(19, 0, Value::BOOLEAN(snapshot->capabilities & GQL_CSR_EDGE_LABELS));
	output.SetValue(20, 0, Value::BOOLEAN(snapshot->capabilities & GQL_CSR_VERTEX_LABELS));
	output.SetValue(21, 0, Value::BOOLEAN(snapshot->capabilities & GQL_CSR_VERTEX_LABEL_POSTINGS));
	output.SetValue(22, 0, Value::BOOLEAN(snapshot->capabilities & GQL_CSR_EDGE_STATS));
	output.SetValue(27, 0, Value::BOOLEAN(snapshot->has_label_fanout_stats));
	output.SetValue(28, 0, Value::UBIGINT(snapshot->label_fanout_stats.size()));
	output.SetValue(29, 0, Value(snapshot->endpoint_statistics_status));
	output.SetValue(30, 0,
	                Value::UBIGINT(snapshot->endpoint_statistics ? snapshot->endpoint_statistics->groups.size() : 0));
	output.SetValue(31, 0,
	                Value::UBIGINT(snapshot->endpoint_statistics ? snapshot->endpoint_statistics->counts.size() : 0));
	output.SetValue(
	    32, 0, Value::UBIGINT(snapshot->endpoint_statistics ? snapshot->endpoint_statistics->AllocatedBytes() : 0));
	output.SetValue(
	    23, 0,
	    Value::BOOLEAN((snapshot->capabilities & GQL_CSR_OUT_DEGREES) || (snapshot->capabilities & GQL_CSR_OUTGOING)));
	output.SetValue(24, 0, Value::UBIGINT(storage->acquisition_count.load()));
	output.SetValue(25, 0, snapshot->has_source_snapshot ? Value(snapshot->source_catalog) : Value());
	output.SetValue(26, 0, snapshot->has_source_snapshot ? Value::UBIGINT(snapshot->source_snapshot_id) : Value());
	output.SetValue(33, 0, Value::UBIGINT(delta_vertices));
	output.SetValue(34, 0, Value::UBIGINT(delta_edges));
	state.done = true;
}

struct CsrEdgeStatsState : GlobalTableFunctionState {
	shared_ptr<const GqlCsrSnapshot> snapshot;
	vector<pair<string, uint32_t>> labels;
	idx_t offset = 0;
};

static unique_ptr<FunctionData> CsrEdgeStatsBind(ClientContext &, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<CsrBindData>();
	result->graph_name = input.inputs[0].GetValue<string>();
	names = {"edge_label",          "edge_count",          "outgoing_vertex_count", "incoming_vertex_count",
	         "avg_outgoing_degree", "avg_incoming_degree", "max_outgoing_degree",   "max_incoming_degree"};
	return_types = {LogicalType::VARCHAR, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT,
	                LogicalType::DOUBLE,  LogicalType::DOUBLE,  LogicalType::UBIGINT, LogicalType::UBIGINT};
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> CsrEdgeStatsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &data = input.bind_data->Cast<CsrBindData>();
	auto result = make_uniq<CsrEdgeStatsState>();
	result->snapshot = GqlGetCsrSnapshot(context, data.graph_name);
	for (const auto &entry : result->snapshot->label_ids) {
		if (entry.second < result->snapshot->edge_label_stats.size() &&
		    result->snapshot->edge_label_stats[entry.second].edge_count > 0) {
			result->labels.emplace_back(entry.first, entry.second);
		}
	}
	std::sort(result->labels.begin(), result->labels.end());
	return std::move(result);
}

static void CsrEdgeStatsFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<CsrEdgeStatsState>();
	auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.labels.size() - state.offset);
	for (idx_t index = 0; index < count; index++) {
		const auto &label = state.labels[state.offset + index];
		const auto &stats = state.snapshot->edge_label_stats[label.second];
		output.SetValue(0, index, Value(label.first));
		output.SetValue(1, index, Value::UBIGINT(stats.edge_count));
		output.SetValue(2, index, Value::UBIGINT(stats.outgoing_vertex_count));
		output.SetValue(3, index, Value::UBIGINT(stats.incoming_vertex_count));
		output.SetValue(4, index,
		                Value(stats.outgoing_vertex_count == 0
		                          ? 0.0
		                          : static_cast<double>(stats.edge_count) / stats.outgoing_vertex_count));
		output.SetValue(5, index,
		                Value(stats.incoming_vertex_count == 0
		                          ? 0.0
		                          : static_cast<double>(stats.edge_count) / stats.incoming_vertex_count));
		output.SetValue(6, index, Value::UBIGINT(stats.max_outgoing_degree));
		output.SetValue(7, index, Value::UBIGINT(stats.max_incoming_degree));
	}
	state.offset += count;
	output.SetChildCardinality(count);
}

TableFunction GqlNeighborsFunction() {
	TableFunction function("gql_neighbors", {LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR},
	                       NeighborsFunction);
	function.bind = NeighborsBind;
	function.init_global = NeighborsInit;
	return function;
}

TableFunction GqlCsrVerticesFunction() {
	TableFunction function("gql_csr_vertices", {LogicalType::VARCHAR, LogicalType::VARCHAR}, CsrVerticesFunction);
	function.bind = CsrVerticesBind;
	function.init_global = CsrVerticesInit;
	function.cardinality = CsrVerticesCardinality;
	return function;
}

TableFunctionSet GqlCsrExpandFunctions() {
	TableFunctionSet functions("gql_csr_expand");
	child_list_t<LogicalType> fields {{"graph_name", LogicalType::VARCHAR},
	                                  {"vertex_id", LogicalType::UBIGINT},
	                                  {"direction", LogicalType::VARCHAR},
	                                  {"edge_label", LogicalType::VARCHAR}};
	auto add_overload = [&]() {
		TableFunction function("gql_csr_expand", {LogicalType::STRUCT(fields)}, nullptr, CsrExpandBind,
		                       CsrExpandGlobalInit, CsrExpandLocalInit);
		function.in_out_function = CsrExpandFunction;
		functions.AddFunction(std::move(function));
	};
	add_overload();
	// Lowering passes a label the pattern requires, so the rows dropped here would
	// otherwise be expanded, joined, and only then rejected by the label predicate.
	fields.emplace_back("neighbor_label", LogicalType::VARCHAR);
	add_overload();
	return functions;
}

bool GqlCsrHasVertexLabels(const GqlCsrSnapshot &snapshot) {
	return snapshot.vertex_label_offsets.size() == snapshot.vertex_ids.size() + 1;
}

// A label check costs cache misses per row, so it must remove enough rows to pay.
static constexpr double GQL_NEIGHBOR_LABEL_MAX_MATCHING = 0.9;

bool GqlNeighborLabelIsSelective(const GqlCsrSnapshot &snapshot, const string &edge_label, const string &direction,
                                 const string &neighbor_label) {
	if (!snapshot.has_label_fanout_stats) {
		return true;
	}
	auto edge = snapshot.label_ids.find(StringUtil::Lower(edge_label));
	auto vertex = snapshot.label_ids.find(StringUtil::Lower(neighbor_label));
	if (edge == snapshot.label_ids.end() || vertex == snapshot.label_ids.end() ||
	    edge->second >= snapshot.edge_label_stats.size()) {
		return true;
	}
	auto stats = snapshot.label_fanout_stats.find(GqlCsrLabelFanoutKey(vertex->second, edge->second));
	if (stats == snapshot.label_fanout_stats.end()) {
		return true;
	}
	// The neighbor is the far endpoint: the target of an outgoing expansion.
	const auto matching = direction == "out" ? stats->second.incoming_edges : stats->second.outgoing_edges;
	return static_cast<double>(matching) <
	       GQL_NEIGHBOR_LABEL_MAX_MATCHING * static_cast<double>(snapshot.edge_label_stats[edge->second].edge_count);
}

TableFunction GqlCsrPathExpandFunction() {
	auto input_type = LogicalType::STRUCT({{"graph_name", LogicalType::VARCHAR},
	                                       {"vertex_id", LogicalType::UBIGINT},
	                                       {"direction", LogicalType::VARCHAR},
	                                       {"edge_label", LogicalType::VARCHAR},
	                                       {"minimum_repetitions", LogicalType::UBIGINT},
	                                       {"maximum_repetitions", LogicalType::UBIGINT},
	                                       {"unbounded", LogicalType::BOOLEAN}});
	TableFunction function("gql_csr_path_expand", {input_type}, nullptr, CsrPathExpandBind, CsrExpandGlobalInit,
	                       CsrPathExpandLocalInit);
	function.in_out_function = CsrPathExpandFunction;
	return function;
}

static TableFunction GqlElementFetchFunction(const string &name) {
	TableFunction function(Identifier(name), {LogicalType::UBIGINT}, nullptr, ElementFetchBind, ElementFetchGlobalInit,
	                       ElementFetchLocalInit);
	function.in_out_function = ElementFetchFunction;
	function.projection_pushdown = true;
	return function;
}

TableFunction GqlVertexFetchFunction() {
	return GqlElementFetchFunction("gql_vertex_fetch");
}

TableFunction GqlProjectedVertexFetchFunction() {
	auto function = GqlElementFetchFunction("gql_vertex_fetch_projected");
	function.GetSignature() = FunctionSignature(vector<LogicalType> {LogicalType::ANY}, function.GetReturnType());
	return function;
}

TableFunction GqlEdgeFetchFunction() {
	return GqlElementFetchFunction("gql_edge_fetch");
}

TableFunction GqlCsrAllShortestFunction() {
	TableFunction function("gql_csr_all_shortest", {LogicalType::ANY}, nullptr, CsrAllShortestBind, CsrExpandGlobalInit,
	                       CsrAllShortestLocalInit);
	function.in_out_function = CsrAllShortestFunction;
	return function;
}

TableFunction GqlCsrShortestExpandFunction() {
	auto input_type = LogicalType::STRUCT({{"graph_name", LogicalType::VARCHAR},
	                                       {"vertex_id", LogicalType::UBIGINT},
	                                       {"direction", LogicalType::VARCHAR},
	                                       {"edge_label", LogicalType::VARCHAR},
	                                       {"minimum_repetitions", LogicalType::UBIGINT},
	                                       {"maximum_repetitions", LogicalType::UBIGINT},
	                                       {"unbounded", LogicalType::BOOLEAN}});
	TableFunction function("gql_csr_shortest_expand", {input_type}, nullptr, CsrShortestBind, CsrExpandGlobalInit,
	                       CsrShortestLocalInit);
	function.in_out_function = CsrShortestFunction;
	return function;
}

TableFunction GqlVertexLookupFunction() {
	TableFunction function(Identifier("gql_vertex_lookup"), {LogicalType::ANY}, nullptr, VertexLookupBind,
	                       VertexLookupGlobalInit, VertexLookupLocalInit);
	function.in_out_function = VertexLookupFunction;
	function.projection_pushdown = true;
	return function;
}

TableFunction GqlBuildCsrFunction() {
	TableFunction function("gql_build_csr", {LogicalType::VARCHAR}, BuildCsrFunction);
	function.bind = [](ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
	                   vector<Identifier> &names) -> unique_ptr<FunctionData> {
		auto result = make_uniq<CsrBindData>();
		result->graph_name = input.inputs[0].GetValue<string>();
		auto endpoint = input.named_parameters.find("endpoint_statistics");
		if (endpoint != input.named_parameters.end()) {
			if (endpoint->second.IsNull())
				throw BinderException("endpoint_statistics must be a non-NULL BOOLEAN");
			result->endpoint_statistics = endpoint->second.GetValue<bool>();
		}
		names = {"graph_name", "graph_version", "vertex_count", "edge_count", "memory_bytes", "build_count"};
		return_types = {LogicalType::VARCHAR, LogicalType::UBIGINT, LogicalType::UBIGINT,
		                LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT};
		return std::move(result);
	};
	function.init_global = SingleRowInit;
	function.GetSignature().WithTypedKwargs(
	    "options", [](TypedKwargs &options) { options.Add("endpoint_statistics", LogicalType::BOOLEAN); });
	return function;
}

TableFunction GqlCsrStatsFunction() {
	TableFunction function("gql_csr_stats", {LogicalType::VARCHAR}, CsrStatsFunction);
	function.bind = CsrStatsBind;
	function.GetSignature().WithTypedKwargs(
	    "options", [](TypedKwargs &options) { options.Add("weight_property", LogicalType::VARCHAR); });
	function.init_global = SingleRowInit;
	return function;
}

TableFunction GqlCsrEdgeStatsFunction() {
	TableFunction function("gql_csr_edge_stats", {LogicalType::VARCHAR}, CsrEdgeStatsFunction, CsrEdgeStatsBind,
	                       CsrEdgeStatsInit);
	return function;
}

} // namespace duckdb
