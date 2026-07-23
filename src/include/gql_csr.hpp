#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

class ClientContext;

struct GqlCsrSnapshot {
  uint64_t graph_id;
  uint64_t graph_version;
  bool dense_vertex_ids = false;
  vector<uint64_t> vertex_ids;
  unordered_map<uint64_t, idx_t> ordinal_by_id;
  vector<idx_t> vertex_label_offsets;
  vector<uint32_t> vertex_label_ids;
  vector<idx_t> outgoing_offsets;
  vector<idx_t> outgoing_neighbors;
  vector<uint64_t> outgoing_edge_ids;
  vector<uint32_t> outgoing_label_ids;
  vector<idx_t> incoming_offsets;
  vector<idx_t> incoming_neighbors;
  vector<uint64_t> incoming_edge_ids;
  vector<uint32_t> incoming_label_ids;
  unordered_map<string, uint32_t> label_ids;
  idx_t memory_bytes;
};

bool GqlTryGetCsrOrdinal(const GqlCsrSnapshot &snapshot, uint64_t vertex_id,
                         idx_t &ordinal);

shared_ptr<const GqlCsrSnapshot> GqlGetCsrSnapshot(ClientContext &context,
                                                   const string &graph_name);

TableFunction GqlNeighborsFunction();
TableFunction GqlBuildCsrFunction();
TableFunction GqlCsrStatsFunction();

} // namespace duckdb
