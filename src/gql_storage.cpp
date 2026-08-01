#include "gql_storage.hpp"

#include "gql_catalog.hpp"
#include "gql_sql_utils.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {

static constexpr const char *GQL_STATE_KEY = "gql_client_state";

struct GqlClientState : ClientContextState {
	void QueryEnd(ClientContext &context, optional_ptr<ErrorData> error) override {
		if (!error || !error->HasError()) {
			return;
		}
		auto owned_transaction = mutation_owned_transaction;
		mutation_owned_transaction = false;
		mutation_command_id.clear();
		if (owned_transaction && context.transaction.HasActiveTransaction()) {
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

void GqlEnsureStorage(Connection &connection) {
	GqlQuery(connection, "CREATE SCHEMA IF NOT EXISTS gql_internal");
	GqlQuery(connection, "CREATE SEQUENCE IF NOT EXISTS gql_internal.graph_id_seq START 1");
	GqlQuery(connection, "CREATE SEQUENCE IF NOT EXISTS gql_internal.element_table_id_seq START 1");
	GqlQuery(connection, "CREATE SEQUENCE IF NOT EXISTS gql_internal.label_mapping_id_seq START 1");
	GqlQuery(connection, "CREATE SEQUENCE IF NOT EXISTS gql_internal.property_index_id_seq START 1");
	GqlQuery(connection, "CREATE SEQUENCE IF NOT EXISTS gql_internal.schema_element_id_seq START 1");
	GqlQuery(connection, "CREATE TABLE IF NOT EXISTS gql_internal.graphs ("
	                     "graph_id UBIGINT PRIMARY KEY DEFAULT nextval('gql_internal.graph_id_seq'), "
	                     "graph_name VARCHAR NOT NULL UNIQUE, "
	                     "graph_version UBIGINT NOT NULL DEFAULT 0, "
	                     "created_at TIMESTAMP NOT NULL DEFAULT current_timestamp)");
	GqlQuery(connection, "CREATE TABLE IF NOT EXISTS gql_internal.graph_storage ("
	                     "graph_id UBIGINT PRIMARY KEY, "
	                     "storage_mode VARCHAR NOT NULL, "
	                     "default_catalog VARCHAR, "
	                     "default_schema VARCHAR, "
	                     "schema_version UBIGINT NOT NULL DEFAULT 0, "
	                     "csr_policy VARCHAR NOT NULL DEFAULT 'DISABLED', "
	                     "CHECK (storage_mode IN ('EMPTY', 'TABLE_BACKED')), "
	                     "CHECK (csr_policy IN ('DISABLED', 'MANUAL', 'AUTO')))");
	GqlQuery(connection, "CREATE TABLE IF NOT EXISTS gql_internal.graph_schemas ("
	                     "graph_id UBIGINT PRIMARY KEY, "
	                     "schema_kind VARCHAR NOT NULL, "
	                     "is_typed BOOLEAN NOT NULL, "
	                     "CHECK (schema_kind IN ('ANY', 'INLINE')))");
	GqlQuery(connection, "CREATE TABLE IF NOT EXISTS gql_internal.graph_schema_elements ("
	                     "schema_element_id UBIGINT PRIMARY KEY DEFAULT "
	                     "nextval('gql_internal.schema_element_id_seq'), "
	                     "graph_id UBIGINT NOT NULL, "
	                     "element_ordinal UBIGINT NOT NULL, "
	                     "element_kind VARCHAR NOT NULL, "
	                     "type_name VARCHAR NOT NULL, "
	                     "local_alias VARCHAR, "
	                     "source_alias VARCHAR, "
	                     "target_alias VARCHAR, "
	                     "direction VARCHAR NOT NULL, "
	                     "UNIQUE(graph_id, element_kind, type_name), "
	                     "UNIQUE(graph_id, element_ordinal), "
	                     "CHECK (element_kind IN ('NODE', 'EDGE')), "
	                     "CHECK (direction IN ('NONE', 'RIGHT', 'LEFT', 'ANY')))");
	GqlQuery(connection, "CREATE TABLE IF NOT EXISTS gql_internal.graph_schema_labels ("
	                     "schema_element_id UBIGINT NOT NULL, "
	                     "label_ordinal UBIGINT NOT NULL, "
	                     "label_name VARCHAR NOT NULL, "
	                     "PRIMARY KEY(schema_element_id, label_name), "
	                     "UNIQUE(schema_element_id, label_ordinal))");
	GqlQuery(connection, "CREATE TABLE IF NOT EXISTS gql_internal.graph_schema_properties ("
	                     "schema_element_id UBIGINT NOT NULL, "
	                     "property_ordinal UBIGINT NOT NULL, "
	                     "property_name VARCHAR NOT NULL, "
	                     "gql_type VARCHAR NOT NULL, "
	                     "nullable BOOLEAN NOT NULL, "
	                     "PRIMARY KEY(schema_element_id, property_name), "
	                     "UNIQUE(schema_element_id, property_ordinal))");
	auto native_storage_schema = GqlQuery(
	    connection, "SELECT count(*) FROM duckdb_constraints() WHERE schema_name = 'gql_internal' AND "
	                "table_name = 'graph_storage' AND constraint_type = 'CHECK' AND constraint_text LIKE '%EMPTY%'");
	if (native_storage_schema->GetValue(0, 0).GetValue<int64_t>() == 0) {
		throw InvalidInputException(
		    "This database uses the removed legacy EAV graph catalog; create a fresh database and reload graphs with "
		    "COPY GRAPH");
	}
	GqlQuery(connection, "CREATE TABLE IF NOT EXISTS gql_internal.graph_element_tables ("
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
	GqlQuery(connection, "CREATE TABLE IF NOT EXISTS gql_internal.graph_edge_endpoints ("
	                     "edge_table_id UBIGINT PRIMARY KEY, "
	                     "source_vertex_table_id UBIGINT NOT NULL, "
	                     "target_vertex_table_id UBIGINT NOT NULL, "
	                     "source_columns VARCHAR[] NOT NULL, "
	                     "target_columns VARCHAR[] NOT NULL, "
	                     "source_key_columns VARCHAR[] NOT NULL, "
	                     "target_key_columns VARCHAR[] NOT NULL)");
	GqlQuery(connection, "CREATE TABLE IF NOT EXISTS gql_internal.graph_label_mappings ("
	                     "label_mapping_id UBIGINT PRIMARY KEY DEFAULT nextval('gql_internal.label_mapping_id_seq'), "
	                     "element_table_id UBIGINT NOT NULL, "
	                     "label_name VARCHAR, "
	                     "mapping_kind VARCHAR NOT NULL, "
	                     "column_name VARCHAR, "
	                     "CHECK ((mapping_kind = 'STATIC' AND label_name IS NOT NULL AND column_name IS NULL) OR "
	                     "(mapping_kind IN ('SCALAR_COLUMN', 'LIST_COLUMN') AND label_name IS NULL AND "
	                     "column_name IS NOT NULL)))");
	GqlQuery(connection, "CREATE TABLE IF NOT EXISTS gql_internal.graph_property_mappings ("
	                     "element_table_id UBIGINT NOT NULL, "
	                     "property_name VARCHAR NOT NULL, "
	                     "column_name VARCHAR NOT NULL, "
	                     "gql_type VARCHAR NOT NULL, "
	                     "nullable BOOLEAN NOT NULL, "
	                     "writable BOOLEAN NOT NULL, "
	                     "PRIMARY KEY(element_table_id, property_name))");
	GqlQuery(connection, "CREATE TABLE IF NOT EXISTS gql_internal.graph_property_indexes ("
	                     "property_index_id UBIGINT PRIMARY KEY, "
	                     "element_table_id UBIGINT NOT NULL, "
	                     "property_name VARCHAR NOT NULL, "
	                     "column_name VARCHAR NOT NULL, "
	                     "index_name VARCHAR NOT NULL UNIQUE, "
	                     "created_at TIMESTAMP NOT NULL DEFAULT current_timestamp, "
	                     "UNIQUE(element_table_id, property_name))");
	GqlQuery(connection, "INSERT INTO gql_internal.graph_storage (graph_id, storage_mode, schema_version, csr_policy) "
	                     "SELECT graph_id, 'EMPTY', 0, 'DISABLED' FROM gql_internal.graphs ON CONFLICT DO NOTHING");
	GqlQuery(connection, "INSERT INTO gql_internal.graph_schemas (graph_id, schema_kind, is_typed) "
	                     "SELECT graph_id, 'ANY', false FROM gql_internal.graphs ON CONFLICT DO NOTHING");
}

static bool GraphExists(Connection &connection, const string &graph_name) {
	auto result = GqlQuery(connection, "SELECT count(*) FROM gql_internal.graphs WHERE graph_name = " +
	                                       GqlQuoteLiteral(graph_name));
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

struct GraphSchemaProperty {
	string name;
	string gql_type;
	bool nullable;

	bool operator==(const GraphSchemaProperty &other) const {
		return name == other.name && gql_type == other.gql_type && nullable == other.nullable;
	}
};

struct GraphSchemaElement {
	string kind;
	string type_name;
	string local_alias;
	string source_alias;
	string target_alias;
	string direction;
	vector<string> labels;
	vector<GraphSchemaProperty> properties;

	bool operator==(const GraphSchemaElement &other) const {
		return kind == other.kind && type_name == other.type_name && local_alias == other.local_alias &&
		       source_alias == other.source_alias && target_alias == other.target_alias &&
		       direction == other.direction && labels == other.labels && properties == other.properties;
	}
};

static string TypedPropertyDuckType(const string &gql_type) {
	auto type = StringUtil::Upper(gql_type);
	static const unordered_map<string, string> TYPE_MAP = {
	    {"BOOL", "BOOLEAN"},
	    {"BOOLEAN", "BOOLEAN"},
	    {"STRING", "VARCHAR"},
	    {"CHAR", "VARCHAR"},
	    {"VARCHAR", "VARCHAR"},
	    {"BYTES", "BLOB"},
	    {"BINARY", "BLOB"},
	    {"VARBINARY", "BLOB"},
	    {"INT8", "TINYINT"},
	    {"INTEGER8", "TINYINT"},
	    {"INT16", "SMALLINT"},
	    {"INTEGER16", "SMALLINT"},
	    {"SMALLINT", "SMALLINT"},
	    {"SMALLINTEGER", "SMALLINT"},
	    {"INT32", "INTEGER"},
	    {"INTEGER32", "INTEGER"},
	    {"INT", "INTEGER"},
	    {"INTEGER", "INTEGER"},
	    {"INT64", "BIGINT"},
	    {"INTEGER64", "BIGINT"},
	    {"BIGINT", "BIGINT"},
	    {"BIGINTEGER", "BIGINT"},
	    {"INT128", "HUGEINT"},
	    {"INTEGER128", "HUGEINT"},
	    {"UINT8", "UTINYINT"},
	    {"UINT16", "USMALLINT"},
	    {"USMALLINT", "USMALLINT"},
	    {"UINT32", "UINTEGER"},
	    {"UINT", "UINTEGER"},
	    {"UINT64", "UBIGINT"},
	    {"UBIGINT", "UBIGINT"},
	    {"UINT128", "UHUGEINT"},
	    {"FLOAT32", "FLOAT"},
	    {"REAL", "FLOAT"},
	    {"FLOAT", "FLOAT"},
	    {"FLOAT64", "DOUBLE"},
	    {"DOUBLE", "DOUBLE"},
	    {"DOUBLEPRECISION", "DOUBLE"},
	    {"DATE", "DATE"},
	    {"TIME", "TIME"},
	    {"LOCALTIME", "TIME"},
	    {"ZONEDTIME", "TIME WITH TIME ZONE"},
	    {"TIMEWITHTIMEZONE", "TIME WITH TIME ZONE"},
	    {"LOCALDATETIME", "TIMESTAMP"},
	    {"TIMESTAMP", "TIMESTAMP"},
	    {"TIMESTAMPWITHOUTTIMEZONE", "TIMESTAMP"},
	    {"ZONEDDATETIME", "TIMESTAMP WITH TIME ZONE"},
	    {"TIMESTAMPWITHTIMEZONE", "TIMESTAMP WITH TIME ZONE"},
	};
	auto entry = TYPE_MAP.find(type);
	if (entry != TYPE_MAP.end()) {
		return entry->second;
	}
	if (type == "DECIMAL" || type == "DEC") {
		return "DECIMAL";
	}
	if (StringUtil::StartsWith(type, "DECIMAL(") || StringUtil::StartsWith(type, "DEC(")) {
		auto open = type.find('(');
		if (type.back() == ')' && open != string::npos) {
			auto parameters = type.substr(open + 1, type.size() - open - 2);
			bool valid = !parameters.empty();
			idx_t commas = 0;
			for (auto character : parameters) {
				if (character == ',') {
					commas++;
				} else if (character < '0' || character > '9') {
					valid = false;
				}
			}
			if (valid && commas <= 1) {
				return "DECIMAL(" + parameters + ")";
			}
		}
	}
	throw BinderException("Typed graph property type '%s' cannot be materialized as a DuckDB column", gql_type);
}

struct TypedPhysicalProperty {
	string name;
	string gql_type;
	string duck_type;
};

static vector<TypedPhysicalProperty> CollectTypedProperties(const vector<GraphSchemaElement> &elements,
                                                            const string &kind) {
	vector<TypedPhysicalProperty> result;
	unordered_map<string, idx_t> indexes;
	for (const auto &element : elements) {
		if (element.kind != kind) {
			continue;
		}
		for (const auto &property : element.properties) {
			auto duck_type = TypedPropertyDuckType(property.gql_type);
			auto inserted = indexes.emplace(property.name, result.size());
			if (inserted.second) {
				result.push_back({property.name, property.gql_type, std::move(duck_type)});
			} else if (result[inserted.first->second].duck_type != duck_type) {
				throw BinderException("Typed graph property '%s' has incompatible types '%s' and '%s'", property.name,
				                      result[inserted.first->second].gql_type, property.gql_type);
			}
		}
	}
	return result;
}

static vector<string> TypedNodeLabels(const GraphSchemaElement &element) {
	if (!element.labels.empty()) {
		return element.labels;
	}
	return {element.type_name};
}

static string TypedNodeCondition(const GraphSchemaElement &element) {
	auto labels = TypedNodeLabels(element);
	string result = "len(" + GqlQuoteIdentifier("__gql_label") + ") = " + to_string(labels.size());
	for (const auto &label : labels) {
		result += " AND list_contains(" + GqlQuoteIdentifier("__gql_label") + ", " + GqlQuoteLiteral(label) + ")";
	}
	return "(" + result + ")";
}

static string TypedEdgeLabel(const GraphSchemaElement &element) {
	return element.labels.empty() ? element.type_name : element.labels[0];
}

static string TypedElementCondition(const GraphSchemaElement &element) {
	if (element.kind == "NODE") {
		return TypedNodeCondition(element);
	}
	return GqlQuoteIdentifier("__gql_type") + " = " + GqlQuoteLiteral(TypedEdgeLabel(element));
}

static void ValidateTypedMaterialization(const vector<GraphSchemaElement> &elements) {
	unordered_set<string> node_discriminators;
	unordered_set<string> edge_discriminators;
	for (const auto &element : elements) {
		for (const auto &property : element.properties) {
			if (StringUtil::StartsWith(property.name, "__gql_")) {
				throw BinderException("Typed graph property '%s' conflicts with a reserved storage column",
				                      property.name);
			}
			TypedPropertyDuckType(property.gql_type);
		}
		if (element.kind == "NODE") {
			auto labels = TypedNodeLabels(element);
			std::sort(labels.begin(), labels.end());
			auto discriminator = StringUtil::Join(labels, "\x1f");
			if (!node_discriminators.insert(discriminator).second) {
				throw BinderException("Typed graph node types must have distinct label sets");
			}
		} else {
			if (element.labels.size() > 1) {
				throw BinderException(
				    "Typed graph edge '%s' has multiple labels, but native edge storage supports one type",
				    element.type_name);
			}
			if (!edge_discriminators.insert(TypedEdgeLabel(element)).second) {
				throw BinderException("Typed graph edge types must have distinct labels");
			}
		}
	}
	CollectTypedProperties(elements, "NODE");
	CollectTypedProperties(elements, "EDGE");
}

static string TypedPropertyConstraints(const vector<GraphSchemaElement> &elements, const string &kind,
                                       const vector<TypedPhysicalProperty> &properties) {
	string result;
	for (const auto &element : elements) {
		if (element.kind != kind) {
			continue;
		}
		auto condition = TypedElementCondition(element);
		unordered_map<string, bool> declared;
		for (const auto &property : element.properties) {
			declared.emplace(property.name, property.nullable);
			if (!property.nullable) {
				result += ", CHECK (NOT " + condition + " OR " + GqlQuoteIdentifier(property.name) + " IS NOT NULL)";
			}
		}
		for (const auto &property : properties) {
			if (declared.find(property.name) == declared.end()) {
				result += ", CHECK (NOT " + condition + " OR " + GqlQuoteIdentifier(property.name) + " IS NULL)";
			}
		}
	}
	return result;
}

static string TypedDiscriminatorConstraint(const vector<GraphSchemaElement> &elements, const string &kind) {
	string expression;
	for (const auto &element : elements) {
		if (element.kind != kind) {
			continue;
		}
		if (!expression.empty()) {
			expression += " OR ";
		}
		expression += TypedElementCondition(element);
	}
	return ", CHECK (" + (expression.empty() ? "false" : expression) + ")";
}

static string TypedPropertyColumns(const vector<TypedPhysicalProperty> &properties) {
	string result;
	for (const auto &property : properties) {
		result += ", " + GqlQuoteIdentifier(property.name) + " " + property.duck_type;
	}
	return result;
}

static void MaterializeTypedGraph(Connection &connection, uint64_t graph_id, const string &graph_name,
                                  const vector<GraphSchemaElement> &elements) {
	auto vertex_properties = CollectTypedProperties(elements, "NODE");
	auto edge_properties = CollectTypedProperties(elements, "EDGE");
	GqlQuery(connection, "CREATE SCHEMA IF NOT EXISTS gql_data");
	auto current_catalog = GqlQuery(connection, "SELECT current_database()")->GetValue(0, 0).GetValue<string>();
	auto vertex_table = "graph_" + to_string(graph_id) + "_vertices";
	auto edge_table = "graph_" + to_string(graph_id) + "_edges";
	auto qualified_vertex = GqlQuoteIdentifier(current_catalog) + ".gql_data." + GqlQuoteIdentifier(vertex_table);
	auto qualified_edge = GqlQuoteIdentifier(current_catalog) + ".gql_data." + GqlQuoteIdentifier(edge_table);

	GqlQuery(connection, "CREATE TABLE " + qualified_vertex + " (" + GqlQuoteIdentifier("__gql_id") +
	                         " UBIGINT PRIMARY KEY, " + GqlQuoteIdentifier("__gql_label") + " VARCHAR[] NOT NULL" +
	                         TypedPropertyColumns(vertex_properties) + TypedDiscriminatorConstraint(elements, "NODE") +
	                         TypedPropertyConstraints(elements, "NODE", vertex_properties) + ")");
	GqlQuery(connection, "CREATE TABLE " + qualified_edge + " (" + GqlQuoteIdentifier("__gql_edge_id") +
	                         " UBIGINT PRIMARY KEY, " + GqlQuoteIdentifier("__gql_source_id") + " UBIGINT NOT NULL, " +
	                         GqlQuoteIdentifier("__gql_target_id") + " UBIGINT NOT NULL, " +
	                         GqlQuoteIdentifier("__gql_type") + " VARCHAR NOT NULL" +
	                         TypedPropertyColumns(edge_properties) + TypedDiscriminatorConstraint(elements, "EDGE") +
	                         TypedPropertyConstraints(elements, "EDGE", edge_properties) + ")");
	GqlQuery(connection, "CREATE SEQUENCE gql_internal." +
	                         GqlQuoteIdentifier("graph_" + to_string(graph_id) + "_vertex_id_seq") + " START 1");
	GqlQuery(connection, "CREATE SEQUENCE gql_internal." +
	                         GqlQuoteIdentifier("graph_" + to_string(graph_id) + "_edge_id_seq") + " START 1");
	GqlAttachManagedGraphTables(connection, graph_name, qualified_vertex, "__gql_id", "__gql_label", qualified_edge,
	                            "__gql_edge_id", "__gql_source_id", "__gql_target_id", "__gql_type", false);
}

struct CreateGraphBindData : TableFunctionData {
	CreateGraphBindData(string graph_name_p, bool conditional_p, string schema_kind_p, bool typed_p,
	                    vector<GraphSchemaElement> elements_p)
	    : graph_name(std::move(graph_name_p)), conditional(conditional_p), schema_kind(std::move(schema_kind_p)),
	      typed(typed_p), elements(std::move(elements_p)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<CreateGraphBindData>(graph_name, conditional, schema_kind, typed, elements);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto other = dynamic_cast<const CreateGraphBindData *>(&other_p);
		return other && graph_name == other->graph_name && conditional == other->conditional &&
		       schema_kind == other->schema_kind && typed == other->typed && elements == other->elements;
	}

	string graph_name;
	bool conditional;
	string schema_kind;
	bool typed;
	vector<GraphSchemaElement> elements;
};

struct PropertyIndexBindData : TableFunctionData {
	PropertyIndexBindData(string graph_name_p, string property_name_p)
	    : graph_name(std::move(graph_name_p)), property_name(std::move(property_name_p)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<PropertyIndexBindData>(graph_name, property_name);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto other = dynamic_cast<const PropertyIndexBindData *>(&other_p);
		return other && graph_name == other->graph_name && property_name == other->property_name;
	}

	string graph_name;
	string property_name;
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
	names = {"success", "graph_name"};
	return_types = {LogicalType::BOOLEAN, LogicalType::VARCHAR};
	return make_uniq<CommandBindData>(input.inputs[0].GetValue<string>(), input.inputs.size() > 1 &&
	                                                                          !input.inputs[1].IsNull() &&
	                                                                          input.inputs[1].GetValue<bool>());
}

static vector<string> ReadCreateGraphStrings(const Value &value) {
	vector<string> result;
	for (const auto &entry : ListValue::GetChildren(value)) {
		if (entry.IsNull()) {
			throw BinderException("Typed graph schema values cannot be NULL");
		}
		result.push_back(entry.GetValue<string>());
	}
	return result;
}

static vector<uint64_t> ReadCreateGraphIndices(const Value &value) {
	vector<uint64_t> result;
	for (const auto &entry : ListValue::GetChildren(value)) {
		if (entry.IsNull()) {
			throw BinderException("Typed graph schema element indices cannot be NULL");
		}
		result.push_back(entry.GetValue<uint64_t>());
	}
	return result;
}

static vector<bool> ReadCreateGraphBooleans(const Value &value) {
	vector<bool> result;
	for (const auto &entry : ListValue::GetChildren(value)) {
		if (entry.IsNull()) {
			throw BinderException("Typed graph schema flags cannot be NULL");
		}
		result.push_back(entry.GetValue<bool>());
	}
	return result;
}

static unique_ptr<FunctionData> CreateGraphBind(ClientContext &, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 16 || input.inputs[0].IsNull() || input.inputs[1].IsNull() || input.inputs[2].IsNull() ||
	    input.inputs[3].IsNull()) {
		throw BinderException("Invalid CREATE GRAPH schema payload");
	}
	auto graph_name = input.inputs[0].GetValue<string>();
	auto conditional = input.inputs[1].GetValue<bool>();
	auto schema_kind = input.inputs[2].GetValue<string>();
	auto typed = input.inputs[3].GetValue<bool>();
	auto element_kinds = ReadCreateGraphStrings(input.inputs[4]);
	auto type_names = ReadCreateGraphStrings(input.inputs[5]);
	auto local_aliases = ReadCreateGraphStrings(input.inputs[6]);
	auto source_aliases = ReadCreateGraphStrings(input.inputs[7]);
	auto target_aliases = ReadCreateGraphStrings(input.inputs[8]);
	auto directions = ReadCreateGraphStrings(input.inputs[9]);
	if (graph_name.empty()) {
		throw BinderException("A graph name is required");
	}
	if ((schema_kind != "ANY" && schema_kind != "INLINE") || (schema_kind == "ANY" && typed) ||
	    (schema_kind == "INLINE" && !typed)) {
		throw BinderException("Invalid CREATE GRAPH schema kind");
	}
	auto element_count = element_kinds.size();
	if (type_names.size() != element_count || local_aliases.size() != element_count ||
	    source_aliases.size() != element_count || target_aliases.size() != element_count ||
	    directions.size() != element_count || (schema_kind == "ANY" && element_count != 0) ||
	    (schema_kind == "INLINE" && element_count == 0)) {
		throw BinderException("Invalid typed graph element schema");
	}

	vector<GraphSchemaElement> elements(element_count);
	std::unordered_set<string> element_names;
	std::unordered_set<string> node_aliases;
	for (idx_t index = 0; index < element_count; index++) {
		auto &element = elements[index];
		element.kind = element_kinds[index];
		element.type_name = type_names[index];
		element.local_alias = local_aliases[index];
		element.source_alias = source_aliases[index];
		element.target_alias = target_aliases[index];
		element.direction = directions[index];
		if ((element.kind != "NODE" && element.kind != "EDGE") || element.type_name.empty() ||
		    (element.direction != "NONE" && element.direction != "RIGHT" && element.direction != "LEFT" &&
		     element.direction != "ANY") ||
		    !element_names.insert(element.kind + ":" + element.type_name).second) {
			throw BinderException("Invalid or duplicate typed graph element type '%s'", element.type_name);
		}
		if (element.kind == "NODE") {
			if (element.direction != "NONE" || element.local_alias.empty() || !element.source_alias.empty() ||
			    !element.target_alias.empty() || !node_aliases.insert(element.local_alias).second) {
				throw BinderException("Invalid or duplicate typed graph node alias '%s'", element.local_alias);
			}
		} else if (element.direction == "NONE" || element.source_alias.empty() || element.target_alias.empty()) {
			throw BinderException("Typed graph edge '%s' requires source and target node aliases", element.type_name);
		}
	}
	for (const auto &element : elements) {
		if (element.kind == "EDGE" && (node_aliases.find(element.source_alias) == node_aliases.end() ||
		                               node_aliases.find(element.target_alias) == node_aliases.end())) {
			throw BinderException("Typed graph edge '%s' references an unknown node alias", element.type_name);
		}
	}

	auto label_indices = ReadCreateGraphIndices(input.inputs[10]);
	auto label_names = ReadCreateGraphStrings(input.inputs[11]);
	if (label_indices.size() != label_names.size()) {
		throw BinderException("Invalid typed graph label schema");
	}
	vector<std::unordered_set<string>> labels_seen(element_count);
	for (idx_t index = 0; index < label_indices.size(); index++) {
		if (label_indices[index] >= element_count || label_names[index].empty() ||
		    !labels_seen[label_indices[index]].insert(label_names[index]).second) {
			throw BinderException("Invalid or duplicate typed graph label '%s'", label_names[index]);
		}
		elements[label_indices[index]].labels.push_back(label_names[index]);
	}

	auto property_indices = ReadCreateGraphIndices(input.inputs[12]);
	auto property_names = ReadCreateGraphStrings(input.inputs[13]);
	auto property_types = ReadCreateGraphStrings(input.inputs[14]);
	auto property_nullables = ReadCreateGraphBooleans(input.inputs[15]);
	if (property_names.size() != property_indices.size() || property_types.size() != property_indices.size() ||
	    property_nullables.size() != property_indices.size()) {
		throw BinderException("Invalid typed graph property schema");
	}
	vector<std::unordered_set<string>> properties_seen(element_count);
	for (idx_t index = 0; index < property_indices.size(); index++) {
		if (property_indices[index] >= element_count || property_names[index].empty() ||
		    property_types[index].empty() ||
		    !properties_seen[property_indices[index]].insert(property_names[index]).second) {
			throw BinderException("Invalid or duplicate typed graph property '%s'", property_names[index]);
		}
		elements[property_indices[index]].properties.push_back(
		    {property_names[index], property_types[index], property_nullables[index]});
	}
	if (typed) {
		ValidateTypedMaterialization(elements);
	}

	names = {"success", "graph_name"};
	return_types = {LogicalType::BOOLEAN, LogicalType::VARCHAR};
	return make_uniq<CreateGraphBindData>(std::move(graph_name), conditional, std::move(schema_kind), typed,
	                                      std::move(elements));
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

static unique_ptr<FunctionData> PropertyIndexBind(ClientContext &, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 2 || input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("A graph name and vertex property name are required");
	}
	auto graph_name = input.inputs[0].GetValue<string>();
	auto property_name = input.inputs[1].GetValue<string>();
	if (graph_name.empty() || property_name.empty()) {
		throw BinderException("Graph and vertex property names must be non-empty");
	}
	names = {"success", "graph_name", "property_name", "index_name"};
	return_types = {LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	return make_uniq<PropertyIndexBindData>(std::move(graph_name), std::move(property_name));
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
	if (!context.transaction.IsAutoCommit()) {
		throw NotImplementedException("CREATE GRAPH is not eligible inside an explicit transaction");
	}
	auto &data = input.bind_data->Cast<CreateGraphBindData>();
	Connection connection(*context.db);
	GqlEnsureStorage(connection);
	if (GraphExists(connection, data.graph_name)) {
		if (!data.conditional) {
			throw InvalidInputException("Graph '%s' already exists", data.graph_name);
		}
		EmitCommandResult(output, state, data.graph_name);
		return;
	}
	connection.BeginTransaction();
	try {
		auto graph_id = GqlQuery(connection, "INSERT INTO gql_internal.graphs (graph_name) VALUES (" +
		                                         GqlQuoteLiteral(data.graph_name) + ") RETURNING graph_id")
		                    ->GetValue(0, 0)
		                    .GetValue<uint64_t>();
		GqlQuery(connection, "INSERT INTO gql_internal.graph_storage "
		                     "(graph_id, storage_mode, schema_version, csr_policy) VALUES (" +
		                         to_string(graph_id) + ", 'EMPTY', 0, 'DISABLED')");
		GqlQuery(connection, "INSERT INTO gql_internal.graph_schemas (graph_id, schema_kind, is_typed) VALUES (" +
		                         to_string(graph_id) + ", " + GqlQuoteLiteral(data.schema_kind) + ", " +
		                         (data.typed ? "true" : "false") + ")");
		for (idx_t element_index = 0; element_index < data.elements.size(); element_index++) {
			auto &element = data.elements[element_index];
			auto local_alias = element.local_alias.empty() ? "NULL" : GqlQuoteLiteral(element.local_alias);
			auto source_alias = element.source_alias.empty() ? "NULL" : GqlQuoteLiteral(element.source_alias);
			auto target_alias = element.target_alias.empty() ? "NULL" : GqlQuoteLiteral(element.target_alias);
			auto schema_element_id =
			    GqlQuery(connection, "INSERT INTO gql_internal.graph_schema_elements "
			                         "(graph_id, element_ordinal, element_kind, type_name, local_alias, source_alias, "
			                         "target_alias, direction) VALUES (" +
			                             to_string(graph_id) + ", " + to_string(element_index) + ", " +
			                             GqlQuoteLiteral(element.kind) + ", " + GqlQuoteLiteral(element.type_name) +
			                             ", " + local_alias + ", " + source_alias + ", " + target_alias + ", " +
			                             GqlQuoteLiteral(element.direction) + ") RETURNING schema_element_id")
			        ->GetValue(0, 0)
			        .GetValue<uint64_t>();
			for (idx_t label_index = 0; label_index < element.labels.size(); label_index++) {
				GqlQuery(connection, "INSERT INTO gql_internal.graph_schema_labels "
				                     "(schema_element_id, label_ordinal, label_name) VALUES (" +
				                         to_string(schema_element_id) + ", " + to_string(label_index) + ", " +
				                         GqlQuoteLiteral(element.labels[label_index]) + ")");
			}
			for (idx_t property_index = 0; property_index < element.properties.size(); property_index++) {
				auto &property = element.properties[property_index];
				GqlQuery(connection,
				         "INSERT INTO gql_internal.graph_schema_properties "
				         "(schema_element_id, property_ordinal, property_name, gql_type, nullable) VALUES (" +
				             to_string(schema_element_id) + ", " + to_string(property_index) + ", " +
				             GqlQuoteLiteral(property.name) + ", " + GqlQuoteLiteral(property.gql_type) + ", " +
				             (property.nullable ? "true" : "false") + ")");
			}
		}
		if (data.typed) {
			MaterializeTypedGraph(connection, graph_id, data.graph_name, data.elements);
		}
		connection.Commit();
	} catch (...) {
		if (connection.HasActiveTransaction()) {
			connection.Rollback();
		}
		throw;
	}
	EmitCommandResult(output, state, data.graph_name);
}

static void DropGraph(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<SingleRowState>();
	if (state.done) {
		return;
	}
	if (!context.transaction.IsAutoCommit()) {
		throw NotImplementedException("DROP GRAPH is not eligible inside an explicit transaction");
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
		auto graph_id = GqlQuery(connection, "SELECT graph_id FROM gql_internal.graphs WHERE graph_name = " +
		                                         GqlQuoteLiteral(data.graph_name))
		                    ->GetValue(0, 0)
		                    .GetValue<uint64_t>();
		GqlQuery(connection, "DROP SEQUENCE IF EXISTS gql_internal." +
		                         GqlQuoteIdentifier("graph_" + to_string(graph_id) + "_vertex_id_seq"));
		GqlQuery(connection, "DROP SEQUENCE IF EXISTS gql_internal." +
		                         GqlQuoteIdentifier("graph_" + to_string(graph_id) + "_edge_id_seq"));
		auto managed = GqlQuery(connection, "SELECT catalog_name, schema_name, table_name FROM "
		                                    "gql_internal.graph_element_tables WHERE graph_id = " +
		                                        to_string(graph_id) + " ORDER BY element_kind");
		for (idx_t row = 0; row < managed->RowCount(); row++) {
			auto table = GqlQuoteIdentifier(managed->GetValue(0, row).GetValue<string>()) + "." +
			             GqlQuoteIdentifier(managed->GetValue(1, row).GetValue<string>()) + "." +
			             GqlQuoteIdentifier(managed->GetValue(2, row).GetValue<string>());
			GqlQuery(connection, "DROP TABLE IF EXISTS " + table);
		}
		GqlQuery(connection, "DELETE FROM gql_internal.graph_property_mappings WHERE element_table_id IN "
		                     "(SELECT element_table_id FROM gql_internal.graph_element_tables WHERE graph_id = " +
		                         to_string(graph_id) + ")");
		GqlQuery(connection, "DELETE FROM gql_internal.graph_property_indexes WHERE element_table_id IN "
		                     "(SELECT element_table_id FROM gql_internal.graph_element_tables WHERE graph_id = " +
		                         to_string(graph_id) + ")");
		GqlQuery(connection, "DELETE FROM gql_internal.graph_label_mappings WHERE element_table_id IN "
		                     "(SELECT element_table_id FROM gql_internal.graph_element_tables WHERE graph_id = " +
		                         to_string(graph_id) + ")");
		GqlQuery(connection, "DELETE FROM gql_internal.graph_edge_endpoints WHERE edge_table_id IN "
		                     "(SELECT element_table_id FROM gql_internal.graph_element_tables WHERE graph_id = " +
		                         to_string(graph_id) + ")");
		GqlQuery(connection, "DELETE FROM gql_internal.graph_element_tables WHERE graph_id = " + to_string(graph_id));
		GqlQuery(connection, "DELETE FROM gql_internal.graph_schema_properties WHERE schema_element_id IN "
		                     "(SELECT schema_element_id FROM gql_internal.graph_schema_elements WHERE graph_id = " +
		                         to_string(graph_id) + ")");
		GqlQuery(connection, "DELETE FROM gql_internal.graph_schema_labels WHERE schema_element_id IN "
		                     "(SELECT schema_element_id FROM gql_internal.graph_schema_elements WHERE graph_id = " +
		                         to_string(graph_id) + ")");
		GqlQuery(connection, "DELETE FROM gql_internal.graph_schema_elements WHERE graph_id = " + to_string(graph_id));
		GqlQuery(connection, "DELETE FROM gql_internal.graph_schemas WHERE graph_id = " + to_string(graph_id));
		GqlQuery(connection, "DELETE FROM gql_internal.graph_storage WHERE graph_id = " + to_string(graph_id));
		GqlQuery(connection, "DELETE FROM gql_internal.graphs WHERE graph_id = " + to_string(graph_id));
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
	auto mode = GqlQuery(connection, "SELECT storage_mode FROM gql_internal.graph_storage gs JOIN "
	                                 "gql_internal.graphs g USING (graph_id) WHERE g.graph_name = " +
	                                     GqlQuoteLiteral(data.graph_name));
	if (mode->RowCount() == 0) {
		throw InvalidInputException("Graph '%s' does not exist", data.graph_name);
	}
	auto gql_state = context.registered_state->GetOrCreate<GqlClientState>(GQL_STATE_KEY);
	gql_state->graph_name = data.graph_name;
	EmitCommandResult(output, state, data.graph_name);
}

struct PropertyIndexTarget {
	uint64_t graph_id;
	uint64_t element_table_id;
	string property_name;
	string column_name;
	string catalog_name;
	string schema_name;
	string table_name;
};

static PropertyIndexTarget ResolvePropertyIndexTarget(Connection &connection, const string &graph_name,
                                                      const string &property_name) {
	auto target = GqlQuery(connection, "SELECT g.graph_id, et.element_table_id, pm.property_name, pm.column_name, "
	                                   "et.catalog_name, et.schema_name, et.table_name "
	                                   "FROM gql_internal.graphs g "
	                                   "JOIN gql_internal.graph_storage gs USING (graph_id) "
	                                   "JOIN gql_internal.graph_element_tables et USING (graph_id) "
	                                   "JOIN gql_internal.graph_property_mappings pm USING (element_table_id) "
	                                   "WHERE g.graph_name = " +
	                                       GqlQuoteLiteral(graph_name) +
	                                       " AND gs.storage_mode = 'TABLE_BACKED' AND et.element_kind = 'VERTEX' AND "
	                                       "lower(pm.property_name) = lower(" +
	                                       GqlQuoteLiteral(property_name) + ")");
	if (target->RowCount() == 0) {
		auto graph = GqlQuery(connection, "SELECT count(*) FROM gql_internal.graphs WHERE graph_name = " +
		                                      GqlQuoteLiteral(graph_name));
		if (graph->GetValue(0, 0).GetValue<int64_t>() == 0) {
			throw InvalidInputException("Graph '%s' does not exist", graph_name);
		}
		throw InvalidInputException("Graph '%s' has no vertex property '%s'", graph_name, property_name);
	}
	if (target->RowCount() != 1) {
		throw InternalException("Graph vertex property mapping is ambiguous");
	}
	return {target->GetValue(0, 0).GetValue<uint64_t>(), target->GetValue(1, 0).GetValue<uint64_t>(),
	        target->GetValue(2, 0).GetValue<string>(),   target->GetValue(3, 0).GetValue<string>(),
	        target->GetValue(4, 0).GetValue<string>(),   target->GetValue(5, 0).GetValue<string>(),
	        target->GetValue(6, 0).GetValue<string>()};
}

static bool PhysicalIndexExists(Connection &connection, const PropertyIndexTarget &target, const string &index_name) {
	auto count =
	    GqlQuery(connection,
	             "SELECT count(*) FROM duckdb_indexes() WHERE database_name = " + GqlQuoteLiteral(target.catalog_name) +
	                 " AND schema_name = " + GqlQuoteLiteral(target.schema_name) + " AND table_name = " +
	                 GqlQuoteLiteral(target.table_name) + " AND index_name = " + GqlQuoteLiteral(index_name));
	return count->GetValue(0, 0).GetValue<int64_t>() != 0;
}

static void EmitPropertyIndexResult(DataChunk &output, SingleRowState &state, const PropertyIndexBindData &data,
                                    const string &property_name, const string &index_name) {
	output.SetCardinality(1);
	output.SetValue(0, 0, Value(true));
	output.SetValue(1, 0, Value(data.graph_name));
	output.SetValue(2, 0, Value(property_name));
	output.SetValue(3, 0, Value(index_name));
	state.done = true;
}

static void CreatePropertyIndex(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<SingleRowState>();
	if (state.done) {
		return;
	}
	if (!context.transaction.IsAutoCommit()) {
		throw NotImplementedException("CREATE PROPERTY INDEX is not eligible inside an explicit transaction");
	}
	auto &data = input.bind_data->Cast<PropertyIndexBindData>();
	Connection connection(*context.db);
	GqlEnsureStorage(connection);
	auto target = ResolvePropertyIndexTarget(connection, data.graph_name, data.property_name);
	auto existing = GqlQuery(connection, "SELECT index_name FROM gql_internal.graph_property_indexes WHERE "
	                                     "element_table_id = " +
	                                         to_string(target.element_table_id) +
	                                         " AND property_name = " + GqlQuoteLiteral(target.property_name));
	if (existing->RowCount() == 1) {
		auto index_name = existing->GetValue(0, 0).GetValue<string>();
		if (PhysicalIndexExists(connection, target, index_name)) {
			EmitPropertyIndexResult(output, state, data, target.property_name, index_name);
			return;
		}
		GqlQuery(connection, "DELETE FROM gql_internal.graph_property_indexes WHERE element_table_id = " +
		                         to_string(target.element_table_id) +
		                         " AND property_name = " + GqlQuoteLiteral(target.property_name));
	}

	connection.BeginTransaction();
	try {
		auto property_index_id = GqlQuery(connection, "SELECT nextval('gql_internal.property_index_id_seq')::UBIGINT")
		                             ->GetValue(0, 0)
		                             .GetValue<uint64_t>();
		auto index_name = "gql_property_index_" + to_string(property_index_id);
		auto qualified_table = GqlQuoteIdentifier(target.catalog_name) + "." + GqlQuoteIdentifier(target.schema_name) +
		                       "." + GqlQuoteIdentifier(target.table_name);
		GqlQuery(connection, "CREATE INDEX " + GqlQuoteIdentifier(index_name) + " ON " + qualified_table + " (" +
		                         GqlQuoteIdentifier(target.column_name) + ")");
		GqlQuery(connection, "INSERT INTO gql_internal.graph_property_indexes "
		                     "(property_index_id, element_table_id, property_name, column_name, index_name) VALUES (" +
		                         to_string(property_index_id) + ", " + to_string(target.element_table_id) + ", " +
		                         GqlQuoteLiteral(target.property_name) + ", " + GqlQuoteLiteral(target.column_name) +
		                         ", " + GqlQuoteLiteral(index_name) + ")");
		connection.Commit();
		EmitPropertyIndexResult(output, state, data, target.property_name, index_name);
	} catch (...) {
		if (connection.HasActiveTransaction()) {
			connection.Rollback();
		}
		throw;
	}
}

static void DropPropertyIndex(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<SingleRowState>();
	if (state.done) {
		return;
	}
	if (!context.transaction.IsAutoCommit()) {
		throw NotImplementedException("DROP PROPERTY INDEX is not eligible inside an explicit transaction");
	}
	auto &data = input.bind_data->Cast<PropertyIndexBindData>();
	Connection connection(*context.db);
	GqlEnsureStorage(connection);
	auto target = ResolvePropertyIndexTarget(connection, data.graph_name, data.property_name);
	auto existing = GqlQuery(connection, "SELECT index_name FROM gql_internal.graph_property_indexes WHERE "
	                                     "element_table_id = " +
	                                         to_string(target.element_table_id) +
	                                         " AND property_name = " + GqlQuoteLiteral(target.property_name));
	if (existing->RowCount() == 0) {
		EmitPropertyIndexResult(output, state, data, target.property_name, string());
		return;
	}
	auto index_name = existing->GetValue(0, 0).GetValue<string>();
	connection.BeginTransaction();
	try {
		auto qualified_index = GqlQuoteIdentifier(target.catalog_name) + "." + GqlQuoteIdentifier(target.schema_name) +
		                       "." + GqlQuoteIdentifier(index_name);
		GqlQuery(connection, "DROP INDEX IF EXISTS " + qualified_index);
		GqlQuery(connection, "DELETE FROM gql_internal.graph_property_indexes WHERE element_table_id = " +
		                         to_string(target.element_table_id) +
		                         " AND property_name = " + GqlQuoteLiteral(target.property_name));
		connection.Commit();
		EmitPropertyIndexResult(output, state, data, target.property_name, index_name);
	} catch (...) {
		if (connection.HasActiveTransaction()) {
			connection.Rollback();
		}
		throw;
	}
}

struct GraphRow {
	Value graph_id;
	Value graph_name;
	Value graph_version;
	Value vertex_count;
	Value edge_count;
	Value created_at;
};

struct PropertyIndexRow {
	Value graph_name;
	Value property_name;
	Value index_name;
	Value catalog_name;
	Value schema_name;
	Value table_name;
	Value column_name;
};

struct PropertyIndexRowsState : GlobalTableFunctionState {
	bool initialized = false;
	idx_t offset = 0;
	vector<PropertyIndexRow> rows;
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

static unique_ptr<FunctionData> PropertyIndexesBind(ClientContext &, TableFunctionBindInput &,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	names = {"graph_name", "property_name", "index_name", "catalog_name", "schema_name", "table_name", "column_name"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> PropertyIndexesInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<PropertyIndexRowsState>();
}

static void PropertyIndexesFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<PropertyIndexRowsState>();
	if (!state.initialized) {
		Connection connection(*context.db);
		GqlEnsureStorage(connection);
		auto indexes = GqlQuery(
		    connection,
		    "SELECT g.graph_name, pi.property_name, pi.index_name, et.catalog_name, et.schema_name, et.table_name, "
		    "pi.column_name FROM gql_internal.graph_property_indexes pi "
		    "JOIN gql_internal.graph_element_tables et USING (element_table_id) "
		    "JOIN gql_internal.graphs g USING (graph_id) ORDER BY g.graph_name, pi.property_name");
		for (idx_t row = 0; row < indexes->RowCount(); row++) {
			state.rows.push_back({indexes->GetValue(0, row), indexes->GetValue(1, row), indexes->GetValue(2, row),
			                      indexes->GetValue(3, row), indexes->GetValue(4, row), indexes->GetValue(5, row),
			                      indexes->GetValue(6, row)});
		}
		state.initialized = true;
	}
	auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.offset);
	for (idx_t index = 0; index < count; index++) {
		auto &row = state.rows[state.offset + index];
		output.SetValue(0, index, row.graph_name);
		output.SetValue(1, index, row.property_name);
		output.SetValue(2, index, row.index_name);
		output.SetValue(3, index, row.catalog_name);
		output.SetValue(4, index, row.schema_name);
		output.SetValue(5, index, row.table_name);
		output.SetValue(6, index, row.column_name);
	}
	state.offset += count;
	output.SetCardinality(count);
}

static void LoadGraphRows(ClientContext &context, GraphRowsState &state) {
	Connection connection(*context.db);
	GqlEnsureStorage(connection);
	auto graphs = GqlQuery(connection, "SELECT g.graph_id, g.graph_name, g.graph_version, g.created_at "
	                                   "FROM gql_internal.graphs g ORDER BY g.graph_name");
	for (idx_t row = 0; row < graphs->RowCount(); row++) {
		auto graph_id = graphs->GetValue(0, row).GetValue<uint64_t>();
		auto tables = GqlQuery(connection, "SELECT element_kind, catalog_name, schema_name, table_name FROM "
		                                   "gql_internal.graph_element_tables WHERE graph_id = " +
		                                       to_string(graph_id));
		uint64_t vertex_count = 0;
		uint64_t edge_count = 0;
		for (idx_t table_row = 0; table_row < tables->RowCount(); table_row++) {
			auto qualified = GqlQuoteIdentifier(tables->GetValue(1, table_row).GetValue<string>()) + "." +
			                 GqlQuoteIdentifier(tables->GetValue(2, table_row).GetValue<string>()) + "." +
			                 GqlQuoteIdentifier(tables->GetValue(3, table_row).GetValue<string>());
			auto count =
			    GqlQuery(connection, "SELECT count(*)::UBIGINT FROM " + qualified)->GetValue(0, 0).GetValue<uint64_t>();
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
	TableFunction function("gql_create_graph",
	                       {LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::BOOLEAN,
	                        LogicalType::LIST(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::VARCHAR),
	                        LogicalType::LIST(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::VARCHAR),
	                        LogicalType::LIST(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::VARCHAR),
	                        LogicalType::LIST(LogicalType::UBIGINT), LogicalType::LIST(LogicalType::VARCHAR),
	                        LogicalType::LIST(LogicalType::UBIGINT), LogicalType::LIST(LogicalType::VARCHAR),
	                        LogicalType::LIST(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::BOOLEAN)},
	                       CreateGraph);
	function.bind = CreateGraphBind;
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

TableFunction GqlCreatePropertyIndexFunction() {
	TableFunction function("gql_create_property_index", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                       CreatePropertyIndex);
	function.bind = PropertyIndexBind;
	function.init_global = SingleRowInit;
	return function;
}

TableFunction GqlDropPropertyIndexFunction() {
	TableFunction function("gql_drop_property_index", {LogicalType::VARCHAR, LogicalType::VARCHAR}, DropPropertyIndex);
	function.bind = PropertyIndexBind;
	function.init_global = SingleRowInit;
	return function;
}

TableFunction GqlPropertyIndexesFunction() {
	TableFunction function("gql_property_indexes", {}, PropertyIndexesFunction);
	function.bind = PropertyIndexesBind;
	function.init_global = PropertyIndexesInit;
	return function;
}

TableFunction GqlMutationControlFunction() {
	TableFunction function("gql_mutation_control", {LogicalType::VARCHAR, LogicalType::BOOLEAN}, MutationControl);
	function.bind = MutationControlBind;
	function.init_global = SingleRowInit;
	return function;
}

} // namespace duckdb
