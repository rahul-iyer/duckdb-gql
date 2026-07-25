#include "gql_import.hpp"

#include "gql_catalog.hpp"
#include "gql_sql_utils.hpp"
#include "gql_storage.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace duckdb {

static constexpr const char *IMPORT_ROW_ID = "__gql_import_row_id";

enum class GraphHeaderFieldRole : uint8_t { PROPERTY, ID, LABEL, START_ID, END_ID, TYPE };

struct GraphHeaderField {
	string column_name;
	string property_name;
	string declared_type;
	string id_group;
	LogicalType source_type;
	GraphHeaderFieldRole role = GraphHeaderFieldRole::PROPERTY;
};

struct GraphHeaderSchema {
	GraphHeaderField id;
	GraphHeaderField start_id;
	GraphHeaderField end_id;
	vector<GraphHeaderField> labels;
	vector<GraphHeaderField> types;
	vector<GraphHeaderField> properties;
	bool has_id = false;
	bool has_start_id = false;
	bool has_end_id = false;
};

struct CopyGraphState : GlobalTableFunctionState {
	bool done = false;
};

static unique_ptr<GlobalTableFunctionState> CopyGraphInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<CopyGraphState>();
}

static string Trimmed(string value) {
	StringUtil::Trim(value);
	return value;
}

static idx_t ScalarCount(Connection &connection, const string &sql) {
	auto result = GqlQuery(connection, sql);
	return result->GetValue(0, 0).GetValue<idx_t>();
}

static bool EndsWith(const string &value, const string &suffix) {
	return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static bool ParseSpecialField(const string &raw_name, const string &marker, string &prefix, string &group) {
	auto upper = StringUtil::Upper(raw_name);
	auto marker_position = upper.find(marker);
	if (marker_position == string::npos) {
		return false;
	}
	auto suffix_position = marker_position + marker.size();
	if (suffix_position < upper.size() && upper[suffix_position] == '(') {
		auto close = upper.find(')', suffix_position + 1);
		if (close == string::npos || close + 1 != upper.size()) {
			return false;
		}
		group = Trimmed(raw_name.substr(suffix_position + 1, close - suffix_position - 1));
		suffix_position = close + 1;
	}
	if (suffix_position != upper.size()) {
		return false;
	}
	prefix = Trimmed(raw_name.substr(0, marker_position));
	return true;
}

static GraphHeaderField ParseField(const string &column_name, const LogicalType &source_type) {
	GraphHeaderField result;
	result.column_name = column_name;
	result.source_type = source_type;
	string header = Trimmed(column_name);
	auto options = header.find('{');
	if (options != string::npos) {
		if (header.back() != '}') {
			throw InvalidInputException("Invalid graph-header field '%s'", column_name);
		}
		header = Trimmed(header.substr(0, options));
	}

	string prefix;
	string group;
	if (ParseSpecialField(header, ":START_ID", prefix, group)) {
		result.role = GraphHeaderFieldRole::START_ID;
		result.id_group = group;
		return result;
	}
	if (ParseSpecialField(header, ":END_ID", prefix, group)) {
		result.role = GraphHeaderFieldRole::END_ID;
		result.id_group = group;
		return result;
	}
	if (ParseSpecialField(header, ":LABEL", prefix, group)) {
		result.role = GraphHeaderFieldRole::LABEL;
		return result;
	}
	if (ParseSpecialField(header, ":TYPE", prefix, group)) {
		result.role = GraphHeaderFieldRole::TYPE;
		return result;
	}
	if (ParseSpecialField(header, ":ID", prefix, group)) {
		result.role = GraphHeaderFieldRole::ID;
		result.property_name = StringUtil::Lower(prefix);
		result.id_group = group;
		return result;
	}

	auto separator = header.rfind(':');
	if (separator == string::npos) {
		result.property_name = StringUtil::Lower(header);
	} else {
		result.property_name = StringUtil::Lower(Trimmed(header.substr(0, separator)));
		result.declared_type = StringUtil::Lower(Trimmed(header.substr(separator + 1)));
	}
	if (result.property_name.empty()) {
		throw InvalidInputException("Graph-header property field '%s' has no property name", column_name);
	}
	return result;
}

static GraphHeaderSchema ReadSchema(Connection &connection, const string &table_name, bool relationships) {
	auto result = GqlQuery(connection, "SELECT * FROM " + GqlQuoteIdentifier(table_name) + " LIMIT 0");
	GraphHeaderSchema schema;
	for (idx_t index = 0; index < result->names.size(); index++) {
		if (result->names[index] == IMPORT_ROW_ID) {
			continue;
		}
		auto field = ParseField(result->names[index], result->types[index]);
		if (field.role != GraphHeaderFieldRole::PROPERTY && field.source_type.IsNested()) {
			throw InvalidInputException("Graph structural field '%s' must be scalar in the initial importer",
			                            field.column_name);
		}
		switch (field.role) {
		case GraphHeaderFieldRole::ID:
			if (relationships) {
				throw InvalidInputException("Relationship :ID fields are not supported by the graph-header importer");
			}
			if (schema.has_id) {
				throw InvalidInputException("Graph-header node input must contain exactly one :ID field");
			}
			schema.id = field;
			schema.has_id = true;
			if (!field.property_name.empty()) {
				schema.properties.push_back(field);
			}
			break;
		case GraphHeaderFieldRole::START_ID:
			if (!relationships || schema.has_start_id) {
				throw InvalidInputException("Graph-header relationship input must contain exactly one :START_ID field");
			}
			schema.start_id = field;
			schema.has_start_id = true;
			break;
		case GraphHeaderFieldRole::END_ID:
			if (!relationships || schema.has_end_id) {
				throw InvalidInputException("Graph-header relationship input must contain exactly one :END_ID field");
			}
			schema.end_id = field;
			schema.has_end_id = true;
			break;
		case GraphHeaderFieldRole::LABEL:
			if (relationships) {
				throw InvalidInputException("Use :TYPE, not :LABEL, for graph-header relationships");
			}
			schema.labels.push_back(field);
			break;
		case GraphHeaderFieldRole::TYPE:
			if (!relationships) {
				throw InvalidInputException("Graph-header node input cannot contain a :TYPE field");
			}
			schema.types.push_back(field);
			break;
		case GraphHeaderFieldRole::PROPERTY:
			schema.properties.push_back(field);
			break;
		}
	}
	if (!relationships && !schema.has_id) {
		throw InvalidInputException("Graph-header node input requires one :ID field");
	}
	if (relationships && (!schema.has_start_id || !schema.has_end_id || schema.types.size() != 1)) {
		throw InvalidInputException(
		    "Graph-header relationship input requires :START_ID, :END_ID, and exactly one :TYPE field");
	}
	return schema;
}

static string ScanExpression(const string &path, const string &format) {
	if (format == "csv") {
		return "read_csv(" + GqlQuoteLiteral(path) + ", header = true, auto_detect = true, all_varchar = true)";
	}
	return "read_parquet(" + GqlQuoteLiteral(path) + ")";
}

static void CreateRawTable(Connection &connection, const string &table_name, const string &path, const string &format) {
	GqlQuery(connection, "CREATE TEMP TABLE " + GqlQuoteIdentifier(table_name) +
	                         " AS SELECT row_number() OVER ()::UBIGINT AS " + GqlQuoteIdentifier(IMPORT_ROW_ID) +
	                         ", * FROM " + ScanExpression(path, format));
}

static string Column(const string &alias, const GraphHeaderField &field) {
	return alias + "." + GqlQuoteIdentifier(field.column_name);
}

static string ExternalId(const string &alias, const GraphHeaderField &field) {
	return "CAST(" + Column(alias, field) + " AS VARCHAR)";
}

struct CopyGraphBindData : TableFunctionData {
	CopyGraphBindData(string graph_name_p, string vertex_path_p, string edge_path_p, bool validate_p)
	    : graph_name(std::move(graph_name_p)), vertex_path(std::move(vertex_path_p)), edge_path(std::move(edge_path_p)),
	      validate(validate_p) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<CopyGraphBindData>(graph_name, vertex_path, edge_path, validate);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto other = dynamic_cast<const CopyGraphBindData *>(&other_p);
		return other && graph_name == other->graph_name && vertex_path == other->vertex_path &&
		       edge_path == other->edge_path && validate == other->validate;
	}

	string graph_name;
	string vertex_path;
	string edge_path;
	bool validate;
};

static unique_ptr<FunctionData> CopyGraphBind(ClientContext &, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 4 || input.inputs[0].IsNull() || input.inputs[1].IsNull() || input.inputs[2].IsNull() ||
	    input.inputs[3].IsNull()) {
		throw BinderException("COPY GRAPH requires a graph, vertex file, edge file, and validation mode");
	}
	auto graph_name = input.inputs[0].GetValue<string>();
	auto vertex_path = input.inputs[1].GetValue<string>();
	auto edge_path = input.inputs[2].GetValue<string>();
	if (graph_name.empty() || vertex_path.empty() || edge_path.empty()) {
		throw BinderException("COPY GRAPH graph and file names cannot be empty");
	}
	names = {"success", "graph_name", "vertex_count", "edge_count"};
	return_types = {LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::UBIGINT, LogicalType::UBIGINT};
	return make_uniq<CopyGraphBindData>(graph_name, vertex_path, edge_path, input.inputs[3].GetValue<bool>());
}

static string InferFileFormat(const string &path) {
	auto lower = StringUtil::Lower(path);
	if (EndsWith(lower, ".parquet")) {
		return "parquet";
	}
	if (EndsWith(lower, ".csv") || EndsWith(lower, ".csv.gz") || EndsWith(lower, ".csv.zst")) {
		return "csv";
	}
	throw InvalidInputException("COPY GRAPH cannot infer the format of '%s'; expected CSV, CSV.GZ, CSV.ZST, or "
	                            "Parquet",
	                            path);
}

static string NativeValueExpression(const GraphHeaderField &field, const string &alias) {
	auto source = Column(alias, field);
	auto type = StringUtil::Lower(field.declared_type);
	if (EndsWith(type, "[]")) {
		auto element_type = type.substr(0, type.size() - 2);
		string target_type;
		if (element_type == "boolean" || element_type == "bool") {
			target_type = "BOOLEAN";
		} else if (element_type == "byte" || element_type == "short" || element_type == "int" ||
		           element_type == "integer" || element_type == "long") {
			target_type = "BIGINT";
		} else if (element_type == "float" || element_type == "double") {
			target_type = "DOUBLE";
		} else if (element_type == "string" || element_type == "char") {
			target_type = "VARCHAR";
		} else {
			throw InvalidInputException("COPY GRAPH list property type '%s' is not supported yet", field.declared_type);
		}
		if (field.source_type.id() == LogicalTypeId::LIST) {
			return "CAST(" + source + " AS " + target_type + "[])";
		}
		return "CASE WHEN " + source + " IS NULL THEN NULL WHEN " + source + " = '' THEN []::" + target_type +
		       "[] ELSE list_transform(string_split(" + source + ", ';'), item -> CAST(item AS " + target_type +
		       ")) END";
	}
	if (type.empty()) {
		return "CAST(" + source + " AS " + field.source_type.ToString() + ")";
	}
	if (type == "boolean" || type == "bool") {
		return "CAST(" + source + " AS BOOLEAN)";
	}
	if (type == "byte" || type == "short" || type == "int" || type == "integer" || type == "long") {
		return "CAST(" + source + " AS BIGINT)";
	}
	if (type == "float" || type == "double") {
		return "CAST(" + source + " AS DOUBLE)";
	}
	if (type == "string" || type == "char") {
		return "CAST(" + source + " AS VARCHAR)";
	}
	if (type == "variant") {
		auto payload = "substring(" + source + ", 3)";
		return "CASE WHEN " + source + " IS NULL THEN NULL WHEN left(" + source + ", 2) = 'i:' THEN CAST(CAST(" +
		       payload + " AS BIGINT) AS VARIANT) WHEN left(" + source + ", 2) = 'd:' THEN CAST(CAST(" + payload +
		       " AS DOUBLE) AS VARIANT) WHEN left(" + source + ", 2) = 'b:' THEN CAST(CAST(" + payload +
		       " AS BOOLEAN) AS VARIANT) WHEN left(" + source + ", 2) = 's:' THEN CAST(" + payload +
		       " AS VARIANT) ELSE error('Invalid COPY GRAPH VARIANT encoding') END";
	}
	if (type == "date") {
		return "CAST(" + source + " AS DATE)";
	}
	if (type == "localtime") {
		return "CAST(" + source + " AS TIME_NS)";
	}
	if (type == "localdatetime") {
		return "CAST(" + source + " AS TIMESTAMP_NS)";
	}
	if (type == "datetime") {
		return "CAST(" + source + " AS TIMESTAMPTZ)";
	}
	throw InvalidInputException("COPY GRAPH property type '%s' is not supported yet", field.declared_type);
}

static void ValidateNativeProperties(const vector<GraphHeaderField> &properties, const char *kind) {
	unordered_set<string> names;
	for (const auto &property : properties) {
		if (StringUtil::StartsWith(property.property_name, "__gql_")) {
			throw InvalidInputException("COPY GRAPH %s property '%s' uses the reserved __gql_ prefix", kind,
			                            property.property_name);
		}
		if (!names.insert(property.property_name).second) {
			throw InvalidInputException("COPY GRAPH %s property '%s' is declared more than once", kind,
			                            property.property_name);
		}
	}
}

static string NativePropertyProjection(const vector<GraphHeaderField> &properties, const string &alias) {
	string result;
	for (const auto &property : properties) {
		result += ", " + NativeValueExpression(property, alias) + " AS " + GqlQuoteIdentifier(property.property_name);
	}
	return result;
}

static string NativeLabelExpression(const GraphHeaderSchema &schema, const string &alias) {
	if (schema.labels.empty()) {
		return "[]::VARCHAR[]";
	}
	auto label = "CAST(" + Column(alias, schema.labels[0]) + " AS VARCHAR)";
	return "CASE WHEN " + label + " IS NULL OR trim(" + label +
	       ") = '' THEN []::VARCHAR[] ELSE list_filter(list_transform(string_split(" + label +
	       ", ';'), item -> lower(trim(item))), item -> item <> '') END";
}

static void ValidateNativeInput(Connection &connection, const GraphHeaderSchema &nodes,
                                const GraphHeaderSchema &edges) {
	if (nodes.labels.size() > 1) {
		throw InvalidInputException("COPY GRAPH currently supports at most one node :LABEL column");
	}
	auto node_id = ExternalId("n", nodes.id);
	auto node_validation =
	    GqlQuery(connection, "SELECT count(*), count(*) FILTER (WHERE " + node_id + " IS NULL OR trim(" + node_id +
	                             ") = ''), count(DISTINCT " + node_id + ") FROM gql_copy_nodes n");
	auto node_count = node_validation->GetValue(0, 0).GetValue<int64_t>();
	if (node_validation->GetValue(1, 0).GetValue<int64_t>() != 0) {
		throw InvalidInputException("Graph-header node :ID values must be non-null and non-empty");
	}
	if (node_validation->GetValue(2, 0).GetValue<int64_t>() != node_count) {
		throw InvalidInputException("Graph-header node :ID values must be unique");
	}
	auto relationship_type = ExternalId("r", edges.types[0]);
	auto start_id = ExternalId("r", edges.start_id);
	auto end_id = ExternalId("r", edges.end_id);
	auto edge_validation = GqlQuery(
	    connection, "SELECT count(*) FILTER (WHERE " + relationship_type + " IS NULL OR trim(" + relationship_type +
	                    ") = ''), count(*) FILTER (WHERE " + start_id + " IS NULL OR " + end_id + " IS NULL OR n." +
	                    GqlQuoteIdentifier(IMPORT_ROW_ID) + " IS NULL OR n2." + GqlQuoteIdentifier(IMPORT_ROW_ID) +
	                    " IS NULL) FROM gql_copy_edges r LEFT JOIN gql_copy_nodes n ON " + start_id + " = " + node_id +
	                    " LEFT JOIN gql_copy_nodes n2 ON " + end_id + " = " + ExternalId("n2", nodes.id));
	if (edge_validation->GetValue(0, 0).GetValue<int64_t>() != 0) {
		throw InvalidInputException("COPY GRAPH relationship :TYPE values must be non-null and non-empty");
	}
	if (edge_validation->GetValue(1, 0).GetValue<int64_t>() != 0) {
		throw InvalidInputException("COPY GRAPH relationship endpoints must reference imported node :ID values");
	}
}

static void CopyGraph(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<CopyGraphState>();
	if (state.done) {
		return;
	}
	if (!context.transaction.IsAutoCommit()) {
		throw NotImplementedException("COPY GRAPH is not eligible inside an explicit transaction");
	}
	auto &data = input.bind_data->Cast<CopyGraphBindData>();
	Connection connection(*context.db);
	GqlEnsureStorage(connection);
	idx_t vertex_count = 0;
	idx_t edge_count = 0;
	string qualified_vertex;
	string qualified_edge;
	connection.BeginTransaction();
	try {
		auto graph =
		    GqlQuery(connection, "SELECT g.graph_id, gs.storage_mode, "
		                         "(SELECT count(*) FROM gql_internal.graph_element_tables et WHERE et.graph_id = "
		                         "g.graph_id) FROM gql_internal.graphs g JOIN gql_internal.graph_storage gs "
		                         "USING (graph_id) WHERE g.graph_name = " +
		                             GqlQuoteLiteral(data.graph_name));
		if (graph->RowCount() == 0) {
			throw InvalidInputException("Graph '%s' does not exist; create it before COPY GRAPH", data.graph_name);
		}
		if (graph->GetValue(1, 0).GetValue<string>() != "EMPTY" || graph->GetValue(2, 0).GetValue<int64_t>() != 0) {
			throw InvalidInputException("Graph '%s' must be empty before COPY GRAPH", data.graph_name);
		}
		auto graph_id = graph->GetValue(0, 0).GetValue<uint64_t>();
		CreateRawTable(connection, "gql_copy_nodes", data.vertex_path, InferFileFormat(data.vertex_path));
		CreateRawTable(connection, "gql_copy_edges", data.edge_path, InferFileFormat(data.edge_path));
		auto nodes = ReadSchema(connection, "gql_copy_nodes", false);
		auto edges = ReadSchema(connection, "gql_copy_edges", true);
		if (edges.start_id.id_group != nodes.id.id_group || edges.end_id.id_group != nodes.id.id_group) {
			throw InvalidInputException("COPY GRAPH relationship ID groups must match the node :ID group");
		}
		if (nodes.labels.size() > 1) {
			throw InvalidInputException("COPY GRAPH currently supports at most one node :LABEL column");
		}
		ValidateNativeProperties(nodes.properties, "vertex");
		ValidateNativeProperties(edges.properties, "edge");
		if (data.validate) {
			ValidateNativeInput(connection, nodes, edges);
		}

		GqlQuery(connection, "CREATE SCHEMA IF NOT EXISTS gql_data");
		auto vertex_table = "graph_" + to_string(graph_id) + "_vertices";
		auto edge_table = "graph_" + to_string(graph_id) + "_edges";
		auto current_catalog = GqlQuery(connection, "SELECT current_database()")->GetValue(0, 0).GetValue<string>();
		qualified_vertex = GqlQuoteIdentifier(current_catalog) + ".gql_data." + GqlQuoteIdentifier(vertex_table);
		qualified_edge = GqlQuoteIdentifier(current_catalog) + ".gql_data." + GqlQuoteIdentifier(edge_table);

		GqlQuery(connection, "CREATE TABLE " + qualified_vertex + " AS SELECT n." + GqlQuoteIdentifier(IMPORT_ROW_ID) +
		                         " AS " + GqlQuoteIdentifier("__gql_id") + ", " + NativeLabelExpression(nodes, "n") +
		                         " AS " + GqlQuoteIdentifier("__gql_label") +
		                         NativePropertyProjection(nodes.properties, "n") + " FROM gql_copy_nodes n");
		vertex_count = ScalarCount(connection, "SELECT count(*) FROM " + qualified_vertex);
		GqlQuery(connection, "CREATE SEQUENCE gql_internal." +
		                         GqlQuoteIdentifier("graph_" + to_string(graph_id) + "_vertex_id_seq") + " START " +
		                         to_string(vertex_count + 1));

		auto start_id = ExternalId("r", edges.start_id);
		auto end_id = ExternalId("r", edges.end_id);
		auto relationship_type = ExternalId("r", edges.types[0]);
		GqlQuery(connection, "CREATE TABLE " + qualified_edge + " AS SELECT row_number() OVER ()::UBIGINT AS " +
		                         GqlQuoteIdentifier("__gql_edge_id") + ", s." + GqlQuoteIdentifier(IMPORT_ROW_ID) +
		                         " AS " + GqlQuoteIdentifier("__gql_source_id") + ", t." +
		                         GqlQuoteIdentifier(IMPORT_ROW_ID) + " AS " + GqlQuoteIdentifier("__gql_target_id") +
		                         ", lower(trim(" + relationship_type + ")) AS " + GqlQuoteIdentifier("__gql_type") +
		                         NativePropertyProjection(edges.properties, "r") +
		                         " FROM gql_copy_edges r LEFT JOIN gql_copy_nodes s ON " + start_id + " = " +
		                         ExternalId("s", nodes.id) + " LEFT JOIN gql_copy_nodes t ON " + end_id + " = " +
		                         ExternalId("t", nodes.id));
		edge_count = ScalarCount(connection, "SELECT count(*) FROM " + qualified_edge);
		GqlQuery(connection, "CREATE SEQUENCE gql_internal." +
		                         GqlQuoteIdentifier("graph_" + to_string(graph_id) + "_edge_id_seq") + " START " +
		                         to_string(edge_count + 1));

		GqlAttachManagedGraphTables(connection, data.graph_name, qualified_vertex, "__gql_id", "__gql_label",
		                            qualified_edge, "__gql_edge_id", "__gql_source_id", "__gql_target_id", "__gql_type",
		                            false);
		connection.Commit();
	} catch (...) {
		if (connection.HasActiveTransaction()) {
			connection.Rollback();
		}
		throw;
	}
	output.SetCardinality(1);
	output.SetValue(0, 0, Value(true));
	output.SetValue(1, 0, Value(data.graph_name));
	output.SetValue(2, 0, Value::UBIGINT(vertex_count));
	output.SetValue(3, 0, Value::UBIGINT(edge_count));
	state.done = true;
}

TableFunction GqlCopyGraphFunction() {
	TableFunction function("gql_copy_graph_native",
	                       {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BOOLEAN},
	                       CopyGraph);
	function.bind = CopyGraphBind;
	function.init_global = CopyGraphInit;
	return function;
}

} // namespace duckdb
