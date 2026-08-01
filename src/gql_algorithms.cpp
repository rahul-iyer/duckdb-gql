#include "gql_algorithms.hpp"

#include "gql_catalog.hpp"
#include "gql_csr.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/execution/execution_context.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace duckdb {

static const vector<GqlProcedureDefinition> &AlgorithmProcedures() {
	static const vector<GqlProcedureDefinition> procedures = {
	    {"algo",
	     "bfs",
	     GqlProcedureInputMode::BATCH,
	     {{"graph", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION},
	      {"frontier", {GqlTypeId::ELEMENT_ID, false}, GqlProcedureArgumentMode::INPUT},
	      {"vertex_label", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION, true},
	      {"edge_label", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION, true},
	      {"target_vertex_id", {GqlTypeId::INTEGER, false}, GqlProcedureArgumentMode::CONFIGURATION, true}},
	     {{"vertex_id", {GqlTypeId::ELEMENT_ID, false}},
	      {"depth", {GqlTypeId::INTEGER, false}},
	      {"parent_vertex_id", {GqlTypeId::ELEMENT_ID, true}},
	      {"edge_id", {GqlTypeId::ELEMENT_ID, true}},
	      {"visit_order", {GqlTypeId::INTEGER, false}}}},
	    {"algo",
	     "dfs",
	     GqlProcedureInputMode::BATCH,
	     {{"graph", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION},
	      {"frontier", {GqlTypeId::ELEMENT_ID, false}, GqlProcedureArgumentMode::INPUT},
	      {"vertex_label", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION, true}},
	     {{"vertex_id", {GqlTypeId::ELEMENT_ID, false}},
	      {"depth", {GqlTypeId::INTEGER, false}},
	      {"parent_vertex_id", {GqlTypeId::ELEMENT_ID, true}},
	      {"edge_id", {GqlTypeId::ELEMENT_ID, true}},
	      {"visit_order", {GqlTypeId::INTEGER, false}}}},
	    {"algo",
	     "sssp",
	     GqlProcedureInputMode::BATCH,
	     {{"graph", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION},
	      {"source", {GqlTypeId::ELEMENT_ID, false}, GqlProcedureArgumentMode::INPUT},
	      {"vertex_label", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION, true},
	      {"edge_label", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION, true},
	      {"target_vertex_id", {GqlTypeId::INTEGER, false}, GqlProcedureArgumentMode::CONFIGURATION, true}},
	     {{"vertex_id", {GqlTypeId::ELEMENT_ID, false}},
	      {"distance", {GqlTypeId::INTEGER, false}},
	      {"parent_vertex_id", {GqlTypeId::ELEMENT_ID, true}},
	      {"edge_id", {GqlTypeId::ELEMENT_ID, true}},
	      {"settled_order", {GqlTypeId::INTEGER, false}}}},
	    {"algo",
	     "shortest_path_length",
	     GqlProcedureInputMode::BATCH,
	     {{"graph", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION},
	      {"source", {GqlTypeId::ELEMENT_ID, false}, GqlProcedureArgumentMode::INPUT},
	      {"target", {GqlTypeId::ELEMENT_ID, false}, GqlProcedureArgumentMode::INPUT},
	      {"vertex_label", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION, true},
	      {"edge_label", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION, true}},
	     {{"distance", {GqlTypeId::INTEGER, false}}}},
	    {"algo",
	     "pagerank",
	     GqlProcedureInputMode::NONE,
	     {{"graph", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION},
	      {"vertex_label", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION, true}},
	     {{"vertex_id", {GqlTypeId::ELEMENT_ID, false}},
	      {"rank", {GqlTypeId::DOUBLE, false}},
	      {"iterations", {GqlTypeId::INTEGER, false}},
	      {"converged", {GqlTypeId::BOOLEAN, false}}}},
	    {"algo",
	     "wcc",
	     GqlProcedureInputMode::NONE,
	     {{"graph", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION},
	      {"vertex_label", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION, true}},
	     {{"vertex_id", {GqlTypeId::ELEMENT_ID, false}},
	      {"component_id", {GqlTypeId::ELEMENT_ID, false}},
	      {"component_size", {GqlTypeId::INTEGER, false}}}},
	    {"algo",
	     "scc",
	     GqlProcedureInputMode::NONE,
	     {{"graph", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION},
	      {"vertex_label", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION, true}},
	     {{"vertex_id", {GqlTypeId::ELEMENT_ID, false}},
	      {"component_id", {GqlTypeId::ELEMENT_ID, false}},
	      {"component_size", {GqlTypeId::INTEGER, false}}}},
	    {"algo",
	     "triangle_count",
	     GqlProcedureInputMode::NONE,
	     {{"graph", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION},
	      {"vertex_label", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION, true}},
	     {{"vertex_id", {GqlTypeId::ELEMENT_ID, false}},
	      {"triangle_count", {GqlTypeId::INTEGER, false}},
	      {"degree", {GqlTypeId::INTEGER, false}},
	      {"local_clustering_coefficient", {GqlTypeId::DOUBLE, false}},
	      {"global_triangle_count", {GqlTypeId::INTEGER, false}}}},
	    {"algo",
	     "lcc",
	     GqlProcedureInputMode::NONE,
	     {{"graph", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION},
	      {"vertex_label", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION, true}},
	     {{"vertex_id", {GqlTypeId::ELEMENT_ID, false}},
	      {"degree", {GqlTypeId::INTEGER, false}},
	      {"directed_neighbor_edge_count", {GqlTypeId::INTEGER, false}},
	      {"local_clustering_coefficient", {GqlTypeId::DOUBLE, false}}}},
	    {"algo",
	     "louvain",
	     GqlProcedureInputMode::NONE,
	     {{"graph", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION},
	      {"vertex_label", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION, true}},
	     {{"vertex_id", {GqlTypeId::ELEMENT_ID, false}},
	      {"community_id", {GqlTypeId::ELEMENT_ID, false}},
	      {"community_size", {GqlTypeId::INTEGER, false}},
	      {"modularity", {GqlTypeId::DOUBLE, false}},
	      {"levels", {GqlTypeId::INTEGER, false}},
	      {"iterations", {GqlTypeId::INTEGER, false}},
	      {"converged", {GqlTypeId::BOOLEAN, false}}}},
	    {"algo",
	     "degree",
	     GqlProcedureInputMode::NONE,
	     {{"graph", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION},
	      {"vertex_label", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION, true}},
	     {{"vertex_id", {GqlTypeId::ELEMENT_ID, false}},
	      {"out_degree", {GqlTypeId::INTEGER, false}},
	      {"in_degree", {GqlTypeId::INTEGER, false}},
	      {"total_degree", {GqlTypeId::INTEGER, false}}}},
	    {"algo",
	     "closeness",
	     GqlProcedureInputMode::NONE,
	     {{"graph", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION},
	      {"vertex_label", {GqlTypeId::STRING, false}, GqlProcedureArgumentMode::CONFIGURATION, true}},
	     {{"vertex_id", {GqlTypeId::ELEMENT_ID, false}},
	      {"reachable_count", {GqlTypeId::INTEGER, false}},
	      {"distance_sum", {GqlTypeId::INTEGER, false}},
	      {"closeness_centrality", {GqlTypeId::DOUBLE, false}}}}};
	return procedures;
}

const GqlProcedureDefinition *GqlFindProcedure(const string &procedure_namespace, const string &name) {
	for (const auto &procedure : AlgorithmProcedures()) {
		if (StringUtil::CIEquals(procedure.procedure_namespace, procedure_namespace) &&
		    StringUtil::CIEquals(procedure.name, name)) {
			return &procedure;
		}
	}
	return nullptr;
}

enum class CsrDirection : uint8_t { OUT, IN, BOTH };

static const Value *NamedParameter(const TableFunctionBindInput &input, const string &name) {
	auto entry = input.named_parameters.find(name);
	return entry == input.named_parameters.end() ? nullptr : &entry->second;
}

static string ReadLabelParameter(const TableFunctionBindInput &input, const string &name) {
	auto value = NamedParameter(input, name);
	return value && !value->IsNull() ? StringUtil::Lower(value->GetValue<string>()) : string();
}

static string ReadVertexLabel(const TableFunctionBindInput &input, idx_t positional_index) {
	auto named = ReadLabelParameter(input, "vertex_label");
	string positional;
	if (input.inputs.size() > positional_index && !input.inputs[positional_index].IsNull()) {
		positional = StringUtil::Lower(input.inputs[positional_index].GetValue<string>());
	}
	if (!named.empty() && !positional.empty()) {
		throw BinderException("GQL algorithm vertex_label cannot be both positional and named");
	}
	return positional.empty() ? named : positional;
}

static CsrDirection ReadDirection(const TableFunctionBindInput &input) {
	auto value = NamedParameter(input, "direction");
	auto direction = value && !value->IsNull() ? StringUtil::Lower(value->GetValue<string>()) : "out";
	if (direction == "out") {
		return CsrDirection::OUT;
	}
	if (direction == "in") {
		return CsrDirection::IN;
	}
	if (direction == "both") {
		return CsrDirection::BOTH;
	}
	throw BinderException("GQL CSR traversal direction must be 'out', 'in', or 'both'");
}

struct TraversalBindData : TableFunctionData {
	string graph_name;
	uint64_t start_vertex_id;
	CsrDirection direction = CsrDirection::OUT;
	idx_t max_depth = std::numeric_limits<idx_t>::max();
	string edge_label;
	string vertex_label;
	bool has_target = false;
	uint64_t target_vertex_id = 0;
};

static unique_ptr<FunctionData> TraversalBind(ClientContext &, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	if ((input.inputs.size() != 2 && input.inputs.size() != 3) || input.inputs[0].IsNull() ||
	    input.inputs[1].IsNull()) {
		throw BinderException("GQL CSR traversal requires a graph name and start vertex ID");
	}
	auto result = make_uniq<TraversalBindData>();
	result->graph_name = input.inputs[0].GetValue<string>();
	auto start_vertex_id = input.inputs[1].GetValue<int64_t>();
	if (start_vertex_id < 0) {
		throw BinderException("GQL CSR start vertex ID must be non-negative");
	}
	result->start_vertex_id = NumericCast<uint64_t>(start_vertex_id);
	result->direction = ReadDirection(input);
	if (auto value = NamedParameter(input, "max_depth")) {
		if (!value->IsNull()) {
			auto max_depth = value->GetValue<int64_t>();
			if (max_depth < 0) {
				throw BinderException("GQL CSR max_depth must be non-negative");
			}
			result->max_depth = NumericCast<idx_t>(max_depth);
		}
	}
	result->edge_label = ReadLabelParameter(input, "edge_label");
	result->vertex_label = ReadVertexLabel(input, 2);
	if (auto value = NamedParameter(input, "target_vertex_id")) {
		if (!value->IsNull()) {
			auto target_vertex_id = value->GetValue<int64_t>();
			if (target_vertex_id < 0) {
				throw BinderException("GQL CSR target vertex ID must be non-negative");
			}
			result->has_target = true;
			result->target_vertex_id = NumericCast<uint64_t>(target_vertex_id);
		}
	}
	if (result->graph_name.empty()) {
		throw BinderException("GQL CSR traversal requires a graph name");
	}
	names = {"vertex_id", "depth", "parent_vertex_id", "edge_id", "visit_order"};
	return_types = {LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT,
	                LogicalType::UBIGINT};
	return std::move(result);
}

static unique_ptr<FunctionData> SsspBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
	auto result = TraversalBind(context, input, return_types, names);
	names = {"vertex_id", "distance", "parent_vertex_id", "edge_id", "settled_order"};
	return result;
}

static uint32_t ResolveLabel(const GqlCsrSnapshot &snapshot, const string &label_name, bool &filter_label) {
	filter_label = !label_name.empty();
	if (!filter_label) {
		return 0;
	}
	auto label = snapshot.label_ids.find(StringUtil::Lower(label_name));
	return label == snapshot.label_ids.end() ? std::numeric_limits<uint32_t>::max() : label->second;
}

static vector<uint8_t> BuildVertexMask(const GqlCsrSnapshot &snapshot, const string &vertex_label,
                                       idx_t &projected_count) {
	bool filter_label;
	auto required_label = ResolveLabel(snapshot, vertex_label, filter_label);
	if (!filter_label) {
		projected_count = snapshot.vertex_ids.size();
		return {};
	}
	vector<uint8_t> mask(snapshot.vertex_ids.size(), false);
	projected_count = 0;
	for (idx_t vertex = 0; vertex < snapshot.vertex_ids.size(); vertex++) {
		for (idx_t offset = snapshot.vertex_label_offsets[vertex]; offset < snapshot.vertex_label_offsets[vertex + 1];
		     offset++) {
			if (snapshot.vertex_label_ids[offset] != required_label) {
				continue;
			}
			mask[vertex] = true;
			projected_count++;
			break;
		}
	}
	return mask;
}

static bool InVertexProjection(const vector<uint8_t> &mask, idx_t vertex) {
	return mask.empty() || mask[vertex];
}

template <class CALLBACK>
static bool VisitRange(const vector<uint64_t> &offsets, const GqlCsrOrdinals &neighbors,
                       const vector<uint64_t> &edge_ids, const GqlCsrEdgeLabels &label_ids, idx_t vertex,
                       bool filter_label, uint32_t required_label, CALLBACK &&callback) {
	for (idx_t offset = offsets[vertex]; offset < offsets[vertex + 1]; offset++) {
		if (filter_label && label_ids[offset] != required_label) {
			continue;
		}
		if (!callback(neighbors[offset], edge_ids[offset])) {
			return false;
		}
	}
	return true;
}

template <class CALLBACK>
static void VisitNeighbors(const GqlCsrSnapshot &snapshot, idx_t vertex, CsrDirection direction, bool filter_label,
                           uint32_t required_label, CALLBACK &&callback) {
	if (direction != CsrDirection::IN &&
	    !VisitRange(snapshot.outgoing_offsets, snapshot.outgoing_neighbors, snapshot.outgoing_edge_ids,
	                snapshot.outgoing_label_ids, vertex, filter_label, required_label, callback)) {
		return;
	}
	if (direction != CsrDirection::OUT) {
		VisitRange(snapshot.incoming_offsets, snapshot.incoming_neighbors, snapshot.incoming_edge_ids,
		           snapshot.incoming_label_ids, vertex, filter_label, required_label, callback);
	}
}

static idx_t RequireVertex(const GqlCsrSnapshot &snapshot, uint64_t vertex_id, const string &role) {
	idx_t ordinal;
	if (!GqlTryGetCsrOrdinal(snapshot, vertex_id, ordinal)) {
		throw InvalidInputException("GQL CSR %s vertex %llu does not exist", role,
		                            static_cast<unsigned long long>(vertex_id));
	}
	return ordinal;
}

static idx_t RequireProjectedVertex(const GqlCsrSnapshot &snapshot, const vector<uint8_t> &vertex_mask,
                                    uint64_t vertex_id, const string &role) {
	auto vertex = RequireVertex(snapshot, vertex_id, role);
	if (!InVertexProjection(vertex_mask, vertex)) {
		throw InvalidInputException("GQL CSR %s vertex %llu is outside the vertex-label projection", role,
		                            static_cast<unsigned long long>(vertex_id));
	}
	return vertex;
}

struct TraversalRow {
	idx_t vertex;
	idx_t depth;
	idx_t parent;
	uint64_t edge_id;
	bool has_parent;
};

struct TraversalOutputWriter {
	explicit TraversalOutputWriter(DataChunk &output_p) : output(output_p) {
		vertex_ids = FlatVector::GetData<uint64_t>(output.data[0]);
		depths = FlatVector::GetData<uint64_t>(output.data[1]);
		parent_ids = FlatVector::GetData<uint64_t>(output.data[2]);
		edge_ids = FlatVector::GetData<uint64_t>(output.data[3]);
		visit_orders = FlatVector::GetData<uint64_t>(output.data[4]);
	}

	void Write(idx_t row_index, const GqlCsrSnapshot &snapshot, const TraversalRow &row, idx_t visit_order) {
		vertex_ids[row_index] = snapshot.vertex_ids[row.vertex];
		depths[row_index] = row.depth;
		visit_orders[row_index] = visit_order;
		auto &parent_validity = FlatVector::Validity(output.data[2]);
		auto &edge_validity = FlatVector::Validity(output.data[3]);
		if (row.has_parent) {
			parent_ids[row_index] = snapshot.vertex_ids[row.parent];
			edge_ids[row_index] = row.edge_id;
			parent_validity.SetValid(row_index);
			edge_validity.SetValid(row_index);
		} else {
			parent_validity.SetInvalid(row_index);
			edge_validity.SetInvalid(row_index);
		}
	}

	DataChunk &output;
	uint64_t *vertex_ids;
	uint64_t *depths;
	uint64_t *parent_ids;
	uint64_t *edge_ids;
	uint64_t *visit_orders;
};

struct BfsState : GlobalTableFunctionState {
	bool initialized = false;
	bool finished = false;
	shared_ptr<const GqlCsrSnapshot> snapshot;
	vector<uint8_t> vertex_mask;
	vector<uint8_t> visited;
	vector<TraversalRow> queue;
	idx_t head = 0;
	idx_t visit_order = 0;
	bool filter_label = false;
	uint32_t required_label = 0;
	bool has_target = false;
	idx_t target = 0;
};

static unique_ptr<GlobalTableFunctionState> BfsInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<BfsState>();
}

static void InitializeBfs(ClientContext &context, const TraversalBindData &data, BfsState &state) {
	state.snapshot = GqlGetCsrSnapshot(context, data.graph_name);
	idx_t projected_count;
	state.vertex_mask = BuildVertexMask(*state.snapshot, data.vertex_label, projected_count);
	auto start = RequireProjectedVertex(*state.snapshot, state.vertex_mask, data.start_vertex_id, "start");
	state.has_target = data.has_target;
	if (data.has_target) {
		state.target = RequireProjectedVertex(*state.snapshot, state.vertex_mask, data.target_vertex_id, "target");
	}
	state.required_label = ResolveLabel(*state.snapshot, data.edge_label, state.filter_label);
	state.visited.resize(state.snapshot->vertex_ids.size(), false);
	state.visited[start] = true;
	state.queue.push_back({start, 0, 0, 0, false});
	state.initialized = true;
}

static void BfsFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<TraversalBindData>();
	auto &state = input.global_state->Cast<BfsState>();
	if (!state.initialized) {
		InitializeBfs(context, data, state);
	}
	TraversalOutputWriter writer(output);
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && state.head < state.queue.size() && !state.finished) {
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
		auto row = state.queue[state.head++];
		writer.Write(count++, *state.snapshot, row, state.visit_order++);
		if (state.has_target && row.vertex == state.target) {
			state.finished = true;
			break;
		}
		if (row.depth >= data.max_depth) {
			continue;
		}
		VisitNeighbors(*state.snapshot, row.vertex, data.direction, state.filter_label, state.required_label,
		               [&](idx_t neighbor, uint64_t edge_id) {
			               if (!InVertexProjection(state.vertex_mask, neighbor) || state.visited[neighbor]) {
				               return true;
			               }
			               state.visited[neighbor] = true;
			               state.queue.push_back({neighbor, row.depth + 1, row.vertex, edge_id, true});
			               return true;
		               });
	}
	output.SetCardinality(count);
}

struct DfsFrame {
	idx_t vertex;
	idx_t depth;
	uint8_t phase;
	idx_t offset;
	idx_t end;
};

struct DfsState : GlobalTableFunctionState {
	bool initialized = false;
	bool emit_start = true;
	bool finished = false;
	shared_ptr<const GqlCsrSnapshot> snapshot;
	vector<uint8_t> vertex_mask;
	vector<uint8_t> visited;
	vector<DfsFrame> stack;
	TraversalRow start_row;
	idx_t visit_order = 0;
	bool filter_label = false;
	uint32_t required_label = 0;
	bool has_target = false;
	idx_t target = 0;
};

static unique_ptr<GlobalTableFunctionState> DfsInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<DfsState>();
}

static DfsFrame MakeDfsFrame(const GqlCsrSnapshot &snapshot, idx_t vertex, idx_t depth, CsrDirection direction) {
	if (direction == CsrDirection::IN) {
		return {vertex, depth, 1, snapshot.incoming_offsets[vertex], snapshot.incoming_offsets[vertex + 1]};
	}
	return {vertex, depth, 0, snapshot.outgoing_offsets[vertex], snapshot.outgoing_offsets[vertex + 1]};
}

static bool NextDfsNeighbor(const GqlCsrSnapshot &snapshot, CsrDirection direction, bool filter_label,
                            uint32_t required_label, const vector<uint8_t> &vertex_mask, DfsFrame &frame,
                            idx_t &neighbor, uint64_t &edge_id) {
	while (true) {
		const auto &neighbors = frame.phase == 0 ? snapshot.outgoing_neighbors : snapshot.incoming_neighbors;
		const auto &edge_ids = frame.phase == 0 ? snapshot.outgoing_edge_ids : snapshot.incoming_edge_ids;
		const auto &label_ids = frame.phase == 0 ? snapshot.outgoing_label_ids : snapshot.incoming_label_ids;
		while (frame.offset < frame.end) {
			auto offset = frame.offset++;
			if (filter_label && label_ids[offset] != required_label) {
				continue;
			}
			neighbor = neighbors[offset];
			if (!InVertexProjection(vertex_mask, neighbor)) {
				continue;
			}
			edge_id = edge_ids[offset];
			return true;
		}
		if (direction == CsrDirection::BOTH && frame.phase == 0) {
			frame.phase = 1;
			frame.offset = snapshot.incoming_offsets[frame.vertex];
			frame.end = snapshot.incoming_offsets[frame.vertex + 1];
			continue;
		}
		return false;
	}
}

static void InitializeDfs(ClientContext &context, const TraversalBindData &data, DfsState &state) {
	state.snapshot = GqlGetCsrSnapshot(context, data.graph_name);
	idx_t projected_count;
	state.vertex_mask = BuildVertexMask(*state.snapshot, data.vertex_label, projected_count);
	auto start = RequireProjectedVertex(*state.snapshot, state.vertex_mask, data.start_vertex_id, "start");
	state.has_target = data.has_target;
	if (data.has_target) {
		state.target = RequireProjectedVertex(*state.snapshot, state.vertex_mask, data.target_vertex_id, "target");
	}
	state.required_label = ResolveLabel(*state.snapshot, data.edge_label, state.filter_label);
	state.visited.resize(state.snapshot->vertex_ids.size(), false);
	state.visited[start] = true;
	state.start_row = {start, 0, 0, 0, false};
	state.stack.push_back(MakeDfsFrame(*state.snapshot, start, 0, data.direction));
	state.initialized = true;
}

static void DfsFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<TraversalBindData>();
	auto &state = input.global_state->Cast<DfsState>();
	if (!state.initialized) {
		InitializeDfs(context, data, state);
	}
	TraversalOutputWriter writer(output);
	idx_t count = 0;
	if (state.emit_start && count < STANDARD_VECTOR_SIZE) {
		writer.Write(count++, *state.snapshot, state.start_row, state.visit_order++);
		state.emit_start = false;
		if (state.has_target && state.start_row.vertex == state.target) {
			state.finished = true;
		}
	}
	while (count < STANDARD_VECTOR_SIZE && !state.finished && !state.stack.empty()) {
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
		auto &frame = state.stack.back();
		if (frame.depth >= data.max_depth) {
			state.stack.pop_back();
			continue;
		}
		idx_t neighbor;
		uint64_t edge_id;
		if (!NextDfsNeighbor(*state.snapshot, data.direction, state.filter_label, state.required_label,
		                     state.vertex_mask, frame, neighbor, edge_id)) {
			state.stack.pop_back();
			continue;
		}
		if (state.visited[neighbor]) {
			continue;
		}
		state.visited[neighbor] = true;
		TraversalRow row {neighbor, frame.depth + 1, frame.vertex, edge_id, true};
		state.stack.push_back(MakeDfsFrame(*state.snapshot, neighbor, row.depth, data.direction));
		writer.Write(count++, *state.snapshot, row, state.visit_order++);
		if (state.has_target && neighbor == state.target) {
			state.finished = true;
		}
	}
	output.SetCardinality(count);
}

struct PageRankBindData : TableFunctionData {
	string graph_name;
	double damping = 0.85;
	idx_t max_iterations = 100;
	double tolerance = 1e-8;
	string edge_label;
	string vertex_label;
};

static unique_ptr<FunctionData> PageRankBind(ClientContext &, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	if ((input.inputs.size() != 1 && input.inputs.size() != 2) || input.inputs[0].IsNull()) {
		throw BinderException("GQL PageRank requires a graph name");
	}
	auto result = make_uniq<PageRankBindData>();
	result->graph_name = input.inputs[0].GetValue<string>();
	if (auto value = NamedParameter(input, "damping")) {
		if (!value->IsNull()) {
			result->damping = value->GetValue<double>();
		}
	}
	if (auto value = NamedParameter(input, "max_iterations")) {
		if (!value->IsNull()) {
			auto max_iterations = value->GetValue<int64_t>();
			if (max_iterations <= 0) {
				throw BinderException("GQL PageRank max_iterations must be positive");
			}
			result->max_iterations = NumericCast<idx_t>(max_iterations);
		}
	}
	if (auto value = NamedParameter(input, "tolerance")) {
		if (!value->IsNull()) {
			result->tolerance = value->GetValue<double>();
		}
	}
	result->edge_label = ReadLabelParameter(input, "edge_label");
	result->vertex_label = ReadVertexLabel(input, 1);
	if (result->graph_name.empty()) {
		throw BinderException("GQL PageRank requires a graph name");
	}
	if (!std::isfinite(result->damping) || result->damping <= 0.0 || result->damping >= 1.0) {
		throw BinderException("GQL PageRank damping must be between 0 and 1");
	}
	if (result->max_iterations == 0) {
		throw BinderException("GQL PageRank max_iterations must be positive");
	}
	if (!std::isfinite(result->tolerance) || result->tolerance <= 0.0) {
		throw BinderException("GQL PageRank tolerance must be positive and finite");
	}
	names = {"vertex_id", "rank", "iterations", "converged"};
	return_types = {LogicalType::UBIGINT, LogicalType::DOUBLE, LogicalType::UBIGINT, LogicalType::BOOLEAN};
	return std::move(result);
}

struct PageRankState : GlobalTableFunctionState {
	bool initialized = false;
	shared_ptr<const GqlCsrSnapshot> snapshot;
	vector<uint8_t> vertex_mask;
	vector<double> ranks;
	idx_t iterations = 0;
	bool converged = false;
	idx_t offset = 0;
};

static unique_ptr<GlobalTableFunctionState> PageRankInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<PageRankState>();
}

static void ComputePageRank(ClientContext &context, const PageRankBindData &data, PageRankState &state) {
	state.snapshot = GqlGetCsrSnapshot(context, data.graph_name);
	const auto vertex_count = state.snapshot->vertex_ids.size();
	idx_t projected_count;
	state.vertex_mask = BuildVertexMask(*state.snapshot, data.vertex_label, projected_count);
	state.ranks.assign(vertex_count, 0.0);
	if (projected_count == 0) {
		state.converged = true;
		state.initialized = true;
		return;
	}
	bool filter_label;
	auto required_label = ResolveLabel(*state.snapshot, data.edge_label, filter_label);
	vector<idx_t> out_degrees(vertex_count, 0);
	for (idx_t source = 0; source < vertex_count; source++) {
		if (!InVertexProjection(state.vertex_mask, source)) {
			continue;
		}
		if (state.vertex_mask.empty() && !filter_label) {
			out_degrees[source] =
			    state.snapshot->outgoing_offsets[source + 1] - state.snapshot->outgoing_offsets[source];
			continue;
		}
		for (idx_t offset = state.snapshot->outgoing_offsets[source];
		     offset < state.snapshot->outgoing_offsets[source + 1]; offset++) {
			auto target = state.snapshot->outgoing_neighbors[offset];
			out_degrees[source] += InVertexProjection(state.vertex_mask, target) &&
			                       (!filter_label || state.snapshot->outgoing_label_ids[offset] == required_label);
		}
	}

	for (idx_t vertex = 0; vertex < vertex_count; vertex++) {
		if (InVertexProjection(state.vertex_mask, vertex)) {
			state.ranks[vertex] = 1.0 / static_cast<double>(projected_count);
		}
	}
	vector<double> next(vertex_count, 0.0);
	for (idx_t iteration = 1; iteration <= data.max_iterations; iteration++) {
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
		double dangling_rank = 0.0;
		for (idx_t source = 0; source < vertex_count; source++) {
			if (InVertexProjection(state.vertex_mask, source) && out_degrees[source] == 0) {
				dangling_rank += state.ranks[source];
			}
		}
		auto base = (1.0 - data.damping) / static_cast<double>(projected_count) +
		            data.damping * dangling_rank / static_cast<double>(projected_count);
		std::fill(next.begin(), next.end(), 0.0);
		for (idx_t vertex = 0; vertex < vertex_count; vertex++) {
			if (InVertexProjection(state.vertex_mask, vertex)) {
				next[vertex] = base;
			}
		}
		for (idx_t source = 0; source < vertex_count; source++) {
			if (out_degrees[source] == 0) {
				continue;
			}
			auto contribution = data.damping * state.ranks[source] / static_cast<double>(out_degrees[source]);
			for (idx_t offset = state.snapshot->outgoing_offsets[source];
			     offset < state.snapshot->outgoing_offsets[source + 1]; offset++) {
				if (filter_label && state.snapshot->outgoing_label_ids[offset] != required_label) {
					continue;
				}
				auto target = state.snapshot->outgoing_neighbors[offset];
				if (InVertexProjection(state.vertex_mask, target)) {
					next[target] += contribution;
				}
			}
		}
		double difference = 0.0;
		for (idx_t vertex = 0; vertex < vertex_count; vertex++) {
			if (InVertexProjection(state.vertex_mask, vertex)) {
				difference += std::abs(next[vertex] - state.ranks[vertex]);
			}
		}
		state.ranks.swap(next);
		state.iterations = iteration;
		if (difference <= data.tolerance) {
			state.converged = true;
			break;
		}
	}
	state.initialized = true;
}

static void PageRankFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<PageRankBindData>();
	auto &state = input.global_state->Cast<PageRankState>();
	if (!state.initialized) {
		ComputePageRank(context, data, state);
	}
	auto vertex_ids = FlatVector::GetData<uint64_t>(output.data[0]);
	auto ranks = FlatVector::GetData<double>(output.data[1]);
	auto iterations = FlatVector::GetData<uint64_t>(output.data[2]);
	auto converged = FlatVector::GetData<bool>(output.data[3]);
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && state.offset < state.ranks.size()) {
		auto vertex = state.offset++;
		if (!InVertexProjection(state.vertex_mask, vertex)) {
			continue;
		}
		vertex_ids[count] = state.snapshot->vertex_ids[vertex];
		ranks[count] = state.ranks[vertex];
		iterations[count] = state.iterations;
		converged[count] = state.converged;
		count++;
	}
	output.SetCardinality(count);
}

struct GraphAlgorithmBindData : TableFunctionData {
	string graph_name;
	string edge_label;
	string vertex_label;
};

static unique_ptr<GraphAlgorithmBindData> ReadGraphAlgorithmBind(const TableFunctionBindInput &input,
                                                                 const string &algorithm_name) {
	if ((input.inputs.size() != 1 && input.inputs.size() != 2) || input.inputs[0].IsNull()) {
		throw BinderException("GQL %s requires a graph name", algorithm_name);
	}
	auto result = make_uniq<GraphAlgorithmBindData>();
	result->graph_name = input.inputs[0].GetValue<string>();
	if (result->graph_name.empty()) {
		throw BinderException("GQL %s requires a graph name", algorithm_name);
	}
	result->edge_label = ReadLabelParameter(input, "edge_label");
	result->vertex_label = ReadVertexLabel(input, 1);
	return result;
}

static unique_ptr<FunctionData> ComponentBind(const string &algorithm_name, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	auto result = ReadGraphAlgorithmBind(input, algorithm_name);
	names = {"vertex_id", "component_id", "component_size"};
	return_types = {LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT};
	return std::move(result);
}

static unique_ptr<FunctionData> WccBind(ClientContext &, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	return ComponentBind("WCC", input, return_types, names);
}

static unique_ptr<FunctionData> SccBind(ClientContext &, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	return ComponentBind("SCC", input, return_types, names);
}

struct ComponentState : GlobalTableFunctionState {
	bool initialized = false;
	shared_ptr<const GqlCsrSnapshot> snapshot;
	vector<uint8_t> vertex_mask;
	vector<uint64_t> component_ids;
	vector<idx_t> component_sizes;
	idx_t offset = 0;
};

static unique_ptr<GlobalTableFunctionState> ComponentInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<ComponentState>();
}

static idx_t FindComponentRoot(vector<idx_t> &parents, idx_t vertex) {
	auto root = vertex;
	while (parents[root] != root) {
		root = parents[root];
	}
	while (parents[vertex] != vertex) {
		auto parent = parents[vertex];
		parents[vertex] = root;
		vertex = parent;
	}
	return root;
}

static void ComputeWcc(ClientContext &context, const GraphAlgorithmBindData &data, ComponentState &state) {
	state.snapshot = GqlGetCsrSnapshot(context, data.graph_name);
	const auto vertex_count = state.snapshot->vertex_ids.size();
	idx_t projected_count;
	state.vertex_mask = BuildVertexMask(*state.snapshot, data.vertex_label, projected_count);
	vector<idx_t> parents(vertex_count);
	vector<idx_t> sizes(vertex_count, 1);
	for (idx_t vertex = 0; vertex < vertex_count; vertex++) {
		parents[vertex] = vertex;
	}
	bool filter_label;
	auto required_label = ResolveLabel(*state.snapshot, data.edge_label, filter_label);
	for (idx_t source = 0; source < vertex_count; source++) {
		if (!InVertexProjection(state.vertex_mask, source)) {
			continue;
		}
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
		VisitRange(state.snapshot->outgoing_offsets, state.snapshot->outgoing_neighbors,
		           state.snapshot->outgoing_edge_ids, state.snapshot->outgoing_label_ids, source, filter_label,
		           required_label, [&](idx_t target, uint64_t) {
			           if (!InVertexProjection(state.vertex_mask, target)) {
				           return true;
			           }
			           auto source_root = FindComponentRoot(parents, source);
			           auto target_root = FindComponentRoot(parents, target);
			           if (source_root == target_root) {
				           return true;
			           }
			           if (sizes[source_root] < sizes[target_root]) {
				           std::swap(source_root, target_root);
			           }
			           parents[target_root] = source_root;
			           sizes[source_root] += sizes[target_root];
			           return true;
		           });
	}

	vector<uint64_t> minimum_ids(vertex_count, std::numeric_limits<uint64_t>::max());
	vector<idx_t> component_sizes(vertex_count, 0);
	for (idx_t vertex = 0; vertex < vertex_count; vertex++) {
		if (!InVertexProjection(state.vertex_mask, vertex)) {
			continue;
		}
		auto root = FindComponentRoot(parents, vertex);
		minimum_ids[root] = MinValue<uint64_t>(minimum_ids[root], state.snapshot->vertex_ids[vertex]);
		component_sizes[root]++;
	}
	state.component_ids.resize(vertex_count);
	state.component_sizes.resize(vertex_count);
	for (idx_t vertex = 0; vertex < vertex_count; vertex++) {
		if (!InVertexProjection(state.vertex_mask, vertex)) {
			continue;
		}
		auto root = FindComponentRoot(parents, vertex);
		state.component_ids[vertex] = minimum_ids[root];
		state.component_sizes[vertex] = component_sizes[root];
	}
	state.initialized = true;
}

struct SccDfsFrame {
	idx_t vertex;
	idx_t offset;
	idx_t end;
};

static void ComputeScc(ClientContext &context, const GraphAlgorithmBindData &data, ComponentState &state) {
	state.snapshot = GqlGetCsrSnapshot(context, data.graph_name);
	const auto vertex_count = state.snapshot->vertex_ids.size();
	idx_t projected_count;
	state.vertex_mask = BuildVertexMask(*state.snapshot, data.vertex_label, projected_count);
	bool filter_label;
	auto required_label = ResolveLabel(*state.snapshot, data.edge_label, filter_label);
	vector<uint8_t> visited(vertex_count, false);
	vector<idx_t> finish_order;
	finish_order.reserve(vertex_count);
	vector<SccDfsFrame> dfs_stack;

	for (idx_t start = 0; start < vertex_count; start++) {
		if (!InVertexProjection(state.vertex_mask, start) || visited[start]) {
			continue;
		}
		visited[start] = true;
		dfs_stack.push_back(
		    {start, state.snapshot->outgoing_offsets[start], state.snapshot->outgoing_offsets[start + 1]});
		while (!dfs_stack.empty()) {
			if (context.IsInterrupted()) {
				throw InterruptException();
			}
			auto &frame = dfs_stack.back();
			bool descended = false;
			while (frame.offset < frame.end) {
				auto edge_offset = frame.offset++;
				if (filter_label && state.snapshot->outgoing_label_ids[edge_offset] != required_label) {
					continue;
				}
				auto neighbor = state.snapshot->outgoing_neighbors[edge_offset];
				if (!InVertexProjection(state.vertex_mask, neighbor) || visited[neighbor]) {
					continue;
				}
				visited[neighbor] = true;
				dfs_stack.push_back({neighbor, state.snapshot->outgoing_offsets[neighbor],
				                     state.snapshot->outgoing_offsets[neighbor + 1]});
				descended = true;
				break;
			}
			if (!descended) {
				finish_order.push_back(frame.vertex);
				dfs_stack.pop_back();
			}
		}
	}

	std::fill(visited.begin(), visited.end(), false);
	state.component_ids.resize(vertex_count);
	state.component_sizes.resize(vertex_count);
	vector<idx_t> stack;
	vector<idx_t> members;
	for (auto order = finish_order.rbegin(); order != finish_order.rend(); ++order) {
		auto start = *order;
		if (visited[start]) {
			continue;
		}
		members.clear();
		stack.push_back(start);
		visited[start] = true;
		uint64_t minimum_id = state.snapshot->vertex_ids[start];
		while (!stack.empty()) {
			if (context.IsInterrupted()) {
				throw InterruptException();
			}
			auto vertex = stack.back();
			stack.pop_back();
			members.push_back(vertex);
			minimum_id = MinValue<uint64_t>(minimum_id, state.snapshot->vertex_ids[vertex]);
			VisitRange(state.snapshot->incoming_offsets, state.snapshot->incoming_neighbors,
			           state.snapshot->incoming_edge_ids, state.snapshot->incoming_label_ids, vertex, filter_label,
			           required_label, [&](idx_t neighbor, uint64_t) {
				           if (InVertexProjection(state.vertex_mask, neighbor) && !visited[neighbor]) {
					           visited[neighbor] = true;
					           stack.push_back(neighbor);
				           }
				           return true;
			           });
		}
		for (auto vertex : members) {
			state.component_ids[vertex] = minimum_id;
			state.component_sizes[vertex] = members.size();
		}
	}
	state.initialized = true;
}

static void WriteComponentRows(ComponentState &state, DataChunk &output) {
	auto vertex_ids = FlatVector::GetData<uint64_t>(output.data[0]);
	auto component_ids = FlatVector::GetData<uint64_t>(output.data[1]);
	auto component_sizes = FlatVector::GetData<uint64_t>(output.data[2]);
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && state.offset < state.component_ids.size()) {
		auto vertex = state.offset++;
		if (!InVertexProjection(state.vertex_mask, vertex)) {
			continue;
		}
		vertex_ids[count] = state.snapshot->vertex_ids[vertex];
		component_ids[count] = state.component_ids[vertex];
		component_sizes[count] = state.component_sizes[vertex];
		count++;
	}
	output.SetCardinality(count);
}

static void WccFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<GraphAlgorithmBindData>();
	auto &state = input.global_state->Cast<ComponentState>();
	if (!state.initialized) {
		ComputeWcc(context, data, state);
	}
	WriteComponentRows(state, output);
}

static void SccFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<GraphAlgorithmBindData>();
	auto &state = input.global_state->Cast<ComponentState>();
	if (!state.initialized) {
		ComputeScc(context, data, state);
	}
	WriteComponentRows(state, output);
}

static unique_ptr<FunctionData> TriangleCountBind(ClientContext &, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	auto result = ReadGraphAlgorithmBind(input, "triangle count");
	names = {"vertex_id", "triangle_count", "degree", "local_clustering_coefficient", "global_triangle_count"};
	return_types = {LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::DOUBLE,
	                LogicalType::UBIGINT};
	return std::move(result);
}

struct TriangleCountState : GlobalTableFunctionState {
	bool initialized = false;
	shared_ptr<const GqlCsrSnapshot> snapshot;
	vector<uint8_t> vertex_mask;
	vector<idx_t> triangle_counts;
	vector<idx_t> degrees;
	idx_t global_triangle_count = 0;
	idx_t offset = 0;
};

static unique_ptr<GlobalTableFunctionState> TriangleCountInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<TriangleCountState>();
}

static void ComputeTriangleCounts(ClientContext &context, const GraphAlgorithmBindData &data,
                                  TriangleCountState &state) {
	state.snapshot = GqlGetCsrSnapshot(context, data.graph_name);
	const auto vertex_count = state.snapshot->vertex_ids.size();
	idx_t projected_count;
	state.vertex_mask = BuildVertexMask(*state.snapshot, data.vertex_label, projected_count);
	bool filter_label;
	auto required_label = ResolveLabel(*state.snapshot, data.edge_label, filter_label);

	vector<std::pair<idx_t, idx_t>> undirected_edges;
	undirected_edges.reserve(state.snapshot->outgoing_neighbors.size());
	for (idx_t source = 0; source < vertex_count; source++) {
		if (!InVertexProjection(state.vertex_mask, source)) {
			continue;
		}
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
		for (idx_t offset = state.snapshot->outgoing_offsets[source];
		     offset < state.snapshot->outgoing_offsets[source + 1]; offset++) {
			if (filter_label && state.snapshot->outgoing_label_ids[offset] != required_label) {
				continue;
			}
			auto target = state.snapshot->outgoing_neighbors[offset];
			if (source == target || !InVertexProjection(state.vertex_mask, target)) {
				continue;
			}
			undirected_edges.emplace_back(MinValue<idx_t>(source, target), MaxValue<idx_t>(source, target));
		}
	}
	std::sort(undirected_edges.begin(), undirected_edges.end());
	undirected_edges.erase(std::unique(undirected_edges.begin(), undirected_edges.end()), undirected_edges.end());

	state.degrees.assign(vertex_count, 0);
	for (const auto &edge : undirected_edges) {
		state.degrees[edge.first]++;
		state.degrees[edge.second]++;
	}
	vector<idx_t> forward_degrees(vertex_count, 0);
	auto is_lower_rank = [&](idx_t left, idx_t right) {
		return state.degrees[left] < state.degrees[right] ||
		       (state.degrees[left] == state.degrees[right] && left < right);
	};
	for (const auto &edge : undirected_edges) {
		auto source = is_lower_rank(edge.first, edge.second) ? edge.first : edge.second;
		forward_degrees[source]++;
	}
	vector<idx_t> forward_offsets(vertex_count + 1, 0);
	for (idx_t vertex = 0; vertex < vertex_count; vertex++) {
		forward_offsets[vertex + 1] = forward_offsets[vertex] + forward_degrees[vertex];
	}
	vector<idx_t> forward_neighbors(undirected_edges.size());
	auto cursors = forward_offsets;
	for (const auto &edge : undirected_edges) {
		auto source = is_lower_rank(edge.first, edge.second) ? edge.first : edge.second;
		auto target = source == edge.first ? edge.second : edge.first;
		forward_neighbors[cursors[source]++] = target;
	}
	undirected_edges.clear();
	undirected_edges.shrink_to_fit();

	state.triangle_counts.assign(vertex_count, 0);
	const auto unmarked = std::numeric_limits<idx_t>::max();
	vector<idx_t> markers(vertex_count, unmarked);
	for (idx_t source = 0; source < vertex_count; source++) {
		if (!InVertexProjection(state.vertex_mask, source)) {
			continue;
		}
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
		for (idx_t offset = forward_offsets[source]; offset < forward_offsets[source + 1]; offset++) {
			markers[forward_neighbors[offset]] = source;
		}
		for (idx_t offset = forward_offsets[source]; offset < forward_offsets[source + 1]; offset++) {
			auto middle = forward_neighbors[offset];
			for (idx_t middle_offset = forward_offsets[middle]; middle_offset < forward_offsets[middle + 1];
			     middle_offset++) {
				auto target = forward_neighbors[middle_offset];
				if (markers[target] != source) {
					continue;
				}
				state.triangle_counts[source]++;
				state.triangle_counts[middle]++;
				state.triangle_counts[target]++;
				state.global_triangle_count++;
			}
		}
	}
	state.initialized = true;
}

static void TriangleCountFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<GraphAlgorithmBindData>();
	auto &state = input.global_state->Cast<TriangleCountState>();
	if (!state.initialized) {
		ComputeTriangleCounts(context, data, state);
	}
	auto vertex_ids = FlatVector::GetData<uint64_t>(output.data[0]);
	auto triangle_counts = FlatVector::GetData<uint64_t>(output.data[1]);
	auto degrees = FlatVector::GetData<uint64_t>(output.data[2]);
	auto coefficients = FlatVector::GetData<double>(output.data[3]);
	auto global_counts = FlatVector::GetData<uint64_t>(output.data[4]);
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && state.offset < state.triangle_counts.size()) {
		auto vertex = state.offset++;
		if (!InVertexProjection(state.vertex_mask, vertex)) {
			continue;
		}
		vertex_ids[count] = state.snapshot->vertex_ids[vertex];
		triangle_counts[count] = state.triangle_counts[vertex];
		degrees[count] = state.degrees[vertex];
		coefficients[count] = state.degrees[vertex] < 2
		                          ? 0.0
		                          : (2.0 * state.triangle_counts[vertex]) /
		                                (static_cast<double>(state.degrees[vertex]) * (state.degrees[vertex] - 1));
		global_counts[count] = state.global_triangle_count;
		count++;
	}
	output.SetCardinality(count);
}

static unique_ptr<FunctionData> LccBind(ClientContext &, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	auto result = ReadGraphAlgorithmBind(input, "local clustering coefficient");
	names = {"vertex_id", "degree", "directed_neighbor_edge_count", "local_clustering_coefficient"};
	return_types = {LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::DOUBLE};
	return std::move(result);
}

struct LccState : GlobalTableFunctionState {
	bool initialized = false;
	shared_ptr<const GqlCsrSnapshot> snapshot;
	vector<uint8_t> vertex_mask;
	vector<idx_t> degrees;
	vector<idx_t> directed_neighbor_edge_counts;
	idx_t offset = 0;
};

static unique_ptr<GlobalTableFunctionState> LccInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<LccState>();
}

static void ComputeLcc(ClientContext &context, const GraphAlgorithmBindData &data, LccState &state) {
	state.snapshot = GqlGetCsrSnapshot(context, data.graph_name);
	const auto vertex_count = state.snapshot->vertex_ids.size();
	idx_t projected_count;
	state.vertex_mask = BuildVertexMask(*state.snapshot, data.vertex_label, projected_count);
	bool filter_label;
	auto required_label = ResolveLabel(*state.snapshot, data.edge_label, filter_label);

	// Graphalytics defines N(v) as the unique union of incoming and outgoing
	// neighbors, but preserves direction when counting edges between members of
	// N(v). Materialize each relation once: directed_edges supplies the
	// numerator, while undirected_edges supplies the neighbor sets.
	vector<std::pair<idx_t, idx_t>> directed_edges;
	directed_edges.reserve(state.snapshot->outgoing_neighbors.size());
	for (idx_t source = 0; source < vertex_count; source++) {
		if (!InVertexProjection(state.vertex_mask, source)) {
			continue;
		}
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
		for (idx_t offset = state.snapshot->outgoing_offsets[source];
		     offset < state.snapshot->outgoing_offsets[source + 1]; offset++) {
			if (filter_label && state.snapshot->outgoing_label_ids[offset] != required_label) {
				continue;
			}
			auto target = state.snapshot->outgoing_neighbors[offset];
			if (source != target && InVertexProjection(state.vertex_mask, target)) {
				directed_edges.emplace_back(source, target);
			}
		}
	}
	std::sort(directed_edges.begin(), directed_edges.end());
	directed_edges.erase(std::unique(directed_edges.begin(), directed_edges.end()), directed_edges.end());

	vector<std::pair<idx_t, idx_t>> undirected_edges;
	undirected_edges.reserve(directed_edges.size());
	for (const auto &edge : directed_edges) {
		undirected_edges.emplace_back(MinValue(edge.first, edge.second), MaxValue(edge.first, edge.second));
	}
	std::sort(undirected_edges.begin(), undirected_edges.end());
	undirected_edges.erase(std::unique(undirected_edges.begin(), undirected_edges.end()), undirected_edges.end());

	state.degrees.assign(vertex_count, 0);
	for (const auto &edge : undirected_edges) {
		state.degrees[edge.first]++;
		state.degrees[edge.second]++;
	}
	vector<idx_t> neighbor_offsets(vertex_count + 1, 0);
	for (idx_t vertex = 0; vertex < vertex_count; vertex++) {
		neighbor_offsets[vertex + 1] = neighbor_offsets[vertex] + state.degrees[vertex];
	}
	vector<idx_t> neighbors(undirected_edges.size() * 2);
	auto cursors = neighbor_offsets;
	for (const auto &edge : undirected_edges) {
		neighbors[cursors[edge.first]++] = edge.second;
		neighbors[cursors[edge.second]++] = edge.first;
	}
	undirected_edges.clear();
	undirected_edges.shrink_to_fit();
	for (idx_t vertex = 0; vertex < vertex_count; vertex++) {
		std::sort(neighbors.begin() + neighbor_offsets[vertex], neighbors.begin() + neighbor_offsets[vertex + 1]);
	}

	// For each directed u->w, every vertex in N(u) intersect N(w) sees that
	// arc between two of its neighbors. Reciprocal arcs therefore contribute
	// twice, exactly as required for directed LCC; symmetric undirected input
	// naturally reduces to the conventional 2T/(d(d-1)) coefficient.
	state.directed_neighbor_edge_counts.assign(vertex_count, 0);
	for (const auto &edge : directed_edges) {
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
		auto left = neighbor_offsets[edge.first];
		auto left_end = neighbor_offsets[edge.first + 1];
		auto right = neighbor_offsets[edge.second];
		auto right_end = neighbor_offsets[edge.second + 1];
		while (left < left_end && right < right_end) {
			auto left_neighbor = neighbors[left];
			auto right_neighbor = neighbors[right];
			if (left_neighbor < right_neighbor) {
				left++;
			} else if (right_neighbor < left_neighbor) {
				right++;
			} else {
				state.directed_neighbor_edge_counts[left_neighbor]++;
				left++;
				right++;
			}
		}
	}
	state.initialized = true;
}

static void LccFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<GraphAlgorithmBindData>();
	auto &state = input.global_state->Cast<LccState>();
	if (!state.initialized) {
		ComputeLcc(context, data, state);
	}
	auto vertex_ids = FlatVector::GetData<uint64_t>(output.data[0]);
	auto degrees = FlatVector::GetData<uint64_t>(output.data[1]);
	auto edge_counts = FlatVector::GetData<uint64_t>(output.data[2]);
	auto coefficients = FlatVector::GetData<double>(output.data[3]);
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && state.offset < state.degrees.size()) {
		auto vertex = state.offset++;
		if (!InVertexProjection(state.vertex_mask, vertex)) {
			continue;
		}
		vertex_ids[count] = state.snapshot->vertex_ids[vertex];
		degrees[count] = state.degrees[vertex];
		edge_counts[count] = state.directed_neighbor_edge_counts[vertex];
		coefficients[count] = state.degrees[vertex] < 2
		                          ? 0.0
		                          : static_cast<double>(state.directed_neighbor_edge_counts[vertex]) /
		                                (static_cast<double>(state.degrees[vertex]) * (state.degrees[vertex] - 1));
		count++;
	}
	output.SetCardinality(count);
}

struct LouvainBindData : GraphAlgorithmBindData {
	double resolution = 1.0;
	idx_t max_iterations = 32;
	idx_t max_levels = 32;
	double tolerance = 1e-12;
};

static unique_ptr<FunctionData> LouvainBind(ClientContext &, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	auto projection = ReadGraphAlgorithmBind(input, "Louvain");
	auto result = make_uniq<LouvainBindData>();
	result->graph_name = std::move(projection->graph_name);
	result->edge_label = std::move(projection->edge_label);
	result->vertex_label = std::move(projection->vertex_label);
	if (auto value = NamedParameter(input, "resolution")) {
		if (!value->IsNull()) {
			result->resolution = value->GetValue<double>();
		}
	}
	if (auto value = NamedParameter(input, "max_iterations")) {
		if (!value->IsNull()) {
			auto max_iterations = value->GetValue<int64_t>();
			if (max_iterations <= 0) {
				throw BinderException("GQL Louvain max_iterations must be positive");
			}
			result->max_iterations = NumericCast<idx_t>(max_iterations);
		}
	}
	if (auto value = NamedParameter(input, "max_levels")) {
		if (!value->IsNull()) {
			auto max_levels = value->GetValue<int64_t>();
			if (max_levels <= 0) {
				throw BinderException("GQL Louvain max_levels must be positive");
			}
			result->max_levels = NumericCast<idx_t>(max_levels);
		}
	}
	if (auto value = NamedParameter(input, "tolerance")) {
		if (!value->IsNull()) {
			result->tolerance = value->GetValue<double>();
		}
	}
	if (!std::isfinite(result->resolution) || result->resolution <= 0.0) {
		throw BinderException("GQL Louvain resolution must be positive and finite");
	}
	if (!std::isfinite(result->tolerance) || result->tolerance < 0.0) {
		throw BinderException("GQL Louvain tolerance must be non-negative and finite");
	}
	names = {"vertex_id", "community_id", "community_size", "modularity", "levels", "iterations", "converged"};
	return_types = {LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::DOUBLE,
	                LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::BOOLEAN};
	return std::move(result);
}

struct LouvainGraph {
	vector<uint64_t> offsets;
	GqlCsrOrdinals neighbors;
	vector<double> weights;
	vector<double> degrees;
	double total_degree = 0.0;

	idx_t VertexCount() const {
		return degrees.size();
	}

	double EdgeWeight(idx_t edge) const {
		return weights.empty() ? 1.0 : weights[edge];
	}
};

template <class CALLBACK>
static void VisitLouvainNeighbors(const GqlCsrSnapshot &snapshot, idx_t snapshot_vertex,
                                  const vector<idx_t> &snapshot_to_projected, vector<idx_t> &markers,
                                  idx_t projected_vertex, bool filter_label, uint32_t required_label,
                                  CALLBACK &&callback) {
	auto visit = [&](idx_t snapshot_neighbor, uint64_t) {
		auto projected_neighbor = snapshot_to_projected[snapshot_neighbor];
		if (projected_neighbor == std::numeric_limits<idx_t>::max() || projected_neighbor == projected_vertex ||
		    markers[projected_neighbor] == projected_vertex) {
			return true;
		}
		markers[projected_neighbor] = projected_vertex;
		callback(projected_neighbor);
		return true;
	};
	VisitRange(snapshot.outgoing_offsets, snapshot.outgoing_neighbors, snapshot.outgoing_edge_ids,
	           snapshot.outgoing_label_ids, snapshot_vertex, filter_label, required_label, visit);
	VisitRange(snapshot.incoming_offsets, snapshot.incoming_neighbors, snapshot.incoming_edge_ids,
	           snapshot.incoming_label_ids, snapshot_vertex, filter_label, required_label, visit);
}

static LouvainGraph BuildLouvainGraph(ClientContext &context, const GqlCsrSnapshot &snapshot,
                                      const vector<uint8_t> &vertex_mask, const string &edge_label,
                                      idx_t projected_count, vector<idx_t> &projected_to_snapshot) {
	const auto invalid = std::numeric_limits<idx_t>::max();
	vector<idx_t> snapshot_to_projected(snapshot.vertex_ids.size(), invalid);
	projected_to_snapshot.reserve(projected_count);
	for (idx_t snapshot_vertex = 0; snapshot_vertex < snapshot.vertex_ids.size(); snapshot_vertex++) {
		if (!InVertexProjection(vertex_mask, snapshot_vertex)) {
			continue;
		}
		snapshot_to_projected[snapshot_vertex] = projected_to_snapshot.size();
		projected_to_snapshot.push_back(snapshot_vertex);
	}

	LouvainGraph graph;
	const auto vertex_count = projected_to_snapshot.size();
	graph.offsets.assign(vertex_count + 1, 0);
	graph.degrees.assign(vertex_count, 0.0);
	if (vertex_count == 0) {
		return graph;
	}
	bool filter_label;
	auto required_label = ResolveLabel(snapshot, edge_label, filter_label);
	vector<idx_t> markers(vertex_count, invalid);
	for (idx_t vertex = 0; vertex < vertex_count; vertex++) {
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
		idx_t degree = 0;
		VisitLouvainNeighbors(snapshot, projected_to_snapshot[vertex], snapshot_to_projected, markers, vertex,
		                      filter_label, required_label, [&](idx_t) { degree++; });
		graph.offsets[vertex + 1] = graph.offsets[vertex] + degree;
		graph.degrees[vertex] = static_cast<double>(degree);
	}

	const auto edge_count = NumericCast<idx_t>(graph.offsets.back());
	graph.neighbors.Resize(edge_count, vertex_count - 1 <= std::numeric_limits<uint32_t>::max());
	std::fill(markers.begin(), markers.end(), invalid);
	for (idx_t vertex = 0; vertex < vertex_count; vertex++) {
		auto cursor = NumericCast<idx_t>(graph.offsets[vertex]);
		VisitLouvainNeighbors(snapshot, projected_to_snapshot[vertex], snapshot_to_projected, markers, vertex,
		                      filter_label, required_label,
		                      [&](idx_t neighbor) { graph.neighbors.Set(cursor++, neighbor); });
	}
	graph.total_degree = static_cast<double>(edge_count);
	return graph;
}

struct LouvainMoveResult {
	vector<idx_t> communities;
	idx_t community_count = 0;
	idx_t iterations = 0;
	bool converged = false;
};

static LouvainMoveResult LouvainLocalMove(ClientContext &context, const LouvainGraph &graph,
                                          const LouvainBindData &data) {
	LouvainMoveResult result;
	const auto vertex_count = graph.VertexCount();
	result.communities.resize(vertex_count);
	vector<double> community_totals(vertex_count, 0.0);
	for (idx_t vertex = 0; vertex < vertex_count; vertex++) {
		result.communities[vertex] = vertex;
		community_totals[vertex] = graph.degrees[vertex];
	}
	if (vertex_count == 0 || graph.total_degree == 0.0) {
		result.community_count = vertex_count;
		result.iterations = vertex_count == 0 ? 0 : 1;
		result.converged = true;
		return result;
	}

	vector<uint64_t> affinity_markers(vertex_count, 0);
	vector<double> affinities(vertex_count, 0.0);
	vector<idx_t> candidates;
	candidates.reserve(64);
	const auto minimum_score_gain = data.tolerance * graph.total_degree;
	uint64_t affinity_epoch = 0;
	for (idx_t iteration = 0; iteration < data.max_iterations; iteration++) {
		bool moved = false;
		for (idx_t vertex = 0; vertex < vertex_count; vertex++) {
			if (context.IsInterrupted()) {
				throw InterruptException();
			}
			const auto degree = graph.degrees[vertex];
			if (degree == 0.0) {
				continue;
			}
			const auto current = result.communities[vertex];
			candidates.clear();
			if (++affinity_epoch == 0) {
				std::fill(affinity_markers.begin(), affinity_markers.end(), 0);
				affinity_epoch = 1;
			}
			auto touch = [&](idx_t community) {
				if (affinity_markers[community] == affinity_epoch) {
					return;
				}
				affinity_markers[community] = affinity_epoch;
				affinities[community] = 0.0;
				candidates.push_back(community);
			};
			touch(current);
			for (idx_t edge = graph.offsets[vertex]; edge < graph.offsets[vertex + 1]; edge++) {
				auto neighbor = graph.neighbors[edge];
				if (neighbor == vertex) {
					continue;
				}
				auto community = result.communities[neighbor];
				touch(community);
				affinities[community] += graph.EdgeWeight(edge);
			}

			community_totals[current] -= degree;
			auto score = [&](idx_t community) {
				return affinities[community] -
				       data.resolution * degree * community_totals[community] / graph.total_degree;
			};
			auto best = current;
			auto current_score = score(current);
			auto best_score = current_score;
			for (auto community : candidates) {
				auto candidate_score = score(community);
				if (candidate_score > best_score + minimum_score_gain ||
				    (candidate_score > current_score + minimum_score_gain &&
				     std::abs(candidate_score - best_score) <= minimum_score_gain && community < best)) {
					best = community;
					best_score = candidate_score;
				}
			}
			result.communities[vertex] = best;
			community_totals[best] += degree;
			moved |= best != current;
		}
		result.iterations = iteration + 1;
		if (!moved) {
			result.converged = true;
			break;
		}
	}

	const auto invalid = std::numeric_limits<idx_t>::max();
	vector<idx_t> compact(vertex_count, invalid);
	for (idx_t vertex = 0; vertex < vertex_count; vertex++) {
		auto community = result.communities[vertex];
		if (compact[community] == invalid) {
			compact[community] = result.community_count++;
		}
		result.communities[vertex] = compact[community];
	}
	return result;
}

static LouvainGraph CoarsenLouvainGraph(ClientContext &context, const LouvainGraph &graph,
                                        const LouvainMoveResult &move) {
	LouvainGraph coarse;
	const auto coarse_count = move.community_count;
	coarse.offsets.reserve(coarse_count + 1);
	coarse.offsets.push_back(0);
	coarse.degrees.assign(coarse_count, 0.0);

	vector<idx_t> member_offsets(coarse_count + 1, 0);
	for (auto community : move.communities) {
		member_offsets[community + 1]++;
	}
	for (idx_t community = 0; community < coarse_count; community++) {
		member_offsets[community + 1] += member_offsets[community];
	}
	vector<idx_t> members(graph.VertexCount());
	auto member_cursors = member_offsets;
	for (idx_t vertex = 0; vertex < graph.VertexCount(); vertex++) {
		members[member_cursors[move.communities[vertex]]++] = vertex;
	}

	const auto invalid = std::numeric_limits<idx_t>::max();
	vector<idx_t> markers(coarse_count, invalid);
	vector<double> affinities(coarse_count, 0.0);
	vector<idx_t> targets;
	vector<idx_t> coarse_neighbors;
	vector<double> coarse_weights;
	auto estimated_count =
	    coarse_count > std::numeric_limits<idx_t>::max() / 8 ? std::numeric_limits<idx_t>::max() : coarse_count * 8;
	auto reserve_count = MinValue<idx_t>(graph.neighbors.size(), estimated_count);
	coarse_neighbors.reserve(reserve_count);
	coarse_weights.reserve(reserve_count);
	for (idx_t community = 0; community < coarse_count; community++) {
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
		targets.clear();
		for (idx_t member = member_offsets[community]; member < member_offsets[community + 1]; member++) {
			auto vertex = members[member];
			for (idx_t edge = graph.offsets[vertex]; edge < graph.offsets[vertex + 1]; edge++) {
				auto target = move.communities[graph.neighbors[edge]];
				if (markers[target] != community) {
					markers[target] = community;
					affinities[target] = 0.0;
					targets.push_back(target);
				}
				affinities[target] += graph.EdgeWeight(edge);
			}
		}
		std::sort(targets.begin(), targets.end());
		for (auto target : targets) {
			auto weight = affinities[target];
			coarse_neighbors.push_back(target);
			coarse_weights.push_back(weight);
			coarse.degrees[community] += weight;
		}
		coarse.offsets.push_back(coarse_neighbors.size());
	}

	coarse.neighbors.Resize(coarse_neighbors.size(), coarse_count - 1 <= std::numeric_limits<uint32_t>::max());
	for (idx_t edge = 0; edge < coarse_neighbors.size(); edge++) {
		coarse.neighbors.Set(edge, coarse_neighbors[edge]);
	}
	coarse.weights = std::move(coarse_weights);
	coarse.total_degree = graph.total_degree;
	return coarse;
}

static double LouvainModularity(const LouvainGraph &graph, const vector<idx_t> &communities, idx_t community_count,
                                double resolution) {
	if (graph.total_degree == 0.0) {
		return 0.0;
	}
	vector<double> community_degrees(community_count, 0.0);
	double internal_weight = 0.0;
	for (idx_t vertex = 0; vertex < graph.VertexCount(); vertex++) {
		auto community = communities[vertex];
		community_degrees[community] += graph.degrees[vertex];
		for (idx_t edge = graph.offsets[vertex]; edge < graph.offsets[vertex + 1]; edge++) {
			if (communities[graph.neighbors[edge]] == community) {
				internal_weight += graph.EdgeWeight(edge);
			}
		}
	}
	double degree_penalty = 0.0;
	for (auto degree : community_degrees) {
		auto fraction = degree / graph.total_degree;
		degree_penalty += fraction * fraction;
	}
	return internal_weight / graph.total_degree - resolution * degree_penalty;
}

struct LouvainState : GlobalTableFunctionState {
	bool initialized = false;
	shared_ptr<const GqlCsrSnapshot> snapshot;
	vector<idx_t> projected_to_snapshot;
	vector<idx_t> communities;
	vector<uint64_t> community_ids;
	vector<idx_t> community_sizes;
	double modularity = 0.0;
	idx_t levels = 0;
	idx_t iterations = 0;
	bool converged = false;
	idx_t offset = 0;
};

static unique_ptr<GlobalTableFunctionState> LouvainInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<LouvainState>();
}

static void ComputeLouvain(ClientContext &context, const LouvainBindData &data, LouvainState &state) {
	state.snapshot = GqlGetCsrSnapshot(context, data.graph_name);
	idx_t projected_count;
	auto vertex_mask = BuildVertexMask(*state.snapshot, data.vertex_label, projected_count);
	auto original_graph = BuildLouvainGraph(context, *state.snapshot, vertex_mask, data.edge_label, projected_count,
	                                        state.projected_to_snapshot);
	if (projected_count == 0) {
		state.converged = true;
		state.initialized = true;
		return;
	}
	vector<idx_t> original_communities(projected_count);
	for (idx_t vertex = 0; vertex < projected_count; vertex++) {
		original_communities[vertex] = vertex;
	}
	idx_t final_community_count = projected_count;
	state.converged = true;
	const LouvainGraph *graph = &original_graph;
	unique_ptr<LouvainGraph> coarse_graph;
	for (idx_t level = 0; level < data.max_levels; level++) {
		auto move = LouvainLocalMove(context, *graph, data);
		state.levels++;
		state.iterations += move.iterations;
		state.converged &= move.converged;
		for (idx_t vertex = 0; vertex < projected_count; vertex++) {
			original_communities[vertex] = move.communities[original_communities[vertex]];
		}
		final_community_count = move.community_count;
		if (move.community_count == graph->VertexCount()) {
			break;
		}
		if (level + 1 == data.max_levels) {
			state.converged = false;
			break;
		}
		coarse_graph = make_uniq<LouvainGraph>(CoarsenLouvainGraph(context, *graph, move));
		graph = coarse_graph.get();
	}

	state.modularity = LouvainModularity(original_graph, original_communities, final_community_count, data.resolution);
	const auto invalid_id = std::numeric_limits<uint64_t>::max();
	vector<uint64_t> minimum_ids(final_community_count, invalid_id);
	vector<idx_t> sizes(final_community_count, 0);
	for (idx_t vertex = 0; vertex < projected_count; vertex++) {
		auto community = original_communities[vertex];
		auto snapshot_vertex = state.projected_to_snapshot[vertex];
		minimum_ids[community] = MinValue(minimum_ids[community], state.snapshot->vertex_ids[snapshot_vertex]);
		sizes[community]++;
	}
	state.communities = std::move(original_communities);
	state.community_ids = std::move(minimum_ids);
	state.community_sizes = std::move(sizes);
	state.initialized = true;
}

static void LouvainFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<LouvainBindData>();
	auto &state = input.global_state->Cast<LouvainState>();
	if (!state.initialized) {
		ComputeLouvain(context, data, state);
	}
	auto vertex_ids = FlatVector::GetData<uint64_t>(output.data[0]);
	auto community_ids = FlatVector::GetData<uint64_t>(output.data[1]);
	auto community_sizes = FlatVector::GetData<uint64_t>(output.data[2]);
	auto modularities = FlatVector::GetData<double>(output.data[3]);
	auto levels = FlatVector::GetData<uint64_t>(output.data[4]);
	auto iterations = FlatVector::GetData<uint64_t>(output.data[5]);
	auto converged = FlatVector::GetData<bool>(output.data[6]);
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && state.offset < state.projected_to_snapshot.size()) {
		auto vertex = state.offset++;
		auto community = state.communities[vertex];
		vertex_ids[count] = state.snapshot->vertex_ids[state.projected_to_snapshot[vertex]];
		community_ids[count] = state.community_ids[community];
		community_sizes[count] = state.community_sizes[community];
		modularities[count] = state.modularity;
		levels[count] = state.levels;
		iterations[count] = state.iterations;
		converged[count] = state.converged;
		count++;
	}
	output.SetCardinality(count);
}

static unique_ptr<FunctionData> DegreeBind(ClientContext &, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	auto result = ReadGraphAlgorithmBind(input, "degree");
	names = {"vertex_id", "out_degree", "in_degree", "total_degree"};
	return_types = {LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT};
	return std::move(result);
}

struct DegreeState : GlobalTableFunctionState {
	bool initialized = false;
	shared_ptr<const GqlCsrSnapshot> snapshot;
	vector<uint8_t> vertex_mask;
	vector<idx_t> out_degrees;
	vector<idx_t> in_degrees;
	idx_t offset = 0;
};

static unique_ptr<GlobalTableFunctionState> DegreeInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<DegreeState>();
}

static void ComputeDegrees(ClientContext &context, const GraphAlgorithmBindData &data, DegreeState &state) {
	state.snapshot = GqlGetCsrSnapshot(context, data.graph_name);
	auto vertex_count = state.snapshot->vertex_ids.size();
	idx_t projected_count;
	state.vertex_mask = BuildVertexMask(*state.snapshot, data.vertex_label, projected_count);
	state.out_degrees.assign(vertex_count, 0);
	state.in_degrees.assign(vertex_count, 0);
	bool filter_label;
	auto required_label = ResolveLabel(*state.snapshot, data.edge_label, filter_label);
	for (idx_t vertex = 0; vertex < vertex_count; vertex++) {
		if (!InVertexProjection(state.vertex_mask, vertex)) {
			continue;
		}
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
		if (!filter_label && state.vertex_mask.empty()) {
			state.out_degrees[vertex] =
			    state.snapshot->outgoing_offsets[vertex + 1] - state.snapshot->outgoing_offsets[vertex];
			state.in_degrees[vertex] =
			    state.snapshot->incoming_offsets[vertex + 1] - state.snapshot->incoming_offsets[vertex];
			continue;
		}
		for (idx_t offset = state.snapshot->outgoing_offsets[vertex];
		     offset < state.snapshot->outgoing_offsets[vertex + 1]; offset++) {
			state.out_degrees[vertex] +=
			    InVertexProjection(state.vertex_mask, state.snapshot->outgoing_neighbors[offset]) &&
			    (!filter_label || state.snapshot->outgoing_label_ids[offset] == required_label);
		}
		for (idx_t offset = state.snapshot->incoming_offsets[vertex];
		     offset < state.snapshot->incoming_offsets[vertex + 1]; offset++) {
			state.in_degrees[vertex] +=
			    InVertexProjection(state.vertex_mask, state.snapshot->incoming_neighbors[offset]) &&
			    (!filter_label || state.snapshot->incoming_label_ids[offset] == required_label);
		}
	}
	state.initialized = true;
}

static void DegreeFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<GraphAlgorithmBindData>();
	auto &state = input.global_state->Cast<DegreeState>();
	if (!state.initialized) {
		ComputeDegrees(context, data, state);
	}
	auto vertex_ids = FlatVector::GetData<uint64_t>(output.data[0]);
	auto out_degrees = FlatVector::GetData<uint64_t>(output.data[1]);
	auto in_degrees = FlatVector::GetData<uint64_t>(output.data[2]);
	auto total_degrees = FlatVector::GetData<uint64_t>(output.data[3]);
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && state.offset < state.out_degrees.size()) {
		auto vertex = state.offset++;
		if (!InVertexProjection(state.vertex_mask, vertex)) {
			continue;
		}
		vertex_ids[count] = state.snapshot->vertex_ids[vertex];
		out_degrees[count] = state.out_degrees[vertex];
		in_degrees[count] = state.in_degrees[vertex];
		total_degrees[count] = state.out_degrees[vertex] + state.in_degrees[vertex];
		count++;
	}
	output.SetCardinality(count);
}

struct ClosenessBindData : TableFunctionData {
	string graph_name;
	CsrDirection direction = CsrDirection::OUT;
	string edge_label;
	string vertex_label;
};

static unique_ptr<FunctionData> ClosenessBind(ClientContext &, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	if ((input.inputs.size() != 1 && input.inputs.size() != 2) || input.inputs[0].IsNull()) {
		throw BinderException("GQL closeness requires a graph name");
	}
	auto result = make_uniq<ClosenessBindData>();
	result->graph_name = input.inputs[0].GetValue<string>();
	if (result->graph_name.empty()) {
		throw BinderException("GQL closeness requires a graph name");
	}
	result->direction = ReadDirection(input);
	result->edge_label = ReadLabelParameter(input, "edge_label");
	result->vertex_label = ReadVertexLabel(input, 1);
	names = {"vertex_id", "reachable_count", "distance_sum", "closeness_centrality"};
	return_types = {LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::DOUBLE};
	return std::move(result);
}

struct ClosenessState : GlobalTableFunctionState {
	bool initialized = false;
	shared_ptr<const GqlCsrSnapshot> snapshot;
	vector<uint8_t> vertex_mask;
	vector<idx_t> reachable_counts;
	vector<uint64_t> distance_sums;
	vector<double> scores;
	idx_t offset = 0;
};

static unique_ptr<GlobalTableFunctionState> ClosenessInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<ClosenessState>();
}

static void ComputeCloseness(ClientContext &context, const ClosenessBindData &data, ClosenessState &state) {
	state.snapshot = GqlGetCsrSnapshot(context, data.graph_name);
	auto vertex_count = state.snapshot->vertex_ids.size();
	idx_t projected_count;
	state.vertex_mask = BuildVertexMask(*state.snapshot, data.vertex_label, projected_count);
	state.reachable_counts.assign(vertex_count, 0);
	state.distance_sums.assign(vertex_count, 0);
	state.scores.assign(vertex_count, 0.0);
	if (projected_count <= 1) {
		state.initialized = true;
		return;
	}
	bool filter_label;
	auto required_label = ResolveLabel(*state.snapshot, data.edge_label, filter_label);
	vector<uint32_t> visited(vertex_count, 0);
	vector<idx_t> distances(vertex_count, 0);
	vector<idx_t> queue;
	queue.reserve(vertex_count);
	uint32_t stamp = 0;
	for (idx_t source = 0; source < vertex_count; source++) {
		if (!InVertexProjection(state.vertex_mask, source)) {
			continue;
		}
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
		if (++stamp == 0) {
			std::fill(visited.begin(), visited.end(), 0);
			stamp = 1;
		}
		queue.clear();
		queue.push_back(source);
		visited[source] = stamp;
		distances[source] = 0;
		idx_t head = 0;
		uint64_t distance_sum = 0;
		idx_t reachable_count = 0;
		while (head < queue.size()) {
			auto vertex = queue[head++];
			VisitNeighbors(*state.snapshot, vertex, data.direction, filter_label, required_label,
			               [&](idx_t neighbor, uint64_t) {
				               if (!InVertexProjection(state.vertex_mask, neighbor) || visited[neighbor] == stamp) {
					               return true;
				               }
				               visited[neighbor] = stamp;
				               distances[neighbor] = distances[vertex] + 1;
				               if (distance_sum > NumericLimits<uint64_t>::Maximum() - distances[neighbor]) {
					               throw OutOfRangeException("GQL closeness distance sum overflow");
				               }
				               distance_sum += distances[neighbor];
				               reachable_count++;
				               queue.push_back(neighbor);
				               return true;
			               });
		}
		state.reachable_counts[source] = reachable_count;
		state.distance_sums[source] = distance_sum;
		if (distance_sum > 0) {
			auto reachable = static_cast<double>(reachable_count);
			// Generalized normalized closeness (Wasserman-Faust): inverse average
			// distance among reachable vertices, penalized by unreachable vertices.
			state.scores[source] = (reachable * reachable) /
			                       (static_cast<double>(projected_count - 1) * static_cast<double>(distance_sum));
		}
	}
	state.initialized = true;
}

static void ClosenessFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &data = input.bind_data->Cast<ClosenessBindData>();
	auto &state = input.global_state->Cast<ClosenessState>();
	if (!state.initialized) {
		ComputeCloseness(context, data, state);
	}
	auto vertex_ids = FlatVector::GetData<uint64_t>(output.data[0]);
	auto reachable_counts = FlatVector::GetData<uint64_t>(output.data[1]);
	auto distance_sums = FlatVector::GetData<uint64_t>(output.data[2]);
	auto scores = FlatVector::GetData<double>(output.data[3]);
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE && state.offset < state.scores.size()) {
		auto vertex = state.offset++;
		if (!InVertexProjection(state.vertex_mask, vertex)) {
			continue;
		}
		vertex_ids[count] = state.snapshot->vertex_ids[vertex];
		reachable_counts[count] = state.reachable_counts[vertex];
		distance_sums[count] = state.distance_sums[vertex];
		scores[count] = state.scores[vertex];
		count++;
	}
	output.SetCardinality(count);
}

// Generic blocking procedure node used by MATCH -> CALL pipelines. DuckDB
// binds the TABLE argument as this operator's child, so input production,
// algorithm execution, and result consumption share one physical plan and one
// transaction. Procedure metadata decides whether the child rows are merely a
// sequencing dependency (NONE) or a batch argument such as a frontier (BATCH).
struct AlgorithmCallBindData : TableFunctionData {
	const GqlProcedureDefinition *definition = nullptr;
	vector<GqlLiteral> configuration;
};

struct AlgorithmCallGlobalState : GlobalTableFunctionState {
	idx_t MaxThreads() const override {
		return 1;
	}

	bool initialized = false;
	vector<uint64_t> frontier;
	vector<uint64_t> targets;
	bool shortest_path_done = false;
	unique_ptr<FunctionData> nested_bind_data;
	unique_ptr<GlobalTableFunctionState> nested_global_state;
};

struct AlgorithmCallLocalState : LocalTableFunctionState {};

struct PipelineDfsState : GlobalTableFunctionState {
	shared_ptr<const GqlCsrSnapshot> snapshot;
	vector<uint8_t> vertex_mask;
	vector<uint8_t> visited;
	vector<idx_t> frontier;
	idx_t next_frontier = 0;
	vector<DfsFrame> stack;
	idx_t visit_order = 0;
};

static vector<string> ReadPipelineStringList(const Value &value) {
	vector<string> result;
	for (const auto &entry : ListValue::GetChildren(value)) {
		result.push_back(entry.GetValue<string>());
	}
	return result;
}

static vector<uint8_t> ReadPipelineByteList(const Value &value) {
	vector<uint8_t> result;
	for (const auto &entry : ListValue::GetChildren(value)) {
		result.push_back(entry.GetValue<uint8_t>());
	}
	return result;
}

static bool TryFindAlgorithmProperty(const GqlElementTableBinding &table, const string &property, string &column) {
	for (const auto &entry : table.property_columns) {
		if (StringUtil::CIEquals(entry.first, property)) {
			column = entry.second;
			return true;
		}
	}
	return false;
}

static const GqlProcedureOutputDefinition *FindAlgorithmOutput(const GqlProcedureDefinition &definition,
                                                               const string &name) {
	for (const auto &output : definition.outputs) {
		if (StringUtil::CIEquals(output.name, name)) {
			return &output;
		}
	}
	return nullptr;
}

static Value AlgorithmLiteralValue(GqlLiteralType type, const string &text) {
	switch (type) {
	case GqlLiteralType::NULL_VALUE:
		return Value();
	case GqlLiteralType::BOOLEAN:
		return Value::BOOLEAN(StringUtil::CIEquals(text, "true"));
	case GqlLiteralType::INTEGER:
		return Value::BIGINT(std::stoll(text));
	case GqlLiteralType::DECIMAL:
		return Value(text).DefaultCastAs(LogicalType::DECIMAL(38, 18));
	case GqlLiteralType::DOUBLE:
		return Value::DOUBLE(std::stod(text));
	case GqlLiteralType::STRING:
		return Value(text);
	}
	throw InternalException("Unknown GQL algorithm argument type");
}

enum class AlgorithmYieldSource : uint8_t { OUTPUT, VERTEX_PROPERTY, EDGE_PROPERTY };

struct AlgorithmYieldColumn {
	string name;
	string column;
	AlgorithmYieldSource source;
};

static unique_ptr<ParsedExpression> AlgorithmColumn(const string &table, const string &column,
                                                    const string &alias = string()) {
	auto result = make_uniq<ColumnRefExpression>(column, table);
	if (!alias.empty()) {
		result->SetAlias(alias);
	}
	return std::move(result);
}

static unique_ptr<TableRef> AlgorithmElementTable(const GqlElementTableBinding &table, const string &alias) {
	auto result = make_uniq<BaseTableRef>();
	result->catalog_name = table.catalog_name;
	result->schema_name = table.schema_name;
	result->table_name = table.table_name;
	result->alias = alias;
	return std::move(result);
}

static void AppendAlgorithmJoin(unique_ptr<TableRef> &root, unique_ptr<TableRef> right, JoinType type,
                                const string &left_table, const string &left_column, const string &right_table,
                                const string &right_column) {
	auto join = make_uniq<JoinRef>(JoinRefType::REGULAR);
	join->left = std::move(root);
	join->right = std::move(right);
	join->type = type;
	join->condition =
	    make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, AlgorithmColumn(left_table, left_column),
	                                    AlgorithmColumn(right_table, right_column));
	root = std::move(join);
}

static unique_ptr<TableRef> AlgorithmResultBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	if (input.inputs.size() != 5 || input.inputs[0].IsNull() || input.inputs[1].IsNull() || input.inputs[2].IsNull() ||
	    input.inputs[3].IsNull() || input.inputs[4].IsNull()) {
		throw BinderException("Invalid GQL algorithm result");
	}
	auto procedure_name = input.inputs[0].GetValue<string>();
	auto definition = GqlFindProcedure("algo", procedure_name);
	if (!definition) {
		throw BinderException("Unknown GQL procedure 'algo.%s'", procedure_name);
	}
	auto argument_values = ReadPipelineStringList(input.inputs[1]);
	auto argument_types = ReadPipelineByteList(input.inputs[2]);
	auto argument_names = ReadPipelineStringList(input.inputs[3]);
	auto yield_names = ReadPipelineStringList(input.inputs[4]);
	if (argument_values.empty() || argument_values.size() != argument_types.size() ||
	    argument_values.size() != argument_names.size() || yield_names.empty()) {
		throw BinderException("Invalid arguments for GQL procedure 'algo.%s'", procedure_name);
	}

	bool needs_properties = false;
	for (const auto &yield_name : yield_names) {
		needs_properties |= !FindAlgorithmOutput(*definition, yield_name);
	}

	GqlTableGraphBinding graph;
	if (needs_properties) {
		if (static_cast<GqlLiteralType>(argument_types[0]) != GqlLiteralType::STRING) {
			throw BinderException("GQL procedure 'algo.%s' requires a graph name before properties "
			                      "can be yielded",
			                      procedure_name);
		}
		if (!GqlTryLoadTableGraph(context, argument_values[0], graph)) {
			throw BinderException("Graph '%s' has no registered node or edge property tables", argument_values[0]);
		}
	}

	const bool has_vertex_identity = FindAlgorithmOutput(*definition, "vertex_id") != nullptr;
	const bool has_edge_identity = FindAlgorithmOutput(*definition, "edge_id") != nullptr;
	vector<AlgorithmYieldColumn> columns;
	case_insensitive_set_t seen_yields;
	bool needs_vertex_join = false;
	bool needs_edge_join = false;
	for (const auto &yield_name : yield_names) {
		if (!seen_yields.insert(yield_name).second) {
			continue;
		}
		if (auto output = FindAlgorithmOutput(*definition, yield_name)) {
			columns.push_back({yield_name, output->name, AlgorithmYieldSource::OUTPUT});
			continue;
		}
		string vertex_column;
		string edge_column;
		auto is_vertex_property = TryFindAlgorithmProperty(graph.vertex, yield_name, vertex_column);
		auto is_edge_property = TryFindAlgorithmProperty(graph.edge, yield_name, edge_column);
		auto can_use_vertex = is_vertex_property && has_vertex_identity;
		auto can_use_edge = is_edge_property && has_edge_identity;
		if (can_use_vertex && can_use_edge) {
			throw BinderException("GQL YIELD property '%s' is ambiguous because it is registered on "
			                      "both nodes and edges",
			                      yield_name);
		}
		if (can_use_vertex) {
			columns.push_back({yield_name, vertex_column, AlgorithmYieldSource::VERTEX_PROPERTY});
			needs_vertex_join = true;
			continue;
		}
		if (can_use_edge) {
			columns.push_back({yield_name, edge_column, AlgorithmYieldSource::EDGE_PROPERTY});
			needs_edge_join = true;
			continue;
		}
		if (is_edge_property && !has_edge_identity) {
			throw BinderException("GQL procedure 'algo.%s' cannot yield edge property '%s' because "
			                      "it does not return an edge identity",
			                      procedure_name, yield_name);
		}
		throw BinderException("GQL procedure 'algo.%s' has no output or registered node/edge "
		                      "property '%s'",
		                      procedure_name, yield_name);
	}

	vector<unique_ptr<ParsedExpression>> arguments;
	for (idx_t index = 0; index < argument_values.size(); index++) {
		auto type = static_cast<GqlLiteralType>(argument_types[index]);
		auto argument = make_uniq<ConstantExpression>(AlgorithmLiteralValue(type, argument_values[index]));
		if (!argument_names[index].empty()) {
			argument->SetAlias(argument_names[index]);
		}
		arguments.push_back(std::move(argument));
	}
	auto algorithm = make_uniq<TableFunctionRef>();
	algorithm->function = make_uniq<FunctionExpression>("system", "algo", definition->name, std::move(arguments));
	algorithm->alias = "__gql_algo";
	unique_ptr<TableRef> from = std::move(algorithm);
	if (needs_vertex_join) {
		AppendAlgorithmJoin(from, AlgorithmElementTable(graph.vertex, "__gql_vertex"), JoinType::INNER, "__gql_algo",
		                    "vertex_id", "__gql_vertex", graph.vertex.key_column);
	}
	if (needs_edge_join) {
		AppendAlgorithmJoin(from, AlgorithmElementTable(graph.edge, "__gql_edge"), JoinType::LEFT, "__gql_algo",
		                    "edge_id", "__gql_edge", graph.edge.key_column);
	}

	auto select = make_uniq<SelectNode>();
	select->from_table = std::move(from);
	for (const auto &column : columns) {
		switch (column.source) {
		case AlgorithmYieldSource::OUTPUT:
			select->select_list.push_back(AlgorithmColumn("__gql_algo", column.column, column.name));
			break;
		case AlgorithmYieldSource::VERTEX_PROPERTY:
			select->select_list.push_back(AlgorithmColumn("__gql_vertex", column.column, column.name));
			break;
		case AlgorithmYieldSource::EDGE_PROPERTY:
			select->select_list.push_back(AlgorithmColumn("__gql_edge", column.column, column.name));
			break;
		}
	}
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(select);
	return make_uniq<SubqueryRef>(std::move(statement));
}

static unique_ptr<FunctionData> AlgorithmCallBind(ClientContext &, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 4 || input.inputs[1].IsNull() || input.inputs[2].IsNull() || input.inputs[3].IsNull()) {
		throw BinderException("Invalid GQL algorithm CALL node");
	}
	auto procedure_name = input.inputs[1].GetValue<string>();
	auto definition = GqlFindProcedure("algo", procedure_name);
	if (!definition) {
		throw BinderException("Unknown GQL procedure 'algo.%s'", procedure_name);
	}
	auto values = ReadPipelineStringList(input.inputs[2]);
	auto types = ReadPipelineByteList(input.inputs[3]);
	idx_t required_configuration_count = 0;
	idx_t expected_configuration_count = 0;
	idx_t expected_input_count = 0;
	for (const auto &argument : definition->arguments) {
		if (argument.mode == GqlProcedureArgumentMode::CONFIGURATION) {
			expected_configuration_count++;
			required_configuration_count += !argument.optional;
		} else {
			expected_input_count++;
		}
	}
	if (values.size() < required_configuration_count || values.size() > expected_configuration_count ||
	    types.size() != values.size() ||
	    input.input_table_types.size() !=
	        (definition->input_mode == GqlProcedureInputMode::NONE ? 1 : expected_input_count)) {
		throw BinderException("Invalid input shape for GQL procedure 'algo.%s'", procedure_name);
	}

	auto result = make_uniq<AlgorithmCallBindData>();
	result->definition = definition;
	idx_t configuration_index = 0;
	for (const auto &argument : definition->arguments) {
		if (argument.mode != GqlProcedureArgumentMode::CONFIGURATION) {
			continue;
		}
		if (configuration_index >= values.size()) {
			if (argument.optional) {
				continue;
			}
			throw BinderException("Missing configuration for GQL procedure 'algo.%s'", procedure_name);
		}
		auto literal_type = static_cast<GqlLiteralType>(types[configuration_index]);
		GqlLiteral literal;
		literal.type = literal_type;
		literal.value = values[configuration_index++];
		if (literal_type == GqlLiteralType::NULL_VALUE ||
		    (argument.type.id == GqlTypeId::STRING && literal_type != GqlLiteralType::STRING) ||
		    (argument.type.id == GqlTypeId::INTEGER && literal_type != GqlLiteralType::INTEGER)) {
			throw BinderException("Invalid configuration for GQL procedure 'algo.%s'", procedure_name);
		}
		if (argument.type.id == GqlTypeId::INTEGER && std::stoll(literal.value) < 0) {
			throw BinderException("GQL procedure configuration '%s' must be non-negative", argument.name);
		}
		result->configuration.push_back(std::move(literal));
	}
	for (const auto &output : definition->outputs) {
		names.push_back(output.name);
		// Algorithm ordinals/counts are non-negative and their hot writers use
		// uint64_t arrays. Keep that physical representation while the GQL binder
		// continues to expose the values as semantic INTEGERs.
		if (definition->name == "shortest_path_length" && output.name == "distance") {
			return_types.push_back(LogicalType::BIGINT);
		} else {
			return_types.push_back(output.type.id == GqlTypeId::INTEGER ? LogicalType::UBIGINT
			                                                            : GqlDuckType(output.type));
		}
	}
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> AlgorithmCallGlobalInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<AlgorithmCallGlobalState>();
}

static unique_ptr<LocalTableFunctionState> AlgorithmCallLocalInit(ExecutionContext &, TableFunctionInitInput &,
                                                                  GlobalTableFunctionState *) {
	return make_uniq<AlgorithmCallLocalState>();
}

static OperatorResultType AlgorithmCallInput(ExecutionContext &, TableFunctionInput &input, DataChunk &child,
                                             DataChunk &output) {
	auto &data = input.bind_data->Cast<AlgorithmCallBindData>();
	auto &state = input.global_state->Cast<AlgorithmCallGlobalState>();
	output.SetCardinality(0);
	if (data.definition->input_mode == GqlProcedureInputMode::NONE) {
		return OperatorResultType::NEED_MORE_INPUT;
	}
	auto shortest_path = data.definition->name == "shortest_path_length";
	auto expected_columns = shortest_path ? 2 : 1;
	if (data.definition->input_mode != GqlProcedureInputMode::BATCH || child.ColumnCount() != expected_columns) {
		throw InternalException("Invalid runtime input for GQL procedure node");
	}
	for (idx_t row = 0; row < child.size(); row++) {
		auto value = child.data[0].GetValue(row);
		if (value.IsNull()) {
			continue;
		}
		state.frontier.push_back(value.DefaultCastAs(LogicalType::UBIGINT).GetValue<uint64_t>());
		if (shortest_path) {
			auto target = child.data[1].GetValue(row);
			if (target.IsNull()) {
				state.frontier.pop_back();
				continue;
			}
			state.targets.push_back(target.DefaultCastAs(LogicalType::UBIGINT).GetValue<uint64_t>());
		}
	}
	return OperatorResultType::NEED_MORE_INPUT;
}

static int64_t ComputeShortestPathLength(ClientContext &context, const AlgorithmCallBindData &data,
                                         AlgorithmCallGlobalState &state) {
	std::sort(state.frontier.begin(), state.frontier.end());
	state.frontier.erase(std::unique(state.frontier.begin(), state.frontier.end()), state.frontier.end());
	std::sort(state.targets.begin(), state.targets.end());
	state.targets.erase(std::unique(state.targets.begin(), state.targets.end()), state.targets.end());
	if (state.frontier.size() != 1 || state.targets.size() != 1) {
		throw InvalidInputException(
		    "GQL shortest_path_length requires exactly one source and one target; found %llu and %llu",
		    static_cast<unsigned long long>(state.frontier.size()),
		    static_cast<unsigned long long>(state.targets.size()));
	}

	auto snapshot = GqlGetCsrSnapshot(context, data.configuration[0].value);
	auto vertex_label = data.configuration.size() > 1 ? data.configuration[1].value : string();
	auto edge_label = data.configuration.size() > 2 ? data.configuration[2].value : string();
	idx_t projected_count;
	auto vertex_mask = BuildVertexMask(*snapshot, vertex_label, projected_count);
	auto source = RequireProjectedVertex(*snapshot, vertex_mask, state.frontier[0], "source");
	auto target = RequireProjectedVertex(*snapshot, vertex_mask, state.targets[0], "target");
	if (source == target) {
		return 0;
	}
	bool filter_label;
	auto required_label = ResolveLabel(*snapshot, edge_label, filter_label);
	vector<uint8_t> visited(snapshot->vertex_ids.size(), false);
	vector<pair<idx_t, idx_t>> queue;
	visited[source] = true;
	queue.emplace_back(source, 0);
	for (idx_t head = 0; head < queue.size(); head++) {
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
		auto vertex = queue[head].first;
		auto depth = queue[head].second;
		bool found = false;
		VisitNeighbors(*snapshot, vertex, CsrDirection::OUT, filter_label, required_label,
		               [&](idx_t neighbor, uint64_t) {
			               if (!InVertexProjection(vertex_mask, neighbor) || visited[neighbor]) {
				               return true;
			               }
			               if (neighbor == target) {
				               found = true;
				               return false;
			               }
			               visited[neighbor] = true;
			               queue.emplace_back(neighbor, depth + 1);
			               return true;
		               });
		if (found) {
			return NumericCast<int64_t>(depth + 1);
		}
	}
	return -1;
}

static void InitializePipelineBfs(ClientContext &context, const AlgorithmCallBindData &data,
                                  AlgorithmCallGlobalState &state) {
	auto graph_name = data.configuration[0].value;
	auto snapshot = GqlGetCsrSnapshot(context, graph_name);
	std::sort(state.frontier.begin(), state.frontier.end());
	state.frontier.erase(std::unique(state.frontier.begin(), state.frontier.end()), state.frontier.end());
	auto traversal = make_uniq<TraversalBindData>();
	traversal->graph_name = graph_name;
	traversal->vertex_label = data.configuration.size() > 1 ? data.configuration[1].value : string();
	traversal->edge_label = data.configuration.size() > 2 ? data.configuration[2].value : string();
	if (data.configuration.size() > 3) {
		traversal->has_target = true;
		traversal->target_vertex_id = NumericCast<uint64_t>(std::stoull(data.configuration[3].value));
	}
	auto bfs = make_uniq<BfsState>();
	bfs->initialized = true;
	bfs->snapshot = std::move(snapshot);
	idx_t projected_count;
	bfs->vertex_mask = BuildVertexMask(*bfs->snapshot, traversal->vertex_label, projected_count);
	bfs->required_label = ResolveLabel(*bfs->snapshot, traversal->edge_label, bfs->filter_label);
	bfs->has_target = traversal->has_target;
	if (traversal->has_target) {
		bfs->target = RequireProjectedVertex(*bfs->snapshot, bfs->vertex_mask, traversal->target_vertex_id, "target");
	}
	bfs->visited.resize(bfs->snapshot->vertex_ids.size(), false);
	for (auto vertex_id : state.frontier) {
		auto vertex = RequireProjectedVertex(*bfs->snapshot, bfs->vertex_mask, vertex_id, "frontier");
		if (!bfs->visited[vertex]) {
			bfs->visited[vertex] = true;
			bfs->queue.push_back({vertex, 0, 0, 0, false});
		}
	}
	state.nested_bind_data = std::move(traversal);
	state.nested_global_state = std::move(bfs);
}

static void InitializePipelineDfs(ClientContext &context, const AlgorithmCallBindData &data,
                                  AlgorithmCallGlobalState &state) {
	auto graph_name = data.configuration[0].value;
	auto dfs = make_uniq<PipelineDfsState>();
	dfs->snapshot = GqlGetCsrSnapshot(context, graph_name);
	idx_t projected_count;
	dfs->vertex_mask = BuildVertexMask(
	    *dfs->snapshot, data.configuration.size() > 1 ? data.configuration[1].value : string(), projected_count);
	std::sort(state.frontier.begin(), state.frontier.end());
	state.frontier.erase(std::unique(state.frontier.begin(), state.frontier.end()), state.frontier.end());
	dfs->visited.resize(dfs->snapshot->vertex_ids.size(), false);
	for (auto vertex_id : state.frontier) {
		dfs->frontier.push_back(RequireProjectedVertex(*dfs->snapshot, dfs->vertex_mask, vertex_id, "frontier"));
	}
	state.nested_global_state = std::move(dfs);
}

static void PipelineDfsFunction(ClientContext &context, PipelineDfsState &state, DataChunk &output) {
	TraversalOutputWriter writer(output);
	idx_t count = 0;
	while (count < STANDARD_VECTOR_SIZE) {
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
		if (state.stack.empty()) {
			while (state.next_frontier < state.frontier.size() && state.visited[state.frontier[state.next_frontier]]) {
				state.next_frontier++;
			}
			if (state.next_frontier >= state.frontier.size()) {
				break;
			}
			auto start = state.frontier[state.next_frontier++];
			state.visited[start] = true;
			writer.Write(count++, *state.snapshot, {start, 0, 0, 0, false}, state.visit_order++);
			state.stack.push_back(MakeDfsFrame(*state.snapshot, start, 0, CsrDirection::OUT));
			continue;
		}
		auto &frame = state.stack.back();
		while (true) {
			idx_t neighbor;
			uint64_t edge_id;
			if (!NextDfsNeighbor(*state.snapshot, CsrDirection::OUT, false, 0, state.vertex_mask, frame, neighbor,
			                     edge_id)) {
				state.stack.pop_back();
				break;
			}
			if (state.visited[neighbor]) {
				continue;
			}
			state.visited[neighbor] = true;
			TraversalRow row {neighbor, frame.depth + 1, frame.vertex, edge_id, true};
			writer.Write(count++, *state.snapshot, row, state.visit_order++);
			state.stack.push_back(MakeDfsFrame(*state.snapshot, neighbor, row.depth, CsrDirection::OUT));
			break;
		}
	}
	output.SetCardinality(count);
}

static void InitializeAlgorithmCall(ExecutionContext &context, const AlgorithmCallBindData &data,
                                    AlgorithmCallGlobalState &state) {
	auto &name = data.definition->name;
	auto graph_name = data.configuration[0].value;
	auto vertex_label = data.configuration.size() > 1 ? data.configuration[1].value : string();
	if (name == "bfs" || name == "sssp") {
		InitializePipelineBfs(context.client, data, state);
		auto source_count = state.nested_global_state->Cast<BfsState>().queue.size();
		if (name == "sssp" && source_count != 1) {
			throw InvalidInputException(
			    "GQL SSSP requires exactly one distinct source vertex; found %llu from %llu input rows",
			    static_cast<unsigned long long>(source_count), static_cast<unsigned long long>(state.frontier.size()));
		}
	} else if (name == "shortest_path_length") {
		// The two matched element IDs are retained in the call state and the
		// scalar BFS result is produced during finalize.
	} else if (name == "dfs") {
		InitializePipelineDfs(context.client, data, state);
	} else if (name == "pagerank") {
		auto bind = make_uniq<PageRankBindData>();
		bind->graph_name = graph_name;
		bind->vertex_label = vertex_label;
		state.nested_bind_data = std::move(bind);
		state.nested_global_state = make_uniq<PageRankState>();
	} else if (name == "wcc" || name == "scc") {
		auto bind = make_uniq<GraphAlgorithmBindData>();
		bind->graph_name = graph_name;
		bind->vertex_label = vertex_label;
		state.nested_bind_data = std::move(bind);
		state.nested_global_state = make_uniq<ComponentState>();
	} else if (name == "triangle_count") {
		auto bind = make_uniq<GraphAlgorithmBindData>();
		bind->graph_name = graph_name;
		bind->vertex_label = vertex_label;
		state.nested_bind_data = std::move(bind);
		state.nested_global_state = make_uniq<TriangleCountState>();
	} else if (name == "lcc") {
		auto bind = make_uniq<GraphAlgorithmBindData>();
		bind->graph_name = graph_name;
		bind->vertex_label = vertex_label;
		state.nested_bind_data = std::move(bind);
		state.nested_global_state = make_uniq<LccState>();
	} else if (name == "louvain") {
		auto bind = make_uniq<LouvainBindData>();
		bind->graph_name = graph_name;
		bind->vertex_label = vertex_label;
		state.nested_bind_data = std::move(bind);
		state.nested_global_state = make_uniq<LouvainState>();
	} else if (name == "degree") {
		auto bind = make_uniq<GraphAlgorithmBindData>();
		bind->graph_name = graph_name;
		bind->vertex_label = vertex_label;
		state.nested_bind_data = std::move(bind);
		state.nested_global_state = make_uniq<DegreeState>();
	} else if (name == "closeness") {
		auto bind = make_uniq<ClosenessBindData>();
		bind->graph_name = graph_name;
		bind->vertex_label = vertex_label;
		state.nested_bind_data = std::move(bind);
		state.nested_global_state = make_uniq<ClosenessState>();
	} else {
		throw InternalException("Unimplemented GQL algorithm procedure node");
	}
	state.initialized = true;
}

static OperatorFinalizeResultType AlgorithmCallFinalize(ExecutionContext &context, TableFunctionInput &input,
                                                        DataChunk &output) {
	auto &data = input.bind_data->Cast<AlgorithmCallBindData>();
	auto &state = input.global_state->Cast<AlgorithmCallGlobalState>();
	if (!state.initialized) {
		InitializeAlgorithmCall(context, data, state);
	}
	if (data.definition->name == "shortest_path_length") {
		if (state.shortest_path_done) {
			return OperatorFinalizeResultType::FINISHED;
		}
		output.SetCardinality(1);
		output.SetValue(0, 0, Value::BIGINT(ComputeShortestPathLength(context.client, data, state)));
		state.shortest_path_done = true;
	} else if (data.definition->name == "bfs" || data.definition->name == "sssp") {
		TableFunctionInput nested(state.nested_bind_data.get(), nullptr, state.nested_global_state.get());
		BfsFunction(context.client, nested, output);
	} else if (data.definition->name == "dfs") {
		PipelineDfsFunction(context.client, state.nested_global_state->Cast<PipelineDfsState>(), output);
	} else {
		TableFunctionInput nested(state.nested_bind_data.get(), nullptr, state.nested_global_state.get());
		if (data.definition->name == "pagerank") {
			PageRankFunction(context.client, nested, output);
		} else if (data.definition->name == "wcc") {
			WccFunction(context.client, nested, output);
		} else if (data.definition->name == "scc") {
			SccFunction(context.client, nested, output);
		} else if (data.definition->name == "triangle_count") {
			TriangleCountFunction(context.client, nested, output);
		} else if (data.definition->name == "lcc") {
			LccFunction(context.client, nested, output);
		} else if (data.definition->name == "louvain") {
			LouvainFunction(context.client, nested, output);
		} else if (data.definition->name == "degree") {
			DegreeFunction(context.client, nested, output);
		} else if (data.definition->name == "closeness") {
			ClosenessFunction(context.client, nested, output);
		} else {
			throw InternalException("Unimplemented GQL algorithm procedure output");
		}
	}
	return output.size() == 0 ? OperatorFinalizeResultType::FINISHED : OperatorFinalizeResultType::HAVE_MORE_OUTPUT;
}

TableFunction GqlAlgorithmCallFunction() {
	TableFunction function("gql_algorithm_call",
	                       {LogicalType::TABLE, LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR),
	                        LogicalType::LIST(LogicalType::UTINYINT)},
	                       nullptr, AlgorithmCallBind, AlgorithmCallGlobalInit, AlgorithmCallLocalInit);
	function.in_out_function = AlgorithmCallInput;
	function.in_out_function_final = AlgorithmCallFinalize;
	return function;
}

TableFunction GqlAlgorithmResultFunction() {
	TableFunction function("gql_algorithm_result",
	                       {LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR),
	                        LogicalType::LIST(LogicalType::UTINYINT), LogicalType::LIST(LogicalType::VARCHAR),
	                        LogicalType::LIST(LogicalType::VARCHAR)},
	                       nullptr, nullptr);
	function.bind_replace = AlgorithmResultBindReplace;
	return function;
}

static void AddTraversalNamedParameters(TableFunction &function) {
	function.varargs = LogicalType::VARCHAR;
	function.named_parameters["direction"] = LogicalType::VARCHAR;
	function.named_parameters["max_depth"] = LogicalType::BIGINT;
	function.named_parameters["edge_label"] = LogicalType::VARCHAR;
	function.named_parameters["vertex_label"] = LogicalType::VARCHAR;
	function.named_parameters["target_vertex_id"] = LogicalType::BIGINT;
}

TableFunction GqlBfsFunction() {
	TableFunction function("bfs", {LogicalType::VARCHAR, LogicalType::BIGINT}, BfsFunction);
	function.bind = TraversalBind;
	function.init_global = BfsInit;
	AddTraversalNamedParameters(function);
	return function;
}

TableFunction GqlDfsFunction() {
	TableFunction function("dfs", {LogicalType::VARCHAR, LogicalType::BIGINT}, DfsFunction);
	function.bind = TraversalBind;
	function.init_global = DfsInit;
	AddTraversalNamedParameters(function);
	return function;
}

TableFunction GqlSsspFunction() {
	TableFunction function("sssp", {LogicalType::VARCHAR, LogicalType::BIGINT}, BfsFunction);
	function.bind = SsspBind;
	function.init_global = BfsInit;
	AddTraversalNamedParameters(function);
	return function;
}

TableFunction GqlPageRankFunction() {
	TableFunction function("pagerank", {LogicalType::VARCHAR}, PageRankFunction);
	function.bind = PageRankBind;
	function.init_global = PageRankInit;
	function.named_parameters["damping"] = LogicalType::DOUBLE;
	function.named_parameters["max_iterations"] = LogicalType::BIGINT;
	function.named_parameters["tolerance"] = LogicalType::DOUBLE;
	function.named_parameters["edge_label"] = LogicalType::VARCHAR;
	function.named_parameters["vertex_label"] = LogicalType::VARCHAR;
	function.varargs = LogicalType::VARCHAR;
	return function;
}

static void AddProjectionParameters(TableFunction &function) {
	function.varargs = LogicalType::VARCHAR;
	function.named_parameters["edge_label"] = LogicalType::VARCHAR;
	function.named_parameters["vertex_label"] = LogicalType::VARCHAR;
}

TableFunction GqlWccFunction() {
	TableFunction function("wcc", {LogicalType::VARCHAR}, WccFunction);
	function.bind = WccBind;
	function.init_global = ComponentInit;
	AddProjectionParameters(function);
	return function;
}

TableFunction GqlSccFunction() {
	TableFunction function("scc", {LogicalType::VARCHAR}, SccFunction);
	function.bind = SccBind;
	function.init_global = ComponentInit;
	AddProjectionParameters(function);
	return function;
}

TableFunction GqlTriangleCountFunction() {
	TableFunction function("triangle_count", {LogicalType::VARCHAR}, TriangleCountFunction);
	function.bind = TriangleCountBind;
	function.init_global = TriangleCountInit;
	AddProjectionParameters(function);
	return function;
}

TableFunction GqlLccFunction() {
	TableFunction function("lcc", {LogicalType::VARCHAR}, LccFunction);
	function.bind = LccBind;
	function.init_global = LccInit;
	AddProjectionParameters(function);
	return function;
}

TableFunction GqlLouvainFunction() {
	TableFunction function("louvain", {LogicalType::VARCHAR}, LouvainFunction);
	function.bind = LouvainBind;
	function.init_global = LouvainInit;
	function.named_parameters["resolution"] = LogicalType::DOUBLE;
	function.named_parameters["max_iterations"] = LogicalType::BIGINT;
	function.named_parameters["max_levels"] = LogicalType::BIGINT;
	function.named_parameters["tolerance"] = LogicalType::DOUBLE;
	AddProjectionParameters(function);
	return function;
}

TableFunction GqlDegreeFunction() {
	TableFunction function("degree", {LogicalType::VARCHAR}, DegreeFunction);
	function.bind = DegreeBind;
	function.init_global = DegreeInit;
	AddProjectionParameters(function);
	return function;
}

TableFunction GqlClosenessFunction() {
	TableFunction function("closeness", {LogicalType::VARCHAR}, ClosenessFunction);
	function.bind = ClosenessBind;
	function.init_global = ClosenessInit;
	function.named_parameters["direction"] = LogicalType::VARCHAR;
	AddProjectionParameters(function);
	return function;
}

} // namespace duckdb
