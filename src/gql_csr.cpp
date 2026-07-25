#include "gql_csr.hpp"

#include "gql_catalog.hpp"
#include "gql_sql_utils.hpp"
#include "gql_storage.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/storage/object_cache.hpp"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {

static constexpr const char *GQL_CSR_STATE_KEY = "gql_csr_state";
static constexpr const char *GQL_CSR_WRITE_OBSERVER_STATE_KEY = "gql_csr_write_observer_state";
static constexpr const char *GQL_CSR_GENERATION_CACHE_KEY = "gql_csr_generation_state";

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

static void AdvanceCsrTableWriteGeneration(ClientContext &context, const string &table_key) {
	auto state = GetCsrGenerationState(context);
	lock_guard<mutex> guard(state->lock);
	state->table_write_generations[table_key]++;
}

struct GqlCsrWriteObserverState : ClientContextState {
	RebindQueryInfo OnExecutePrepared(ClientContext &context, PreparedStatementCallbackInfo &info,
	                                  RebindQueryInfo current_rebind) override {
		if (!info.prepared_statement.properties.IsReadOnly()) {
			AdvanceCsrWriteGeneration(context);
		}
		return current_rebind;
	}
};

void GqlRegisterCsrWriteObserver(ClientContext &context) {
	context.registered_state->GetOrCreate<GqlCsrWriteObserverState>(GQL_CSR_WRITE_OBSERVER_STATE_KEY);
}

void GqlNotifyCsrTableWritePlanned(ClientContext &context, const string &catalog_name, const string &schema_name,
                                   const string &table_name) {
	GqlRegisterCsrWriteObserver(context);
	AdvanceCsrTableWriteGeneration(context, CsrTableKey(catalog_name, schema_name, table_name));
}

void GqlNotifyCsrPreparedWriteExecution(ClientContext &context) {
	GqlRegisterCsrWriteObserver(context);
	AdvanceCsrWriteGeneration(context);
}

static string QualifiedTable(const GqlElementTableBinding &table) {
	return GqlQuoteIdentifier(table.catalog_name) + "." + GqlQuoteIdentifier(table.schema_name) + "." +
	       GqlQuoteIdentifier(table.table_name);
}

struct GqlCsrCacheState : ClientContextState {
	unordered_map<uint64_t, shared_ptr<GqlCsrSnapshot>> snapshots;
	unordered_map<string, uint64_t> graph_ids_by_name;
	uint64_t build_count = 0;
};

struct GraphVersion {
	uint64_t graph_id;
	uint64_t graph_version;
};

static GraphVersion ReadGraphVersion(Connection &connection, const string &graph_name) {
	auto result = GqlQuery(connection, "SELECT graph_id, graph_version FROM "
	                                   "gql_internal.graphs WHERE graph_name = " +
	                                       GqlQuoteLiteral(graph_name));
	if (result->RowCount() == 0) {
		throw InvalidInputException("Graph '%s' does not exist", graph_name);
	}
	return {result->GetValue(0, 0).GetValue<uint64_t>(), result->GetValue(1, 0).GetValue<uint64_t>()};
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

static shared_ptr<GqlCsrSnapshot> BuildTableSnapshot(ClientContext &context, const string &graph_name) {
	GqlTableGraphBinding binding;
	if (!GqlTryLoadTableGraph(context, graph_name, binding)) {
		throw InvalidInputException("CSR algorithms require a table-backed graph; "
		                            "load graph '%s' with COPY GRAPH first",
		                            graph_name);
	}
	Connection connection(*context.db);
	auto snapshot = make_shared_ptr<GqlCsrSnapshot>();
	snapshot->write_generation = ReadCsrWriteGeneration(context);
	snapshot->vertex_table_key =
	    CsrTableKey(binding.vertex.catalog_name, binding.vertex.schema_name, binding.vertex.table_name);
	snapshot->edge_table_key =
	    CsrTableKey(binding.edge.catalog_name, binding.edge.schema_name, binding.edge.table_name);
	snapshot->vertex_write_generation = ReadCsrTableWriteGeneration(context, snapshot->vertex_table_key);
	snapshot->edge_write_generation = ReadCsrTableWriteGeneration(context, snapshot->edge_table_key);
	connection.BeginTransaction();
	auto graph = ReadGraphVersion(connection, graph_name);
	snapshot->graph_id = graph.graph_id;
	snapshot->graph_version = graph.graph_version;

	auto vertex_count = GqlQuery(connection, "SELECT count(*)::UBIGINT FROM " + QualifiedTable(binding.vertex));
	snapshot->vertex_ids.reserve(NumericCast<idx_t>(vertex_count->GetValue(0, 0).GetValue<uint64_t>()));
	auto vertex_label_projection =
	    binding.vertex.label_column.empty() ? "CAST(NULL AS VARCHAR[])"
	    : binding.vertex.label_is_list
	        ? GqlQuoteIdentifier(binding.vertex.label_column)
	        : "string_split(CAST(" + GqlQuoteIdentifier(binding.vertex.label_column) + " AS VARCHAR), ';')";
	auto vertices = connection.SendQuery("SELECT CAST(" + GqlQuoteIdentifier(binding.vertex.key_column) +
	                                     " AS UBIGINT) AS vertex_id, " + vertex_label_projection + " FROM " +
	                                     QualifiedTable(binding.vertex) + " ORDER BY vertex_id");
	GqlThrowOnError(*vertices);
	snapshot->vertex_label_offsets.push_back(0);
	while (auto chunk = vertices->Fetch()) {
		UnifiedVectorFormat vertex_data;
		UnifiedVectorFormat label_data;
		UnifiedVectorFormat label_child_data;
		chunk->data[0].ToUnifiedFormat(chunk->size(), vertex_data);
		chunk->data[1].ToUnifiedFormat(chunk->size(), label_data);
		auto &label_child = ListVector::GetEntry(chunk->data[1]);
		label_child.ToUnifiedFormat(ListVector::GetListSize(chunk->data[1]), label_child_data);
		auto ids = UnifiedVectorFormat::GetData<uint64_t>(vertex_data);
		auto label_lists = UnifiedVectorFormat::GetData<list_entry_t>(label_data);
		auto labels = UnifiedVectorFormat::GetData<string_t>(label_child_data);
		for (idx_t row = 0; row < chunk->size(); row++) {
			auto index = vertex_data.sel->get_index(row);
			if (!vertex_data.validity.RowIsValid(index)) {
				throw InvalidInputException("Table-backed CSR vertex keys must not contain NULL values");
			}
			auto vertex_id = ids[index];
			if (!snapshot->vertex_ids.empty() && snapshot->vertex_ids.back() == vertex_id) {
				throw InvalidInputException("Table-backed CSR requires unique vertex keys; duplicate key %llu",
				                            static_cast<unsigned long long>(vertex_id));
			}
			snapshot->vertex_ids.push_back(vertex_id);
			auto label_index = label_data.sel->get_index(row);
			if (label_data.validity.RowIsValid(label_index)) {
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
			snapshot->vertex_label_offsets.push_back(snapshot->vertex_label_ids.size());
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

	// Invert the per-vertex labels into compact posting lists. The posting
	// index belongs to the versioned CSR snapshot, so it inherits the same
	// invalidation rules as topology and can never outlive its source tables.
	const auto vertex_label_count = snapshot->label_ids.size();
	snapshot->vertex_label_posting_offsets.assign(vertex_label_count + 2, 0);
	for (const auto label_id : snapshot->vertex_label_ids) {
		snapshot->vertex_label_posting_offsets[label_id + 1]++;
	}
	FinalizeOffsets(snapshot->vertex_label_posting_offsets);
	const bool compact_vertex_ordinals =
	    snapshot->vertex_ids.empty() || snapshot->vertex_ids.size() - 1 <= std::numeric_limits<uint32_t>::max();
	snapshot->vertex_label_postings.Resize(snapshot->vertex_label_ids.size(), compact_vertex_ordinals);
	auto vertex_label_cursor = snapshot->vertex_label_posting_offsets;
	for (idx_t vertex = 0; vertex < snapshot->vertex_ids.size(); vertex++) {
		for (idx_t offset = snapshot->vertex_label_offsets[vertex]; offset < snapshot->vertex_label_offsets[vertex + 1];
		     offset++) {
			auto label_id = snapshot->vertex_label_ids[offset];
			snapshot->vertex_label_postings.Set(vertex_label_cursor[label_id]++, vertex);
		}
	}

	auto label_projection = binding.edge.label_column.empty()
	                            ? "CAST(NULL AS VARCHAR)"
	                            : "CAST(" + GqlQuoteIdentifier(binding.edge.label_column) + " AS VARCHAR)";
	auto edge_count = GqlQuery(connection, "SELECT count(*)::UBIGINT FROM " + QualifiedTable(binding.edge));
	auto expected_edges = NumericCast<idx_t>(edge_count->GetValue(0, 0).GetValue<uint64_t>());
	auto edge_key = "CAST(" + GqlQuoteIdentifier(binding.edge.key_column) + " AS UBIGINT)";
	auto edge_source = "CAST(" + GqlQuoteIdentifier(binding.edge_source_column) + " AS UBIGINT)";
	auto edge_target = "CAST(" + GqlQuoteIdentifier(binding.edge_target_column) + " AS UBIGINT)";
	auto edge_table = QualifiedTable(binding.edge);
	auto endpoint_projection = "SELECT " + edge_key + ", " + edge_source + ", " + edge_target + " FROM " + edge_table;
	auto edge_projection = "SELECT " + edge_key + ", " + edge_source + ", " + edge_target + ", " + label_projection +
	                       " FROM " + edge_table;
	// COPY GRAPH owns these tables and generates monotonically unique IDs. Keep
	// duplicate validation for any future non-managed/table-attachment path,
	// but do not build an O(E) hash set for the managed fast path.
	auto validate_edge_ids = binding.edge.schema_name != "gql_data" || binding.edge.key_column != "__gql_edge_id";
	unordered_set<uint64_t> edge_ids;
	if (validate_edge_ids) {
		edge_ids.reserve(expected_edges);
	}
	snapshot->outgoing_offsets.assign(snapshot->vertex_ids.size() + 1, 0);
	snapshot->incoming_offsets.assign(snapshot->vertex_ids.size() + 1, 0);
	auto degree_rows = connection.SendQuery(endpoint_projection);
	GqlThrowOnError(*degree_rows);
	idx_t counted_edges = 0;
	while (auto chunk = degree_rows->Fetch()) {
		UnifiedVectorFormat edge_id_data;
		UnifiedVectorFormat source_data;
		UnifiedVectorFormat target_data;
		chunk->data[0].ToUnifiedFormat(chunk->size(), edge_id_data);
		chunk->data[1].ToUnifiedFormat(chunk->size(), source_data);
		chunk->data[2].ToUnifiedFormat(chunk->size(), target_data);
		auto edge_id_values = UnifiedVectorFormat::GetData<uint64_t>(edge_id_data);
		auto source_values = UnifiedVectorFormat::GetData<uint64_t>(source_data);
		auto target_values = UnifiedVectorFormat::GetData<uint64_t>(target_data);
		for (idx_t row = 0; row < chunk->size(); row++) {
			auto edge_index = edge_id_data.sel->get_index(row);
			auto source_index = source_data.sel->get_index(row);
			auto target_index = target_data.sel->get_index(row);
			if (!edge_id_data.validity.RowIsValid(edge_index) || !source_data.validity.RowIsValid(source_index) ||
			    !target_data.validity.RowIsValid(target_index)) {
				throw InvalidInputException("Table-backed CSR edge keys and endpoints "
				                            "must not contain NULL values");
			}
			auto edge_id = edge_id_values[edge_index];
			idx_t source;
			idx_t target;
			if (!GqlTryGetCsrOrdinal(*snapshot, source_values[source_index], source) ||
			    !GqlTryGetCsrOrdinal(*snapshot, target_values[target_index], target)) {
				throw InvalidInputException("Graph contains edge %llu with an invalid endpoint",
				                            static_cast<unsigned long long>(edge_id));
			}
			snapshot->outgoing_offsets[source + 1]++;
			snapshot->incoming_offsets[target + 1]++;
			counted_edges++;
		}
	}
	if (counted_edges != expected_edges) {
		throw InternalException("GQL CSR edge scan returned an unexpected row count");
	}
	degree_rows.reset();

	FinalizeOffsets(snapshot->outgoing_offsets);
	FinalizeOffsets(snapshot->incoming_offsets);
	const bool compact_neighbors =
	    snapshot->vertex_ids.empty() || snapshot->vertex_ids.size() - 1 <= std::numeric_limits<uint32_t>::max();
	snapshot->outgoing_neighbors.Resize(expected_edges, compact_neighbors);
	snapshot->outgoing_edge_ids.resize(expected_edges);
	snapshot->outgoing_label_ids.Reset(expected_edges, true, 0);
	snapshot->incoming_neighbors.Resize(expected_edges, compact_neighbors);
	snapshot->incoming_edge_ids.resize(expected_edges);
	snapshot->incoming_label_ids.Reset(expected_edges, true, 0);
	auto outgoing_cursor = snapshot->outgoing_offsets;
	auto incoming_cursor = snapshot->incoming_offsets;
	auto edge_rows = connection.SendQuery(edge_projection);
	GqlThrowOnError(*edge_rows);
	idx_t scattered_edges = 0;
	bool edge_labels_uniform = true;
	bool saw_edge_label = false;
	uint32_t uniform_edge_label_id = 0;
	while (auto chunk = edge_rows->Fetch()) {
		UnifiedVectorFormat edge_id_data;
		UnifiedVectorFormat source_data;
		UnifiedVectorFormat target_data;
		UnifiedVectorFormat label_data;
		chunk->data[0].ToUnifiedFormat(chunk->size(), edge_id_data);
		chunk->data[1].ToUnifiedFormat(chunk->size(), source_data);
		chunk->data[2].ToUnifiedFormat(chunk->size(), target_data);
		chunk->data[3].ToUnifiedFormat(chunk->size(), label_data);
		auto edge_id_values = UnifiedVectorFormat::GetData<uint64_t>(edge_id_data);
		auto source_values = UnifiedVectorFormat::GetData<uint64_t>(source_data);
		auto target_values = UnifiedVectorFormat::GetData<uint64_t>(target_data);
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
			auto label_index = label_data.sel->get_index(row);
			auto label = label_data.validity.RowIsValid(label_index) ? label_values[label_index].GetString() : string();
			if (label.empty() || label.find(';') != string::npos) {
				throw InvalidInputException("Table-backed CSR requires every edge to have exactly one type");
			}
			auto label_id = CsrLabelId(*snapshot, label);
			if (!saw_edge_label) {
				uniform_edge_label_id = label_id;
				saw_edge_label = true;
				snapshot->outgoing_label_ids.Reset(expected_edges, true, label_id);
				snapshot->incoming_label_ids.Reset(expected_edges, true, label_id);
			} else if (edge_labels_uniform && label_id != uniform_edge_label_id) {
				edge_labels_uniform = false;
				snapshot->outgoing_label_ids.MaterializeUniform();
				snapshot->incoming_label_ids.MaterializeUniform();
			}
			auto outgoing = outgoing_cursor[source]++;
			snapshot->outgoing_neighbors.Set(outgoing, target);
			snapshot->outgoing_edge_ids[outgoing] = edge_id;
			if (!edge_labels_uniform) {
				snapshot->outgoing_label_ids.Set(outgoing, label_id);
			}
			auto incoming = incoming_cursor[target]++;
			snapshot->incoming_neighbors.Set(incoming, source);
			snapshot->incoming_edge_ids[incoming] = edge_id;
			if (!edge_labels_uniform) {
				snapshot->incoming_label_ids.Set(incoming, label_id);
			}
			scattered_edges++;
		}
	}
	if (scattered_edges != expected_edges) {
		throw InternalException("GQL CSR edge scan changed within a read transaction");
	}
	snapshot->topology_bytes =
	    snapshot->outgoing_offsets.capacity() * sizeof(uint64_t) + snapshot->outgoing_neighbors.AllocatedBytes() +
	    snapshot->incoming_offsets.capacity() * sizeof(uint64_t) + snapshot->incoming_neighbors.AllocatedBytes();
	snapshot->identity_bytes = snapshot->vertex_ids.AllocatedBytes() +
	                           snapshot->outgoing_edge_ids.capacity() * sizeof(uint64_t) +
	                           snapshot->incoming_edge_ids.capacity() * sizeof(uint64_t);
	snapshot->label_bytes =
	    snapshot->vertex_label_offsets.capacity() * sizeof(idx_t) +
	    snapshot->vertex_label_ids.capacity() * sizeof(uint32_t) +
	    snapshot->vertex_label_posting_offsets.capacity() * sizeof(uint64_t) +
	    snapshot->vertex_label_postings.AllocatedBytes() + snapshot->outgoing_label_ids.AllocatedBytes() +
	    snapshot->incoming_label_ids.AllocatedBytes() + LabelDictionaryStorageBytes(snapshot->label_ids);
	snapshot->auxiliary_bytes = sizeof(GqlCsrSnapshot) + HashContainerStorageBytes(snapshot->ordinal_by_id) +
	                            snapshot->vertex_table_key.capacity() + snapshot->edge_table_key.capacity();
	snapshot->build_auxiliary_bytes = transient_vertex_id_bytes + outgoing_cursor.capacity() * sizeof(uint64_t) +
	                                  incoming_cursor.capacity() * sizeof(uint64_t) +
	                                  HashContainerStorageBytes(edge_ids);
	snapshot->memory_bytes =
	    snapshot->topology_bytes + snapshot->identity_bytes + snapshot->label_bytes + snapshot->auxiliary_bytes;
	connection.Commit();
	return snapshot;
}

static shared_ptr<GqlCsrSnapshot> GetPreparedTableSnapshot(ClientContext &context, const string &graph_name,
                                                           GqlCsrCacheState &cache) {
	auto graph_id = cache.graph_ids_by_name.find(graph_name);
	if (graph_id == cache.graph_ids_by_name.end()) {
		throw InvalidInputException("CSR for table-backed graph '%s' has not been built on this "
		                            "connection; run CALL gql_build_csr('%s') first",
		                            graph_name, graph_name);
	}
	Connection connection(*context.db);
	auto graph = ReadGraphVersion(connection, graph_name);
	auto entry = cache.snapshots.find(graph_id->second);
	if (graph.graph_id != graph_id->second || entry == cache.snapshots.end() ||
	    entry->second->graph_version != graph.graph_version ||
	    entry->second->write_generation != ReadCsrWriteGeneration(context) ||
	    entry->second->vertex_write_generation !=
	        ReadCsrTableWriteGeneration(context, entry->second->vertex_table_key) ||
	    entry->second->edge_write_generation != ReadCsrTableWriteGeneration(context, entry->second->edge_table_key)) {
		throw InvalidInputException("CSR for table-backed graph '%s' has not been built on this "
		                            "connection; run CALL gql_build_csr('%s') first",
		                            graph_name, graph_name);
	}
	return entry->second;
}

shared_ptr<const GqlCsrSnapshot> GqlGetCsrSnapshot(ClientContext &context, const string &graph_name) {
	if (!context.transaction.IsAutoCommit()) {
		throw NotImplementedException("CSR algorithms are not eligible inside an explicit transaction");
	}
	auto cache = context.registered_state->GetOrCreate<GqlCsrCacheState>(GQL_CSR_STATE_KEY);
	return GetPreparedTableSnapshot(context, graph_name, *cache);
}

shared_ptr<const GqlCsrSnapshot> GqlTryGetCsrSnapshot(ClientContext &context, const string &graph_name) {
	if (!context.transaction.IsAutoCommit()) {
		return nullptr;
	}
	auto cache = context.registered_state->GetOrCreate<GqlCsrCacheState>(GQL_CSR_STATE_KEY);
	try {
		return GetPreparedTableSnapshot(context, graph_name, *cache);
	} catch (const InvalidInputException &) {
		return nullptr;
	}
}

struct CsrBindData : TableFunctionData {
	string graph_name;
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
                                                vector<LogicalType> &return_types, vector<string> &names) {
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
	idx_t cursor = 0;
	idx_t end = 0;
};

static unique_ptr<GlobalTableFunctionState> CsrVerticesInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &data = input.bind_data->Cast<CsrVerticesBindData>();
	auto result = make_uniq<CsrVerticesState>();
	result->snapshot = GqlGetCsrSnapshot(context, data.graph_name);
	TryCsrVertexLabelRange(*result->snapshot, data.label, result->cursor, result->end);
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
	output.SetCardinality(count);
}

static unique_ptr<NodeStatistics> CsrVerticesCardinality(ClientContext &, const FunctionData *bind_data) {
	auto &data = bind_data->Cast<CsrVerticesBindData>();
	return make_uniq<NodeStatistics>(data.estimated_count, data.estimated_count);
}

static unique_ptr<FunctionData> NeighborsBind(ClientContext &, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
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
		auto snapshot = GqlGetCsrSnapshot(context, data.graph_name);
		idx_t vertex;
		if (GqlTryGetCsrOrdinal(*snapshot, data.vertex_id, vertex)) {
			const auto &offsets = data.direction == "out" ? snapshot->outgoing_offsets : snapshot->incoming_offsets;
			const auto &neighbors =
			    data.direction == "out" ? snapshot->outgoing_neighbors : snapshot->incoming_neighbors;
			const auto &edge_ids = data.direction == "out" ? snapshot->outgoing_edge_ids : snapshot->incoming_edge_ids;
			for (idx_t index = offsets[vertex]; index < offsets[vertex + 1]; index++) {
				state.rows.emplace_back(snapshot->vertex_ids[neighbors[index]], edge_ids[index]);
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
	output.SetCardinality(count);
}

struct CsrExpandLocalState : LocalTableFunctionState {
	shared_ptr<const GqlCsrSnapshot> snapshot;
	idx_t cursor = 0;
	idx_t end = 0;
	bool initialized = false;
	bool outgoing = true;
	bool filter_label = false;
	bool label_exists = true;
	uint32_t label_id = 0;
	uint64_t vertex_id = 0;
	string edge_label;
};

static unique_ptr<FunctionData> CsrExpandBind(ClientContext &, TableFunctionBindInput &,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	names = {"__gql_edge_id", "__gql_source_id", "__gql_target_id", "__gql_type"};
	return_types = {LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::VARCHAR};
	return make_uniq<TableFunctionData>();
}

static unique_ptr<GlobalTableFunctionState> CsrExpandGlobalInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<GlobalTableFunctionState>();
}

static unique_ptr<LocalTableFunctionState> CsrExpandLocalInit(ExecutionContext &, TableFunctionInitInput &,
                                                              GlobalTableFunctionState *) {
	return make_uniq<CsrExpandLocalState>();
}

static void InitializeCsrExpansion(ExecutionContext &context, DataChunk &input, CsrExpandLocalState &state) {
	if (input.size() != 1 || input.ColumnCount() != 1) {
		throw InternalException("GQL CSR expansion requires one lateral input row");
	}
	auto packed = input.data[0].GetValue(0);
	if (packed.IsNull()) {
		state.initialized = true;
		return;
	}
	auto &fields = StructValue::GetChildren(packed);
	if (fields.size() != 4 || fields[0].IsNull() || fields[2].IsNull() || fields[3].IsNull()) {
		throw InvalidInputException("GQL CSR expansion requires graph, direction, and edge label");
	}
	auto graph_name = fields[0].GetValue<string>();
	auto direction = StringUtil::Lower(fields[2].GetValue<string>());
	state.edge_label = StringUtil::Lower(fields[3].GetValue<string>());
	if (direction != "out" && direction != "in") {
		throw InvalidInputException("GQL CSR expansion direction must be 'out' or 'in'");
	}
	if (state.edge_label.empty()) {
		throw InvalidInputException("GQL CSR expansion requires one edge label");
	}
	state.outgoing = direction == "out";
	state.filter_label = true;
	state.snapshot = GqlGetCsrSnapshot(context.client, graph_name);
	auto label = state.snapshot->label_ids.find(state.edge_label);
	if (label == state.snapshot->label_ids.end()) {
		state.label_exists = false;
		state.initialized = true;
		return;
	}
	state.label_id = label->second;
	if (fields[1].IsNull()) {
		state.initialized = true;
		return;
	}
	state.vertex_id = fields[1].GetValue<uint64_t>();
	idx_t vertex;
	if (!GqlTryGetCsrOrdinal(*state.snapshot, state.vertex_id, vertex)) {
		state.initialized = true;
		return;
	}
	const auto &offsets = state.outgoing ? state.snapshot->outgoing_offsets : state.snapshot->incoming_offsets;
	state.cursor = offsets[vertex];
	state.end = offsets[vertex + 1];
	state.initialized = true;
}

static OperatorResultType CsrExpandFunction(ExecutionContext &context, TableFunctionInput &data_p, DataChunk &input,
                                            DataChunk &output) {
	auto &state = data_p.local_state->Cast<CsrExpandLocalState>();
	if (!state.initialized) {
		InitializeCsrExpansion(context, input, state);
	}
	if (!state.snapshot || !state.label_exists) {
		state = CsrExpandLocalState();
		return OperatorResultType::NEED_MORE_INPUT;
	}

	const auto &neighbors = state.outgoing ? state.snapshot->outgoing_neighbors : state.snapshot->incoming_neighbors;
	const auto &edge_ids = state.outgoing ? state.snapshot->outgoing_edge_ids : state.snapshot->incoming_edge_ids;
	const auto &label_ids = state.outgoing ? state.snapshot->outgoing_label_ids : state.snapshot->incoming_label_ids;
	idx_t count = 0;
	while (state.cursor < state.end && count < STANDARD_VECTOR_SIZE) {
		auto index = state.cursor++;
		if (state.filter_label && label_ids[index] != state.label_id) {
			continue;
		}
		auto neighbor_id = state.snapshot->vertex_ids[neighbors[index]];
		output.SetValue(0, count, Value::UBIGINT(edge_ids[index]));
		output.SetValue(1, count, Value::UBIGINT(state.outgoing ? state.vertex_id : neighbor_id));
		output.SetValue(2, count, Value::UBIGINT(state.outgoing ? neighbor_id : state.vertex_id));
		output.SetValue(3, count, Value(state.edge_label));
		count++;
	}
	output.SetCardinality(count);
	if (state.cursor < state.end) {
		return OperatorResultType::HAVE_MORE_OUTPUT;
	}
	state = CsrExpandLocalState();
	return OperatorResultType::NEED_MORE_INPUT;
}

static unique_ptr<FunctionData> CsrStatsBind(ClientContext &, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<CsrBindData>();
	result->graph_name = input.inputs[0].GetValue<string>();
	names = {
	    "graph_name",     "graph_version",  "vertex_count",         "edge_count",          "memory_bytes",
	    "build_count",    "cached",         "neighbor_width_bytes", "vertex_ids_explicit", "edge_labels_uniform",
	    "topology_bytes", "identity_bytes", "label_bytes",          "auxiliary_bytes",     "build_auxiliary_bytes"};
	return_types = {LogicalType::VARCHAR, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT,
	                LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::BOOLEAN, LogicalType::UBIGINT,
	                LogicalType::BOOLEAN, LogicalType::BOOLEAN, LogicalType::UBIGINT, LogicalType::UBIGINT,
	                LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT};
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
	auto cache = context.registered_state->GetOrCreate<GqlCsrCacheState>(GQL_CSR_STATE_KEY);
	GqlTableGraphBinding binding;
	if (!GqlTryLoadTableGraph(context, data.graph_name, binding)) {
		throw InvalidInputException("Graph '%s' has no native tables; load it with "
		                            "COPY GRAPH before building CSR",
		                            data.graph_name);
	}
	auto snapshot = BuildTableSnapshot(context, data.graph_name);
	cache->snapshots[snapshot->graph_id] = snapshot;
	cache->graph_ids_by_name[data.graph_name] = snapshot->graph_id;
	cache->build_count++;

	output.SetCardinality(1);
	output.SetValue(0, 0, Value(data.graph_name));
	output.SetValue(1, 0, Value::UBIGINT(snapshot->graph_version));
	output.SetValue(2, 0, Value::UBIGINT(snapshot->vertex_ids.size()));
	output.SetValue(3, 0, Value::UBIGINT(snapshot->outgoing_edge_ids.size()));
	output.SetValue(4, 0, Value::UBIGINT(snapshot->memory_bytes));
	output.SetValue(5, 0, Value::UBIGINT(cache->build_count));
	state.done = true;
}

static void CsrStatsFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<SingleRowState>();
	if (state.done) {
		return;
	}
	auto &data = input.bind_data->Cast<CsrBindData>();
	auto cache = context.registered_state->GetOrCreate<GqlCsrCacheState>(GQL_CSR_STATE_KEY);
	auto snapshot = GqlGetCsrSnapshot(context, data.graph_name);
	output.SetCardinality(1);
	output.SetValue(0, 0, Value(data.graph_name));
	output.SetValue(1, 0, Value::UBIGINT(snapshot->graph_version));
	output.SetValue(2, 0, Value::UBIGINT(snapshot->vertex_ids.size()));
	output.SetValue(3, 0, Value::UBIGINT(snapshot->outgoing_edge_ids.size()));
	output.SetValue(4, 0, Value::UBIGINT(snapshot->memory_bytes));
	output.SetValue(5, 0, Value::UBIGINT(cache->build_count));
	output.SetValue(6, 0, Value(true));
	output.SetValue(7, 0, Value::UBIGINT(snapshot->outgoing_neighbors.WidthBytes()));
	output.SetValue(8, 0, Value(!snapshot->vertex_ids.IsImplicitDense()));
	output.SetValue(9, 0, Value(snapshot->outgoing_label_ids.IsUniform()));
	output.SetValue(10, 0, Value::UBIGINT(snapshot->topology_bytes));
	output.SetValue(11, 0, Value::UBIGINT(snapshot->identity_bytes));
	output.SetValue(12, 0, Value::UBIGINT(snapshot->label_bytes));
	output.SetValue(13, 0, Value::UBIGINT(snapshot->auxiliary_bytes));
	output.SetValue(14, 0, Value::UBIGINT(snapshot->build_auxiliary_bytes));
	state.done = true;
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

TableFunction GqlCsrExpandFunction() {
	auto input_type = LogicalType::STRUCT({{"graph_name", LogicalType::VARCHAR},
	                                       {"vertex_id", LogicalType::UBIGINT},
	                                       {"direction", LogicalType::VARCHAR},
	                                       {"edge_label", LogicalType::VARCHAR}});
	TableFunction function("gql_csr_expand", {input_type}, nullptr, CsrExpandBind, CsrExpandGlobalInit,
	                       CsrExpandLocalInit);
	function.in_out_function = CsrExpandFunction;
	return function;
}

TableFunction GqlBuildCsrFunction() {
	TableFunction function("gql_build_csr", {LogicalType::VARCHAR}, BuildCsrFunction);
	function.bind = [](ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &return_types,
	                   vector<string> &names) -> unique_ptr<FunctionData> {
		auto result = make_uniq<CsrBindData>();
		result->graph_name = input.inputs[0].GetValue<string>();
		names = {"graph_name", "graph_version", "vertex_count", "edge_count", "memory_bytes", "build_count"};
		return_types = {LogicalType::VARCHAR, LogicalType::UBIGINT, LogicalType::UBIGINT,
		                LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT};
		return std::move(result);
	};
	function.init_global = SingleRowInit;
	return function;
}

TableFunction GqlCsrStatsFunction() {
	TableFunction function("gql_csr_stats", {LogicalType::VARCHAR}, CsrStatsFunction);
	function.bind = CsrStatsBind;
	function.init_global = SingleRowInit;
	return function;
}

} // namespace duckdb
