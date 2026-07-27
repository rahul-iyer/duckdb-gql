#include "gql_catalog.hpp"

#include "gql_sql_utils.hpp"
#include "gql_storage.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/main/table_description.hpp"
#include "duckdb/parser/qualified_name.hpp"

namespace duckdb {

static string QualifiedTable(const TableDescription &table) {
	return GqlQuoteIdentifier(table.database) + "." + GqlQuoteIdentifier(table.schema) + "." +
	       GqlQuoteIdentifier(table.table);
}

static unique_ptr<TableDescription> ResolveTable(Connection &connection, const string &input) {
	auto qualified = QualifiedName::Parse(input);
	if (qualified.name.empty()) {
		throw BinderException("A registered graph table name cannot be empty");
	}
	if (qualified.catalog == INVALID_CATALOG || qualified.schema == INVALID_SCHEMA) {
		auto defaults = GqlQuery(connection, "SELECT current_database(), current_schema()");
		if (qualified.catalog == INVALID_CATALOG) {
			qualified.catalog = defaults->GetValue(0, 0).GetValue<string>();
		}
		if (qualified.schema == INVALID_SCHEMA) {
			qualified.schema = defaults->GetValue(1, 0).GetValue<string>();
		}
	}
	auto table = connection.TableInfo(qualified.catalog, qualified.schema, qualified.name);
	if (!table) {
		throw BinderException("Registered graph table '%s' does not exist", input);
	}
	// TableInfo preserves the requested qualification. Store the resolved names
	// rather than relying on a future connection's search path.
	table->database = qualified.catalog;
	table->schema = qualified.schema;
	table->table = qualified.name;
	return table;
}

static const ColumnDefinition &ResolveColumn(const TableDescription &table, const string &requested) {
	for (const auto &column : table.columns) {
		if (StringUtil::CIEquals(column.Name(), requested)) {
			return column;
		}
	}
	throw BinderException("Column '%s' does not exist in registered graph table %s.%s", requested, table.schema,
	                      table.table);
}

static bool ValidateLabelColumn(const TableDescription &table, const ColumnDefinition &column, bool allow_list) {
	if (column.Type().id() == LogicalTypeId::VARCHAR) {
		return false;
	}
	if (allow_list && column.Type() == LogicalType::LIST(LogicalType::VARCHAR)) {
		return true;
	}
	throw BinderException("Graph label/type column '%s' in %s.%s must be %s, found %s", column.Name(), table.schema,
	                      table.table, allow_list ? "VARCHAR or VARCHAR[]" : "VARCHAR", column.Type().ToString());
}

static void ValidateKey(Connection &connection, const TableDescription &table, const ColumnDefinition &key) {
	auto column = GqlQuoteIdentifier(key.Name());
	auto result = GqlQuery(connection, "SELECT count(*)::UBIGINT, count(" + column + ")::UBIGINT, count(DISTINCT " +
	                                       column + ")::UBIGINT FROM " + QualifiedTable(table));
	auto rows = result->GetValue(0, 0).GetValue<uint64_t>();
	auto non_null = result->GetValue(1, 0).GetValue<uint64_t>();
	auto distinct = result->GetValue(2, 0).GetValue<uint64_t>();
	if (rows != non_null) {
		throw InvalidInputException("Registered graph key %s.%s.%s contains NULL values", table.schema, table.table,
		                            key.Name());
	}
	if (rows != distinct) {
		throw InvalidInputException("Registered graph key %s.%s.%s is not unique", table.schema, table.table,
		                            key.Name());
	}
}

static void ValidateEndpoint(Connection &connection, const TableDescription &edge, const ColumnDefinition &endpoint,
                             const TableDescription &vertex, const ColumnDefinition &key, const char *role) {
	auto edge_column = GqlQuoteIdentifier(endpoint.Name());
	auto vertex_column = GqlQuoteIdentifier(key.Name());
	auto sql = "SELECT count(*)::UBIGINT FROM " + QualifiedTable(edge) + " e LEFT JOIN " + QualifiedTable(vertex) +
	           " v ON e." + edge_column + " = v." + vertex_column + " WHERE e." + edge_column + " IS NULL OR v." +
	           vertex_column + " IS NULL";
	auto result = GqlQuery(connection, sql);
	if (result->GetValue(0, 0).GetValue<uint64_t>() != 0) {
		throw InvalidInputException("Registered graph %s endpoints contain NULL or missing vertex keys", role);
	}
}

static bool IsStructuralColumn(const string &name, const vector<string> &structural) {
	for (const auto &candidate : structural) {
		if (!candidate.empty() && StringUtil::CIEquals(name, candidate)) {
			return true;
		}
	}
	return false;
}

static void InsertProperties(Connection &connection, uint64_t element_table_id, const TableDescription &table,
                             const vector<string> &structural) {
	for (const auto &column : table.columns) {
		if (column.Generated() || IsStructuralColumn(column.Name(), structural)) {
			continue;
		}
		GqlQuery(connection, "INSERT INTO gql_internal.graph_property_mappings "
		                     "(element_table_id, property_name, column_name, "
		                     "gql_type, nullable, writable) VALUES (" +
		                         to_string(element_table_id) + ", " + GqlQuoteLiteral(column.Name()) + ", " +
		                         GqlQuoteLiteral(column.Name()) + ", " + GqlQuoteLiteral(column.Type().ToString()) +
		                         ", true, " + (table.readonly ? "false" : "true") + ")");
	}
}

static uint64_t InsertElementTable(Connection &connection, uint64_t graph_id, const char *kind,
                                   const TableDescription &table, const string &key_column, const char *ownership) {
	auto result = GqlQuery(
	    connection, "INSERT INTO gql_internal.graph_element_tables "
	                "(graph_id, element_kind, catalog_name, schema_name, "
	                "table_name, key_columns, "
	                "ownership, access_mode) VALUES (" +
	                    to_string(graph_id) + ", " + GqlQuoteLiteral(kind) + ", " + GqlQuoteLiteral(table.database) +
	                    ", " + GqlQuoteLiteral(table.schema) + ", " + GqlQuoteLiteral(table.table) + ", [" +
	                    GqlQuoteLiteral(key_column) + "], " + GqlQuoteLiteral(ownership) + ", " +
	                    (table.readonly ? "'READ_ONLY'" : "'READ_WRITE'") + ") RETURNING element_table_id");
	return result->GetValue(0, 0).GetValue<uint64_t>();
}

static void InsertLabelMapping(Connection &connection, uint64_t element_table_id, const string &column_name,
                               bool is_list) {
	if (column_name.empty()) {
		return;
	}
	GqlQuery(connection, "INSERT INTO gql_internal.graph_label_mappings "
	                     "(element_table_id, mapping_kind, column_name) VALUES (" +
	                         to_string(element_table_id) + ", " +
	                         GqlQuoteLiteral(is_list ? "LIST_COLUMN" : "SCALAR_COLUMN") + ", " +
	                         GqlQuoteLiteral(column_name) + ")");
}

void GqlAttachManagedGraphTables(Connection &connection, const string &graph_name, const string &vertex_table,
                                 const string &vertex_key, const string &vertex_label, const string &edge_table,
                                 const string &edge_key, const string &edge_source, const string &edge_target,
                                 const string &edge_label, bool validate) {
	GqlEnsureStorage(connection);
	auto vertex = ResolveTable(connection, vertex_table);
	auto edge = ResolveTable(connection, edge_table);
	auto &resolved_vertex_key = ResolveColumn(*vertex, vertex_key);
	auto &resolved_edge_key = ResolveColumn(*edge, edge_key);
	auto &resolved_edge_source = ResolveColumn(*edge, edge_source);
	auto &resolved_edge_target = ResolveColumn(*edge, edge_target);
	auto &resolved_vertex_label = ResolveColumn(*vertex, vertex_label);
	auto &resolved_edge_label = ResolveColumn(*edge, edge_label);
	auto vertex_label_is_list = ValidateLabelColumn(*vertex, resolved_vertex_label, true);
	auto edge_label_is_list = ValidateLabelColumn(*edge, resolved_edge_label, false);
	if (resolved_edge_source.Type() != resolved_vertex_key.Type() ||
	    resolved_edge_target.Type() != resolved_vertex_key.Type()) {
		throw BinderException("Graph edge endpoint types must exactly match the vertex key type (%s)",
		                      resolved_vertex_key.Type().ToString());
	}
	if (validate) {
		ValidateKey(connection, *vertex, resolved_vertex_key);
		ValidateKey(connection, *edge, resolved_edge_key);
		ValidateEndpoint(connection, *edge, resolved_edge_source, *vertex, resolved_vertex_key, "source");
		ValidateEndpoint(connection, *edge, resolved_edge_target, *vertex, resolved_vertex_key, "destination");
	}

	auto graph = GqlQuery(connection, "SELECT g.graph_id, coalesce(gs.storage_mode, 'EMPTY'), "
	                                  "(SELECT count(*) FROM gql_internal.graph_element_tables et WHERE et.graph_id = "
	                                  "g.graph_id) FROM gql_internal.graphs g LEFT JOIN "
	                                  "gql_internal.graph_storage gs USING (graph_id) WHERE g.graph_name = " +
	                                      GqlQuoteLiteral(graph_name));
	if (graph->RowCount() == 0) {
		throw InvalidInputException("Graph '%s' does not exist; create it before COPY GRAPH", graph_name);
	}
	auto graph_id = graph->GetValue(0, 0).GetValue<uint64_t>();
	auto storage_mode = graph->GetValue(1, 0).GetValue<string>();
	auto element_tables = graph->GetValue(2, 0).GetValue<int64_t>();
	if (storage_mode == "TABLE_BACKED" || element_tables != 0) {
		throw InvalidInputException("Graph '%s' already has native table storage", graph_name);
	}
	if (storage_mode != "EMPTY") {
		throw InvalidInputException("Graph '%s' uses unsupported legacy storage mode '%s'; recreate and reload it with "
		                            "COPY GRAPH",
		                            graph_name, storage_mode);
	}

	auto vertex_id = InsertElementTable(connection, graph_id, "VERTEX", *vertex, resolved_vertex_key.Name(), "MANAGED");
	auto edge_id = InsertElementTable(connection, graph_id, "EDGE", *edge, resolved_edge_key.Name(), "MANAGED");
	InsertLabelMapping(connection, vertex_id, resolved_vertex_label.Name(), vertex_label_is_list);
	InsertLabelMapping(connection, edge_id, resolved_edge_label.Name(), edge_label_is_list);
	InsertProperties(connection, vertex_id, *vertex, {resolved_vertex_key.Name(), resolved_vertex_label.Name()});
	InsertProperties(connection, edge_id, *edge,
	                 {resolved_edge_key.Name(), resolved_edge_source.Name(), resolved_edge_target.Name(),
	                  resolved_edge_label.Name()});
	GqlQuery(connection, "INSERT INTO gql_internal.graph_edge_endpoints "
	                     "(edge_table_id, source_vertex_table_id, target_vertex_table_id, source_columns, "
	                     "target_columns, source_key_columns, target_key_columns) VALUES (" +
	                         to_string(edge_id) + ", " + to_string(vertex_id) + ", " + to_string(vertex_id) + ", [" +
	                         GqlQuoteLiteral(resolved_edge_source.Name()) + "], [" +
	                         GqlQuoteLiteral(resolved_edge_target.Name()) + "], [" +
	                         GqlQuoteLiteral(resolved_vertex_key.Name()) + "], [" +
	                         GqlQuoteLiteral(resolved_vertex_key.Name()) + "])");
	GqlQuery(
	    connection,
	    "UPDATE gql_internal.graph_storage SET storage_mode = 'TABLE_BACKED', default_catalog = " +
	        GqlQuoteLiteral(vertex->database) + ", default_schema = " + GqlQuoteLiteral(vertex->schema) +
	        ", schema_version = schema_version + 1, csr_policy = 'MANUAL' WHERE graph_id = " + to_string(graph_id));
	GqlQuery(connection, "UPDATE gql_internal.graphs SET graph_version = graph_version + 1 WHERE graph_id = " +
	                         to_string(graph_id));
}

static string ReadSingleColumn(const Value &value, const char *description) {
	const auto &children = ListValue::GetChildren(value);
	if (children.size() != 1 || children[0].IsNull()) {
		throw InvalidInputException("Table-backed graph %s must contain exactly one column", description);
	}
	return children[0].GetValue<string>();
}

static void LoadProperties(Connection &connection, GqlElementTableBinding &table) {
	auto result = GqlQuery(connection, "SELECT property_name, column_name FROM "
	                                   "gql_internal.graph_property_mappings WHERE element_table_id = " +
	                                       to_string(table.element_table_id));
	for (idx_t row = 0; row < result->RowCount(); row++) {
		table.property_columns.emplace(result->GetValue(0, row).GetValue<string>(),
		                               result->GetValue(1, row).GetValue<string>());
	}
}

static void LoadPropertyIndexes(Connection &connection, GqlElementTableBinding &table) {
	auto result = GqlQuery(connection, "SELECT property_name, index_name FROM "
	                                   "gql_internal.graph_property_indexes WHERE element_table_id = " +
	                                       to_string(table.element_table_id));
	for (idx_t row = 0; row < result->RowCount(); row++) {
		table.property_indexes.emplace(result->GetValue(0, row).GetValue<string>(),
		                               result->GetValue(1, row).GetValue<string>());
	}
}

static void LoadLabel(Connection &connection, GqlElementTableBinding &table) {
	auto result = GqlQuery(connection, "SELECT mapping_kind, column_name FROM "
	                                   "gql_internal.graph_label_mappings WHERE "
	                                   "element_table_id = " +
	                                       to_string(table.element_table_id));
	if (result->RowCount() == 0) {
		return;
	}
	if (result->RowCount() != 1) {
		throw NotImplementedException("Table-backed MATCH currently requires one label/type column");
	}
	auto mapping_kind = result->GetValue(0, 0).GetValue<string>();
	if (mapping_kind != "SCALAR_COLUMN" && mapping_kind != "LIST_COLUMN") {
		throw NotImplementedException("Table-backed MATCH does not support label mapping kind '%s'", mapping_kind);
	}
	table.label_is_list = mapping_kind == "LIST_COLUMN";
	table.label_column = result->GetValue(1, 0).GetValue<string>();
}

bool GqlTryLoadTableGraph(ClientContext &context, const string &graph_name, GqlTableGraphBinding &result) {
	Connection connection(*context.db);
	auto storage = GqlQuery(connection, "SELECT g.graph_id, gs.storage_mode FROM gql_internal.graphs g JOIN "
	                                    "gql_internal.graph_storage gs USING (graph_id) WHERE g.graph_name = " +
	                                        GqlQuoteLiteral(graph_name));
	if (storage->RowCount() == 0) {
		throw InvalidInputException("Graph '%s' does not exist", graph_name);
	}
	result.graph_id = storage->GetValue(0, 0).GetValue<uint64_t>();
	if (storage->GetValue(1, 0).GetValue<string>() != "TABLE_BACKED") {
		return false;
	}

	auto tables =
	    GqlQuery(connection, "SELECT element_table_id, element_kind, catalog_name, schema_name, "
	                         "table_name, "
	                         "key_columns, ownership FROM gql_internal.graph_element_tables WHERE graph_id = " +
	                             to_string(result.graph_id) + " ORDER BY element_kind");
	if (tables->RowCount() != 2) {
		throw InvalidInputException("Table-backed graph '%s' must contain one vertex and one edge table", graph_name);
	}
	for (idx_t row = 0; row < tables->RowCount(); row++) {
		auto kind = tables->GetValue(1, row).GetValue<string>();
		auto *target = kind == "VERTEX" ? &result.vertex : kind == "EDGE" ? &result.edge : nullptr;
		if (!target || target->element_table_id != 0) {
			throw InvalidInputException("Table-backed graph '%s' contains invalid element table metadata", graph_name);
		}
		target->element_table_id = tables->GetValue(0, row).GetValue<uint64_t>();
		target->catalog_name = tables->GetValue(2, row).GetValue<string>();
		target->schema_name = tables->GetValue(3, row).GetValue<string>();
		target->table_name = tables->GetValue(4, row).GetValue<string>();
		target->key_column = ReadSingleColumn(tables->GetValue(5, row), "element key");
		target->ownership = tables->GetValue(6, row).GetValue<string>();
		LoadLabel(connection, *target);
		LoadProperties(connection, *target);
		LoadPropertyIndexes(connection, *target);
	}

	auto endpoints = GqlQuery(connection, "SELECT source_vertex_table_id, target_vertex_table_id, source_columns, "
	                                      "target_columns FROM gql_internal.graph_edge_endpoints WHERE "
	                                      "edge_table_id = " +
	                                          to_string(result.edge.element_table_id));
	if (endpoints->RowCount() != 1 ||
	    endpoints->GetValue(0, 0).GetValue<uint64_t>() != result.vertex.element_table_id ||
	    endpoints->GetValue(1, 0).GetValue<uint64_t>() != result.vertex.element_table_id) {
		throw InvalidInputException("Table-backed graph '%s' has invalid endpoint metadata", graph_name);
	}
	result.edge_source_column = ReadSingleColumn(endpoints->GetValue(2, 0), "edge source");
	result.edge_target_column = ReadSingleColumn(endpoints->GetValue(3, 0), "edge destination");
	return true;
}

} // namespace duckdb
