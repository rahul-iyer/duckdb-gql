#include "gql_csr.hpp"

#include "gql_catalog.hpp"
#include "gql_storage.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/parser/keyword_helper.hpp"

#include <unordered_map>
#include <unordered_set>

namespace duckdb {

static constexpr const char *GQL_CSR_STATE_KEY = "gql_csr_state";

static string QuoteLiteral(const string &value) {
  string result = "'";
  for (const auto character : value) {
    result += character == '\'' ? "''" : string(1, character);
  }
  return result + "'";
}

static string QuoteIdentifier(const string &value) {
  return KeywordHelper::WriteQuoted(value, '"');
}

static string QualifiedTable(const GqlElementTableBinding &table) {
  return QuoteIdentifier(table.catalog_name) + "." +
         QuoteIdentifier(table.schema_name) + "." +
         QuoteIdentifier(table.table_name);
}

static void ThrowOnError(const BaseQueryResult &result) {
  if (result.HasError()) {
    throw InvalidInputException("GQL CSR error: %s", result.GetError());
  }
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

static GraphVersion ReadGraphVersion(Connection &connection,
                                     const string &graph_name) {
  auto result = connection.Query("SELECT graph_id, graph_version FROM "
                                 "gql_internal.graphs WHERE graph_name = " +
                                 QuoteLiteral(graph_name));
  ThrowOnError(*result);
  if (result->RowCount() == 0) {
    throw InvalidInputException("Graph '%s' does not exist", graph_name);
  }
  return {result->GetValue(0, 0).GetValue<uint64_t>(),
          result->GetValue(1, 0).GetValue<uint64_t>()};
}

static void FinalizeOffsets(vector<uint64_t> &offsets) {
  for (idx_t index = 1; index < offsets.size(); index++) {
    offsets[index] += offsets[index - 1];
  }
}

bool GqlTryGetCsrOrdinal(const GqlCsrSnapshot &snapshot, uint64_t vertex_id,
                         idx_t &ordinal) {
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
         container.size() *
             (sizeof(typename HASH_CONTAINER::value_type) + 2 * sizeof(void *));
}

static idx_t LabelDictionaryStorageBytes(
    const unordered_map<string, uint32_t> &labels) {
  idx_t bytes = HashContainerStorageBytes(labels);
  for (const auto &entry : labels) {
    bytes += entry.first.capacity() + 1;
  }
  return bytes;
}

static shared_ptr<GqlCsrSnapshot> BuildTableSnapshot(ClientContext &context,
                                                     const string &graph_name) {
  GqlTableGraphBinding binding;
  if (!GqlTryLoadTableGraph(context, graph_name, binding)) {
    throw InvalidInputException("CSR algorithms require a table-backed graph; "
                                "load graph '%s' with COPY GRAPH first",
                                graph_name);
  }
  Connection connection(*context.db);
  auto snapshot = make_shared_ptr<GqlCsrSnapshot>();
  connection.BeginTransaction();
  auto graph = ReadGraphVersion(connection, graph_name);
  snapshot->graph_id = graph.graph_id;
  snapshot->graph_version = graph.graph_version;

  auto vertex_count = connection.Query("SELECT count(*)::UBIGINT FROM " +
                                       QualifiedTable(binding.vertex));
  ThrowOnError(*vertex_count);
  snapshot->vertex_ids.reserve(
      NumericCast<idx_t>(vertex_count->GetValue(0, 0).GetValue<uint64_t>()));
  auto vertex_label_projection =
      binding.vertex.label_column.empty()
          ? "CAST(NULL AS VARCHAR)"
          : "CAST(" + QuoteIdentifier(binding.vertex.label_column) +
                " AS VARCHAR)";
  auto vertices = connection.SendQuery(
      "SELECT CAST(" + QuoteIdentifier(binding.vertex.key_column) +
      " AS UBIGINT) AS vertex_id, " + vertex_label_projection + " FROM " +
      QualifiedTable(binding.vertex) + " ORDER BY vertex_id");
  ThrowOnError(*vertices);
  snapshot->vertex_label_offsets.push_back(0);
  while (auto chunk = vertices->Fetch()) {
    UnifiedVectorFormat vertex_data;
    UnifiedVectorFormat label_data;
    chunk->data[0].ToUnifiedFormat(chunk->size(), vertex_data);
    chunk->data[1].ToUnifiedFormat(chunk->size(), label_data);
    auto ids = UnifiedVectorFormat::GetData<uint64_t>(vertex_data);
    auto labels = UnifiedVectorFormat::GetData<string_t>(label_data);
    for (idx_t row = 0; row < chunk->size(); row++) {
      auto index = vertex_data.sel->get_index(row);
      if (!vertex_data.validity.RowIsValid(index)) {
        throw InvalidInputException(
            "Table-backed CSR vertex keys must not contain NULL values");
      }
      auto vertex_id = ids[index];
      if (!snapshot->vertex_ids.empty() &&
          snapshot->vertex_ids.back() == vertex_id) {
        throw InvalidInputException(
            "Table-backed CSR requires unique vertex keys; duplicate key %llu",
            static_cast<unsigned long long>(vertex_id));
      }
      snapshot->vertex_ids.push_back(vertex_id);
      auto label_index = label_data.sel->get_index(row);
      if (label_data.validity.RowIsValid(label_index)) {
        for (auto &label :
             StringUtil::Split(labels[label_index].GetString(), ';')) {
          if (!label.empty()) {
            snapshot->vertex_label_ids.push_back(CsrLabelId(*snapshot, label));
          }
        }
      }
      snapshot->vertex_label_offsets.push_back(
          snapshot->vertex_label_ids.size());
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

  auto label_projection = binding.edge.label_column.empty()
                              ? "CAST(NULL AS VARCHAR)"
                              : "CAST(" +
                                    QuoteIdentifier(binding.edge.label_column) +
                                    " AS VARCHAR)";
  auto edge_count = connection.Query("SELECT count(*)::UBIGINT FROM " +
                                     QualifiedTable(binding.edge));
  ThrowOnError(*edge_count);
  auto expected_edges =
      NumericCast<idx_t>(edge_count->GetValue(0, 0).GetValue<uint64_t>());
  auto edge_key =
      "CAST(" + QuoteIdentifier(binding.edge.key_column) + " AS UBIGINT)";
  auto edge_source =
      "CAST(" + QuoteIdentifier(binding.edge_source_column) + " AS UBIGINT)";
  auto edge_target =
      "CAST(" + QuoteIdentifier(binding.edge_target_column) + " AS UBIGINT)";
  auto edge_table = QualifiedTable(binding.edge);
  auto endpoint_projection = "SELECT " + edge_key + ", " + edge_source + ", " +
                             edge_target + " FROM " + edge_table;
  auto edge_projection = "SELECT " + edge_key + ", " + edge_source + ", " +
                         edge_target + ", " + label_projection + " FROM " +
                         edge_table;
  // COPY GRAPH owns these tables and generates monotonically unique IDs. Keep
  // duplicate validation for any future non-managed/table-attachment path,
  // but do not build an O(E) hash set for the managed fast path.
  auto validate_edge_ids = binding.edge.schema_name != "gql_data" ||
                           binding.edge.key_column != "__gql_edge_id";
  unordered_set<uint64_t> edge_ids;
  if (validate_edge_ids) {
    edge_ids.reserve(expected_edges);
  }
  snapshot->outgoing_offsets.assign(snapshot->vertex_ids.size() + 1, 0);
  snapshot->incoming_offsets.assign(snapshot->vertex_ids.size() + 1, 0);
  auto degree_rows = connection.SendQuery(endpoint_projection);
  ThrowOnError(*degree_rows);
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
      if (!edge_id_data.validity.RowIsValid(edge_index) ||
          !source_data.validity.RowIsValid(source_index) ||
          !target_data.validity.RowIsValid(target_index)) {
        throw InvalidInputException("Table-backed CSR edge keys and endpoints "
                                    "must not contain NULL values");
      }
      auto edge_id = edge_id_values[edge_index];
      idx_t source;
      idx_t target;
      if (!GqlTryGetCsrOrdinal(*snapshot, source_values[source_index],
                               source) ||
          !GqlTryGetCsrOrdinal(*snapshot, target_values[target_index],
                               target)) {
        throw InvalidInputException(
            "Graph contains edge %llu with an invalid endpoint",
            static_cast<unsigned long long>(edge_id));
      }
      snapshot->outgoing_offsets[source + 1]++;
      snapshot->incoming_offsets[target + 1]++;
      counted_edges++;
    }
  }
  if (counted_edges != expected_edges) {
    throw InternalException(
        "GQL CSR edge scan returned an unexpected row count");
  }
  degree_rows.reset();

  FinalizeOffsets(snapshot->outgoing_offsets);
  FinalizeOffsets(snapshot->incoming_offsets);
  const bool compact_neighbors =
      snapshot->vertex_ids.empty() ||
      snapshot->vertex_ids.size() - 1 <=
          std::numeric_limits<uint32_t>::max();
  snapshot->outgoing_neighbors.Resize(expected_edges, compact_neighbors);
  snapshot->outgoing_edge_ids.resize(expected_edges);
  snapshot->outgoing_label_ids.Reset(expected_edges, true, 0);
  snapshot->incoming_neighbors.Resize(expected_edges, compact_neighbors);
  snapshot->incoming_edge_ids.resize(expected_edges);
  snapshot->incoming_label_ids.Reset(expected_edges, true, 0);
  auto outgoing_cursor = snapshot->outgoing_offsets;
  auto incoming_cursor = snapshot->incoming_offsets;
  auto edge_rows = connection.SendQuery(edge_projection);
  ThrowOnError(*edge_rows);
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
        throw InvalidInputException(
            "Table-backed CSR requires unique edge keys; duplicate key %llu",
            static_cast<unsigned long long>(edge_id));
      }
      idx_t source;
      idx_t target;
      if (!GqlTryGetCsrOrdinal(*snapshot, source_values[source_index],
                               source) ||
          !GqlTryGetCsrOrdinal(*snapshot, target_values[target_index],
                               target)) {
        throw InternalException(
            "GQL CSR endpoints changed within a read transaction");
      }
      auto label_index = label_data.sel->get_index(row);
      auto label = label_data.validity.RowIsValid(label_index)
                       ? label_values[label_index].GetString()
                       : string();
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
    throw InternalException(
        "GQL CSR edge scan changed within a read transaction");
  }
  snapshot->topology_bytes =
      snapshot->outgoing_offsets.capacity() * sizeof(uint64_t) +
      snapshot->outgoing_neighbors.AllocatedBytes() +
      snapshot->incoming_offsets.capacity() * sizeof(uint64_t) +
      snapshot->incoming_neighbors.AllocatedBytes();
  snapshot->identity_bytes =
      snapshot->vertex_ids.AllocatedBytes() +
      snapshot->outgoing_edge_ids.capacity() * sizeof(uint64_t) +
      snapshot->incoming_edge_ids.capacity() * sizeof(uint64_t);
  snapshot->label_bytes =
      snapshot->vertex_label_offsets.capacity() * sizeof(idx_t) +
      snapshot->vertex_label_ids.capacity() * sizeof(uint32_t) +
      snapshot->outgoing_label_ids.AllocatedBytes() +
      snapshot->incoming_label_ids.AllocatedBytes() +
      LabelDictionaryStorageBytes(snapshot->label_ids);
  snapshot->auxiliary_bytes =
      sizeof(GqlCsrSnapshot) +
      HashContainerStorageBytes(snapshot->ordinal_by_id);
  snapshot->build_auxiliary_bytes =
      transient_vertex_id_bytes +
      outgoing_cursor.capacity() * sizeof(uint64_t) +
      incoming_cursor.capacity() * sizeof(uint64_t) +
      HashContainerStorageBytes(edge_ids);
  snapshot->memory_bytes =
      snapshot->topology_bytes + snapshot->identity_bytes +
      snapshot->label_bytes + snapshot->auxiliary_bytes;
  connection.Commit();
  return snapshot;
}

static shared_ptr<GqlCsrSnapshot>
GetPreparedTableSnapshot(ClientContext &context, const string &graph_name,
                         GqlCsrCacheState &cache) {
  auto graph_id = cache.graph_ids_by_name.find(graph_name);
  if (graph_id == cache.graph_ids_by_name.end()) {
    throw InvalidInputException(
        "CSR for table-backed graph '%s' has not been built on this "
        "connection; run CALL gql_build_csr('%s') first",
        graph_name, graph_name);
  }
  Connection connection(*context.db);
  auto graph = ReadGraphVersion(connection, graph_name);
  auto entry = cache.snapshots.find(graph_id->second);
  if (graph.graph_id != graph_id->second || entry == cache.snapshots.end() ||
      entry->second->graph_version != graph.graph_version) {
    throw InvalidInputException(
        "CSR for table-backed graph '%s' has not been built on this "
        "connection; run CALL gql_build_csr('%s') first",
        graph_name, graph_name);
  }
  return entry->second;
}

shared_ptr<const GqlCsrSnapshot> GqlGetCsrSnapshot(ClientContext &context,
                                                   const string &graph_name) {
  if (!context.transaction.IsAutoCommit()) {
    throw NotImplementedException(
        "CSR algorithms are not eligible inside an explicit transaction");
  }
  auto cache = context.registered_state->GetOrCreate<GqlCsrCacheState>(
      GQL_CSR_STATE_KEY);
  return GetPreparedTableSnapshot(context, graph_name, *cache);
}

struct CsrBindData : TableFunctionData {
  string graph_name;
  uint64_t vertex_id = 0;
  string direction;
};

static unique_ptr<FunctionData> NeighborsBind(ClientContext &,
                                              TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types,
                                              vector<string> &names) {
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

static unique_ptr<GlobalTableFunctionState>
NeighborsInit(ClientContext &, TableFunctionInitInput &) {
  return make_uniq<NeighborRowsState>();
}

static void NeighborsFunction(ClientContext &context, TableFunctionInput &input,
                              DataChunk &output) {
  auto &state = input.global_state->Cast<NeighborRowsState>();
  if (!state.initialized) {
    auto &data = input.bind_data->Cast<CsrBindData>();
    auto snapshot = GqlGetCsrSnapshot(context, data.graph_name);
    idx_t vertex;
    if (GqlTryGetCsrOrdinal(*snapshot, data.vertex_id, vertex)) {
      const auto &offsets = data.direction == "out"
                                ? snapshot->outgoing_offsets
                                : snapshot->incoming_offsets;
      const auto &neighbors = data.direction == "out"
                                  ? snapshot->outgoing_neighbors
                                  : snapshot->incoming_neighbors;
      const auto &edge_ids = data.direction == "out"
                                 ? snapshot->outgoing_edge_ids
                                 : snapshot->incoming_edge_ids;
      for (idx_t index = offsets[vertex]; index < offsets[vertex + 1];
           index++) {
        state.rows.emplace_back(snapshot->vertex_ids[neighbors[index]],
                                edge_ids[index]);
      }
    }
    state.initialized = true;
  }
  auto count =
      MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
  for (idx_t index = 0; index < count; index++) {
    output.SetValue(0, index,
                    Value::UBIGINT(state.rows[state.offset + index].first));
    output.SetValue(1, index,
                    Value::UBIGINT(state.rows[state.offset + index].second));
  }
  state.offset += count;
  output.SetCardinality(count);
}

static unique_ptr<FunctionData> CsrStatsBind(ClientContext &,
                                             TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types,
                                             vector<string> &names) {
  auto result = make_uniq<CsrBindData>();
  result->graph_name = input.inputs[0].GetValue<string>();
  names = {"graph_name",   "graph_version", "vertex_count", "edge_count",
           "memory_bytes", "build_count",   "cached",
           "neighbor_width_bytes", "vertex_ids_explicit",
           "edge_labels_uniform", "topology_bytes", "identity_bytes",
           "label_bytes", "auxiliary_bytes", "build_auxiliary_bytes"};
  return_types = {LogicalType::VARCHAR, LogicalType::UBIGINT,
                  LogicalType::UBIGINT, LogicalType::UBIGINT,
                  LogicalType::UBIGINT, LogicalType::UBIGINT,
                  LogicalType::BOOLEAN, LogicalType::UBIGINT,
                  LogicalType::BOOLEAN, LogicalType::BOOLEAN,
                  LogicalType::UBIGINT, LogicalType::UBIGINT,
                  LogicalType::UBIGINT, LogicalType::UBIGINT,
                  LogicalType::UBIGINT};
  return std::move(result);
}

struct SingleRowState : GlobalTableFunctionState {
  bool done = false;
};

static unique_ptr<GlobalTableFunctionState>
SingleRowInit(ClientContext &, TableFunctionInitInput &) {
  return make_uniq<SingleRowState>();
}

static void BuildCsrFunction(ClientContext &context, TableFunctionInput &input,
                             DataChunk &output) {
  auto &state = input.global_state->Cast<SingleRowState>();
  if (state.done) {
    return;
  }
  if (!context.transaction.IsAutoCommit()) {
    throw NotImplementedException(
        "CSR construction is not eligible inside an explicit transaction");
  }
  auto &data = input.bind_data->Cast<CsrBindData>();
  auto cache = context.registered_state->GetOrCreate<GqlCsrCacheState>(
      GQL_CSR_STATE_KEY);
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

static void CsrStatsFunction(ClientContext &context, TableFunctionInput &input,
                             DataChunk &output) {
  auto &state = input.global_state->Cast<SingleRowState>();
  if (state.done) {
    return;
  }
  auto &data = input.bind_data->Cast<CsrBindData>();
  auto cache = context.registered_state->GetOrCreate<GqlCsrCacheState>(
      GQL_CSR_STATE_KEY);
  auto snapshot = GqlGetCsrSnapshot(context, data.graph_name);
  output.SetCardinality(1);
  output.SetValue(0, 0, Value(data.graph_name));
  output.SetValue(1, 0, Value::UBIGINT(snapshot->graph_version));
  output.SetValue(2, 0, Value::UBIGINT(snapshot->vertex_ids.size()));
  output.SetValue(3, 0, Value::UBIGINT(snapshot->outgoing_edge_ids.size()));
  output.SetValue(4, 0, Value::UBIGINT(snapshot->memory_bytes));
  output.SetValue(5, 0, Value::UBIGINT(cache->build_count));
  output.SetValue(6, 0, Value(true));
  output.SetValue(
      7, 0,
      Value::UBIGINT(snapshot->outgoing_neighbors.WidthBytes()));
  output.SetValue(8, 0,
                  Value(!snapshot->vertex_ids.IsImplicitDense()));
  output.SetValue(9, 0,
                  Value(snapshot->outgoing_label_ids.IsUniform()));
  output.SetValue(10, 0, Value::UBIGINT(snapshot->topology_bytes));
  output.SetValue(11, 0, Value::UBIGINT(snapshot->identity_bytes));
  output.SetValue(12, 0, Value::UBIGINT(snapshot->label_bytes));
  output.SetValue(13, 0, Value::UBIGINT(snapshot->auxiliary_bytes));
  output.SetValue(14, 0,
                  Value::UBIGINT(snapshot->build_auxiliary_bytes));
  state.done = true;
}

TableFunction GqlNeighborsFunction() {
  TableFunction function(
      "gql_neighbors",
      {LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR},
      NeighborsFunction);
  function.bind = NeighborsBind;
  function.init_global = NeighborsInit;
  return function;
}

TableFunction GqlBuildCsrFunction() {
  TableFunction function("gql_build_csr", {LogicalType::VARCHAR},
                         BuildCsrFunction);
  function.bind = [](ClientContext &, TableFunctionBindInput &input,
                     vector<LogicalType> &return_types,
                     vector<string> &names) -> unique_ptr<FunctionData> {
    auto result = make_uniq<CsrBindData>();
    result->graph_name = input.inputs[0].GetValue<string>();
    names = {"graph_name", "graph_version", "vertex_count",
             "edge_count", "memory_bytes",  "build_count"};
    return_types = {LogicalType::VARCHAR, LogicalType::UBIGINT,
                    LogicalType::UBIGINT, LogicalType::UBIGINT,
                    LogicalType::UBIGINT, LogicalType::UBIGINT};
    return std::move(result);
  };
  function.init_global = SingleRowInit;
  return function;
}

TableFunction GqlCsrStatsFunction() {
  TableFunction function("gql_csr_stats", {LogicalType::VARCHAR},
                         CsrStatsFunction);
  function.bind = CsrStatsBind;
  function.init_global = SingleRowInit;
  return function;
}

} // namespace duckdb
