#include "gql_referenced.hpp"

#include "gql_catalog.hpp"
#include "gql_sql_utils.hpp"
#include "duckdb/common/types/column/column_data_allocator.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/query_result_stream.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"

#include <condition_variable>
#include <mutex>

namespace duckdb {

struct ReferencedKeyMap {
	unique_ptr<ColumnDataCollection> keys;
	idx_t offset = 0;
	uint64_t snapshot = 0;
	uint64_t table_id = 0;
	uint64_t build_count = 0;
	atomic<uint64_t> hits {0};
};
struct ReferencedMapSlot {
	bool building = false;
	shared_ptr<ReferencedKeyMap> value;
};
struct ReferencedGraphMaps {
	uint64_t snapshot = 0;
	string token;
	vector<GqlReferencedSource> vertices, edges;
	unordered_map<uint64_t, idx_t> counts;
	unordered_map<uint64_t, ReferencedMapSlot> tables;
};
struct ReferencedMaps : ObjectCacheEntry {
	static string ObjectType() {
		return "gql_referenced_maps";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx {};
	}
	mutex lock;
	std::condition_variable changed;
	unordered_map<uint64_t, shared_ptr<ReferencedGraphMaps>> graphs;
	unordered_map<uint64_t, uint64_t> builds;
};
static shared_ptr<ReferencedMaps> Maps(ClientContext &context) {
	return ObjectCache::GetObjectCache(context).GetOrCreate<ReferencedMaps>("gql_referenced_maps");
}
static string SourceTable(const GqlReferencedSource &source, uint64_t snapshot) {
	return GqlQuoteIdentifier(source.catalog_name) + "." + GqlQuoteIdentifier(source.schema_name) + "." +
	       GqlQuoteIdentifier(source.table_name) + " AT (VERSION => " + to_string(snapshot) + ")";
}

static shared_ptr<ReferencedKeyMap> Acquire(ClientContext &context, const string &graph_name, uint64_t table_id,
                                            uint64_t snapshot) {
	GqlTableGraphBinding graph;
	// Internal CSR reads use a transaction. Validate the committed graph through
	// an independent autocommit context; the map itself reads the requested version.
	Connection metadata(*context.db);
	auto storage = Maps(context);
	shared_ptr<ReferencedGraphMaps> maps;
	// A normal graph bind already validates source schemas. Reusing an immutable
	// map needs only committed graph/attachment identity, not that full validation
	// once again for every occurrence of this function in a join.
	auto identity = GqlQuery(
	    metadata, "SELECT g.graph_id, g.graph_version, s.source_catalog, d.database_oid::UBIGINT, s.snapshot_policy "
	              "FROM gql_internal.graphs g JOIN gql_internal.graph_sources s USING(graph_id) "
	              "JOIN duckdb_databases() d ON lower(d.database_name)=lower(s.source_catalog) "
	              "WHERE g.graph_name=" +
	                  GqlQuoteLiteral(graph_name) + " AND lower(d.type)='ducklake'");
	bool reusable = false;
	if (identity->RowCount() == 1 && identity->GetValue(4, 0).GetValue<string>() == "LIVE") {
		graph.graph_id = identity->GetValue(0, 0).GetValue<uint64_t>();
		graph.graph_version = identity->GetValue(1, 0).GetValue<uint64_t>();
		graph.source_catalog = identity->GetValue(2, 0).GetValue<string>();
		graph.source_catalog_id = identity->GetValue(3, 0).GetValue<uint64_t>();
		auto expected = graph.source_catalog + ":" + to_string(graph.source_catalog_id) + ":" +
		                to_string(graph.graph_version) + ":" + to_string(snapshot);
		lock_guard<mutex> guard(storage->lock);
		auto found = storage->graphs.find(graph.graph_id);
		reusable = found != storage->graphs.end() && found->second && found->second->token == expected;
		if (reusable)
			maps = found->second;
	}
	if (!reusable) {
		if (!GqlTryLoadTableGraph(*metadata.context, graph_name, graph) || !graph.cached_reference_ids) {
			throw BinderException("Referenced key maps require an autocommit DuckLake graph");
		}
	}
	if (graph.has_pinned_snapshot && snapshot != graph.pinned_snapshot_id) {
		throw BinderException("Referenced key map does not match the pinned graph snapshot");
	}
	string token = graph.source_catalog + ":" + to_string(graph.source_catalog_id) + ":" +
	               to_string(graph.graph_version) + ":" + to_string(snapshot);
	{
		unique_lock<mutex> guard(storage->lock);
		if (!maps) {
			auto &current = storage->graphs[graph.graph_id];
			if (!current || current->token != token) {
				// Keep only the current map generation per graph. In-flight bind/scan leases
				// own old immutable generations until their pinned queries finish.
				if (storage->graphs.size() > 4) {
					for (auto it = storage->graphs.begin(); it != storage->graphs.end(); ++it) {
						if (it->first != graph.graph_id) {
							storage->graphs.erase(it);
							break;
						}
					}
				}
				current = make_shared_ptr<ReferencedGraphMaps>();
				current->token = token;
				current->snapshot = snapshot;
				current->vertices = graph.vertex.referenced_sources;
				current->edges = graph.edge.referenced_sources;
			}
			maps = current;
		}
		for (;;) {
			auto &slot = maps->tables[table_id];
			if (slot.value) {
				slot.value->hits++;
				return slot.value;
			}
			if (!slot.building) {
				slot.building = true;
				break;
			}
			storage->changed.wait(guard);
		}
	}
	try {
		const GqlReferencedSource *source = nullptr;
		const vector<GqlReferencedSource> *siblings = nullptr;
		for (const auto *tables : {&maps->vertices, &maps->edges}) {
			for (const auto &entry : *tables)
				if (entry.element_table_id == table_id) {
					source = &entry;
					siblings = tables;
				}
		}
		if (!source)
			throw BinderException("Referenced graph mapping no longer exists");
		Connection connection(*context.db);
		connection.BeginTransaction();
		auto result = make_shared_ptr<ReferencedKeyMap>();
		result->table_id = table_id;
		result->snapshot = snapshot;
		for (const auto &entry : *siblings) {
			if (entry.element_table_id == table_id)
				break;
			idx_t count;
			bool found;
			{
				lock_guard<mutex> guard(storage->lock);
				auto it = maps->counts.find(entry.element_table_id);
				found = it != maps->counts.end();
				count = found ? it->second : 0;
			}
			if (!found) {
				count = GqlQuery(connection, "SELECT count(*)::UBIGINT FROM " + SourceTable(entry, snapshot))
				            ->GetValue(0, 0)
				            .GetValue<uint64_t>();
				lock_guard<mutex> guard(storage->lock);
				maps->counts[entry.element_table_id] = count;
			}
			if (count > DConstants::INVALID_INDEX - result->offset - 1)
				throw InvalidInputException("Referenced graph identity range overflow");
			result->offset += count;
		}
		auto key = GqlQuoteIdentifier(source->key_column.empty() ? string("rowid") : source->key_column);
		// Validate adjacent keys in the native vectorized window, not by materializing
		// Values per key in C++. Sorting/validation happens once per map generation.
		QueryResultStream<> stream(connection.Submit("SELECT " + key + ", " + key + " IS NULL OR " + key +
		                                             " IS NOT DISTINCT FROM lag(" + key + ") OVER (ORDER BY " + key +
		                                             ") AS invalid_key FROM " + SourceTable(*source, snapshot) +
		                                             " ORDER BY " + key));
		auto chunk = stream.Fetch();
		if (stream.HasError())
			throw InvalidInputException("Referenced identity read failed: %s", stream.GetError());
		auto allocator =
		    make_shared_ptr<ColumnDataAllocator>(BufferManager::GetBufferManager(*context.db),
		                                         ColumnDataCollectionLifetime::THROW_ERROR_AFTER_DATABASE_CLOSES);
		allocator->MakeShared();
		result->keys = make_uniq<ColumnDataCollection>(allocator, vector<LogicalType> {stream.GetTypes()[0]});
		ColumnDataAppendState append;
		result->keys->InitializeAppend(append);
		DataChunk keys;
		keys.Initialize(context, result->keys->Types());
		while (chunk && chunk->size()) {
			UnifiedVectorFormat flags;
			chunk->data[1].ToUnifiedFormat(flags);
			auto invalid = UnifiedVectorFormat::GetData<bool>(flags);
			for (idx_t row = 0; row < chunk->size(); row++) {
				if (invalid[flags.sel->get_index(row)])
					throw InvalidInputException("Referenced graph keys must be unique and non-NULL at snapshot %llu",
					                            snapshot);
			}
			keys.ReferenceColumns(*chunk, {0});
			result->keys->Append(append, keys);
			chunk = stream.Fetch();
		}
		if (stream.HasError())
			throw InvalidInputException("Referenced identity read failed: %s", stream.GetError());
		if (result->keys->Count() > DConstants::INVALID_INDEX - result->offset - 1)
			throw InvalidInputException("Referenced graph identity range overflow");
		connection.Commit();
		{
			lock_guard<mutex> guard(storage->lock);
			maps->counts[table_id] = result->keys->Count();
			result->build_count = ++storage->builds[table_id];
			auto &slot = maps->tables[table_id];
			slot.value = result;
			slot.building = false;
		}
		storage->changed.notify_all();
		return result;
	} catch (...) {
		{
			lock_guard<mutex> guard(storage->lock);
			maps->tables[table_id].building = false;
		}
		storage->changed.notify_all();
		throw;
	}
}

struct KeyBindData : TableFunctionData {
	shared_ptr<ReferencedKeyMap> map;
};
struct KeyScan : GlobalTableFunctionState {
	ColumnDataScanState state;
	idx_t row = 0;
	DataChunk chunk;
};
static unique_ptr<FunctionData> KeysBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &types, vector<Identifier> &names) {
	auto result = make_uniq<KeyBindData>();
	result->map = Acquire(context, input.inputs[0].GetValue<string>(), input.inputs[1].GetValue<uint64_t>(),
	                      input.inputs[2].GetValue<uint64_t>());
	types = {result->map->keys->Types()[0], LogicalType::UBIGINT};
	names = {"source_key", "__gql_id"};
	return std::move(result);
}
static unique_ptr<GlobalTableFunctionState> KeysInit(ClientContext &, TableFunctionInitInput &input) {
	auto &map = *input.bind_data->Cast<KeyBindData>().map;
	auto result = make_uniq<KeyScan>();
	map.keys->InitializeScan(result->state);
	map.keys->InitializeScanChunk(result->chunk);
	return std::move(result);
}
static void KeysScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &map = *input.bind_data->Cast<KeyBindData>().map;
	auto &state = input.global_state->Cast<KeyScan>();
	map.keys->Scan(state.state, state.chunk);
	if (!state.chunk.size())
		return;
	output.data[0].Reference(state.chunk.data[0]);
	auto ids = FlatVector::GetDataMutable<uint64_t>(output.data[1]);
	for (idx_t row = 0; row < state.chunk.size(); row++) {
		ids[row] = map.offset + state.row + row + 1;
	}
	output.SetChildCardinality(state.chunk.size());
	state.row += state.chunk.size();
}
static unique_ptr<NodeStatistics> KeysCardinality(ClientContext &, const FunctionData *data) {
	auto count = data->Cast<KeyBindData>().map->keys->Count();
	return make_uniq<NodeStatistics>(count, count);
}
TableFunction GqlReferencedKeysFunction() {
	TableFunction f("gql_referenced_keys", {LogicalType::VARCHAR, LogicalType::UBIGINT, LogicalType::UBIGINT},
	                KeysScan);
	f.bind = KeysBind;
	f.init_global = KeysInit;
	f.cardinality = KeysCardinality;
	return f;
}

struct CacheStatsData : TableFunctionData {
	vector<shared_ptr<ReferencedKeyMap>> maps;
};
struct CacheStatsScan : GlobalTableFunctionState {
	idx_t index = 0;
};
static unique_ptr<FunctionData> StatsBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &types, vector<Identifier> &names) {
	auto result = make_uniq<CacheStatsData>();
	GqlTableGraphBinding graph;
	if (!GqlTryLoadTableGraph(context, input.inputs[0].GetValue<string>(), graph))
		throw BinderException("Graph requires table storage");
	auto storage = Maps(context);
	lock_guard<mutex> guard(storage->lock);
	auto found = storage->graphs.find(graph.graph_id);
	if (found != storage->graphs.end())
		for (const auto &entry : found->second->tables) {
			if (entry.second.value)
				result->maps.push_back(entry.second.value);
		}
	names = {"element_table_id", "source_snapshot_id", "rows", "buffer_bytes", "build_count", "cache_hits"};
	types = {LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT,
	         LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT};
	return std::move(result);
}
static unique_ptr<GlobalTableFunctionState> StatsInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<CacheStatsScan>();
}
static void StatsScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<CacheStatsData>();
	auto &state = input.global_state->Cast<CacheStatsScan>();
	idx_t count = 0;
	while (state.index < data.maps.size() && count < STANDARD_VECTOR_SIZE) {
		auto &map = *data.maps[state.index++];
		uint64_t values[] = {map.table_id,    map.snapshot,   map.keys->Count(), map.keys->AllocationSize(),
		                     map.build_count, map.hits.load()};
		for (idx_t col = 0; col < 6; col++)
			output.data[col].SetValue(count, Value::UBIGINT(values[col]));
		count++;
	}
	output.SetChildCardinality(count);
}
TableFunction GqlReferencedCacheStatsFunction() {
	TableFunction f("gql_referenced_cache_stats", {LogicalType::VARCHAR}, StatsScan);
	f.bind = StatsBind;
	f.init_global = StatsInit;
	return f;
}

} // namespace duckdb
