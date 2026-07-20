#include "gql_storage.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace duckdb {

static constexpr const char *GQL_STATE_KEY = "gql_client_state";

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

struct GqlClientState : ClientContextState {
	void QueryEnd(ClientContext &context, optional_ptr<ErrorData> error) override {
		if (!error || !error->HasError() || !mutation_owned_transaction) {
			return;
		}
		mutation_owned_transaction = false;
		mutation_command_id.clear();
		if (context.transaction.HasActiveTransaction()) {
			context.transaction.Rollback(error);
		}
	}

	string graph_name;
	string mutation_command_id;
	bool mutation_owned_transaction = false;
};

string GqlGetSelectedGraph(ClientContext &context) {
	auto state = context.registered_state->GetOrCreate<GqlClientState>(GQL_STATE_KEY);
	return state->graph_name;
}

static string QuoteLiteral(const string &value) {
	string result = "'";
	for (const auto character : value) {
		if (character == '\'') {
			result += "''";
		} else {
			result += character;
		}
	}
	result += "'";
	return result;
}

static void ThrowOnError(const MaterializedQueryResult &result) {
	if (result.HasError()) {
		throw InvalidInputException("GQL internal storage error: %s", result.GetError());
	}
}

static void EnsureStorage(Connection &connection) {
	auto result = connection.Query("CREATE SCHEMA IF NOT EXISTS gql_internal");
	ThrowOnError(*result);
	result = connection.Query("CREATE SEQUENCE IF NOT EXISTS gql_internal.graph_id_seq START 1");
	ThrowOnError(*result);
	result = connection.Query("CREATE SEQUENCE IF NOT EXISTS gql_internal.object_id_seq START 1");
	ThrowOnError(*result);
	result = connection.Query("CREATE SEQUENCE IF NOT EXISTS gql_internal.label_id_seq START 1");
	ThrowOnError(*result);
	result = connection.Query("CREATE SEQUENCE IF NOT EXISTS gql_internal.property_key_id_seq START 1");
	ThrowOnError(*result);
	result = connection.Query("CREATE TYPE IF NOT EXISTS gql_internal.property_value AS UNION("
	                          "bool_value BOOLEAN, int_value BIGINT, uint_value UBIGINT, "
	                          "decimal_value DECIMAL(38,18), double_value DOUBLE, string_value "
	                          "VARCHAR, "
	                          "blob_value BLOB, date_value DATE, time_value TIME, timestamp_value "
	                          "TIMESTAMP, "
	                          "timestamptz_value TIMESTAMPTZ, interval_value INTERVAL)");
	ThrowOnError(*result);
	result = connection.Query("CREATE TABLE IF NOT EXISTS gql_internal.graphs ("
	                          "graph_id UBIGINT PRIMARY KEY DEFAULT "
	                          "nextval('gql_internal.graph_id_seq'), "
	                          "graph_name VARCHAR NOT NULL UNIQUE, "
	                          "graph_version UBIGINT NOT NULL DEFAULT 0, "
	                          "created_at TIMESTAMP NOT NULL DEFAULT current_timestamp)");
	ThrowOnError(*result);
	result = connection.Query("CREATE TABLE IF NOT EXISTS gql_internal.objects ("
	                          "object_id UBIGINT PRIMARY KEY DEFAULT "
	                          "nextval('gql_internal.object_id_seq'), "
	                          "graph_id UBIGINT NOT NULL, "
	                          "kind UTINYINT NOT NULL, "
	                          "source_id UBIGINT, "
	                          "target_id UBIGINT, "
	                          "CHECK ((kind = 0 AND source_id IS NULL AND target_id IS NULL) OR "
	                          "       (kind = 1 AND source_id IS NOT NULL AND target_id IS NOT "
	                          "NULL)))");
	ThrowOnError(*result);
	result = connection.Query("CREATE TABLE IF NOT EXISTS gql_internal.labels ("
	                          "label_id UBIGINT PRIMARY KEY DEFAULT "
	                          "nextval('gql_internal.label_id_seq'), "
	                          "graph_id UBIGINT NOT NULL, label_name VARCHAR NOT "
	                          "NULL, UNIQUE(graph_id, label_name))");
	ThrowOnError(*result);
	result = connection.Query("CREATE TABLE IF NOT EXISTS gql_internal.object_labels ("
	                          "graph_id UBIGINT NOT NULL, object_id UBIGINT NOT NULL, "
	                          "label_id UBIGINT NOT NULL, "
	                          "PRIMARY KEY(object_id, label_id))");
	ThrowOnError(*result);
	result = connection.Query("CREATE TABLE IF NOT EXISTS gql_internal.property_keys ("
	                          "key_id UBIGINT PRIMARY KEY DEFAULT "
	                          "nextval('gql_internal.property_key_id_seq'), "
	                          "graph_id UBIGINT NOT NULL, key_name VARCHAR NOT NULL, "
	                          "UNIQUE(graph_id, key_name))");
	ThrowOnError(*result);
	result = connection.Query("CREATE TABLE IF NOT EXISTS gql_internal.object_properties ("
	                          "graph_id UBIGINT NOT NULL, object_id UBIGINT NOT NULL, key_id UBIGINT "
	                          "NOT NULL, "
	                          "value gql_internal.property_value NOT NULL, PRIMARY KEY(object_id, "
	                          "key_id))");
	ThrowOnError(*result);
}

static bool GraphExists(Connection &connection, const string &graph_name) {
	auto result =
	    connection.Query("SELECT count(*) FROM gql_internal.graphs WHERE graph_name = " + QuoteLiteral(graph_name));
	ThrowOnError(*result);
	return result->GetValue(0, 0).GetValue<int64_t>() != 0;
}

struct CommandBindData : TableFunctionData {
	CommandBindData(string graph_name_p, bool conditional_p)
	    : graph_name(std::move(graph_name_p)), conditional(conditional_p) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<CommandBindData>(graph_name, conditional);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto other = dynamic_cast<const CommandBindData *>(&other_p);
		return other && graph_name == other->graph_name && conditional == other->conditional;
	}

	string graph_name;
	bool conditional;
};

struct SingleRowState : GlobalTableFunctionState {
	bool done = false;
};

struct MutationControlBindData : TableFunctionData {
	MutationControlBindData(string command_id_p, bool begin_p) : command_id(std::move(command_id_p)), begin(begin_p) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<MutationControlBindData>(command_id, begin);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto other = dynamic_cast<const MutationControlBindData *>(&other_p);
		return other && command_id == other->command_id && begin == other->begin;
	}

	string command_id;
	bool begin;
};

static unique_ptr<GlobalTableFunctionState> SingleRowInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<SingleRowState>();
}

static unique_ptr<FunctionData> CommandBind(ClientContext &, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("A graph name is required");
	}
	auto graph_name = input.inputs[0].GetValue<string>();
	bool conditional = input.inputs.size() > 1 && !input.inputs[1].IsNull() && input.inputs[1].GetValue<bool>();
	names.emplace_back("success");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("graph_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	return make_uniq<CommandBindData>(std::move(graph_name), conditional);
}

static unique_ptr<FunctionData> SetGraphBind(ClientContext &, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("A graph name is required");
	}
	names.emplace_back("success");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("graph_name");
	return_types.emplace_back(LogicalType::VARCHAR);
	return make_uniq<CommandBindData>(input.inputs[0].GetValue<string>(), false);
}

static unique_ptr<FunctionData> MutationControlBind(ClientContext &, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 2 || input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("GQL mutation control requires a command id and phase");
	}
	names.emplace_back("success");
	return_types.emplace_back(LogicalType::BOOLEAN);
	return make_uniq<MutationControlBindData>(input.inputs[0].GetValue<string>(), input.inputs[1].GetValue<bool>());
}

static void MutationControl(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &row_state = input.global_state->Cast<SingleRowState>();
	if (row_state.done) {
		return;
	}
	auto &data = input.bind_data->Cast<MutationControlBindData>();
	auto state = context.registered_state->GetOrCreate<GqlClientState>(GQL_STATE_KEY);
	if (data.begin) {
		if (!state->mutation_command_id.empty()) {
			throw InternalException("A GQL mutation command is already active");
		}
		state->mutation_command_id = data.command_id;
		state->mutation_owned_transaction = context.transaction.IsAutoCommit();
		if (state->mutation_owned_transaction) {
			// BeginQueryInternal already opened the autocommit transaction. Keeping
			// auto-commit disabled carries it across every generated DML statement.
			context.transaction.SetAutoCommit(false);
		}
	} else {
		if (state->mutation_command_id != data.command_id) {
			throw InternalException("GQL mutation command envelope mismatch");
		}
		auto owned_transaction = state->mutation_owned_transaction;
		state->mutation_owned_transaction = false;
		state->mutation_command_id.clear();
		if (owned_transaction) {
			// EndQueryInternal commits the active transaction after this table
			// function finishes, once auto-commit is restored.
			context.transaction.SetAutoCommit(true);
		}
	}
	output.SetCardinality(1);
	output.SetValue(0, 0, Value(true));
	row_state.done = true;
}

static void EmitCommandResult(DataChunk &output, SingleRowState &state, const string &graph_name) {
	output.SetCardinality(1);
	output.SetValue(0, 0, Value(true));
	output.SetValue(1, 0, Value(graph_name));
	state.done = true;
}

static void CreateGraph(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<SingleRowState>();
	if (state.done) {
		return;
	}
	auto &data = input.bind_data->Cast<CommandBindData>();
	Connection connection(*context.db);
	EnsureStorage(connection);
	if (GraphExists(connection, data.graph_name)) {
		if (!data.conditional) {
			throw InvalidInputException("Graph '%s' already exists", data.graph_name);
		}
		EmitCommandResult(output, state, data.graph_name);
		return;
	}
	auto result =
	    connection.Query("INSERT INTO gql_internal.graphs (graph_name) VALUES (" + QuoteLiteral(data.graph_name) + ")");
	ThrowOnError(*result);
	EmitCommandResult(output, state, data.graph_name);
}

static void DropGraph(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<SingleRowState>();
	if (state.done) {
		return;
	}
	auto &data = input.bind_data->Cast<CommandBindData>();
	Connection connection(*context.db);
	EnsureStorage(connection);
	if (!GraphExists(connection, data.graph_name)) {
		if (!data.conditional) {
			throw InvalidInputException("Graph '%s' does not exist", data.graph_name);
		}
		EmitCommandResult(output, state, data.graph_name);
		return;
	}
	auto result = connection.Query("DELETE FROM gql_internal.object_properties WHERE graph_id = "
	                               "(SELECT graph_id FROM gql_internal.graphs WHERE graph_name = " +
	                               QuoteLiteral(data.graph_name) + ")");
	ThrowOnError(*result);
	result = connection.Query("DELETE FROM gql_internal.object_labels WHERE graph_id = "
	                          "(SELECT graph_id FROM gql_internal.graphs WHERE graph_name = " +
	                          QuoteLiteral(data.graph_name) + ")");
	ThrowOnError(*result);
	result = connection.Query("DELETE FROM gql_internal.property_keys WHERE graph_id = "
	                          "(SELECT graph_id FROM gql_internal.graphs WHERE graph_name = " +
	                          QuoteLiteral(data.graph_name) + ")");
	ThrowOnError(*result);
	result = connection.Query("DELETE FROM gql_internal.labels WHERE graph_id = "
	                          "(SELECT graph_id FROM gql_internal.graphs WHERE graph_name = " +
	                          QuoteLiteral(data.graph_name) + ")");
	ThrowOnError(*result);
	result = connection.Query("DELETE FROM gql_internal.objects WHERE graph_id = "
	                          "(SELECT graph_id FROM gql_internal.graphs WHERE graph_name = " +
	                          QuoteLiteral(data.graph_name) + ")");
	ThrowOnError(*result);
	result = connection.Query("DELETE FROM gql_internal.graphs WHERE graph_name = " + QuoteLiteral(data.graph_name));
	ThrowOnError(*result);
	EmitCommandResult(output, state, data.graph_name);
}

static void SetGraph(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<SingleRowState>();
	if (state.done) {
		return;
	}
	auto &data = input.bind_data->Cast<CommandBindData>();
	Connection connection(*context.db);
	EnsureStorage(connection);
	if (!GraphExists(connection, data.graph_name)) {
		throw InvalidInputException("Graph '%s' does not exist", data.graph_name);
	}
	auto gql_state = context.registered_state->GetOrCreate<GqlClientState>(GQL_STATE_KEY);
	gql_state->graph_name = data.graph_name;
	EmitCommandResult(output, state, data.graph_name);
}

struct PropertyInput {
	string name;
	string tag;
	string literal;
};

struct InsertVertexBindData : TableFunctionData {
	vector<string> labels;
	vector<PropertyInput> properties;
};

static vector<string> ReadStringList(const Value &value) {
	vector<string> result;
	for (const auto &entry : ListValue::GetChildren(value)) {
		result.push_back(entry.GetValue<string>());
	}
	return result;
}

static unique_ptr<FunctionData> InsertVertexBind(ClientContext &, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 4) {
		throw BinderException("GQL INSERT requires labels and typed property lists");
	}
	auto result = make_uniq<InsertVertexBindData>();
	result->labels = ReadStringList(input.inputs[0]);
	auto property_names = ReadStringList(input.inputs[1]);
	auto property_tags = ReadStringList(input.inputs[2]);
	auto property_literals = ReadStringList(input.inputs[3]);
	if (property_names.size() != property_tags.size() || property_names.size() != property_literals.size()) {
		throw BinderException("Invalid GQL INSERT property lists");
	}
	for (idx_t index = 0; index < property_names.size(); index++) {
		result->properties.push_back({property_names[index], property_tags[index], property_literals[index]});
	}
	names = {"success", "graph_name", "object_id"};
	return_types = {LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::UBIGINT};
	return std::move(result);
}

static string PropertyValueExpression(const PropertyInput &property) {
	string type;
	string literal = property.literal;
	if (property.tag == "bool_value") {
		type = "BOOLEAN";
	} else if (property.tag == "int_value") {
		type = "BIGINT";
	} else if (property.tag == "decimal_value") {
		type = "DECIMAL(38,18)";
	} else if (property.tag == "double_value") {
		type = "DOUBLE";
	} else if (property.tag == "string_value") {
		type = "VARCHAR";
		literal = QuoteLiteral(literal);
	} else {
		throw InvalidInputException("Unsupported GQL property value tag '%s'", property.tag);
	}
	return "union_value(" + property.tag + " := CAST(" + literal + " AS " + type + "))";
}

static Value InsertStoredObject(Connection &connection, const string &graph_id, uint8_t kind, const string &source_id,
                                const string &target_id, const vector<string> &labels,
                                const vector<PropertyInput> &properties) {
	string endpoints = kind == 0 ? ", NULL, NULL" : ", " + source_id + ", " + target_id;
	auto inserted = connection.Query("INSERT INTO gql_internal.objects (graph_id, kind, source_id, target_id) "
	                                 "VALUES (" +
	                                 graph_id + ", " + to_string(kind) + endpoints + ") RETURNING object_id");
	ThrowOnError(*inserted);
	auto object_id = inserted->GetValue(0, 0);
	auto object_id_text = object_id.ToString();

	for (const auto &label : labels) {
		auto result = connection.Query("INSERT INTO gql_internal.labels (graph_id, label_name) VALUES (" + graph_id +
		                               ", " + QuoteLiteral(label) + ") ON CONFLICT DO NOTHING");
		ThrowOnError(*result);
		result = connection.Query("INSERT INTO gql_internal.object_labels (graph_id, object_id, "
		                          "label_id) SELECT " +
		                          graph_id + ", " + object_id_text +
		                          ", label_id FROM gql_internal.labels WHERE graph_id = " + graph_id +
		                          " AND label_name = " + QuoteLiteral(label));
		ThrowOnError(*result);
	}

	for (const auto &property : properties) {
		auto result = connection.Query("INSERT INTO gql_internal.property_keys (graph_id, key_name) VALUES (" +
		                               graph_id + ", " + QuoteLiteral(property.name) + ") ON CONFLICT DO NOTHING");
		ThrowOnError(*result);
		result = connection.Query("INSERT INTO gql_internal.object_properties (graph_id, object_id, "
		                          "key_id, value) SELECT " +
		                          graph_id + ", " + object_id_text + ", key_id, " + PropertyValueExpression(property) +
		                          " FROM gql_internal.property_keys WHERE graph_id = " + graph_id +
		                          " AND key_name = " + QuoteLiteral(property.name));
		ThrowOnError(*result);
	}
	return object_id;
}

static void InsertVertex(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<SingleRowState>();
	if (state.done) {
		return;
	}
	auto gql_state = context.registered_state->GetOrCreate<GqlClientState>(GQL_STATE_KEY);
	if (gql_state->graph_name.empty()) {
		throw InvalidInputException("No graph selected; use SESSION SET GRAPH before INSERT");
	}
	auto &data = input.bind_data->Cast<InsertVertexBindData>();
	Connection connection(*context.db);
	EnsureStorage(connection);
	if (!GraphExists(connection, gql_state->graph_name)) {
		throw InvalidInputException("Selected graph '%s' no longer exists", gql_state->graph_name);
	}

	Value object_id;
	connection.BeginTransaction();
	try {
		auto graph = connection.Query("SELECT graph_id FROM gql_internal.graphs WHERE graph_name = " +
		                              QuoteLiteral(gql_state->graph_name));
		ThrowOnError(*graph);
		auto graph_id = graph->GetValue(0, 0).ToString();
		object_id = InsertStoredObject(connection, graph_id, 0, "", "", data.labels, data.properties);

		auto updated = connection.Query("UPDATE gql_internal.graphs SET "
		                                "graph_version = graph_version + 1 WHERE "
		                                "graph_id = " +
		                                graph_id);
		ThrowOnError(*updated);
		connection.Commit();
	} catch (...) {
		if (connection.HasActiveTransaction()) {
			connection.Rollback();
		}
		throw;
	}

	output.SetCardinality(1);
	output.SetValue(0, 0, Value(true));
	output.SetValue(1, 0, Value(gql_state->graph_name));
	output.SetValue(2, 0, object_id);
	state.done = true;
}

struct InsertElementInput {
	vector<string> labels;
	vector<PropertyInput> properties;
};

struct InsertEdgeInput : InsertElementInput {
	idx_t source_vertex;
	idx_t target_vertex;
};

struct InsertPathBindData : TableFunctionData {
	vector<InsertElementInput> vertices;
	vector<InsertEdgeInput> edges;
};

static vector<vector<string>> ReadNestedStringLists(const Value &value) {
	vector<vector<string>> result;
	for (const auto &entry : ListValue::GetChildren(value)) {
		result.push_back(ReadStringList(entry));
	}
	return result;
}

static vector<idx_t> ReadIndexList(const Value &value) {
	vector<idx_t> result;
	for (const auto &entry : ListValue::GetChildren(value)) {
		result.push_back(NumericCast<idx_t>(entry.GetValue<uint64_t>()));
	}
	return result;
}

static vector<InsertElementInput> BuildElementInputs(const Value &labels_value, const Value &names_value,
                                                     const Value &tags_value, const Value &literals_value) {
	auto labels = ReadNestedStringLists(labels_value);
	auto names = ReadNestedStringLists(names_value);
	auto tags = ReadNestedStringLists(tags_value);
	auto literals = ReadNestedStringLists(literals_value);
	if (labels.size() != names.size() || labels.size() != tags.size() || labels.size() != literals.size()) {
		throw BinderException("Invalid GQL INSERT element lists");
	}
	vector<InsertElementInput> result;
	for (idx_t element = 0; element < labels.size(); element++) {
		if (names[element].size() != tags[element].size() || names[element].size() != literals[element].size()) {
			throw BinderException("Invalid GQL INSERT property lists");
		}
		InsertElementInput input;
		input.labels = std::move(labels[element]);
		for (idx_t property = 0; property < names[element].size(); property++) {
			input.properties.push_back(
			    {names[element][property], tags[element][property], literals[element][property]});
		}
		result.push_back(std::move(input));
	}
	return result;
}

static unique_ptr<FunctionData> InsertPathBind(ClientContext &, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 10) {
		throw BinderException("GQL path INSERT requires typed vertex and edge lists");
	}
	auto result = make_uniq<InsertPathBindData>();
	result->vertices = BuildElementInputs(input.inputs[0], input.inputs[1], input.inputs[2], input.inputs[3]);
	auto sources = ReadIndexList(input.inputs[4]);
	auto targets = ReadIndexList(input.inputs[5]);
	auto edge_elements = BuildElementInputs(input.inputs[6], input.inputs[7], input.inputs[8], input.inputs[9]);
	if (sources.size() != targets.size() || sources.size() != edge_elements.size()) {
		throw BinderException("Invalid GQL INSERT edge lists");
	}
	for (idx_t index = 0; index < sources.size(); index++) {
		if (sources[index] >= result->vertices.size() || targets[index] >= result->vertices.size()) {
			throw BinderException("Invalid GQL INSERT edge endpoint");
		}
		InsertEdgeInput edge;
		edge.labels = std::move(edge_elements[index].labels);
		edge.properties = std::move(edge_elements[index].properties);
		edge.source_vertex = sources[index];
		edge.target_vertex = targets[index];
		result->edges.push_back(std::move(edge));
	}
	names = {"success", "graph_name", "object_id"};
	return_types = {LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::UBIGINT};
	return std::move(result);
}

static void InsertPath(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<SingleRowState>();
	if (state.done) {
		return;
	}
	auto gql_state = context.registered_state->GetOrCreate<GqlClientState>(GQL_STATE_KEY);
	if (gql_state->graph_name.empty()) {
		throw InvalidInputException("No graph selected; use SESSION SET GRAPH before INSERT");
	}
	auto &data = input.bind_data->Cast<InsertPathBindData>();
	Connection connection(*context.db);
	EnsureStorage(connection);
	if (!GraphExists(connection, gql_state->graph_name)) {
		throw InvalidInputException("Selected graph '%s' no longer exists", gql_state->graph_name);
	}

	vector<Value> vertex_ids;
	connection.BeginTransaction();
	try {
		auto graph = connection.Query("SELECT graph_id FROM gql_internal.graphs WHERE graph_name = " +
		                              QuoteLiteral(gql_state->graph_name));
		ThrowOnError(*graph);
		auto graph_id = graph->GetValue(0, 0).ToString();
		for (const auto &vertex : data.vertices) {
			vertex_ids.push_back(InsertStoredObject(connection, graph_id, 0, "", "", vertex.labels, vertex.properties));
		}
		for (const auto &edge : data.edges) {
			InsertStoredObject(connection, graph_id, 1, vertex_ids[edge.source_vertex].ToString(),
			                   vertex_ids[edge.target_vertex].ToString(), edge.labels, edge.properties);
		}
		auto updated = connection.Query("UPDATE gql_internal.graphs SET "
		                                "graph_version = graph_version + 1 WHERE "
		                                "graph_id = " +
		                                graph_id);
		ThrowOnError(*updated);
		connection.Commit();
	} catch (...) {
		if (connection.HasActiveTransaction()) {
			connection.Rollback();
		}
		throw;
	}

	output.SetCardinality(1);
	output.SetValue(0, 0, Value(true));
	output.SetValue(1, 0, Value(gql_state->graph_name));
	output.SetValue(2, 0, vertex_ids[0]);
	state.done = true;
}

struct GraphRow {
	Value graph_id;
	Value graph_name;
	Value graph_version;
	Value vertex_count;
	Value edge_count;
	Value created_at;
};

struct GraphRowsState : GlobalTableFunctionState {
	bool initialized = false;
	idx_t offset = 0;
	vector<GraphRow> rows;
};

static unique_ptr<FunctionData> GraphsBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                           vector<string> &names) {
	names = {"graph_id", "graph_name", "graph_version", "vertex_count", "edge_count", "created_at"};
	return_types = {LogicalType::UBIGINT, LogicalType::VARCHAR, LogicalType::UBIGINT,
	                LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::TIMESTAMP};
	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> GraphsInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<GraphRowsState>();
}

static void LoadGraphRows(ClientContext &context, GraphRowsState &state) {
	Connection connection(*context.db);
	EnsureStorage(connection);
	auto result = connection.Query("SELECT g.graph_id, g.graph_name, g.graph_version, "
	                               "count(o.object_id) FILTER (WHERE o.kind = 0)::UBIGINT AS vertex_count, "
	                               "count(o.object_id) FILTER (WHERE o.kind = 1)::UBIGINT AS edge_count, "
	                               "g.created_at "
	                               "FROM gql_internal.graphs g LEFT JOIN gql_internal.objects o USING "
	                               "(graph_id) "
	                               "GROUP BY ALL ORDER BY g.graph_name");
	ThrowOnError(*result);
	for (idx_t row = 0; row < result->RowCount(); row++) {
		state.rows.push_back({result->GetValue(0, row), result->GetValue(1, row), result->GetValue(2, row),
		                      result->GetValue(3, row), result->GetValue(4, row), result->GetValue(5, row)});
	}
	state.initialized = true;
}

static void GraphsFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<GraphRowsState>();
	if (!state.initialized) {
		LoadGraphRows(context, state);
	}
	auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	for (idx_t index = 0; index < count; index++) {
		auto &row = state.rows[state.offset + index];
		output.SetValue(0, index, row.graph_id);
		output.SetValue(1, index, row.graph_name);
		output.SetValue(2, index, row.graph_version);
		output.SetValue(3, index, row.vertex_count);
		output.SetValue(4, index, row.edge_count);
		output.SetValue(5, index, row.created_at);
	}
	state.offset += count;
	output.SetCardinality(count);
}

struct VertexRow {
	Value graph_name;
	Value object_id;
	Value labels;
};

struct VertexRowsState : GlobalTableFunctionState {
	bool initialized = false;
	idx_t offset = 0;
	vector<VertexRow> rows;
};

static unique_ptr<FunctionData> VerticesBind(ClientContext &, TableFunctionBindInput &,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	names = {"graph_name", "object_id", "labels"};
	return_types = {LogicalType::VARCHAR, LogicalType::UBIGINT, LogicalType::LIST(LogicalType::VARCHAR)};
	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> VerticesInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<VertexRowsState>();
}

static void VerticesFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<VertexRowsState>();
	if (!state.initialized) {
		Connection connection(*context.db);
		EnsureStorage(connection);
		auto result = connection.Query("SELECT g.graph_name, o.object_id, "
		                               "coalesce(list(l.label_name ORDER BY l.label_name) FILTER (WHERE "
		                               "l.label_id IS NOT NULL), "
		                               "[]::VARCHAR[]) AS labels "
		                               "FROM gql_internal.objects o JOIN gql_internal.graphs g USING "
		                               "(graph_id) "
		                               "LEFT JOIN gql_internal.object_labels ol USING (graph_id, object_id) "
		                               "LEFT JOIN gql_internal.labels l USING (graph_id, label_id) "
		                               "WHERE o.kind = 0 GROUP BY g.graph_name, o.object_id ORDER BY "
		                               "g.graph_name, o.object_id");
		ThrowOnError(*result);
		for (idx_t row = 0; row < result->RowCount(); row++) {
			state.rows.push_back({result->GetValue(0, row), result->GetValue(1, row), result->GetValue(2, row)});
		}
		state.initialized = true;
	}
	auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	for (idx_t index = 0; index < count; index++) {
		auto &row = state.rows[state.offset + index];
		output.SetValue(0, index, row.graph_name);
		output.SetValue(1, index, row.object_id);
		output.SetValue(2, index, row.labels);
	}
	state.offset += count;
	output.SetCardinality(count);
}

struct EdgeRow {
	Value graph_name;
	Value object_id;
	Value source_id;
	Value target_id;
	Value labels;
};

struct EdgeRowsState : GlobalTableFunctionState {
	bool initialized = false;
	idx_t offset = 0;
	vector<EdgeRow> rows;
};

static unique_ptr<FunctionData> EdgesBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                          vector<string> &names) {
	names = {"graph_name", "object_id", "source_id", "target_id", "labels"};
	return_types = {LogicalType::VARCHAR, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT,
	                LogicalType::LIST(LogicalType::VARCHAR)};
	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> EdgesInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<EdgeRowsState>();
}

static void EdgesFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<EdgeRowsState>();
	if (!state.initialized) {
		Connection connection(*context.db);
		EnsureStorage(connection);
		auto result = connection.Query("SELECT g.graph_name, o.object_id, o.source_id, o.target_id, "
		                               "coalesce(list(l.label_name ORDER BY l.label_name) FILTER (WHERE "
		                               "l.label_id IS NOT NULL), "
		                               "[]::VARCHAR[]) AS labels "
		                               "FROM gql_internal.objects o JOIN gql_internal.graphs g USING "
		                               "(graph_id) "
		                               "LEFT JOIN gql_internal.object_labels ol USING (graph_id, object_id) "
		                               "LEFT JOIN gql_internal.labels l USING (graph_id, label_id) "
		                               "WHERE o.kind = 1 GROUP BY g.graph_name, o.object_id, o.source_id, "
		                               "o.target_id "
		                               "ORDER BY g.graph_name, o.object_id");
		ThrowOnError(*result);
		for (idx_t row = 0; row < result->RowCount(); row++) {
			state.rows.push_back({result->GetValue(0, row), result->GetValue(1, row), result->GetValue(2, row),
			                      result->GetValue(3, row), result->GetValue(4, row)});
		}
		state.initialized = true;
	}
	auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	for (idx_t index = 0; index < count; index++) {
		auto &row = state.rows[state.offset + index];
		output.SetValue(0, index, row.graph_name);
		output.SetValue(1, index, row.object_id);
		output.SetValue(2, index, row.source_id);
		output.SetValue(3, index, row.target_id);
		output.SetValue(4, index, row.labels);
	}
	state.offset += count;
	output.SetCardinality(count);
}

struct PropertyRow {
	Value graph_name;
	Value object_id;
	Value property_name;
	Value value;
};

struct PropertyRowsState : GlobalTableFunctionState {
	bool initialized = false;
	idx_t offset = 0;
	vector<PropertyRow> rows;
};

static unique_ptr<FunctionData> PropertiesBind(ClientContext &, TableFunctionBindInput &,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	names = {"graph_name", "object_id", "property_name", "value"};
	return_types = {LogicalType::VARCHAR, LogicalType::UBIGINT, LogicalType::VARCHAR, PropertyValueType()};
	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> PropertiesInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<PropertyRowsState>();
}

static void PropertiesFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<PropertyRowsState>();
	if (!state.initialized) {
		Connection connection(*context.db);
		EnsureStorage(connection);
		auto result = connection.Query("SELECT g.graph_name, p.object_id, k.key_name, p.value "
		                               "FROM gql_internal.object_properties p JOIN gql_internal.graphs g "
		                               "USING (graph_id) "
		                               "JOIN gql_internal.property_keys k USING (graph_id, key_id) "
		                               "ORDER BY g.graph_name, p.object_id, k.key_name");
		ThrowOnError(*result);
		for (idx_t row = 0; row < result->RowCount(); row++) {
			state.rows.push_back({result->GetValue(0, row), result->GetValue(1, row), result->GetValue(2, row),
			                      result->GetValue(3, row)});
		}
		state.initialized = true;
	}
	auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	for (idx_t index = 0; index < count; index++) {
		auto &row = state.rows[state.offset + index];
		output.SetValue(0, index, row.graph_name);
		output.SetValue(1, index, row.object_id);
		output.SetValue(2, index, row.property_name);
		output.SetValue(3, index, row.value);
	}
	state.offset += count;
	output.SetCardinality(count);
}

TableFunction GqlCreateGraphFunction() {
	TableFunction function("gql_create_graph", {LogicalType::VARCHAR, LogicalType::BOOLEAN}, CreateGraph);
	function.bind = CommandBind;
	function.init_global = SingleRowInit;
	return function;
}

TableFunction GqlDropGraphFunction() {
	TableFunction function("gql_drop_graph", {LogicalType::VARCHAR, LogicalType::BOOLEAN}, DropGraph);
	function.bind = CommandBind;
	function.init_global = SingleRowInit;
	return function;
}

TableFunction GqlSetGraphFunction() {
	TableFunction function("gql_set_graph", {LogicalType::VARCHAR}, SetGraph);
	function.bind = SetGraphBind;
	function.init_global = SingleRowInit;
	return function;
}

TableFunction GqlInsertVertexFunction() {
	auto list_type = LogicalType::LIST(LogicalType::VARCHAR);
	TableFunction function("gql_insert_vertex", {list_type, list_type, list_type, list_type}, InsertVertex);
	function.bind = InsertVertexBind;
	function.init_global = SingleRowInit;
	return function;
}

TableFunction GqlInsertPathFunction() {
	auto list_type = LogicalType::LIST(LogicalType::VARCHAR);
	auto nested_list_type = LogicalType::LIST(list_type);
	TableFunction function("gql_insert_path",
	                       {nested_list_type, nested_list_type, nested_list_type, nested_list_type,
	                        LogicalType::LIST(LogicalType::UBIGINT), LogicalType::LIST(LogicalType::UBIGINT),
	                        nested_list_type, nested_list_type, nested_list_type, nested_list_type},
	                       InsertPath);
	function.bind = InsertPathBind;
	function.init_global = SingleRowInit;
	return function;
}

TableFunction GqlGraphsFunction() {
	TableFunction function("gql_graphs", {}, GraphsFunction);
	function.bind = GraphsBind;
	function.init_global = GraphsInit;
	return function;
}

TableFunction GqlVerticesFunction() {
	TableFunction function("gql_vertices", {}, VerticesFunction);
	function.bind = VerticesBind;
	function.init_global = VerticesInit;
	return function;
}

TableFunction GqlEdgesFunction() {
	TableFunction function("gql_edges", {}, EdgesFunction);
	function.bind = EdgesBind;
	function.init_global = EdgesInit;
	return function;
}

TableFunction GqlPropertiesFunction() {
	TableFunction function("gql_properties", {}, PropertiesFunction);
	function.bind = PropertiesBind;
	function.init_global = PropertiesInit;
	return function;
}

TableFunction GqlMutationControlFunction() {
	TableFunction function("gql_mutation_control", {LogicalType::VARCHAR, LogicalType::BOOLEAN}, MutationControl);
	function.bind = MutationControlBind;
	function.init_global = SingleRowInit;
	return function;
}

} // namespace duckdb
