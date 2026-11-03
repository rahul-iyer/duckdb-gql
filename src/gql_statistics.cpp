#include "gql_csr.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <algorithm>

namespace duckdb {

namespace {
constexpr idx_t MAX_GROUPS = 256;
constexpr idx_t MAX_COMBINATIONS = 4096;
constexpr idx_t MAX_LABELS = 16;
constexpr idx_t FIXED_WORKSPACE = 2 * 1024 * 1024;

template <class T>
idx_t HashBytes(const T &entries) {
	return entries.bucket_count() * sizeof(void *) +
	       entries.size() * (sizeof(typename T::value_type) + 2 * sizeof(void *));
}

struct LabelSetHash {
	size_t operator()(const vector<uint32_t> &labels) const {
		uint64_t hash = 1469598103934665603ULL;
		for (auto label : labels)
			hash = (hash ^ label) * 1099511628211ULL;
		return static_cast<size_t>(hash);
	}
};
struct BuildCounts {
	GqlEndpointGroupCounts counts;
	idx_t last_source = DConstants::INVALID_INDEX;
	idx_t last_target = DConstants::INVALID_INDEX;
};
void CheckInterrupt(ClientContext &context) {
	if (context.IsInterrupted())
		throw InterruptException();
}
} // namespace

idx_t GqlEndpointStatistics::AllocatedBytes() const {
	auto bytes = sizeof(*this) + groups.capacity() * sizeof(vector<uint32_t>) +
	             populations.capacity() * sizeof(uint64_t) + HashBytes(counts) + HashBytes(fanout_by_group);
	for (const auto &group : groups)
		bytes += group.capacity() * sizeof(uint32_t);
	return bytes;
}

unique_ptr<GqlEndpointStatistics> GqlCollectEndpointStatistics(ClientContext &context, const GqlCsrSnapshot &snapshot,
                                                               string &status, idx_t &workspace_bytes) {
	const auto vertices = snapshot.vertex_ids.size();
	const auto budget = MinValue<idx_t>(64 * 1024 * 1024, BufferManager::GetBufferManager(context).GetMaxMemory() / 8);
	workspace_bytes = 0;
	if (budget < FIXED_WORKSPACE || vertices > budget - FIXED_WORKSPACE) {
		status = "workspace_limit";
		return nullptr;
	}
	workspace_bytes = vertices + FIXED_WORKSPACE;
	status = "group_limit";
	auto result = make_uniq<GqlEndpointStatistics>();
	vector<uint8_t> group_by_vertex(vertices);
	unordered_map<vector<uint32_t>, uint8_t, LabelSetHash> groups;
	vector<uint32_t> labels, previous;
	uint8_t previous_group = 0;
	for (idx_t vertex = 0; vertex < vertices; vertex++) {
		if ((vertex & 1023) == 0)
			CheckInterrupt(context);
		auto start = snapshot.vertex_label_offsets[vertex], end = snapshot.vertex_label_offsets[vertex + 1];
		if (end - start > MAX_LABELS) {
			status = "label_limit";
			return nullptr;
		}
		labels.clear();
		for (auto offset = start; offset < end; offset++)
			labels.push_back(snapshot.vertex_label_ids[offset]);
		std::sort(labels.begin(), labels.end());
		if (vertex == 0 || labels != previous) {
			auto found = groups.find(labels);
			if (found == groups.end()) {
				if (groups.size() >= MAX_GROUPS)
					return nullptr;
				previous_group = static_cast<uint8_t>(groups.size());
				groups.emplace(labels, previous_group);
				result->groups.push_back(labels);
				result->populations.push_back(0);
			} else
				previous_group = found->second;
			previous = labels;
		}
		group_by_vertex[vertex] = previous_group;
		result->populations[previous_group]++;
	}

	unordered_map<uint64_t, BuildCounts> counts;
	// Complete groups use at most MAX_GROUPS * MAX_LABELS vertex labels and
	// MAX_COMBINATIONS relationship types. Bound the stamp array too.
	if (snapshot.label_ids.size() > MAX_GROUPS * MAX_LABELS + MAX_COMBINATIONS) {
		status = "combination_limit";
		return nullptr;
	}
	vector<idx_t> seen_labels(snapshot.label_ids.size() + 1, DConstants::INVALID_INDEX);
	vector<idx_t> degrees(snapshot.label_ids.size() + 1, 0);
	idx_t visited = 0;
	for (idx_t vertex = 0; vertex < vertices; vertex++) {
		if ((vertex & 1023) == 0)
			CheckInterrupt(context);
		for (idx_t edge = snapshot.outgoing_offsets[vertex]; edge < snapshot.outgoing_offsets[vertex + 1]; edge++) {
			if ((visited++ & 262143) == 0)
				CheckInterrupt(context);
			auto target = snapshot.outgoing_neighbors[edge];
			auto key = GqlEndpointGroupKey(snapshot.outgoing_label_ids[edge], group_by_vertex[vertex],
			                               group_by_vertex[target]);
			auto found = counts.find(key);
			if (found == counts.end()) {
				if (counts.size() >= MAX_COMBINATIONS) {
					status = "combination_limit";
					return nullptr;
				}
				found = counts.emplace(key, BuildCounts()).first;
			}
			auto &entry = found->second;
			auto label = snapshot.outgoing_label_ids[edge];
			auto &marginal = result->fanout_by_group[(static_cast<uint64_t>(label) << 8) | group_by_vertex[vertex]];
			marginal.outgoing_edges++;
			if (seen_labels[label] != vertex) {
				marginal.outgoing_vertices++;
				seen_labels[label] = vertex;
				degrees[label] = 0;
			}
			marginal.outgoing_degree_squares += 2.0 * degrees[label]++ + 1;
			entry.counts.edges++;
			if (entry.last_source != vertex) {
				entry.counts.source_vertices++;
				entry.last_source = vertex;
			}
		}
	}
	std::fill(seen_labels.begin(), seen_labels.end(), DConstants::INVALID_INDEX);
	for (idx_t vertex = 0; vertex < vertices; vertex++) {
		if ((vertex & 1023) == 0)
			CheckInterrupt(context);
		for (idx_t edge = snapshot.incoming_offsets[vertex]; edge < snapshot.incoming_offsets[vertex + 1]; edge++) {
			if ((visited++ & 262143) == 0)
				CheckInterrupt(context);
			auto source = snapshot.incoming_neighbors[edge];
			auto key = GqlEndpointGroupKey(snapshot.incoming_label_ids[edge], group_by_vertex[source],
			                               group_by_vertex[vertex]);
			auto found = counts.find(key);
			if (found == counts.end())
				throw InternalException("Inconsistent endpoint statistics topology");
			auto label = snapshot.incoming_label_ids[edge];
			auto &marginal = result->fanout_by_group[(static_cast<uint64_t>(label) << 8) | group_by_vertex[vertex]];
			marginal.incoming_edges++;
			if (seen_labels[label] != vertex) {
				marginal.incoming_vertices++;
				seen_labels[label] = vertex;
				degrees[label] = 0;
			}
			marginal.incoming_degree_squares += 2.0 * degrees[label]++ + 1;
			if (found->second.last_target != vertex) {
				found->second.counts.target_vertices++;
				found->second.last_target = vertex;
			}
		}
	}
	for (const auto &entry : counts)
		result->counts.emplace(entry.first, entry.second.counts);
	workspace_bytes = group_by_vertex.capacity() + (seen_labels.capacity() + degrees.capacity()) * sizeof(idx_t) +
	                  HashBytes(groups) + HashBytes(counts) +
	                  (labels.capacity() + previous.capacity()) * sizeof(uint32_t);
	for (const auto &group : groups)
		workspace_bytes += group.first.capacity() * sizeof(uint32_t);
	status = "complete";
	return result;
}

bool GqlSummarizeEndpointStatistics(const GqlCsrSnapshot &snapshot, const vector<uint32_t> &source_labels,
                                    uint32_t edge_label, const vector<uint32_t> &target_labels,
                                    GqlEndpointSummary &result) {
	result = GqlEndpointSummary();
	if (!snapshot.endpoint_statistics)
		return false;
	const auto &statistics = *snapshot.endpoint_statistics;
	vector<bool> sources(statistics.groups.size()), targets(statistics.groups.size());
	auto matches = [](const vector<uint32_t> &group, const vector<uint32_t> &required) {
		for (auto label : required)
			if (!std::binary_search(group.begin(), group.end(), label))
				return false;
		return true;
	};
	for (idx_t group = 0; group < statistics.groups.size(); group++) {
		sources[group] = matches(statistics.groups[group], source_labels);
		targets[group] = matches(statistics.groups[group], target_labels);
		if (sources[group])
			result.source_population += statistics.populations[group];
		if (targets[group])
			result.target_population += statistics.populations[group];
	}
	for (const auto &entry : statistics.fanout_by_group) {
		if (static_cast<uint32_t>(entry.first >> 8) != edge_label)
			continue;
		auto group = static_cast<uint8_t>(entry.first);
		if (sources[group]) {
			result.source_edges += entry.second.outgoing_edges;
			result.source_active += entry.second.outgoing_vertices;
			result.source_degree_squares += entry.second.outgoing_degree_squares;
		}
		if (targets[group]) {
			result.target_edges += entry.second.incoming_edges;
			result.target_active += entry.second.incoming_vertices;
			result.target_degree_squares += entry.second.incoming_degree_squares;
		}
	}
	for (const auto &entry : statistics.counts) {
		if (static_cast<uint32_t>(entry.first >> 16) != edge_label)
			continue;
		auto source = static_cast<uint8_t>(entry.first >> 8), target = static_cast<uint8_t>(entry.first);
		if (sources[source] && targets[target])
			result.edges += entry.second.edges;
	}
	return true;
}

namespace {
struct EndpointBind : TableFunctionData {
	string graph_name;
};
struct EndpointScan : GlobalTableFunctionState {
	shared_ptr<const GqlCsrSnapshot> snapshot;
	vector<pair<uint64_t, GqlEndpointGroupCounts>> rows;
	vector<string> labels;
	idx_t position = 0;
};
unique_ptr<FunctionData> EndpointBindFunction(ClientContext &, TableFunctionBindInput &input,
                                              vector<LogicalType> &types, vector<Identifier> &names) {
	auto result = make_uniq<EndpointBind>();
	result->graph_name = input.inputs[0].GetValue<string>();
	names = {
	    "source_labels",          "edge_label",        "target_labels",     "edge_count",    "source_active_vertices",
	    "target_active_vertices", "source_population", "target_population", "graph_version", "source_catalog",
	    "source_snapshot_id"};
	types = {LogicalType::LIST(LogicalType::VARCHAR),
	         LogicalType::VARCHAR,
	         LogicalType::LIST(LogicalType::VARCHAR),
	         LogicalType::UBIGINT,
	         LogicalType::UBIGINT,
	         LogicalType::UBIGINT,
	         LogicalType::UBIGINT,
	         LogicalType::UBIGINT,
	         LogicalType::UBIGINT,
	         LogicalType::VARCHAR,
	         LogicalType::UBIGINT};
	return std::move(result);
}
unique_ptr<GlobalTableFunctionState> EndpointInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<EndpointScan>();
	result->snapshot = GqlGetUnchangedCsrSnapshot(context, input.bind_data->Cast<EndpointBind>().graph_name);
	if (!result->snapshot->endpoint_statistics)
		throw InvalidInputException("Endpoint statistics unavailable (%s); build CSR with endpoint_statistics := true",
		                            result->snapshot->endpoint_statistics_status);
	result->labels.resize(result->snapshot->label_ids.size() + 1);
	for (const auto &label : result->snapshot->label_ids)
		result->labels[label.second] = label.first;
	for (const auto &entry : result->snapshot->endpoint_statistics->counts)
		result->rows.emplace_back(entry.first, entry.second);
	std::sort(result->rows.begin(), result->rows.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
	return std::move(result);
}
void EndpointRead(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<EndpointScan>();
	auto &statistics = *state.snapshot->endpoint_statistics;
	auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.position);
	for (idx_t row = 0; row < count; row++) {
		auto &entry = state.rows[state.position++];
		auto source = static_cast<uint8_t>(entry.first >> 8), target = static_cast<uint8_t>(entry.first);
		auto label = static_cast<uint32_t>(entry.first >> 16);
		auto labels = [&](uint8_t group) {
			vector<Value> values;
			for (auto id : statistics.groups[group])
				values.emplace_back(state.labels[id]);
			return Value::LIST(LogicalType::VARCHAR, values);
		};
		output.SetValue(0, row, labels(source));
		output.SetValue(1, row, Value(state.labels[label]));
		output.SetValue(2, row, labels(target));
		uint64_t values[] = {entry.second.edges, entry.second.source_vertices, entry.second.target_vertices,
		                     statistics.populations[source], statistics.populations[target]};
		for (idx_t col = 0; col < 5; col++)
			output.SetValue(col + 3, row, Value::UBIGINT(values[col]));
		output.SetValue(8, row, Value::UBIGINT(state.snapshot->graph_version));
		output.SetValue(9, row,
		                state.snapshot->has_source_snapshot ? Value(state.snapshot->source_catalog)
		                                                    : Value(LogicalType::VARCHAR));
		output.SetValue(10, row,
		                state.snapshot->has_source_snapshot ? Value::UBIGINT(state.snapshot->source_snapshot_id)
		                                                    : Value(LogicalType::UBIGINT));
	}
	output.SetChildCardinality(count);
}
} // namespace

TableFunction GqlCsrEndpointStatsFunction() {
	return TableFunction("gql_csr_endpoint_stats", {LogicalType::VARCHAR}, EndpointRead, EndpointBindFunction,
	                     EndpointInit);
}

} // namespace duckdb
