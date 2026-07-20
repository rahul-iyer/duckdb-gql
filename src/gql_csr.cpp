#include "gql_csr.hpp"

#include "gql_ir.hpp"
#include "gql_storage.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {

static constexpr const char *GQL_CSR_STATE_KEY = "gql_csr_state";

static LogicalType PropertyValueType() {
	return LogicalType::UNION({{"bool_value", LogicalType::BOOLEAN},
	                           {"int_value", LogicalType::BIGINT},
	                           {"uint_value", LogicalType::UBIGINT},
	                           {"decimal_value", LogicalType::DECIMAL(38, 18)},
	                           {"double_value", LogicalType::DOUBLE},
	                           {"string_value", LogicalType::VARCHAR},
	                           {"blob_value", LogicalType::BLOB},
	                           {"date_value", LogicalType::DATE},
	                           {"time_value", LogicalType::TIME},
	                           {"timestamp_value", LogicalType::TIMESTAMP},
	                           {"timestamptz_value", LogicalType::TIMESTAMP_TZ},
	                           {"interval_value", LogicalType::INTERVAL}});
}

static string QuoteLiteral(const string &value) {
	string result = "'";
	for (const auto character : value) {
		result += character == '\'' ? "''" : string(1, character);
	}
	return result + "'";
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
};

struct GqlCsrSnapshot {
	uint64_t graph_id;
	uint64_t graph_version;
	vector<uint64_t> vertex_ids;
	unordered_map<uint64_t, idx_t> ordinal_by_id;
	vector<idx_t> outgoing_offsets;
	vector<uint64_t> outgoing_neighbors;
	vector<uint64_t> outgoing_edge_ids;
	vector<idx_t> incoming_offsets;
	vector<uint64_t> incoming_neighbors;
	vector<uint64_t> incoming_edge_ids;
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

static GraphVersion ReadGraphVersion(Connection &connection, const string &graph_name) {
	auto result = connection.Query("SELECT graph_id, graph_version FROM "
	                               "gql_internal.graphs WHERE graph_name = " +
	                               QuoteLiteral(graph_name));
	ThrowOnError(*result);
	if (result->RowCount() == 0) {
		throw InvalidInputException("Graph '%s' does not exist", graph_name);
	}
	return {result->GetValue(0, 0).GetValue<uint64_t>(), result->GetValue(1, 0).GetValue<uint64_t>()};
}

static vector<idx_t> BuildOffsets(idx_t vertex_count, const vector<CsrEdge> &edges, bool outgoing) {
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

static shared_ptr<GqlCsrSnapshot> BuildSnapshot(Connection &connection, const GraphVersion &graph) {
	auto snapshot = make_shared_ptr<GqlCsrSnapshot>();
	snapshot->graph_id = graph.graph_id;
	snapshot->graph_version = graph.graph_version;

	auto vertices =
	    connection.Query("SELECT object_id FROM gql_internal.objects WHERE graph_id = " + to_string(graph.graph_id) +
	                     " AND kind = 0 ORDER BY object_id");
	ThrowOnError(*vertices);
	for (idx_t row = 0; row < vertices->RowCount(); row++) {
		auto vertex_id = vertices->GetValue(0, row).GetValue<uint64_t>();
		snapshot->ordinal_by_id.emplace(vertex_id, snapshot->vertex_ids.size());
		snapshot->vertex_ids.push_back(vertex_id);
	}

	vector<CsrEdge> edges;
	auto edge_rows = connection.Query("SELECT object_id, source_id, target_id FROM gql_internal.objects WHERE "
	                                  "graph_id = " +
	                                  to_string(graph.graph_id) + " AND kind = 1");
	ThrowOnError(*edge_rows);
	for (idx_t row = 0; row < edge_rows->RowCount(); row++) {
		auto edge_id = edge_rows->GetValue(0, row).GetValue<uint64_t>();
		auto source_id = edge_rows->GetValue(1, row).GetValue<uint64_t>();
		auto target_id = edge_rows->GetValue(2, row).GetValue<uint64_t>();
		auto source = snapshot->ordinal_by_id.find(source_id);
		auto target = snapshot->ordinal_by_id.find(target_id);
		if (source == snapshot->ordinal_by_id.end() || target == snapshot->ordinal_by_id.end()) {
			throw InvalidInputException("Graph contains edge %llu with an invalid endpoint",
			                            static_cast<unsigned long long>(edge_id));
		}
		edges.push_back({edge_id, source->second, target->second});
	}

	auto outgoing = edges;
	std::sort(outgoing.begin(), outgoing.end(), [](const CsrEdge &left, const CsrEdge &right) {
		return left.source < right.source ||
		       (left.source == right.source &&
		        (left.target < right.target || (left.target == right.target && left.edge_id < right.edge_id)));
	});
	snapshot->outgoing_offsets = BuildOffsets(snapshot->vertex_ids.size(), outgoing, true);
	for (const auto &edge : outgoing) {
		snapshot->outgoing_neighbors.push_back(snapshot->vertex_ids[edge.target]);
		snapshot->outgoing_edge_ids.push_back(edge.edge_id);
	}

	auto incoming = edges;
	std::sort(incoming.begin(), incoming.end(), [](const CsrEdge &left, const CsrEdge &right) {
		return left.target < right.target ||
		       (left.target == right.target &&
		        (left.source < right.source || (left.source == right.source && left.edge_id < right.edge_id)));
	});
	snapshot->incoming_offsets = BuildOffsets(snapshot->vertex_ids.size(), incoming, false);
	for (const auto &edge : incoming) {
		snapshot->incoming_neighbors.push_back(snapshot->vertex_ids[edge.source]);
		snapshot->incoming_edge_ids.push_back(edge.edge_id);
	}

	snapshot->memory_bytes =
	    snapshot->vertex_ids.size() * sizeof(uint64_t) + snapshot->outgoing_offsets.size() * sizeof(idx_t) +
	    snapshot->outgoing_neighbors.size() * sizeof(uint64_t) + snapshot->outgoing_edge_ids.size() * sizeof(uint64_t) +
	    snapshot->incoming_offsets.size() * sizeof(idx_t) + snapshot->incoming_neighbors.size() * sizeof(uint64_t) +
	    snapshot->incoming_edge_ids.size() * sizeof(uint64_t);
	return snapshot;
}

static shared_ptr<GqlCsrSnapshot> GetSnapshot(ClientContext &context, const string &graph_name,
                                              GqlCsrCacheState &cache) {
	Connection connection(*context.db);
	auto graph = ReadGraphVersion(connection, graph_name);
	auto entry = cache.snapshots.find(graph.graph_id);
	if (entry != cache.snapshots.end() && entry->second->graph_version == graph.graph_version) {
		return entry->second;
	}
	auto snapshot = BuildSnapshot(connection, graph);
	cache.snapshots[graph.graph_id] = snapshot;
	cache.build_count++;
	return snapshot;
}

struct CsrBindData : TableFunctionData {
	string graph_name;
	uint64_t vertex_id = 0;
	string direction;
};

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
		auto cache = context.registered_state->GetOrCreate<GqlCsrCacheState>(GQL_CSR_STATE_KEY);
		auto snapshot = GetSnapshot(context, data.graph_name, *cache);
		auto vertex = snapshot->ordinal_by_id.find(data.vertex_id);
		if (vertex != snapshot->ordinal_by_id.end()) {
			const auto &offsets = data.direction == "out" ? snapshot->outgoing_offsets : snapshot->incoming_offsets;
			const auto &neighbors =
			    data.direction == "out" ? snapshot->outgoing_neighbors : snapshot->incoming_neighbors;
			const auto &edge_ids = data.direction == "out" ? snapshot->outgoing_edge_ids : snapshot->incoming_edge_ids;
			for (idx_t index = offsets[vertex->second]; index < offsets[vertex->second + 1]; index++) {
				state.rows.emplace_back(neighbors[index], edge_ids[index]);
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

static unique_ptr<FunctionData> CsrStatsBind(ClientContext &, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<CsrBindData>();
	result->graph_name = input.inputs[0].GetValue<string>();
	names = {"graph_name", "graph_version", "vertex_count", "edge_count", "memory_bytes", "build_count", "cached"};
	return_types = {LogicalType::VARCHAR, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT,
	                LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::BOOLEAN};
	return std::move(result);
}

struct SingleRowState : GlobalTableFunctionState {
	bool done = false;
};

static unique_ptr<GlobalTableFunctionState> SingleRowInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<SingleRowState>();
}

static void CsrStatsFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<SingleRowState>();
	if (state.done) {
		return;
	}
	auto &data = input.bind_data->Cast<CsrBindData>();
	auto cache = context.registered_state->GetOrCreate<GqlCsrCacheState>(GQL_CSR_STATE_KEY);
	auto snapshot = GetSnapshot(context, data.graph_name, *cache);
	output.SetCardinality(1);
	output.SetValue(0, 0, Value(data.graph_name));
	output.SetValue(1, 0, Value::UBIGINT(snapshot->graph_version));
	output.SetValue(2, 0, Value::UBIGINT(snapshot->vertex_ids.size()));
	output.SetValue(3, 0, Value::UBIGINT(snapshot->outgoing_edge_ids.size()));
	output.SetValue(4, 0, Value::UBIGINT(snapshot->memory_bytes));
	output.SetValue(5, 0, Value::UBIGINT(cache->build_count));
	output.SetValue(6, 0, Value(true));
	state.done = true;
}

struct MatchBindData : TableFunctionData {
	vector<string> labels;
	bool has_edge;
	bool reverse;
	vector<GqlExpressionProgram> projections;
	vector<GqlExpressionProgram> predicates;
};

static vector<string> ReadStringList(const Value &value) {
	vector<string> result;
	for (const auto &entry : ListValue::GetChildren(value)) {
		result.push_back(entry.GetValue<string>());
	}
	return result;
}

static unique_ptr<FunctionData> MatchBind(ClientContext &, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<MatchBindData>();
	result->labels = ReadStringList(input.inputs[0]);
	result->has_edge = input.inputs[1].GetValue<bool>();
	result->reverse = input.inputs[2].GetValue<bool>();
	auto projection_names = ReadStringList(input.inputs[4]);
	for (const auto &program : ListValue::GetChildren(input.inputs[3])) {
		result->projections.push_back(GqlDeserializeExpression(program));
	}
	for (const auto &program : ListValue::GetChildren(input.inputs[5])) {
		result->predicates.push_back(GqlDeserializeExpression(program));
	}
	if (result->projections.size() != projection_names.size() || projection_names.empty()) {
		throw BinderException("Invalid GQL MATCH projections");
	}
	auto variable_count = result->has_edge ? 3 : 1;
	if (result->labels.size() != variable_count) {
		throw BinderException("Invalid GQL MATCH labels");
	}
	for (const auto &program : result->projections) {
		for (const auto index : program.binding_indices) {
			if (index != NumericLimits<uint64_t>::Maximum() && index >= variable_count) {
				throw BinderException("Invalid GQL MATCH projection variable");
			}
		}
	}
	for (const auto &program : result->predicates) {
		for (const auto index : program.binding_indices) {
			if (index != NumericLimits<uint64_t>::Maximum() && index >= variable_count) {
				throw BinderException("Invalid GQL MATCH predicate variable");
			}
		}
		auto predicate_type = static_cast<GqlTypeId>(program.result_types[0]);
		if (predicate_type != GqlTypeId::BOOLEAN && predicate_type != GqlTypeId::PROPERTY_VALUE) {
			throw BinderException("Invalid GQL MATCH predicate type");
		}
	}
	names = std::move(projection_names);
	for (const auto &program : result->projections) {
		return_types.push_back(GqlDuckType({static_cast<GqlTypeId>(program.result_types[0]), true}));
	}
	return std::move(result);
}

struct MatchRowsState : GlobalTableFunctionState {
	bool initialized = false;
	idx_t offset = 0;
	vector<vector<Value>> rows;
};

static unique_ptr<GlobalTableFunctionState> MatchInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<MatchRowsState>();
}

static unordered_set<uint64_t> ReadLabelObjects(Connection &connection, uint64_t graph_id, const string &label) {
	unordered_set<uint64_t> result;
	if (label.empty()) {
		return result;
	}
	auto rows = connection.Query("SELECT ol.object_id FROM gql_internal.object_labels ol JOIN "
	                             "gql_internal.labels l USING (graph_id, label_id) "
	                             "WHERE ol.graph_id = " +
	                             to_string(graph_id) + " AND l.label_name = " + QuoteLiteral(label));
	ThrowOnError(*rows);
	for (idx_t row = 0; row < rows->RowCount(); row++) {
		result.insert(rows->GetValue(0, row).GetValue<uint64_t>());
	}
	return result;
}

static bool MatchesLabel(uint64_t object_id, const string &label, const unordered_set<uint64_t> &objects) {
	return label.empty() || objects.find(object_id) != objects.end();
}

using ObjectProperties = unordered_map<uint64_t, unordered_map<string, Value>>;

static ObjectProperties ReadProperties(Connection &connection, uint64_t graph_id) {
	ObjectProperties result;
	auto rows = connection.Query("SELECT p.object_id, k.key_name, p.value FROM "
	                             "gql_internal.object_properties p "
	                             "JOIN gql_internal.property_keys k USING "
	                             "(graph_id, key_id) WHERE p.graph_id = " +
	                             to_string(graph_id));
	ThrowOnError(*rows);
	for (idx_t row = 0; row < rows->RowCount(); row++) {
		result[rows->GetValue(0, row).GetValue<uint64_t>()].emplace(rows->GetValue(1, row).GetValue<string>(),
		                                                            rows->GetValue(2, row));
	}
	return result;
}

static Value ScalarValue(const Value &value) {
	if (value.IsNull()) {
		return Value();
	}
	return value.type().id() == LogicalTypeId::UNION ? UnionValue::GetValue(value) : value;
}

static Value LiteralValue(GqlLiteralType type, const string &text) {
	switch (type) {
	case GqlLiteralType::NULL_VALUE:
		return Value();
	case GqlLiteralType::BOOLEAN:
		return Value::BOOLEAN(text == "true");
	case GqlLiteralType::INTEGER:
		return Value::BIGINT(std::stoll(text));
	case GqlLiteralType::DECIMAL:
		return Value(text).DefaultCastAs(LogicalType::DECIMAL(38, 18));
	case GqlLiteralType::DOUBLE:
		return Value::DOUBLE(std::stod(text));
	case GqlLiteralType::STRING:
		return Value(text);
	}
	throw InternalException("Unknown GQL literal program type");
}

static Value BooleanValue(const Value &value) {
	auto scalar = ScalarValue(value);
	if (scalar.IsNull()) {
		return Value(LogicalType::BOOLEAN);
	}
	if (scalar.type().id() != LogicalTypeId::BOOLEAN) {
		throw InvalidInputException("GQL predicate expected BOOLEAN, found %s", scalar.type().ToString());
	}
	return scalar;
}

static Value EvaluateProgramNode(const GqlExpressionProgram &program, idx_t &cursor, const uint64_t *bindings,
                                 const ObjectProperties &properties) {
	if (cursor >= program.node_types.size()) {
		throw InternalException("Truncated GQL expression program");
	}
	auto node = cursor++;
	auto expression_type = static_cast<GqlExpressionType>(program.node_types[node]);
	auto result_type = static_cast<GqlTypeId>(program.result_types[node]);
	auto operation = program.operators[node];
	switch (expression_type) {
	case GqlExpressionType::LITERAL:
		return LiteralValue(static_cast<GqlLiteralType>(operation), program.values[node]);
	case GqlExpressionType::VARIABLE_REFERENCE:
		return Value::UBIGINT(bindings[program.binding_indices[node]]);
	case GqlExpressionType::PROPERTY_REFERENCE: {
		auto receiver = EvaluateProgramNode(program, cursor, bindings, properties);
		if (receiver.IsNull()) {
			return Value(PropertyValueType());
		}
		auto object_id = receiver.GetValue<uint64_t>();
		auto object = properties.find(object_id);
		if (object == properties.end()) {
			return Value(PropertyValueType());
		}
		auto property = object->second.find(program.properties[node]);
		return property == object->second.end() ? Value(PropertyValueType()) : property->second;
	}
	case GqlExpressionType::ELEMENT_ID:
		return EvaluateProgramNode(program, cursor, bindings, properties);
	case GqlExpressionType::FUNCTION:
		throw NotImplementedException("GQL functions in the legacy CSR expression evaluator");
	case GqlExpressionType::UNARY: {
		auto input = EvaluateProgramNode(program, cursor, bindings, properties);
		auto unary = static_cast<GqlUnaryOperator>(operation);
		if (unary == GqlUnaryOperator::NOT) {
			auto boolean = BooleanValue(input);
			return boolean.IsNull() ? boolean : Value::BOOLEAN(!boolean.GetValue<bool>());
		}
		auto scalar = ScalarValue(input);
		if (scalar.IsNull() || unary == GqlUnaryOperator::PLUS) {
			return scalar;
		}
		if (result_type == GqlTypeId::INTEGER) {
			return Value::BIGINT(-scalar.DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>());
		}
		return Value::DOUBLE(-scalar.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>());
	}
	case GqlExpressionType::IS_NULL: {
		auto input = EvaluateProgramNode(program, cursor, bindings, properties);
		auto is_null = input.IsNull();
		return Value::BOOLEAN(operation ? !is_null : is_null);
	}
	case GqlExpressionType::BINARY: {
		auto left = EvaluateProgramNode(program, cursor, bindings, properties);
		auto right = EvaluateProgramNode(program, cursor, bindings, properties);
		auto binary = static_cast<GqlBinaryOperator>(operation);
		if (binary == GqlBinaryOperator::AND || binary == GqlBinaryOperator::OR || binary == GqlBinaryOperator::XOR) {
			auto left_boolean = BooleanValue(left);
			auto right_boolean = BooleanValue(right);
			if (binary == GqlBinaryOperator::AND) {
				if ((!left_boolean.IsNull() && !left_boolean.GetValue<bool>()) ||
				    (!right_boolean.IsNull() && !right_boolean.GetValue<bool>())) {
					return Value::BOOLEAN(false);
				}
				return left_boolean.IsNull() || right_boolean.IsNull() ? Value(LogicalType::BOOLEAN)
				                                                       : Value::BOOLEAN(true);
			}
			if (binary == GqlBinaryOperator::OR) {
				if ((!left_boolean.IsNull() && left_boolean.GetValue<bool>()) ||
				    (!right_boolean.IsNull() && right_boolean.GetValue<bool>())) {
					return Value::BOOLEAN(true);
				}
				return left_boolean.IsNull() || right_boolean.IsNull() ? Value(LogicalType::BOOLEAN)
				                                                       : Value::BOOLEAN(false);
			}
			return left_boolean.IsNull() || right_boolean.IsNull()
			           ? Value(LogicalType::BOOLEAN)
			           : Value::BOOLEAN(left_boolean.GetValue<bool>() != right_boolean.GetValue<bool>());
		}
		auto left_scalar = ScalarValue(left);
		auto right_scalar = ScalarValue(right);
		if (left_scalar.IsNull() || right_scalar.IsNull()) {
			return Value(GqlDuckType({result_type, true}));
		}
		if (binary == GqlBinaryOperator::CONCATENATE) {
			return Value(left_scalar.DefaultCastAs(LogicalType::VARCHAR).GetValue<string>() +
			             right_scalar.DefaultCastAs(LogicalType::VARCHAR).GetValue<string>());
		}
		switch (binary) {
		case GqlBinaryOperator::EQUAL:
			return Value::BOOLEAN(ValueOperations::Equals(left_scalar, right_scalar));
		case GqlBinaryOperator::NOT_EQUAL:
			return Value::BOOLEAN(ValueOperations::NotEquals(left_scalar, right_scalar));
		case GqlBinaryOperator::LESS_THAN:
			return Value::BOOLEAN(ValueOperations::LessThan(left_scalar, right_scalar));
		case GqlBinaryOperator::GREATER_THAN:
			return Value::BOOLEAN(ValueOperations::GreaterThan(left_scalar, right_scalar));
		case GqlBinaryOperator::LESS_THAN_OR_EQUAL:
			return Value::BOOLEAN(ValueOperations::LessThanEquals(left_scalar, right_scalar));
		case GqlBinaryOperator::GREATER_THAN_OR_EQUAL:
			return Value::BOOLEAN(ValueOperations::GreaterThanEquals(left_scalar, right_scalar));
		case GqlBinaryOperator::MULTIPLY:
		case GqlBinaryOperator::DIVIDE:
		case GqlBinaryOperator::ADD:
		case GqlBinaryOperator::SUBTRACT:
		case GqlBinaryOperator::CONCATENATE:
			break;
		case GqlBinaryOperator::AND:
		case GqlBinaryOperator::OR:
		case GqlBinaryOperator::XOR:
			throw InternalException("Boolean GQL expression reached numeric evaluation");
		}
		if (result_type == GqlTypeId::INTEGER) {
			auto left_value = left_scalar.DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
			auto right_value = right_scalar.DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
			switch (binary) {
			case GqlBinaryOperator::MULTIPLY:
				return Value::BIGINT(left_value * right_value);
			case GqlBinaryOperator::DIVIDE:
				if (right_value == 0) {
					throw InvalidInputException("Division by zero in GQL expression");
				}
				return Value::BIGINT(left_value / right_value);
			case GqlBinaryOperator::ADD:
				return Value::BIGINT(left_value + right_value);
			case GqlBinaryOperator::SUBTRACT:
				return Value::BIGINT(left_value - right_value);
			default:
				break;
			}
		}
		auto left_value = left_scalar.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
		auto right_value = right_scalar.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
		switch (binary) {
		case GqlBinaryOperator::MULTIPLY:
			return Value::DOUBLE(left_value * right_value);
		case GqlBinaryOperator::DIVIDE:
			if (right_value == 0) {
				throw InvalidInputException("Division by zero in GQL expression");
			}
			return Value::DOUBLE(left_value / right_value);
		case GqlBinaryOperator::ADD:
			return Value::DOUBLE(left_value + right_value);
		case GqlBinaryOperator::SUBTRACT:
			return Value::DOUBLE(left_value - right_value);
		default:
			break;
		}
		throw InternalException("Unknown GQL binary expression");
	}
	}
	throw InternalException("Unknown GQL expression program node");
}

static Value EvaluateProgram(const GqlExpressionProgram &program, const uint64_t *bindings,
                             const ObjectProperties &properties) {
	idx_t cursor = 0;
	auto result = EvaluateProgramNode(program, cursor, bindings, properties);
	if (cursor != program.node_types.size()) {
		throw InternalException("GQL expression program has trailing nodes");
	}
	return result;
}

static bool MatchesPredicates(const uint64_t *bindings, const MatchBindData &data, const ObjectProperties &properties) {
	for (const auto &predicate : data.predicates) {
		auto result = BooleanValue(EvaluateProgram(predicate, bindings, properties));
		if (result.IsNull() || !result.GetValue<bool>()) {
			return false;
		}
	}
	return true;
}

static vector<Value> ProjectBindings(const uint64_t *bindings, const MatchBindData &data,
                                     const ObjectProperties &properties) {
	vector<Value> result;
	for (const auto &projection : data.projections) {
		result.push_back(EvaluateProgram(projection, bindings, properties));
	}
	return result;
}

static void MatchFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<MatchRowsState>();
	if (!state.initialized) {
		auto graph_name = GqlGetSelectedGraph(context);
		if (graph_name.empty()) {
			throw InvalidInputException("No graph selected; use SESSION SET GRAPH before MATCH");
		}
		auto &data = input.bind_data->Cast<MatchBindData>();
		auto cache = context.registered_state->GetOrCreate<GqlCsrCacheState>(GQL_CSR_STATE_KEY);
		auto snapshot = GetSnapshot(context, graph_name, *cache);
		Connection connection(*context.db);
		auto source_labels = ReadLabelObjects(connection, snapshot->graph_id, data.labels[0]);
		auto properties = ReadProperties(connection, snapshot->graph_id);
		if (!data.has_edge) {
			for (const auto vertex_id : snapshot->vertex_ids) {
				if (!MatchesLabel(vertex_id, data.labels[0], source_labels)) {
					continue;
				}
				uint64_t bindings[] = {vertex_id};
				if (MatchesPredicates(bindings, data, properties)) {
					state.rows.push_back(ProjectBindings(bindings, data, properties));
				}
			}
		} else if (!data.reverse) {
			auto edge_labels = ReadLabelObjects(connection, snapshot->graph_id, data.labels[1]);
			auto target_labels = ReadLabelObjects(connection, snapshot->graph_id, data.labels[2]);
			for (idx_t source = 0; source < snapshot->vertex_ids.size(); source++) {
				auto source_id = snapshot->vertex_ids[source];
				if (!MatchesLabel(source_id, data.labels[0], source_labels)) {
					continue;
				}
				for (idx_t offset = snapshot->outgoing_offsets[source]; offset < snapshot->outgoing_offsets[source + 1];
				     offset++) {
					auto edge_id = snapshot->outgoing_edge_ids[offset];
					auto target_id = snapshot->outgoing_neighbors[offset];
					if (!MatchesLabel(edge_id, data.labels[1], edge_labels) ||
					    !MatchesLabel(target_id, data.labels[2], target_labels)) {
						continue;
					}
					uint64_t bindings[] = {source_id, edge_id, target_id};
					if (MatchesPredicates(bindings, data, properties)) {
						state.rows.push_back(ProjectBindings(bindings, data, properties));
					}
				}
			}
		} else {
			auto edge_labels = ReadLabelObjects(connection, snapshot->graph_id, data.labels[1]);
			auto right_labels = ReadLabelObjects(connection, snapshot->graph_id, data.labels[2]);
			for (idx_t target = 0; target < snapshot->vertex_ids.size(); target++) {
				auto target_id = snapshot->vertex_ids[target];
				if (!MatchesLabel(target_id, data.labels[0], source_labels)) {
					continue;
				}
				for (idx_t offset = snapshot->incoming_offsets[target]; offset < snapshot->incoming_offsets[target + 1];
				     offset++) {
					auto edge_id = snapshot->incoming_edge_ids[offset];
					auto source_id = snapshot->incoming_neighbors[offset];
					if (!MatchesLabel(edge_id, data.labels[1], edge_labels) ||
					    !MatchesLabel(source_id, data.labels[2], right_labels)) {
						continue;
					}
					uint64_t bindings[] = {target_id, edge_id, source_id};
					if (MatchesPredicates(bindings, data, properties)) {
						state.rows.push_back(ProjectBindings(bindings, data, properties));
					}
				}
			}
		}
		state.initialized = true;
	}
	auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	for (idx_t row = 0; row < count; row++) {
		for (idx_t column = 0; column < state.rows[state.offset + row].size(); column++) {
			output.SetValue(column, row, state.rows[state.offset + row][column]);
		}
	}
	state.offset += count;
	output.SetCardinality(count);
}

TableFunction GqlNeighborsFunction() {
	TableFunction function("gql_neighbors", {LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR},
	                       NeighborsFunction);
	function.bind = NeighborsBind;
	function.init_global = NeighborsInit;
	return function;
}

TableFunction GqlCsrStatsFunction() {
	TableFunction function("gql_csr_stats", {LogicalType::VARCHAR}, CsrStatsFunction);
	function.bind = CsrStatsBind;
	function.init_global = SingleRowInit;
	return function;
}

TableFunction GqlMatchFunction() {
	TableFunction function("gql_match",
	                       {LogicalType::LIST(LogicalType::VARCHAR), LogicalType::BOOLEAN, LogicalType::BOOLEAN,
	                        LogicalType::LIST(GqlExpressionProgramType()), LogicalType::LIST(LogicalType::VARCHAR),
	                        LogicalType::LIST(GqlExpressionProgramType())},
	                       MatchFunction);
	function.bind = MatchBind;
	function.init_global = MatchInit;
	return function;
}

} // namespace duckdb
