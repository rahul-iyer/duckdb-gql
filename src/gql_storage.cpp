#include "gql_storage.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

static constexpr const char *GQL_STATE_KEY = "gql_client_state";

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
	return context.registered_state->GetOrCreate<GqlClientState>(GQL_STATE_KEY)->graph_name;
}

static string QuoteLiteral(const string &value) {
	return KeywordHelper::WriteQuoted(value, '\'');
}

static string QuoteIdentifier(const string &value) {
	return KeywordHelper::WriteQuoted(value, '"');
}

static void ThrowOnError(const MaterializedQueryResult &result) {
	if (result.HasError()) {
		throw InvalidInputException("GQL native catalog error: %s", result.GetError());
	}
}

static unique_ptr<MaterializedQueryResult> Query(Connection &connection, const string &sql) {
	auto result = connection.Query(sql);
	ThrowOnError(*result);
	return result;
}

void GqlEnsureStorage(Connection &connection) {
	Query(connection, "CREATE SCHEMA IF NOT EXISTS gql_internal");
	Query(connection, "CREATE SEQUENCE IF NOT EXISTS gql_internal.graph_id_seq START 1");
	Query(connection, "CREATE SEQUENCE IF NOT EXISTS gql_internal.element_table_id_seq START 1");
	Query(connection, "CREATE SEQUENCE IF NOT EXISTS gql_internal.label_mapping_id_seq START 1");
	Query(connection, "CREATE TABLE IF NOT EXISTS gql_internal.graphs ("
	                  "graph_id UBIGINT PRIMARY KEY DEFAULT nextval('gql_internal.graph_id_seq'), "
	                  "graph_name VARCHAR NOT NULL UNIQUE, "
	                  "graph_version UBIGINT NOT NULL DEFAULT 0, "
	                  "created_at TIMESTAMP NOT NULL DEFAULT current_timestamp)");
	Query(connection, "CREATE TABLE IF NOT EXISTS gql_internal.graph_storage ("
	                  "graph_id UBIGINT PRIMARY KEY, "
	                  "storage_mode VARCHAR NOT NULL, "
	                  "default_catalog VARCHAR, "
	                  "default_schema VARCHAR, "
	                  "schema_version UBIGINT NOT NULL DEFAULT 0, "
	                  "csr_policy VARCHAR NOT NULL DEFAULT 'DISABLED', "
	                  "CHECK (storage_mode IN ('EMPTY', 'TABLE_BACKED')), "
	                  "CHECK (csr_policy IN ('DISABLED', 'MANUAL', 'AUTO')))");
	auto native_storage_schema = Query(
	    connection,
	    "SELECT count(*) FROM duckdb_constraints() WHERE schema_name = 'gql_internal' AND "
	    "table_name = 'graph_storage' AND constraint_type = 'CHECK' AND constraint_text LIKE '%EMPTY%'");
	if (native_storage_schema->GetValue(0, 0).GetValue<int64_t>() == 0) {
		throw InvalidInputException(
		    "This database uses the removed legacy EAV graph catalog; create a fresh database and reload graphs with "
		    "COPY GRAPH");
	}
	Query(connection, "CREATE TABLE IF NOT EXISTS gql_internal.graph_element_tables ("
	                  "element_table_id UBIGINT PRIMARY KEY DEFAULT nextval('gql_internal.element_table_id_seq'), "
	                  "graph_id UBIGINT NOT NULL, "
	                  "element_kind VARCHAR NOT NULL, "
	                  "catalog_name VARCHAR NOT NULL, "
	                  "schema_name VARCHAR NOT NULL, "
	                  "table_name VARCHAR NOT NULL, "
	                  "key_columns VARCHAR[] NOT NULL, "
	                  "ownership VARCHAR NOT NULL, "
	                  "access_mode VARCHAR NOT NULL, "
	                  "extra_properties_column VARCHAR, "
	                  "UNIQUE(graph_id, catalog_name, schema_name, table_name), "
	                  "CHECK (element_kind IN ('VERTEX', 'EDGE')), "
	                  "CHECK (ownership = 'MANAGED'), "
	                  "CHECK (access_mode IN ('READ_ONLY', 'READ_WRITE')))");
	Query(connection, "CREATE TABLE IF NOT EXISTS gql_internal.graph_edge_endpoints ("
	                  "edge_table_id UBIGINT PRIMARY KEY, "
	                  "source_vertex_table_id UBIGINT NOT NULL, "
	                  "target_vertex_table_id UBIGINT NOT NULL, "
	                  "source_columns VARCHAR[] NOT NULL, "
	                  "target_columns VARCHAR[] NOT NULL, "
	                  "source_key_columns VARCHAR[] NOT NULL, "
	                  "target_key_columns VARCHAR[] NOT NULL)");
	Query(connection, "CREATE TABLE IF NOT EXISTS gql_internal.graph_label_mappings ("
	                  "label_mapping_id UBIGINT PRIMARY KEY DEFAULT nextval('gql_internal.label_mapping_id_seq'), "
	                  "element_table_id UBIGINT NOT NULL, "
	                  "label_name VARCHAR, "
	                  "mapping_kind VARCHAR NOT NULL, "
	                  "column_name VARCHAR, "
	                  "CHECK ((mapping_kind = 'STATIC' AND label_name IS NOT NULL AND column_name IS NULL) OR "
	                  "(mapping_kind IN ('SCALAR_COLUMN', 'LIST_COLUMN') AND label_name IS NULL AND "
	                  "column_name IS NOT NULL)))");
	Query(connection, "CREATE TABLE IF NOT EXISTS gql_internal.graph_property_mappings ("
	                  "element_table_id UBIGINT NOT NULL, "
	                  "property_name VARCHAR NOT NULL, "
	                  "column_name VARCHAR NOT NULL, "
	                  "gql_type VARCHAR NOT NULL, "
	                  "nullable BOOLEAN NOT NULL, "
	                  "writable BOOLEAN NOT NULL, "
	                  "PRIMARY KEY(element_table_id, property_name))");
	Query(connection, "INSERT INTO gql_internal.graph_storage (graph_id, storage_mode, schema_version, csr_policy) "
	                  "SELECT graph_id, 'EMPTY', 0, 'DISABLED' FROM gql_internal.graphs ON CONFLICT DO NOTHING");
}

static bool GraphExists(Connection &connection, const string &graph_name) {
	auto result = Query(connection, "SELECT count(*) FROM gql_internal.graphs WHERE graph_name = " +
	                                    QuoteLiteral(graph_name));
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
	MutationControlBindData(string command_id_p, bool begin_p)
	    : command_id(std::move(command_id_p)), begin(begin_p) {
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
	names = {"success", "graph_name"};
	return_types = {LogicalType::BOOLEAN, LogicalType::VARCHAR};
	return make_uniq<CommandBindData>(input.inputs[0].GetValue<string>(),
	                                  input.inputs.size() > 1 && !input.inputs[1].IsNull() &&
	                                      input.inputs[1].GetValue<bool>());
}

static unique_ptr<FunctionData> SetGraphBind(ClientContext &, TableFunctionBindInput &input,
	                                         vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("A graph name is required");
	}
	names = {"success", "graph_name"};
	return_types = {LogicalType::BOOLEAN, LogicalType::VARCHAR};
	return make_uniq<CommandBindData>(input.inputs[0].GetValue<string>(), false);
}

static unique_ptr<FunctionData> MutationControlBind(ClientContext &, TableFunctionBindInput &input,
	                                                vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 2 || input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("GQL mutation control requires a command id and phase");
	}
	names = {"success"};
	return_types = {LogicalType::BOOLEAN};
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
			// BeginQueryInternal has already opened the autocommit transaction.
			// Disabling autocommit carries it across every generated DML statement.
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
			// EndQueryInternal commits after this function finishes.
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
	GqlEnsureStorage(connection);
	if (GraphExists(connection, data.graph_name)) {
		if (!data.conditional) {
			throw InvalidInputException("Graph '%s' already exists", data.graph_name);
		}
		EmitCommandResult(output, state, data.graph_name);
		return;
	}
	Query(connection, "INSERT INTO gql_internal.graphs (graph_name) VALUES (" + QuoteLiteral(data.graph_name) + ")");
	Query(connection, "INSERT INTO gql_internal.graph_storage (graph_id, storage_mode, schema_version, csr_policy) "
	                  "SELECT graph_id, 'EMPTY', 0, 'DISABLED' FROM gql_internal.graphs WHERE graph_name = " +
	                      QuoteLiteral(data.graph_name));
	EmitCommandResult(output, state, data.graph_name);
}

static void DropGraph(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<SingleRowState>();
	if (state.done) {
		return;
	}
	auto &data = input.bind_data->Cast<CommandBindData>();
	Connection connection(*context.db);
	GqlEnsureStorage(connection);
	if (!GraphExists(connection, data.graph_name)) {
		if (!data.conditional) {
			throw InvalidInputException("Graph '%s' does not exist", data.graph_name);
		}
		EmitCommandResult(output, state, data.graph_name);
		return;
	}
	connection.BeginTransaction();
	try {
		auto graph_id = Query(connection, "SELECT graph_id FROM gql_internal.graphs WHERE graph_name = " +
		                                      QuoteLiteral(data.graph_name))
		                    ->GetValue(0, 0)
		                    .GetValue<uint64_t>();
		Query(connection, "DROP SEQUENCE IF EXISTS gql_internal." +
		                      QuoteIdentifier("graph_" + to_string(graph_id) + "_vertex_id_seq"));
		Query(connection, "DROP SEQUENCE IF EXISTS gql_internal." +
		                      QuoteIdentifier("graph_" + to_string(graph_id) + "_edge_id_seq"));
		auto managed = Query(connection, "SELECT catalog_name, schema_name, table_name FROM "
		                                 "gql_internal.graph_element_tables WHERE graph_id = " +
		                                     to_string(graph_id) + " ORDER BY element_kind");
		for (idx_t row = 0; row < managed->RowCount(); row++) {
			auto table = QuoteIdentifier(managed->GetValue(0, row).GetValue<string>()) + "." +
			             QuoteIdentifier(managed->GetValue(1, row).GetValue<string>()) + "." +
			             QuoteIdentifier(managed->GetValue(2, row).GetValue<string>());
			Query(connection, "DROP TABLE IF EXISTS " + table);
		}
		Query(connection, "DELETE FROM gql_internal.graph_property_mappings WHERE element_table_id IN "
		                  "(SELECT element_table_id FROM gql_internal.graph_element_tables WHERE graph_id = " +
		                      to_string(graph_id) + ")");
		Query(connection, "DELETE FROM gql_internal.graph_label_mappings WHERE element_table_id IN "
		                  "(SELECT element_table_id FROM gql_internal.graph_element_tables WHERE graph_id = " +
		                      to_string(graph_id) + ")");
		Query(connection, "DELETE FROM gql_internal.graph_edge_endpoints WHERE edge_table_id IN "
		                  "(SELECT element_table_id FROM gql_internal.graph_element_tables WHERE graph_id = " +
		                      to_string(graph_id) + ")");
		Query(connection, "DELETE FROM gql_internal.graph_element_tables WHERE graph_id = " + to_string(graph_id));
		Query(connection, "DELETE FROM gql_internal.graph_storage WHERE graph_id = " + to_string(graph_id));
		Query(connection, "DELETE FROM gql_internal.graphs WHERE graph_id = " + to_string(graph_id));
		connection.Commit();
	} catch (...) {
		if (connection.HasActiveTransaction()) {
			connection.Rollback();
		}
		throw;
	}
	auto gql_state = context.registered_state->GetOrCreate<GqlClientState>(GQL_STATE_KEY);
	if (gql_state->graph_name == data.graph_name) {
		gql_state->graph_name.clear();
	}
	EmitCommandResult(output, state, data.graph_name);
}

static void SetGraph(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<SingleRowState>();
	if (state.done) {
		return;
	}
	auto &data = input.bind_data->Cast<CommandBindData>();
	Connection connection(*context.db);
	GqlEnsureStorage(connection);
	auto mode = Query(connection, "SELECT storage_mode FROM gql_internal.graph_storage gs JOIN "
	                              "gql_internal.graphs g USING (graph_id) WHERE g.graph_name = " +
	                                  QuoteLiteral(data.graph_name));
	if (mode->RowCount() == 0) {
		throw InvalidInputException("Graph '%s' does not exist", data.graph_name);
	}
	auto gql_state = context.registered_state->GetOrCreate<GqlClientState>(GQL_STATE_KEY);
	gql_state->graph_name = data.graph_name;
	EmitCommandResult(output, state, data.graph_name);
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

static unique_ptr<FunctionData> GraphsBind(ClientContext &, TableFunctionBindInput &,
	                                       vector<LogicalType> &return_types, vector<string> &names) {
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
	GqlEnsureStorage(connection);
	auto graphs = Query(connection, "SELECT g.graph_id, g.graph_name, g.graph_version, g.created_at "
	                                "FROM gql_internal.graphs g ORDER BY g.graph_name");
	for (idx_t row = 0; row < graphs->RowCount(); row++) {
		auto graph_id = graphs->GetValue(0, row).GetValue<uint64_t>();
		auto tables = Query(connection, "SELECT element_kind, catalog_name, schema_name, table_name FROM "
		                                 "gql_internal.graph_element_tables WHERE graph_id = " +
		                                     to_string(graph_id));
		uint64_t vertex_count = 0;
		uint64_t edge_count = 0;
		for (idx_t table_row = 0; table_row < tables->RowCount(); table_row++) {
			auto qualified = QuoteIdentifier(tables->GetValue(1, table_row).GetValue<string>()) + "." +
			                 QuoteIdentifier(tables->GetValue(2, table_row).GetValue<string>()) + "." +
			                 QuoteIdentifier(tables->GetValue(3, table_row).GetValue<string>());
			auto count = Query(connection, "SELECT count(*)::UBIGINT FROM " + qualified)
			                 ->GetValue(0, 0)
			                 .GetValue<uint64_t>();
			if (tables->GetValue(0, table_row).GetValue<string>() == "VERTEX") {
				vertex_count += count;
			} else {
				edge_count += count;
			}
		}
		state.rows.push_back({graphs->GetValue(0, row), graphs->GetValue(1, row), graphs->GetValue(2, row),
		                      Value::UBIGINT(vertex_count), Value::UBIGINT(edge_count), graphs->GetValue(3, row)});
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

TableFunction GqlGraphsFunction() {
	TableFunction function("gql_graphs", {}, GraphsFunction);
	function.bind = GraphsBind;
	function.init_global = GraphsInit;
	return function;
}

TableFunction GqlMutationControlFunction() {
	TableFunction function("gql_mutation_control", {LogicalType::VARCHAR, LogicalType::BOOLEAN}, MutationControl);
	function.bind = MutationControlBind;
	function.init_global = SingleRowInit;
	return function;
}

} // namespace duckdb
