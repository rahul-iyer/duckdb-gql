#include "gql_csr.hpp"

#include "gql_catalog.hpp"
#include "gql_ir.hpp"
#include "gql_storage.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/parser/keyword_helper.hpp"

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {

static constexpr const char *GQL_CSR_STATE_KEY = "gql_csr_state";
static constexpr idx_t GQL_CSR_PATH_BATCH_SIZE = 128;

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

static void ThrowOnError(const MaterializedQueryResult &result) {
  if (result.HasError()) {
    throw InvalidInputException("GQL CSR error: %s", result.GetError());
  }
}

struct CsrEdge {
  uint64_t edge_id;
  idx_t source;
  idx_t target;
  uint32_t label_id;
};

struct GqlCsrSnapshot {
  uint64_t graph_id;
  uint64_t graph_version;
  vector<uint64_t> vertex_ids;
  unordered_map<uint64_t, idx_t> ordinal_by_id;
  vector<idx_t> outgoing_offsets;
  vector<uint64_t> outgoing_neighbors;
  vector<uint64_t> outgoing_edge_ids;
  vector<uint32_t> outgoing_label_ids;
  vector<idx_t> incoming_offsets;
  vector<uint64_t> incoming_neighbors;
  vector<uint64_t> incoming_edge_ids;
  vector<uint32_t> incoming_label_ids;
  unordered_map<string, uint32_t> label_ids;
  idx_t memory_bytes;
};

struct GqlCsrCacheState : ClientContextState {
  unordered_map<uint64_t, shared_ptr<GqlCsrSnapshot>> snapshots;
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

static vector<idx_t> BuildOffsets(idx_t vertex_count,
                                  const vector<CsrEdge> &edges, bool outgoing) {
  vector<idx_t> offsets(vertex_count + 1, 0);
  for (const auto &edge : edges) {
    auto ordinal = outgoing ? edge.source : edge.target;
    offsets[ordinal + 1]++;
  }
  for (idx_t index = 1; index < offsets.size(); index++) {
    offsets[index] += offsets[index - 1];
  }
  return offsets;
}

static uint32_t CsrLabelId(GqlCsrSnapshot &snapshot, const string &label) {
  if (label.empty()) {
    return 0;
  }
  auto entry = snapshot.label_ids.find(label);
  if (entry != snapshot.label_ids.end()) {
    return entry->second;
  }
  auto identifier = NumericCast<uint32_t>(snapshot.label_ids.size() + 1);
  snapshot.label_ids.emplace(label, identifier);
  return identifier;
}

static shared_ptr<GqlCsrSnapshot> BuildTableSnapshot(ClientContext &context,
                                                     const string &graph_name) {
  GqlTableGraphBinding binding;
  if (!GqlTryLoadTableGraph(context, graph_name, binding)) {
    throw NotImplementedException(
        "CSR execution hint currently requires a table-backed graph");
  }
  Connection connection(*context.db);
  auto graph = ReadGraphVersion(connection, graph_name);
  auto snapshot = make_shared_ptr<GqlCsrSnapshot>();
  snapshot->graph_id = graph.graph_id;
  snapshot->graph_version = graph.graph_version;

  auto vertices = connection.Query(
      "SELECT CAST(" + QuoteIdentifier(binding.vertex.key_column) +
      " AS UBIGINT) AS vertex_id FROM " + QualifiedTable(binding.vertex) +
      " ORDER BY vertex_id");
  ThrowOnError(*vertices);
  for (idx_t row = 0; row < vertices->RowCount(); row++) {
    auto vertex_id = vertices->GetValue(0, row).GetValue<uint64_t>();
    if (!snapshot->ordinal_by_id.emplace(vertex_id, snapshot->vertex_ids.size())
             .second) {
      throw InvalidInputException(
          "Table-backed CSR requires unique vertex keys; duplicate key %llu",
          static_cast<unsigned long long>(vertex_id));
    }
    snapshot->vertex_ids.push_back(vertex_id);
  }

  auto label_projection = binding.edge.label_column.empty()
                              ? "CAST(NULL AS VARCHAR)"
                              : "CAST(" +
                                    QuoteIdentifier(binding.edge.label_column) +
                                    " AS VARCHAR)";
  auto edge_rows = connection.Query(
      "SELECT CAST(" + QuoteIdentifier(binding.edge.key_column) +
      " AS UBIGINT), CAST(" + QuoteIdentifier(binding.edge_source_column) +
      " AS UBIGINT), CAST(" + QuoteIdentifier(binding.edge_target_column) +
      " AS UBIGINT), " + label_projection + " FROM " +
      QualifiedTable(binding.edge));
  ThrowOnError(*edge_rows);
  vector<CsrEdge> edges;
  unordered_set<uint64_t> edge_ids;
  for (idx_t row = 0; row < edge_rows->RowCount(); row++) {
    auto edge_id = edge_rows->GetValue(0, row).GetValue<uint64_t>();
    if (!edge_ids.insert(edge_id).second) {
      throw InvalidInputException(
          "Table-backed CSR requires unique edge keys; duplicate key %llu",
          static_cast<unsigned long long>(edge_id));
    }
    auto source_id = edge_rows->GetValue(1, row).GetValue<uint64_t>();
    auto target_id = edge_rows->GetValue(2, row).GetValue<uint64_t>();
    auto source = snapshot->ordinal_by_id.find(source_id);
    auto target = snapshot->ordinal_by_id.find(target_id);
    if (source == snapshot->ordinal_by_id.end() ||
        target == snapshot->ordinal_by_id.end()) {
      throw InvalidInputException(
          "Graph contains edge %llu with an invalid endpoint",
          static_cast<unsigned long long>(edge_id));
    }
    auto label = edge_rows->GetValue(3, row).IsNull()
                     ? string()
                     : edge_rows->GetValue(3, row).GetValue<string>();
    edges.push_back({edge_id, source->second, target->second,
                     CsrLabelId(*snapshot, label)});
  }

  auto outgoing = edges;
  std::sort(
      outgoing.begin(), outgoing.end(),
      [](const CsrEdge &left, const CsrEdge &right) {
        return left.source < right.source ||
               (left.source == right.source &&
                (left.target < right.target || (left.target == right.target &&
                                                left.edge_id < right.edge_id)));
      });
  snapshot->outgoing_offsets =
      BuildOffsets(snapshot->vertex_ids.size(), outgoing, true);
  for (const auto &edge : outgoing) {
    snapshot->outgoing_neighbors.push_back(snapshot->vertex_ids[edge.target]);
    snapshot->outgoing_edge_ids.push_back(edge.edge_id);
    snapshot->outgoing_label_ids.push_back(edge.label_id);
  }

  auto incoming = edges;
  std::sort(
      incoming.begin(), incoming.end(),
      [](const CsrEdge &left, const CsrEdge &right) {
        return left.target < right.target ||
               (left.target == right.target &&
                (left.source < right.source || (left.source == right.source &&
                                                left.edge_id < right.edge_id)));
      });
  snapshot->incoming_offsets =
      BuildOffsets(snapshot->vertex_ids.size(), incoming, false);
  for (const auto &edge : incoming) {
    snapshot->incoming_neighbors.push_back(snapshot->vertex_ids[edge.source]);
    snapshot->incoming_edge_ids.push_back(edge.edge_id);
    snapshot->incoming_label_ids.push_back(edge.label_id);
  }
  snapshot->memory_bytes =
      snapshot->vertex_ids.size() * sizeof(uint64_t) +
      snapshot->outgoing_offsets.size() * sizeof(idx_t) +
      snapshot->outgoing_neighbors.size() * sizeof(uint64_t) +
      snapshot->outgoing_edge_ids.size() * sizeof(uint64_t) +
      snapshot->outgoing_label_ids.size() * sizeof(uint32_t) +
      snapshot->incoming_offsets.size() * sizeof(idx_t) +
      snapshot->incoming_neighbors.size() * sizeof(uint64_t) +
      snapshot->incoming_edge_ids.size() * sizeof(uint64_t) +
      snapshot->incoming_label_ids.size() * sizeof(uint32_t);
  return snapshot;
}

static shared_ptr<GqlCsrSnapshot>
GetPreparedTableSnapshot(ClientContext &context, const string &graph_name,
                         GqlCsrCacheState &cache) {
  Connection connection(*context.db);
  auto graph = ReadGraphVersion(connection, graph_name);
  auto entry = cache.snapshots.find(graph.graph_id);
  if (entry == cache.snapshots.end() ||
      entry->second->graph_version != graph.graph_version) {
    throw InvalidInputException(
        "CSR for table-backed graph '%s' has not been built on this "
        "connection; run SELECT * FROM gql_build_csr('%s') first",
        graph_name, graph_name);
  }
  return entry->second;
}

static shared_ptr<GqlCsrSnapshot> GetTraversalSnapshot(ClientContext &context,
                                                       const string &graph_name,
                                                       GqlCsrCacheState &cache,
                                                       bool &cached) {
  GqlTableGraphBinding binding;
  if (!GqlTryLoadTableGraph(context, graph_name, binding)) {
    throw InvalidInputException(
        "Graph '%s' has no native tables; load it with COPY GRAPH before CSR execution",
        graph_name);
  }
  if (!context.transaction.IsAutoCommit()) {
    throw NotImplementedException(
        "Table-backed CSR is not eligible inside an explicit transaction; "
        "use native execution");
  }
  cached = true;
  return GetPreparedTableSnapshot(context, graph_name, cache);
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
    auto cache = context.registered_state->GetOrCreate<GqlCsrCacheState>(
        GQL_CSR_STATE_KEY);
    bool cached;
    auto snapshot =
        GetTraversalSnapshot(context, data.graph_name, *cache, cached);
    auto vertex = snapshot->ordinal_by_id.find(data.vertex_id);
    if (vertex != snapshot->ordinal_by_id.end()) {
      const auto &offsets = data.direction == "out"
                                ? snapshot->outgoing_offsets
                                : snapshot->incoming_offsets;
      const auto &neighbors = data.direction == "out"
                                  ? snapshot->outgoing_neighbors
                                  : snapshot->incoming_neighbors;
      const auto &edge_ids = data.direction == "out"
                                 ? snapshot->outgoing_edge_ids
                                 : snapshot->incoming_edge_ids;
      for (idx_t index = offsets[vertex->second];
           index < offsets[vertex->second + 1]; index++) {
        state.rows.emplace_back(neighbors[index], edge_ids[index]);
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
           "memory_bytes", "build_count",   "cached"};
  return_types = {LogicalType::VARCHAR, LogicalType::UBIGINT,
                  LogicalType::UBIGINT, LogicalType::UBIGINT,
                  LogicalType::UBIGINT, LogicalType::UBIGINT,
                  LogicalType::BOOLEAN};
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
  Connection connection(*context.db);
  auto graph = ReadGraphVersion(connection, data.graph_name);
  GqlTableGraphBinding binding;
  if (!GqlTryLoadTableGraph(context, data.graph_name, binding)) {
    throw InvalidInputException(
        "Graph '%s' has no native tables; load it with COPY GRAPH before building CSR",
        data.graph_name);
  }
  auto snapshot = BuildTableSnapshot(context, data.graph_name);
  cache->snapshots[graph.graph_id] = snapshot;
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
  bool cached;
  auto snapshot =
      GetTraversalSnapshot(context, data.graph_name, *cache, cached);
  output.SetCardinality(1);
  output.SetValue(0, 0, Value(data.graph_name));
  output.SetValue(1, 0, Value::UBIGINT(snapshot->graph_version));
  output.SetValue(2, 0, Value::UBIGINT(snapshot->vertex_ids.size()));
  output.SetValue(3, 0, Value::UBIGINT(snapshot->outgoing_edge_ids.size()));
  output.SetValue(4, 0, Value::UBIGINT(snapshot->memory_bytes));
  output.SetValue(5, 0, Value::UBIGINT(cache->build_count));
  output.SetValue(6, 0, Value(cached));
  state.done = true;
}

struct CsrPathBindData : TableFunctionData {
  string graph_name;
  bool reverse = false;
  idx_t minimum_repetitions = 0;
  string edge_label;
  bool restrict_starts = false;
  vector<uint64_t> start_ids;
};

static unique_ptr<FunctionData> CsrPathBind(ClientContext &,
                                            TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types,
                                            vector<string> &names) {
  if (input.inputs.size() != 5 || input.inputs[0].IsNull() ||
      input.inputs[1].IsNull() || input.inputs[2].IsNull() ||
      input.inputs[3].IsNull()) {
    throw BinderException("Invalid GQL CSR path scan input");
  }
  auto result = make_uniq<CsrPathBindData>();
  result->graph_name = input.inputs[0].GetValue<string>();
  result->reverse = input.inputs[1].GetValue<bool>();
  result->minimum_repetitions =
      NumericCast<idx_t>(input.inputs[2].GetValue<uint64_t>());
  result->edge_label = input.inputs[3].GetValue<string>();
  result->restrict_starts = !input.inputs[4].IsNull();
  if (result->restrict_starts) {
    for (const auto &entry : ListValue::GetChildren(input.inputs[4])) {
      result->start_ids.push_back(entry.GetValue<uint64_t>());
    }
  }
  if (result->graph_name.empty()) {
    throw BinderException("GQL CSR path scan requires a graph name");
  }
  names = {"start_id", "end_id", "depth"};
  return_types = {LogicalType::UBIGINT, LogicalType::UBIGINT,
                  LogicalType::UBIGINT};
  return std::move(result);
}

static unique_ptr<NodeStatistics> CsrPathCardinality(ClientContext &,
                                                     const FunctionData *) {
  // An unbounded trail scan can be much larger than either element table.
  // A deliberately high estimate keeps DuckDB from choosing the streaming CSR
  // source as the materialized build side of its endpoint hash joins.
  // The estimate must remain high even after a selective source-vertex join.
  // A billion rows was insufficient for multi-million-vertex graphs: DuckDB
  // estimated the source join at only thousands of rows, built the following
  // endpoint hash join from the unbounded path stream, and consumed it before
  // an outer LIMIT could stop execution. An unbounded trail can legitimately
  // exceed this value by many orders of magnitude.
  constexpr idx_t estimate = 1000000000000000000ULL;
  return make_uniq<NodeStatistics>(estimate, estimate);
}

struct CsrPathLink {
  uint64_t edge_id;
  shared_ptr<CsrPathLink> parent;
};

struct CsrPathFrame {
  idx_t vertex_ordinal;
  idx_t next_offset;
  idx_t end_offset;
  idx_t depth;
  shared_ptr<CsrPathLink> path;
  bool emitted = false;
};

struct CsrPathState : GlobalTableFunctionState {
  bool initialized = false;
  shared_ptr<GqlCsrSnapshot> snapshot;
  vector<idx_t> start_ordinals;
  idx_t next_start = 0;
  idx_t current_start = 0;
  std::deque<CsrPathFrame> queue;
  bool filter_label = false;
  uint32_t required_label = 0;
};

static unique_ptr<GlobalTableFunctionState>
CsrPathInit(ClientContext &, TableFunctionInitInput &) {
  return make_uniq<CsrPathState>();
}

static bool PathContainsEdge(const shared_ptr<CsrPathLink> &path,
                             uint64_t edge_id) {
  for (auto entry = path; entry; entry = entry->parent) {
    if (entry->edge_id == edge_id) {
      return true;
    }
  }
  return false;
}

static void PushCsrFrame(CsrPathState &state, const CsrPathBindData &data,
                         idx_t vertex_ordinal, idx_t depth,
                         shared_ptr<CsrPathLink> path) {
  const auto &offsets = data.reverse ? state.snapshot->incoming_offsets
                                     : state.snapshot->outgoing_offsets;
  state.queue.push_back({vertex_ordinal, offsets[vertex_ordinal],
                         offsets[vertex_ordinal + 1], depth, std::move(path),
                         false});
}

static void CsrPathFunction(ClientContext &context, TableFunctionInput &input,
                            DataChunk &output) {
  auto &state = input.global_state->Cast<CsrPathState>();
  auto &data = input.bind_data->Cast<CsrPathBindData>();
  if (!state.initialized) {
    if (!context.transaction.IsAutoCommit()) {
      throw NotImplementedException(
          "CSR execution hint is not eligible inside an explicit transaction; "
          "use native execution");
    }
    auto cache = context.registered_state->GetOrCreate<GqlCsrCacheState>(
        GQL_CSR_STATE_KEY);
    bool cached;
    state.snapshot =
        GetTraversalSnapshot(context, data.graph_name, *cache, cached);
    state.filter_label = !data.edge_label.empty();
    if (state.filter_label) {
      auto label = state.snapshot->label_ids.find(data.edge_label);
      state.required_label = label == state.snapshot->label_ids.end()
                                 ? NumericLimits<uint32_t>::Maximum()
                                 : label->second;
    }
    if (data.restrict_starts) {
      unordered_set<idx_t> seen;
      for (const auto start_id : data.start_ids) {
        auto start = state.snapshot->ordinal_by_id.find(start_id);
        if (start != state.snapshot->ordinal_by_id.end() &&
            seen.insert(start->second).second) {
          state.start_ordinals.push_back(start->second);
        }
      }
    } else {
      state.start_ordinals.reserve(state.snapshot->vertex_ids.size());
      for (idx_t ordinal = 0; ordinal < state.snapshot->vertex_ids.size();
           ordinal++) {
        state.start_ordinals.push_back(ordinal);
      }
    }
    state.initialized = true;
  }

  const auto &neighbors = data.reverse ? state.snapshot->incoming_neighbors
                                       : state.snapshot->outgoing_neighbors;
  const auto &edge_ids = data.reverse ? state.snapshot->incoming_edge_ids
                                      : state.snapshot->outgoing_edge_ids;
  const auto &label_ids = data.reverse ? state.snapshot->incoming_label_ids
                                       : state.snapshot->outgoing_label_ids;
  idx_t count = 0;
  while (count < GQL_CSR_PATH_BATCH_SIZE) {
    if (context.IsInterrupted()) {
      throw InterruptException();
    }
    if (state.queue.empty()) {
      if (state.next_start >= state.start_ordinals.size()) {
        break;
      }
      state.current_start = state.start_ordinals[state.next_start++];
      PushCsrFrame(state, data, state.current_start, 0, nullptr);
    }

    auto &frame = state.queue.front();
    if (!frame.emitted) {
      frame.emitted = true;
      if (frame.depth >= data.minimum_repetitions) {
        output.SetValue(
            0, count,
            Value::UBIGINT(state.snapshot->vertex_ids[state.current_start]));
        output.SetValue(
            1, count,
            Value::UBIGINT(state.snapshot->vertex_ids[frame.vertex_ordinal]));
        output.SetValue(2, count, Value::UBIGINT(frame.depth));
        count++;
        continue;
      }
    }

    bool pushed = false;
    while (frame.next_offset < frame.end_offset) {
      auto offset = frame.next_offset++;
      if (state.filter_label && label_ids[offset] != state.required_label) {
        continue;
      }
      auto edge_id = edge_ids[offset];
      if (PathContainsEdge(frame.path, edge_id)) {
        continue;
      }
      auto neighbor = state.snapshot->ordinal_by_id.find(neighbors[offset]);
      if (neighbor == state.snapshot->ordinal_by_id.end()) {
        throw InternalException(
            "GQL CSR neighbor is missing its vertex ordinal");
      }
      auto path = make_shared_ptr<CsrPathLink>();
      path->edge_id = edge_id;
      path->parent = frame.path;
      PushCsrFrame(state, data, neighbor->second, frame.depth + 1,
                   std::move(path));
      pushed = true;
      break;
    }
    if (!pushed) {
      state.queue.pop_front();
    }
  }
  output.SetCardinality(count);
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

TableFunction GqlCsrPathFunction() {
  TableFunction function("gql_csr_path_scan",
                         {LogicalType::VARCHAR, LogicalType::BOOLEAN,
                          LogicalType::UBIGINT, LogicalType::VARCHAR,
                          LogicalType::LIST(LogicalType::UBIGINT)},
                         CsrPathFunction);
  function.bind = CsrPathBind;
  function.init_global = CsrPathInit;
  function.cardinality = CsrPathCardinality;
  return function;
}

} // namespace duckdb
