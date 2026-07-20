#include "gql_import.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"

namespace duckdb {

static constexpr const char *IMPORT_ROW_ID = "__gql_neo4j_import_row_id";

enum class Neo4jFieldRole : uint8_t { PROPERTY, ID, LABEL, START_ID, END_ID, TYPE };

struct Neo4jField {
	string column_name;
	string property_name;
	string declared_type;
	string id_group;
	LogicalType source_type;
	Neo4jFieldRole role = Neo4jFieldRole::PROPERTY;
};

struct Neo4jSchema {
	Neo4jField id;
	Neo4jField start_id;
	Neo4jField end_id;
	vector<Neo4jField> labels;
	vector<Neo4jField> types;
	vector<Neo4jField> properties;
	bool has_id = false;
	bool has_start_id = false;
	bool has_end_id = false;
};

struct Neo4jImportBindData : TableFunctionData {
	Neo4jImportBindData(string graph_name_p, string node_path_p, string relationship_path_p, string format_p,
	                    bool has_relationships_p)
	    : graph_name(std::move(graph_name_p)), node_path(std::move(node_path_p)),
	      relationship_path(std::move(relationship_path_p)), format(std::move(format_p)),
	      has_relationships(has_relationships_p) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<Neo4jImportBindData>(graph_name, node_path, relationship_path, format, has_relationships);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto other = dynamic_cast<const Neo4jImportBindData *>(&other_p);
		return other && graph_name == other->graph_name && node_path == other->node_path &&
		       relationship_path == other->relationship_path && format == other->format &&
		       has_relationships == other->has_relationships;
	}

	string graph_name;
	string node_path;
	string relationship_path;
	string format;
	bool has_relationships;
};

struct Neo4jImportState : GlobalTableFunctionState {
	bool done = false;
};

static string QuoteLiteral(const string &value) {
	string result = "'";
	for (const auto character : value) {
		result += character == '\'' ? "''" : string(1, character);
	}
	return result + "'";
}

static string QuoteIdentifier(const string &value) {
	string result = "\"";
	for (const auto character : value) {
		result += character == '"' ? "\"\"" : string(1, character);
	}
	return result + "\"";
}

static string Trimmed(string value) {
	StringUtil::Trim(value);
	return value;
}

static void ThrowOnError(const MaterializedQueryResult &result) {
	if (result.HasError()) {
		throw InvalidInputException("Neo4j import failed: %s", result.GetError());
	}
}

static unique_ptr<MaterializedQueryResult> Query(Connection &connection, const string &sql) {
	auto result = connection.Query(sql);
	ThrowOnError(*result);
	return result;
}

static idx_t ScalarCount(Connection &connection, const string &sql) {
	auto result = Query(connection, sql);
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

static Neo4jField ParseField(const string &column_name, const LogicalType &source_type) {
	Neo4jField result;
	result.column_name = column_name;
	result.source_type = source_type;
	string header = Trimmed(column_name);
	auto options = header.find('{');
	if (options != string::npos) {
		if (header.back() != '}') {
			throw InvalidInputException("Invalid Neo4j header field '%s'", column_name);
		}
		header = Trimmed(header.substr(0, options));
	}

	string prefix;
	string group;
	if (ParseSpecialField(header, ":START_ID", prefix, group)) {
		result.role = Neo4jFieldRole::START_ID;
		result.id_group = group;
		return result;
	}
	if (ParseSpecialField(header, ":END_ID", prefix, group)) {
		result.role = Neo4jFieldRole::END_ID;
		result.id_group = group;
		return result;
	}
	if (ParseSpecialField(header, ":LABEL", prefix, group)) {
		result.role = Neo4jFieldRole::LABEL;
		return result;
	}
	if (ParseSpecialField(header, ":TYPE", prefix, group)) {
		result.role = Neo4jFieldRole::TYPE;
		return result;
	}
	if (ParseSpecialField(header, ":ID", prefix, group)) {
		result.role = Neo4jFieldRole::ID;
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
		throw InvalidInputException("Neo4j property header '%s' has no property name", column_name);
	}
	return result;
}

static Neo4jSchema ReadSchema(Connection &connection, const string &table_name, bool relationships) {
	auto result = Query(connection, "SELECT * FROM " + QuoteIdentifier(table_name) + " LIMIT 0");
	Neo4jSchema schema;
	for (idx_t index = 0; index < result->names.size(); index++) {
		if (result->names[index] == IMPORT_ROW_ID) {
			continue;
		}
		auto field = ParseField(result->names[index], result->types[index]);
		if (field.role != Neo4jFieldRole::PROPERTY && field.source_type.IsNested()) {
			throw InvalidInputException("Neo4j structural field '%s' must be scalar in the initial importer",
			                            field.column_name);
		}
		switch (field.role) {
		case Neo4jFieldRole::ID:
			if (relationships) {
				throw InvalidInputException("Relationship :ID fields are not supported by the initial Neo4j importer");
			}
			if (schema.has_id) {
				throw InvalidInputException("Neo4j node input must contain exactly one :ID field");
			}
			schema.id = field;
			schema.has_id = true;
			if (!field.property_name.empty()) {
				schema.properties.push_back(field);
			}
			break;
		case Neo4jFieldRole::START_ID:
			if (!relationships || schema.has_start_id) {
				throw InvalidInputException("Neo4j relationship input must contain exactly one :START_ID field");
			}
			schema.start_id = field;
			schema.has_start_id = true;
			break;
		case Neo4jFieldRole::END_ID:
			if (!relationships || schema.has_end_id) {
				throw InvalidInputException("Neo4j relationship input must contain exactly one :END_ID field");
			}
			schema.end_id = field;
			schema.has_end_id = true;
			break;
		case Neo4jFieldRole::LABEL:
			if (relationships) {
				throw InvalidInputException("Use :TYPE, not :LABEL, for Neo4j relationships");
			}
			schema.labels.push_back(field);
			break;
		case Neo4jFieldRole::TYPE:
			if (!relationships) {
				throw InvalidInputException("Neo4j node input cannot contain a :TYPE field");
			}
			schema.types.push_back(field);
			break;
		case Neo4jFieldRole::PROPERTY:
			schema.properties.push_back(field);
			break;
		}
	}
	if (!relationships && !schema.has_id) {
		throw InvalidInputException("Neo4j node input requires one :ID field");
	}
	if (relationships && (!schema.has_start_id || !schema.has_end_id || schema.types.size() != 1)) {
		throw InvalidInputException(
		    "Neo4j relationship input requires :START_ID, :END_ID, and exactly one :TYPE field");
	}
	return schema;
}

static string ScanExpression(const string &path, const string &format) {
	if (format == "csv") {
		return "read_csv(" + QuoteLiteral(path) + ", header = true, auto_detect = true, all_varchar = true)";
	}
	return "read_parquet(" + QuoteLiteral(path) + ")";
}

static void CreateRawTable(Connection &connection, const string &table_name, const string &path, const string &format) {
	Query(connection, "CREATE TEMP TABLE " + QuoteIdentifier(table_name) +
	                      " AS SELECT row_number() OVER ()::UBIGINT AS " + QuoteIdentifier(IMPORT_ROW_ID) +
	                      ", * FROM " + ScanExpression(path, format));
}

static string Column(const string &alias, const Neo4jField &field) {
	return alias + "." + QuoteIdentifier(field.column_name);
}

static string ExternalId(const string &alias, const Neo4jField &field) {
	return "CAST(" + Column(alias, field) + " AS VARCHAR)";
}

static void ValidateNodeIds(Connection &connection, const string &table_name, const Neo4jField &id) {
	auto id_expression = ExternalId("n", id);
	if (ScalarCount(connection, "SELECT count(*) FROM " + QuoteIdentifier(table_name) + " n WHERE " + id_expression +
	                                " IS NULL OR trim(" + id_expression + ") = ''") != 0) {
		throw InvalidInputException("Neo4j node :ID values must be non-null and non-empty");
	}
	if (ScalarCount(connection, "SELECT count(*) FROM (SELECT " + id_expression + " FROM " +
	                                QuoteIdentifier(table_name) + " n GROUP BY ALL HAVING count(*) > 1) duplicates") !=
	    0) {
		throw InvalidInputException("Neo4j node :ID values must be unique");
	}
}

static string ValueExpression(const Neo4jField &field, const string &alias) {
	auto source = Column(alias, field);
	auto type = StringUtil::Lower(field.declared_type);
	if (EndsWith(type, "[]")) {
		throw InvalidInputException("Neo4j list property '%s' is not supported yet", field.property_name);
	}
	if (!type.empty()) {
		if (type == "boolean" || type == "bool") {
			return "union_value(bool_value := CAST(" + source + " AS BOOLEAN))";
		}
		if (type == "byte" || type == "short" || type == "int" || type == "integer" || type == "long") {
			return "union_value(int_value := CAST(" + source + " AS BIGINT))";
		}
		if (type == "float" || type == "double") {
			return "union_value(double_value := CAST(" + source + " AS DOUBLE))";
		}
		if (type == "string" || type == "char") {
			return "union_value(string_value := CAST(" + source + " AS VARCHAR))";
		}
		if (type == "date") {
			return "union_value(date_value := CAST(" + source + " AS DATE))";
		}
		if (type == "localtime") {
			return "union_value(time_value := CAST(" + source + " AS TIME))";
		}
		if (type == "localdatetime") {
			return "union_value(timestamp_value := CAST(" + source + " AS TIMESTAMP))";
		}
		if (type == "datetime") {
			return "union_value(timestamptz_value := CAST(" + source + " AS TIMESTAMPTZ))";
		}
		throw InvalidInputException("Neo4j property type '%s' is not supported yet", field.declared_type);
	}

	switch (field.source_type.id()) {
	case LogicalTypeId::BOOLEAN:
		return "union_value(bool_value := CAST(" + source + " AS BOOLEAN))";
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
		return "union_value(int_value := CAST(" + source + " AS BIGINT))";
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
		return "union_value(uint_value := CAST(" + source + " AS UBIGINT))";
	case LogicalTypeId::DECIMAL:
		return "union_value(decimal_value := CAST(" + source + " AS DECIMAL(38,18)))";
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		return "union_value(double_value := CAST(" + source + " AS DOUBLE))";
	case LogicalTypeId::DATE:
		return "union_value(date_value := CAST(" + source + " AS DATE))";
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
		return "union_value(time_value := CAST(" + source + " AS TIME))";
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_NS:
		return "union_value(timestamp_value := CAST(" + source + " AS TIMESTAMP))";
	case LogicalTypeId::TIMESTAMP_TZ:
		return "union_value(timestamptz_value := CAST(" + source + " AS TIMESTAMPTZ))";
	case LogicalTypeId::INTERVAL:
		return "union_value(interval_value := CAST(" + source + " AS INTERVAL))";
	case LogicalTypeId::BLOB:
		return "union_value(blob_value := CAST(" + source + " AS BLOB))";
	case LogicalTypeId::CHAR:
	case LogicalTypeId::VARCHAR:
		return "union_value(string_value := CAST(" + source + " AS VARCHAR))";
	default:
		throw InvalidInputException("Neo4j property '%s' has unsupported DuckDB type %s", field.property_name,
		                            field.source_type.ToString());
	}
}

static void InsertProperties(Connection &connection, const string &graph_id, const string &raw_table,
                             const string &map_table, const vector<Neo4jField> &properties) {
	if (properties.empty()) {
		return;
	}
	for (const auto &property : properties) {
		Query(connection, "INSERT INTO gql_internal.property_keys (graph_id, key_name) VALUES (" + graph_id + ", " +
		                      QuoteLiteral(property.property_name) + ") ON CONFLICT DO NOTHING");
		auto source = Column("r", property);
		Query(connection, "INSERT INTO gql_internal.object_properties (graph_id, object_id, key_id, value) SELECT " +
		                      graph_id + ", m.object_id, k.key_id, CAST(" + ValueExpression(property, "r") +
		                      " AS gql_internal.property_value) FROM " + QuoteIdentifier(raw_table) + " r JOIN " +
		                      QuoteIdentifier(map_table) + " m USING (" + QuoteIdentifier(IMPORT_ROW_ID) +
		                      ") JOIN gql_internal.property_keys k ON k.graph_id = " + graph_id + " AND k.key_name = " +
		                      QuoteLiteral(property.property_name) + " WHERE " + source + " IS NOT NULL");
	}
}

static void InsertLabels(Connection &connection, const string &graph_id, const string &raw_table,
                         const string &map_table, const Neo4jField &field, bool split_values) {
	auto cast_value = "CAST(" + Column("r", field) + " AS VARCHAR)";
	auto values = split_values ? "unnest(string_split(" + cast_value + ", ';'))" : "unnest([" + cast_value + "])";
	Query(connection,
	      "INSERT INTO gql_internal.labels (graph_id, label_name) SELECT DISTINCT " + graph_id +
	          ", lower(trim(label_name)) FROM " + QuoteIdentifier(raw_table) + " r, " + values +
	          " labels(label_name) WHERE label_name IS NOT NULL AND trim(label_name) <> '' ON CONFLICT DO NOTHING");
	Query(connection, "INSERT INTO gql_internal.object_labels (graph_id, object_id, label_id) SELECT DISTINCT " +
	                      graph_id + ", m.object_id, l.label_id FROM " + QuoteIdentifier(raw_table) + " r JOIN " +
	                      QuoteIdentifier(map_table) + " m USING (" + QuoteIdentifier(IMPORT_ROW_ID) + "), " + values +
	                      " labels(label_name) JOIN gql_internal.labels l ON l.graph_id = " + graph_id +
	                      " AND l.label_name = lower(trim(labels.label_name)) WHERE labels.label_name IS NOT NULL AND "
	                      "trim(labels.label_name) <> ''");
}

static unique_ptr<FunctionData> Neo4jImportBind(ClientContext &, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 4 || input.inputs[0].IsNull() || input.inputs[1].IsNull() || input.inputs[3].IsNull()) {
		throw BinderException("gql_load_graph requires graph_name, node_path, optional relationship_path, and format");
	}
	auto graph_name = input.inputs[0].GetValue<string>();
	auto node_path = input.inputs[1].GetValue<string>();
	auto has_relationships = !input.inputs[2].IsNull() && !input.inputs[2].GetValue<string>().empty();
	auto relationship_path = has_relationships ? input.inputs[2].GetValue<string>() : string();
	auto format = StringUtil::Lower(input.inputs[3].GetValue<string>());
	if (format != "csv" && format != "parquet") {
		throw BinderException("gql_load_graph format must be 'csv' or 'parquet'");
	}
	if (graph_name.empty() || node_path.empty()) {
		throw BinderException("gql_load_graph graph_name and node_path must be non-empty");
	}
	names = {"success", "graph_name", "vertex_count", "edge_count"};
	return_types = {LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::UBIGINT, LogicalType::UBIGINT};
	return make_uniq<Neo4jImportBindData>(graph_name, node_path, relationship_path, format, has_relationships);
}

static unique_ptr<GlobalTableFunctionState> Neo4jImportInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<Neo4jImportState>();
}

static void Neo4jImport(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<Neo4jImportState>();
	if (state.done) {
		return;
	}
	auto &data = input.bind_data->Cast<Neo4jImportBindData>();
	Connection connection(*context.db);
	idx_t vertex_count = 0;
	idx_t edge_count = 0;
	connection.BeginTransaction();
	try {
		auto graph = Query(connection, "SELECT graph_id FROM gql_internal.graphs WHERE graph_name = " +
		                                   QuoteLiteral(data.graph_name));
		if (graph->RowCount() == 0) {
			throw InvalidInputException("Graph '%s' does not exist; create it before importing", data.graph_name);
		}
		auto graph_id = graph->GetValue(0, 0).ToString();
		if (ScalarCount(connection, "SELECT count(*) FROM gql_internal.objects WHERE graph_id = " + graph_id) != 0) {
			throw InvalidInputException(
			    "Graph '%s' is not empty; the initial Neo4j importer only supports full imports", data.graph_name);
		}

		CreateRawTable(connection, "gql_neo4j_nodes", data.node_path, data.format);
		auto node_schema = ReadSchema(connection, "gql_neo4j_nodes", false);
		ValidateNodeIds(connection, "gql_neo4j_nodes", node_schema.id);
		vertex_count = ScalarCount(connection, "SELECT count(*) FROM gql_neo4j_nodes");
		Query(connection,
		      "CREATE TEMP TABLE gql_neo4j_node_map AS SELECT " + QuoteIdentifier(IMPORT_ROW_ID) + ", CAST(" +
		          QuoteIdentifier(node_schema.id.column_name) +
		          " AS VARCHAR) AS external_id, nextval('gql_internal.object_id_seq')::UBIGINT AS object_id "
		          "FROM gql_neo4j_nodes");
		Query(connection,
		      "INSERT INTO gql_internal.objects (object_id, graph_id, kind, source_id, target_id) SELECT object_id, " +
		          graph_id + ", 0, NULL, NULL FROM gql_neo4j_node_map");
		for (const auto &label : node_schema.labels) {
			InsertLabels(connection, graph_id, "gql_neo4j_nodes", "gql_neo4j_node_map", label, true);
		}
		InsertProperties(connection, graph_id, "gql_neo4j_nodes", "gql_neo4j_node_map", node_schema.properties);

		if (data.has_relationships) {
			CreateRawTable(connection, "gql_neo4j_relationships", data.relationship_path, data.format);
			auto relationship_schema = ReadSchema(connection, "gql_neo4j_relationships", true);
			if (relationship_schema.start_id.id_group != node_schema.id.id_group ||
			    relationship_schema.end_id.id_group != node_schema.id.id_group) {
				throw InvalidInputException("Neo4j relationship ID groups must match the node :ID group");
			}
			auto start_id = ExternalId("r", relationship_schema.start_id);
			auto end_id = ExternalId("r", relationship_schema.end_id);
			auto relationship_type = ExternalId("r", relationship_schema.types[0]);
			if (ScalarCount(connection, "SELECT count(*) FROM gql_neo4j_relationships r WHERE " + relationship_type +
			                                " IS NULL OR trim(" + relationship_type + ") = ''") != 0) {
				throw InvalidInputException("Neo4j relationship :TYPE values must be non-null and non-empty");
			}
			if (ScalarCount(connection,
			                "SELECT count(*) FROM gql_neo4j_relationships r LEFT JOIN gql_neo4j_node_map s ON " +
			                    start_id + " = s.external_id LEFT JOIN gql_neo4j_node_map t ON " + end_id +
			                    " = t.external_id WHERE " + start_id + " IS NULL OR " + end_id +
			                    " IS NULL OR s.object_id IS NULL OR t.object_id IS NULL") != 0) {
				throw InvalidInputException("Neo4j relationship endpoints must reference imported node :ID values");
			}
			edge_count = ScalarCount(connection, "SELECT count(*) FROM gql_neo4j_relationships");
			Query(connection,
			      "CREATE TEMP TABLE gql_neo4j_relationship_map AS SELECT r." + QuoteIdentifier(IMPORT_ROW_ID) +
			          ", nextval('gql_internal.object_id_seq')::UBIGINT AS object_id, s.object_id AS source_id, "
			          "t.object_id AS target_id FROM gql_neo4j_relationships r JOIN gql_neo4j_node_map s ON " +
			          start_id + " = s.external_id JOIN gql_neo4j_node_map t ON " + end_id + " = t.external_id");
			Query(connection, "INSERT INTO gql_internal.objects (object_id, graph_id, kind, source_id, target_id) "
			                  "SELECT object_id, " +
			                      graph_id + ", 1, source_id, target_id FROM gql_neo4j_relationship_map");
			InsertLabels(connection, graph_id, "gql_neo4j_relationships", "gql_neo4j_relationship_map",
			             relationship_schema.types[0], false);
			InsertProperties(connection, graph_id, "gql_neo4j_relationships", "gql_neo4j_relationship_map",
			                 relationship_schema.properties);
		}

		Query(connection,
		      "UPDATE gql_internal.graphs SET graph_version = graph_version + 1 WHERE graph_id = " + graph_id);
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

TableFunction GqlGraphImportFunction() {
	TableFunction function("gql_load_graph",
	                       {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                       Neo4jImport);
	function.bind = Neo4jImportBind;
	function.init_global = Neo4jImportInit;
	return function;
}

} // namespace duckdb
