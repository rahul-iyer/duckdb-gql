#include "gql_catalog.hpp"

#include "gql_sql_utils.hpp"
#include "gql_storage.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/main/table_description.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/storage/object_cache.hpp"

namespace duckdb {

static string QualifiedTable(const TableDescription &table) {
	return GqlQuoteIdentifier(table.qualified_name.Catalog().GetIdentifierName()) + "." +
	       GqlQuoteIdentifier(table.qualified_name.Schema().GetIdentifierName()) + "." +
	       GqlQuoteIdentifier(table.qualified_name.Name().GetIdentifierName());
}

static unique_ptr<TableDescription> ResolveTable(Connection &connection, const string &input) {
	auto qualified = QualifiedName::Parse(input);
	if (qualified.Name().empty()) {
		throw BinderException("A registered graph table name cannot be empty");
	}
	auto catalog = qualified.Catalog();
	auto schema = qualified.Schema();
	if (catalog.empty() || schema.empty()) {
		auto defaults = GqlQuery(connection, "SELECT current_database(), current_schema()");
		if (catalog.empty()) {
			catalog = Identifier(defaults->GetValue(0, 0).GetValue<string>());
		}
		if (schema.empty()) {
			schema = Identifier(defaults->GetValue(1, 0).GetValue<string>());
		}
	}
	auto resolved = QualifiedName(catalog, schema, qualified.Name());
	auto table = connection.TableInfo(catalog, schema, qualified.Name());
	if (!table) {
		throw BinderException("Registered graph table '%s' does not exist", input);
	}
	// Store resolved names rather than relying on a future connection's search path.
	table->qualified_name = std::move(resolved);
	return table;
}

static const ColumnDefinition &ResolveColumn(const TableDescription &table, const string &requested) {
	for (const auto &column : table.columns) {
		if (StringUtil::CIEquals(column.Name().GetIdentifierName(), requested)) {
			return column;
		}
	}
	throw BinderException("Column '%s' does not exist in registered graph table %s.%s", requested,
	                      table.qualified_name.Schema().GetIdentifierName(),
	                      table.qualified_name.Name().GetIdentifierName());
}

static bool ValidateLabelColumn(const TableDescription &table, const ColumnDefinition &column, bool allow_list) {
	if (column.Type().id() == LogicalTypeId::VARCHAR) {
		return false;
	}
	if (allow_list && column.Type() == LogicalType::LIST(LogicalType::VARCHAR)) {
		return true;
	}
	throw BinderException("Graph label/type column '%s' in %s.%s must be %s, found %s",
	                      column.Name().GetIdentifierName(), table.qualified_name.Schema().GetIdentifierName(),
	                      table.qualified_name.Name().GetIdentifierName(),
	                      allow_list ? "VARCHAR or VARCHAR[]" : "VARCHAR", column.Type().ToString());
}

static void ValidateKey(Connection &connection, const TableDescription &table, const ColumnDefinition &key) {
	auto column = GqlQuoteIdentifier(key.Name().GetIdentifierName());
	auto result = GqlQuery(connection, "SELECT count(*)::UBIGINT, count(" + column + ")::UBIGINT, count(DISTINCT " +
	                                       column + ")::UBIGINT FROM " + QualifiedTable(table));
	auto rows = result->GetValue(0, 0).GetValue<uint64_t>();
	auto non_null = result->GetValue(1, 0).GetValue<uint64_t>();
	auto distinct = result->GetValue(2, 0).GetValue<uint64_t>();
	if (rows != non_null) {
		throw InvalidInputException("Registered graph key %s.%s.%s contains NULL values",
		                            table.qualified_name.Schema().GetIdentifierName(),
		                            table.qualified_name.Name().GetIdentifierName(), key.Name().GetIdentifierName());
	}
	if (rows != distinct) {
		throw InvalidInputException("Registered graph key %s.%s.%s is not unique",
		                            table.qualified_name.Schema().GetIdentifierName(),
		                            table.qualified_name.Name().GetIdentifierName(), key.Name().GetIdentifierName());
	}
}

static void ValidateEndpoint(Connection &connection, const TableDescription &edge, const ColumnDefinition &endpoint,
                             const TableDescription &vertex, const ColumnDefinition &key, const char *role) {
	auto edge_column = GqlQuoteIdentifier(endpoint.Name().GetIdentifierName());
	auto vertex_column = GqlQuoteIdentifier(key.Name().GetIdentifierName());
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
		if (column.Generated() || IsStructuralColumn(column.Name().GetIdentifierName(), structural)) {
			continue;
		}
		GqlQuery(connection, "INSERT INTO gql_internal.graph_property_mappings "
		                     "(element_table_id, property_name, column_name, "
		                     "gql_type, nullable, writable) VALUES (" +
		                         to_string(element_table_id) + ", " +
		                         GqlQuoteLiteral(column.Name().GetIdentifierName()) + ", " +
		                         GqlQuoteLiteral(column.Name().GetIdentifierName()) + ", " +
		                         GqlQuoteLiteral(column.Type().ToString()) + ", true, " +
		                         (table.readonly ? "false" : "true") + ")");
	}
}

static uint64_t InsertElementTable(Connection &connection, uint64_t graph_id, const char *kind,
                                   const TableDescription &table, const string &key_column, const char *ownership) {
	auto key_columns = key_column.empty() ? "[]" : "[" + GqlQuoteLiteral(key_column) + "]";
	auto result =
	    GqlQuery(connection, "INSERT INTO gql_internal.graph_element_tables "
	                         "(graph_id, element_kind, catalog_name, schema_name, "
	                         "table_name, key_columns, "
	                         "ownership, access_mode) VALUES (" +
	                             to_string(graph_id) + ", " + GqlQuoteLiteral(kind) + ", " +
	                             GqlQuoteLiteral(table.qualified_name.Catalog().GetIdentifierName()) + ", " +
	                             GqlQuoteLiteral(table.qualified_name.Schema().GetIdentifierName()) + ", " +
	                             GqlQuoteLiteral(table.qualified_name.Name().GetIdentifierName()) + ", " + key_columns +
	                             ", " + GqlQuoteLiteral(ownership) + ", " +
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

	auto vertex_id = InsertElementTable(connection, graph_id, "VERTEX", *vertex,
	                                    resolved_vertex_key.Name().GetIdentifierName(), "MANAGED");
	auto edge_id = InsertElementTable(connection, graph_id, "EDGE", *edge, resolved_edge_key.Name().GetIdentifierName(),
	                                  "MANAGED");
	InsertLabelMapping(connection, vertex_id, resolved_vertex_label.Name().GetIdentifierName(), vertex_label_is_list);
	InsertLabelMapping(connection, edge_id, resolved_edge_label.Name().GetIdentifierName(), edge_label_is_list);
	InsertProperties(
	    connection, vertex_id, *vertex,
	    {resolved_vertex_key.Name().GetIdentifierName(), resolved_vertex_label.Name().GetIdentifierName()});
	InsertProperties(connection, edge_id, *edge,
	                 {resolved_edge_key.Name().GetIdentifierName(), resolved_edge_source.Name().GetIdentifierName(),
	                  resolved_edge_target.Name().GetIdentifierName(), resolved_edge_label.Name().GetIdentifierName()});
	GqlQuery(connection, "INSERT INTO gql_internal.graph_edge_endpoints "
	                     "(edge_table_id, source_vertex_table_id, target_vertex_table_id, source_columns, "
	                     "target_columns, source_key_columns, target_key_columns) VALUES (" +
	                         to_string(edge_id) + ", " + to_string(vertex_id) + ", " + to_string(vertex_id) + ", [" +
	                         GqlQuoteLiteral(resolved_edge_source.Name().GetIdentifierName()) + "], [" +
	                         GqlQuoteLiteral(resolved_edge_target.Name().GetIdentifierName()) + "], [" +
	                         GqlQuoteLiteral(resolved_vertex_key.Name().GetIdentifierName()) + "], [" +
	                         GqlQuoteLiteral(resolved_vertex_key.Name().GetIdentifierName()) + "])");
	GqlQuery(
	    connection,
	    "UPDATE gql_internal.graph_storage SET storage_mode = 'TABLE_BACKED', default_catalog = " +
	        GqlQuoteLiteral(vertex->qualified_name.Catalog().GetIdentifierName()) +
	        ", default_schema = " + GqlQuoteLiteral(vertex->qualified_name.Schema().GetIdentifierName()) +
	        ", schema_version = schema_version + 1, csr_policy = 'MANUAL' WHERE graph_id = " + to_string(graph_id));
	GqlQuery(connection, "UPDATE gql_internal.graphs SET graph_version = graph_version + 1 WHERE graph_id = " +
	                         to_string(graph_id));
}

static bool IsReferencedKeyType(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
		return true;
	default:
		return false;
	}
}

static uint64_t FindSchemaElement(Connection &connection, uint64_t graph_id, const string &kind,
                                  const string &type_name) {
	auto result = GqlQuery(connection, "SELECT schema_element_id FROM gql_internal.graph_schema_elements WHERE "
	                                   "graph_id = " +
	                                       to_string(graph_id) + " AND element_kind = " + GqlQuoteLiteral(kind) +
	                                       " AND lower(type_name) = " + GqlQuoteLiteral(StringUtil::Lower(type_name)));
	if (result->RowCount() != 1) {
		throw BinderException("Referenced graph mapping names unknown %s type '%s'", StringUtil::Lower(kind),
		                      type_name);
	}
	return result->GetValue(0, 0).GetValue<uint64_t>();
}

static void InsertStaticLabels(Connection &connection, uint64_t element_table_id, uint64_t schema_element_id,
                               const string &fallback) {
	auto labels = GqlQuery(connection, "SELECT label_name FROM gql_internal.graph_schema_labels WHERE "
	                                   "schema_element_id = " +
	                                       to_string(schema_element_id) + " ORDER BY label_ordinal");
	if (labels->RowCount() == 0) {
		GqlQuery(connection, "INSERT INTO gql_internal.graph_label_mappings "
		                     "(element_table_id, label_name, mapping_kind) VALUES (" +
		                         to_string(element_table_id) + ", " + GqlQuoteLiteral(fallback) + ", 'STATIC')");
		return;
	}
	for (idx_t row = 0; row < labels->RowCount(); row++) {
		GqlQuery(connection, "INSERT INTO gql_internal.graph_label_mappings "
		                     "(element_table_id, label_name, mapping_kind) VALUES (" +
		                         to_string(element_table_id) + ", " +
		                         GqlQuoteLiteral(labels->GetValue(0, row).GetValue<string>()) + ", 'STATIC')");
	}
}

static void InsertReferencedProperties(Connection &connection, uint64_t element_table_id, uint64_t schema_element_id,
                                       const TableDescription &table, const vector<GqlPropertyColumnMapping> &mappings,
                                       bool validate) {
	auto properties = GqlQuery(connection, "SELECT property_name, gql_type, nullable FROM "
	                                       "gql_internal.graph_schema_properties WHERE schema_element_id = " +
	                                           to_string(schema_element_id) + " ORDER BY property_ordinal");
	if (properties->RowCount() != mappings.size()) {
		throw BinderException("Referenced table %s.%s must map every declared property exactly once",
		                      table.qualified_name.Schema().GetIdentifierName(),
		                      table.qualified_name.Name().GetIdentifierName());
	}
	unordered_set<string> mapped_properties;
	for (const auto &mapping : mappings) {
		auto normalized = StringUtil::Lower(mapping.property_name);
		if (!mapped_properties.insert(normalized).second) {
			throw BinderException("Duplicate referenced property mapping '%s'", mapping.property_name);
		}
		idx_t schema_row = DConstants::INVALID_INDEX;
		for (idx_t row = 0; row < properties->RowCount(); row++) {
			if (StringUtil::CIEquals(properties->GetValue(0, row).GetValue<string>(), mapping.property_name)) {
				schema_row = row;
				break;
			}
		}
		if (schema_row == DConstants::INVALID_INDEX) {
			throw BinderException("Property '%s' is not declared by the mapped graph type", mapping.property_name);
		}
		auto &column = ResolveColumn(table, mapping.source_column);
		auto gql_type = properties->GetValue(1, schema_row).GetValue<string>();
		auto expected = GqlTypedPropertyDuckType(gql_type);
		if (!StringUtil::CIEquals(column.Type().ToString(), expected)) {
			throw BinderException("Referenced property '%s' expects %s but column %s.%s.%s has type %s",
			                      mapping.property_name, expected, table.qualified_name.Schema().GetIdentifierName(),
			                      table.qualified_name.Name().GetIdentifierName(), column.Name().GetIdentifierName(),
			                      column.Type().ToString());
		}
		auto nullable = properties->GetValue(2, schema_row).GetValue<bool>();
		if (validate && !nullable) {
			auto nulls = GqlQuery(connection, "SELECT count(*)::UBIGINT FROM " + QualifiedTable(table) + " WHERE " +
			                                      GqlQuoteIdentifier(column.Name().GetIdentifierName()) + " IS NULL");
			if (nulls->GetValue(0, 0).GetValue<uint64_t>() != 0) {
				throw InvalidInputException("Referenced property %s.%s.%s violates NOT NULL graph schema",
				                            table.qualified_name.Schema().GetIdentifierName(),
				                            table.qualified_name.Name().GetIdentifierName(),
				                            column.Name().GetIdentifierName());
			}
		}
		GqlQuery(connection, "INSERT INTO gql_internal.graph_property_mappings "
		                     "(element_table_id, property_name, column_name, gql_type, nullable, writable) VALUES (" +
		                         to_string(element_table_id) + ", " + GqlQuoteLiteral(mapping.property_name) + ", " +
		                         GqlQuoteLiteral(column.Name().GetIdentifierName()) + ", " + GqlQuoteLiteral(expected) +
		                         ", " + (nullable ? "true" : "false") + ", false)");
	}
}

static void AppendFingerprintField(string &fingerprint, const string &value) {
	fingerprint += to_string(value.size()) + ":" + value + ";";
}

static string ReadSingleColumn(const Value &value, const char *description);

static uint64_t ReadSourceTableOid(Connection &connection, const TableDescription &table) {
	auto result = GqlQuery(connection,
	                       "SELECT table_oid::UBIGINT FROM duckdb_tables() WHERE "
	                       "lower(database_name) = " +
	                           GqlQuoteLiteral(StringUtil::Lower(table.qualified_name.Catalog().GetIdentifierName())) +
	                           " AND lower(schema_name) = " +
	                           GqlQuoteLiteral(StringUtil::Lower(table.qualified_name.Schema().GetIdentifierName())) +
	                           " AND lower(table_name) = " +
	                           GqlQuoteLiteral(StringUtil::Lower(table.qualified_name.Name().GetIdentifierName())));
	if (result->RowCount() != 1 || result->GetValue(0, 0).IsNull()) {
		throw BinderException("Referenced source table %s.%s.%s is not an attached base table",
		                      table.qualified_name.Catalog().GetIdentifierName(),
		                      table.qualified_name.Schema().GetIdentifierName(),
		                      table.qualified_name.Name().GetIdentifierName());
	}
	return result->GetValue(0, 0).GetValue<uint64_t>();
}

static string ReadDuckLakeTableUuid(Connection &connection, const TableDescription &table) {
	auto result =
	    GqlQuery(connection, "SELECT table_uuid::VARCHAR FROM " +
	                             GqlQuoteIdentifier(table.qualified_name.Catalog().GetIdentifierName()) +
	                             ".table_info() WHERE lower(table_name) = " +
	                             GqlQuoteLiteral(StringUtil::Lower(table.qualified_name.Name().GetIdentifierName())));
	if (result->RowCount() == 0 || result->GetValue(0, 0).IsNull()) {
		throw BinderException("Referenced DuckLake table %s.%s.%s has no persistent table UUID",
		                      table.qualified_name.Catalog().GetIdentifierName(),
		                      table.qualified_name.Schema().GetIdentifierName(),
		                      table.qualified_name.Name().GetIdentifierName());
	}
	if (result->RowCount() != 1) {
		throw NotImplementedException("DuckLake table name '%s' is ambiguous across source schemas; referenced "
		                              "graphs currently require mapped table names to be unique within the catalog",
		                              table.qualified_name.Name().GetIdentifierName());
	}
	return result->GetValue(0, 0).GetValue<string>();
}

static string ReadSourceTableIdentity(Connection &connection, const TableDescription &table,
                                      const string &source_kind) {
	if (StringUtil::CIEquals(source_kind, "DUCKLAKE")) {
		auto database = GqlQuery(
		    connection, "SELECT lower(type) FROM duckdb_databases() WHERE lower(database_name) = " +
		                    GqlQuoteLiteral(StringUtil::Lower(table.qualified_name.Catalog().GetIdentifierName())));
		if (database->RowCount() == 1 && !database->GetValue(0, 0).IsNull() &&
		    database->GetValue(0, 0).GetValue<string>() == "ducklake") {
			return "uuid:" + ReadDuckLakeTableUuid(connection, table);
		}
	}
	return "oid:" + to_string(ReadSourceTableOid(connection, table));
}

static uint64_t ReadReferencedDuckLakeSnapshot(Connection &connection, const string &catalog) {
	auto result =
	    GqlQuery(connection, "SELECT id::UBIGINT FROM " + GqlQuoteIdentifier(catalog) + ".current_snapshot()");
	if (result->RowCount() != 1 || result->GetValue(0, 0).IsNull()) {
		throw InvalidInputException("DuckLake catalog '%s' did not expose a current snapshot", catalog);
	}
	return result->GetValue(0, 0).GetValue<uint64_t>();
}

static bool ReadSourceColumnNullable(Connection &connection, const TableDescription &table, const string &column_name) {
	auto result = GqlQuery(connection,
	                       "SELECT is_nullable FROM duckdb_columns() WHERE lower(database_name) = " +
	                           GqlQuoteLiteral(StringUtil::Lower(table.qualified_name.Catalog().GetIdentifierName())) +
	                           " AND lower(schema_name) = " +
	                           GqlQuoteLiteral(StringUtil::Lower(table.qualified_name.Schema().GetIdentifierName())) +
	                           " AND lower(table_name) = " +
	                           GqlQuoteLiteral(StringUtil::Lower(table.qualified_name.Name().GetIdentifierName())) +
	                           " AND lower(column_name) = " + GqlQuoteLiteral(StringUtil::Lower(column_name)));
	if (result->RowCount() != 1 || result->GetValue(0, 0).IsNull()) {
		throw BinderException("Referenced source column %s.%s.%s.%s no longer exists",
		                      table.qualified_name.Catalog().GetIdentifierName(),
		                      table.qualified_name.Schema().GetIdentifierName(),
		                      table.qualified_name.Name().GetIdentifierName(), column_name);
	}
	return result->GetValue(0, 0).GetValue<bool>();
}

static void AppendSourceColumnFingerprint(Connection &connection, string &fingerprint, const TableDescription &table,
                                          const string &role, const string &column_name) {
	auto &column = ResolveColumn(table, column_name);
	AppendFingerprintField(fingerprint, role);
	AppendFingerprintField(fingerprint, StringUtil::Lower(column.Name().GetIdentifierName()));
	AppendFingerprintField(fingerprint, column.Type().ToString());
	AppendFingerprintField(fingerprint, ReadSourceColumnNullable(connection, table, column.Name().GetIdentifierName())
	                                        ? "NULL"
	                                        : "NOT_NULL");
}

// This is intentionally based only on graph-visible source structure. Adding
// an unmapped/private source column must not invalidate a referenced graph.
// Persistent DuckLake table UUIDs (and native DuckDB table OIDs) distinguish a
// compatible table from a new table recreated under the same qualified name.
static string BuildReferencedSchemaFingerprint(Connection &connection, uint64_t graph_id, const string &source_kind) {
	string fingerprint = "v1|";
	auto tables = GqlQuery(connection, "SELECT element_table_id, element_kind, catalog_name, schema_name, table_name, "
	                                   "key_columns FROM gql_internal.graph_element_tables WHERE graph_id = " +
	                                       to_string(graph_id) +
	                                       " AND ownership = 'REFERENCED' ORDER BY element_kind, element_table_id");
	if (tables->RowCount() < 2) {
		throw InvalidInputException("Referenced graph metadata must contain vertex and edge tables");
	}
	for (idx_t row = 0; row < tables->RowCount(); row++) {
		auto element_table_id = tables->GetValue(0, row).GetValue<uint64_t>();
		auto kind = tables->GetValue(1, row).GetValue<string>();
		auto catalog = tables->GetValue(2, row).GetValue<string>();
		auto schema = tables->GetValue(3, row).GetValue<string>();
		auto table_name = tables->GetValue(4, row).GetValue<string>();
		auto table = ResolveTable(connection, GqlQuoteIdentifier(catalog) + "." + GqlQuoteIdentifier(schema) + "." +
		                                          GqlQuoteIdentifier(table_name));
		AppendFingerprintField(fingerprint, kind);
		AppendFingerprintField(fingerprint, StringUtil::Lower(table->qualified_name.Catalog().GetIdentifierName()));
		AppendFingerprintField(fingerprint, StringUtil::Lower(table->qualified_name.Schema().GetIdentifierName()));
		AppendFingerprintField(fingerprint, StringUtil::Lower(table->qualified_name.Name().GetIdentifierName()));
		AppendFingerprintField(fingerprint, ReadSourceTableIdentity(connection, *table, source_kind));
		auto key_columns_value = tables->GetValue(5, row);
		const auto &key_columns = ListValue::GetChildren(key_columns_value);
		if (key_columns.empty() && kind == "EDGE") {
			AppendFingerprintField(fingerprint, "GENERATED_KEY");
		} else {
			AppendSourceColumnFingerprint(connection, fingerprint, *table, "KEY",
			                              ReadSingleColumn(tables->GetValue(5, row), "element key"));
		}

		if (kind == "EDGE") {
			auto endpoints = GqlQuery(connection, "SELECT source_columns, target_columns FROM "
			                                      "gql_internal.graph_edge_endpoints WHERE edge_table_id = " +
			                                          to_string(element_table_id));
			if (endpoints->RowCount() != 1) {
				throw InvalidInputException("Referenced graph contains invalid edge endpoint metadata");
			}
			AppendSourceColumnFingerprint(connection, fingerprint, *table, "SOURCE",
			                              ReadSingleColumn(endpoints->GetValue(0, 0), "edge source"));
			AppendSourceColumnFingerprint(connection, fingerprint, *table, "TARGET",
			                              ReadSingleColumn(endpoints->GetValue(1, 0), "edge destination"));
		}

		auto labels =
		    GqlQuery(connection, "SELECT mapping_kind, coalesce(label_name, ''), coalesce(column_name, '') "
		                         "FROM gql_internal.graph_label_mappings WHERE element_table_id = " +
		                             to_string(element_table_id) + " ORDER BY mapping_kind, label_name, column_name");
		for (idx_t label = 0; label < labels->RowCount(); label++) {
			auto mapping_kind = labels->GetValue(0, label).GetValue<string>();
			AppendFingerprintField(fingerprint, "LABEL");
			AppendFingerprintField(fingerprint, mapping_kind);
			AppendFingerprintField(fingerprint, labels->GetValue(1, label).GetValue<string>());
			auto column_name = labels->GetValue(2, label).GetValue<string>();
			if (!column_name.empty()) {
				AppendSourceColumnFingerprint(connection, fingerprint, *table, "LABEL_COLUMN", column_name);
			}
		}

		auto types = GqlQuery(connection, "SELECT se.element_kind, se.type_name, tm.discriminator_kind, "
		                                  "coalesce(tm.discriminator_value, '') FROM "
		                                  "gql_internal.graph_element_type_mappings tm JOIN "
		                                  "gql_internal.graph_schema_elements se USING (schema_element_id) WHERE "
		                                  "tm.element_table_id = " +
		                                      to_string(element_table_id) + " ORDER BY se.element_kind, se.type_name");
		for (idx_t type = 0; type < types->RowCount(); type++) {
			AppendFingerprintField(fingerprint, "TYPE");
			for (idx_t column = 0; column < 4; column++) {
				AppendFingerprintField(fingerprint, types->GetValue(column, type).GetValue<string>());
			}
		}

		auto properties = GqlQuery(connection, "SELECT property_name, column_name, gql_type, nullable FROM "
		                                       "gql_internal.graph_property_mappings WHERE element_table_id = " +
		                                           to_string(element_table_id) + " ORDER BY property_name");
		for (idx_t property = 0; property < properties->RowCount(); property++) {
			AppendFingerprintField(fingerprint, "PROPERTY");
			AppendFingerprintField(fingerprint, properties->GetValue(0, property).GetValue<string>());
			AppendFingerprintField(fingerprint, properties->GetValue(2, property).GetValue<string>());
			AppendFingerprintField(fingerprint,
			                       properties->GetValue(3, property).GetValue<bool>() ? "NULL" : "NOT_NULL");
			AppendSourceColumnFingerprint(connection, fingerprint, *table, "PROPERTY_COLUMN",
			                              properties->GetValue(1, property).GetValue<string>());
		}
	}
	return fingerprint;
}

void GqlAttachReferencedGraphTables(Connection &connection, const string &graph_name,
                                    const GqlReferencedTableMapping &mapping) {
	GqlEnsureStorage(connection);
	if (mapping.elements.empty()) {
		throw BinderException("Referenced graph requires element table mappings");
	}
	struct ResolvedMapping {
		const GqlReferencedElementMapping *mapping;
		unique_ptr<TableDescription> table;
		const ColumnDefinition *key;
		const ColumnDefinition *source = nullptr;
		const ColumnDefinition *target = nullptr;
		uint64_t schema_id;
		uint64_t table_id = 0;
	};
	vector<ResolvedMapping> resolved;
	unordered_map<string, idx_t> vertices_by_type;
	idx_t vertex_count = 0;
	idx_t edge_count = 0;
	string source_catalog;
	for (const auto &element : mapping.elements) {
		if (element.kind != "VERTEX" && element.kind != "EDGE") {
			throw BinderException("Referenced graph element kind must be VERTEX or EDGE");
		}
		auto table = ResolveTable(connection, element.table);
		if (source_catalog.empty()) {
			source_catalog = table->qualified_name.Catalog().GetIdentifierName();
		} else if (!StringUtil::CIEquals(source_catalog, table->qualified_name.Catalog().GetIdentifierName())) {
			throw BinderException("All referenced graph tables must use the same catalog");
		}
		const ColumnDefinition *key = nullptr;
		if (!element.key.empty()) {
			key = &ResolveColumn(*table, element.key);
			if (!IsReferencedKeyType(key->Type())) {
				throw BinderException("Referenced graph keys must use integer types");
			}
		} else if (element.kind == "VERTEX") {
			throw BinderException("Referenced vertex mappings require a source key");
		}
		ResolvedMapping entry {&element, std::move(table), key};
		if (element.kind == "VERTEX") {
			auto normalized_type = StringUtil::Lower(element.schema_type);
			if (!vertices_by_type.emplace(normalized_type, resolved.size()).second) {
				throw BinderException("Node type '%s' is mapped by more than one vertex table", element.schema_type);
			}
			entry.schema_id = 0;
			vertex_count++;
		} else {
			entry.source = &ResolveColumn(*entry.table, element.source);
			entry.target = &ResolveColumn(*entry.table, element.target);
			entry.schema_id = 0;
			edge_count++;
		}
		resolved.push_back(std::move(entry));
	}
	if (vertex_count == 0 || edge_count == 0) {
		throw BinderException("Referenced graph requires at least one vertex and one edge table mapping");
	}

	auto graph =
	    GqlQuery(connection, "SELECT g.graph_id, gs.storage_mode, g.graph_version FROM gql_internal.graphs g JOIN "
	                         "gql_internal.graph_storage gs USING (graph_id) WHERE g.graph_name = " +
	                             GqlQuoteLiteral(graph_name));
	if (graph->RowCount() != 1 || graph->GetValue(1, 0).GetValue<string>() != "EMPTY") {
		throw InvalidInputException("Graph '%s' must be empty before attaching referenced tables", graph_name);
	}
	auto graph_id = graph->GetValue(0, 0).GetValue<uint64_t>();
	for (auto &entry : resolved) {
		entry.schema_id = FindSchemaElement(connection, graph_id, entry.mapping->kind == "VERTEX" ? "NODE" : "EDGE",
		                                    entry.mapping->schema_type);
		if (mapping.validate && entry.key) {
			ValidateKey(connection, *entry.table, *entry.key);
		}
	}
	for (auto &entry : resolved) {
		if (entry.mapping->kind != "EDGE") {
			continue;
		}
		auto source_entry = vertices_by_type.find(StringUtil::Lower(entry.mapping->source_schema_type));
		auto target_entry = vertices_by_type.find(StringUtil::Lower(entry.mapping->target_schema_type));
		if (source_entry == vertices_by_type.end() || target_entry == vertices_by_type.end()) {
			throw BinderException("Referenced edge type '%s' names an unmapped source or destination node type",
			                      entry.mapping->schema_type);
		}
		auto &source_vertex = resolved[source_entry->second];
		auto &target_vertex = resolved[target_entry->second];
		if (entry.source->Type() != source_vertex.key->Type() || entry.target->Type() != target_vertex.key->Type()) {
			throw BinderException("Referenced edge type '%s' endpoint types must exactly match node keys (%s, %s)",
			                      entry.mapping->schema_type, source_vertex.key->Type().ToString(),
			                      target_vertex.key->Type().ToString());
		}
		if (mapping.validate) {
			ValidateEndpoint(connection, *entry.table, *entry.source, *source_vertex.table, *source_vertex.key,
			                 "source");
			ValidateEndpoint(connection, *entry.table, *entry.target, *target_vertex.table, *target_vertex.key,
			                 "destination");
		}
	}
	for (auto &entry : resolved) {
		entry.table_id = InsertElementTable(connection, graph_id, entry.mapping->kind.c_str(), *entry.table,
		                                    entry.key ? entry.key->Name().GetIdentifierName() : string(), "REFERENCED");
		GqlQuery(connection, "UPDATE gql_internal.graph_element_tables SET access_mode = 'READ_ONLY' WHERE "
		                     "element_table_id = " +
		                         to_string(entry.table_id));
		InsertStaticLabels(connection, entry.table_id, entry.schema_id, entry.mapping->schema_type);
		InsertReferencedProperties(connection, entry.table_id, entry.schema_id, *entry.table, entry.mapping->properties,
		                           mapping.validate);
		GqlQuery(connection, "INSERT INTO gql_internal.graph_element_type_mappings "
		                     "(element_table_id, schema_element_id, discriminator_kind) VALUES (" +
		                         to_string(entry.table_id) + ", " + to_string(entry.schema_id) + ", 'STATIC')");
	}
	for (const auto &entry : resolved) {
		if (entry.mapping->kind != "EDGE") {
			continue;
		}
		auto &source_vertex = resolved[vertices_by_type.at(StringUtil::Lower(entry.mapping->source_schema_type))];
		auto &target_vertex = resolved[vertices_by_type.at(StringUtil::Lower(entry.mapping->target_schema_type))];
		GqlQuery(connection, "INSERT INTO gql_internal.graph_edge_endpoints "
		                     "(edge_table_id, source_vertex_table_id, target_vertex_table_id, source_columns, "
		                     "target_columns, source_key_columns, target_key_columns) VALUES (" +
		                         to_string(entry.table_id) + ", " + to_string(source_vertex.table_id) + ", " +
		                         to_string(target_vertex.table_id) + ", [" +
		                         GqlQuoteLiteral(entry.source->Name().GetIdentifierName()) + "], [" +
		                         GqlQuoteLiteral(entry.target->Name().GetIdentifierName()) + "], [" +
		                         GqlQuoteLiteral(source_vertex.key->Name().GetIdentifierName()) + "], [" +
		                         GqlQuoteLiteral(target_vertex.key->Name().GetIdentifierName()) + "])");
	}

	auto database = GqlQuery(connection, "SELECT lower(type) FROM duckdb_databases() WHERE database_name = " +
	                                         GqlQuoteLiteral(source_catalog));
	if (database->RowCount() != 1) {
		throw BinderException("Referenced source catalog '%s' is not attached", source_catalog);
	}
	auto database_type = database->GetValue(0, 0).GetValue<string>();
	string source_kind;
	if (database_type == "ducklake") {
		source_kind = "DUCKLAKE";
	} else if (database_type == "duckdb") {
		source_kind = "DUCKDB";
	} else {
		throw NotImplementedException("Referenced graphs do not yet support catalog type '%s'", database_type);
	}
	string snapshot = "NULL";
	if (source_kind == "DUCKLAKE") {
		snapshot = to_string(ReadReferencedDuckLakeSnapshot(connection, source_catalog));
	} else if (StringUtil::CIEquals(mapping.snapshot_policy, "PINNED")) {
		throw BinderException("SNAPSHOT_POLICY 'PINNED' requires a DuckLake source catalog attached with "
		                      "SNAPSHOT_VERSION");
	}
	auto pinned_snapshot = StringUtil::CIEquals(mapping.snapshot_policy, "PINNED") ? snapshot : "NULL";
	auto fingerprint = BuildReferencedSchemaFingerprint(connection, graph_id, source_kind);
	GqlQuery(connection, "INSERT INTO gql_internal.graph_sources "
	                     "(graph_id, source_kind, source_catalog, snapshot_policy, pinned_snapshot_id, access_mode, "
	                     "registered_snapshot_id, last_validated_snapshot_id, schema_fingerprint) VALUES (" +
	                         to_string(graph_id) + ", " + GqlQuoteLiteral(source_kind) + ", " +
	                         GqlQuoteLiteral(source_catalog) + ", " + GqlQuoteLiteral(mapping.snapshot_policy) + ", " +
	                         pinned_snapshot + ", " + GqlQuoteLiteral(mapping.access_mode) + ", " + snapshot + ", " +
	                         (mapping.validate ? snapshot : "NULL") + ", " + GqlQuoteLiteral(fingerprint) + ")");
	GqlQuery(connection, "UPDATE gql_internal.graph_storage SET storage_mode = 'TABLE_BACKED', default_catalog = " +
	                         GqlQuoteLiteral(source_catalog) + ", default_schema = " +
	                         GqlQuoteLiteral(resolved[0].table->qualified_name.Schema().GetIdentifierName()) +
	                         ", schema_version = schema_version + 1, "
	                         "csr_policy = 'MANUAL' WHERE graph_id = " +
	                         to_string(graph_id));
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

static const string *FindPropertyColumn(const GqlElementTableBinding &table, const string &property) {
	for (const auto &entry : table.property_columns) {
		if (StringUtil::CIEquals(entry.first, property)) {
			return &entry.second;
		}
	}
	return nullptr;
}

static vector<pair<string, string>> ReadLogicalProperties(Connection &connection, uint64_t graph_id,
                                                          const string &element_kind) {
	auto properties =
	    GqlQuery(connection, "SELECT pm.property_name, pm.gql_type FROM "
	                         "gql_internal.graph_property_mappings pm JOIN "
	                         "gql_internal.graph_element_tables et USING (element_table_id) WHERE "
	                         "et.graph_id = " +
	                             to_string(graph_id) + " AND et.element_kind = " + GqlQuoteLiteral(element_kind) +
	                             " GROUP BY pm.property_name, pm.gql_type ORDER BY lower(pm.property_name)");
	vector<pair<string, string>> result;
	for (idx_t row = 0; row < properties->RowCount(); row++) {
		result.emplace_back(properties->GetValue(0, row).GetValue<string>(),
		                    properties->GetValue(1, row).GetValue<string>());
	}
	return result;
}

struct ReferencedPhysicalBinding {
	GqlElementTableBinding table;
	uint64_t source_table_id = 0;
	uint64_t target_table_id = 0;
	string source_column;
	string target_column;
};

static string QualifiedReferencedTable(const GqlElementTableBinding &table, bool pinned = false,
                                       uint64_t snapshot = 0) {
	auto name = GqlQuoteIdentifier(table.catalog_name) + "." + GqlQuoteIdentifier(table.schema_name) + "." +
	            GqlQuoteIdentifier(table.table_name);
	if (!pinned)
		return name;
	// The wrapper exposes anonymous edge rowids and permits aliases after it.
	return "(SELECT *, rowid FROM " + name + " AT (VERSION => " + to_string(snapshot) + "))";
}

static string ReferencedDenseId(const vector<ReferencedPhysicalBinding> &tables, idx_t table_index,
                                const string &table_alias, bool pinned, uint64_t snapshot) {
	string expression = "CAST(";
	for (idx_t index = 0; index < table_index; index++) {
		expression += "(SELECT count(*)::UBIGINT FROM " +
		              QualifiedReferencedTable(tables[index].table, pinned, snapshot) + ") + ";
	}
	auto order_column =
	    tables[table_index].table.key_column.empty() ? string("rowid") : tables[table_index].table.key_column;
	expression += "row_number() OVER (ORDER BY " + GqlQuoteIdentifier(table_alias) + "." +
	              GqlQuoteIdentifier(order_column) + ") AS UBIGINT)";
	return expression;
}

static string ReferencedVertexLookup(const vector<ReferencedPhysicalBinding> &vertices, idx_t vertex_index,
                                     const string &lookup_alias, bool pinned, uint64_t snapshot) {
	auto base_alias = lookup_alias + "_base";
	return "(SELECT " + GqlQuoteIdentifier(base_alias) + "." +
	       GqlQuoteIdentifier(vertices[vertex_index].table.key_column) + " AS " +
	       GqlQuoteIdentifier("__gql_source_key") + ", " +
	       ReferencedDenseId(vertices, vertex_index, base_alias, pinned, snapshot) + " AS " +
	       GqlQuoteIdentifier("__gql_id") + " FROM " +
	       QualifiedReferencedTable(vertices[vertex_index].table, pinned, snapshot) + " AS " +
	       GqlQuoteIdentifier(base_alias) + ") AS " + GqlQuoteIdentifier(lookup_alias);
}

static idx_t FindReferencedVertex(const vector<ReferencedPhysicalBinding> &vertices, uint64_t element_table_id) {
	for (idx_t index = 0; index < vertices.size(); index++) {
		if (vertices[index].table.element_table_id == element_table_id) {
			return index;
		}
	}
	throw InvalidInputException("Referenced edge endpoint names an unknown vertex mapping");
}

static GqlElementTableBinding BuildReferencedUnion(Connection &connection, uint64_t graph_id, const string &kind,
                                                   const vector<ReferencedPhysicalBinding> &tables,
                                                   const vector<ReferencedPhysicalBinding> &vertices,
                                                   const string &graph_name, bool cached_ids, uint64_t snapshot_id,
                                                   bool pinned = false) {
	GqlElementTableBinding result;
	result.catalog_name = tables[0].table.catalog_name;
	result.schema_name = "gql_internal";
	result.table_name = "graph_" + to_string(graph_id) + "_referenced_" + StringUtil::Lower(kind) + "_union";
	result.ownership = "REFERENCED";
	result.key_column = kind == "VERTEX" ? "__gql_id" : "__gql_edge_id";
	result.label_column = kind == "VERTEX" ? "__gql_label" : "__gql_type";
	result.label_is_list = kind == "VERTEX";
	auto properties = ReadLogicalProperties(connection, graph_id, kind);
	for (const auto &property : properties) {
		result.property_columns.emplace(property.first, property.first);
	}
	for (const auto &entry : tables) {
		result.referenced_sources.push_back({entry.table.element_table_id, entry.table.catalog_name,
		                                     entry.table.schema_name, entry.table.table_name, entry.table.key_column,
		                                     entry.table.static_labels, entry.table.property_columns});
	}
	if (cached_ids) {
		auto mapping = [&](uint64_t id, const string &alias) {
			return "gql_referenced_keys(" + GqlQuoteLiteral(graph_name) + ", " + to_string(id) + "::UBIGINT, " +
			       to_string(snapshot_id) + "::UBIGINT) AS " + GqlQuoteIdentifier(alias);
		};
		for (idx_t index = 0; index < tables.size(); index++) {
			const auto &entry = tables[index];
			const auto alias = "gql_referenced_" + StringUtil::Lower(kind) + "_" + to_string(index);
			const auto ids = alias + "_ids";
			if (index)
				result.relation_sql += " UNION ALL ";
			result.relation_sql +=
			    "SELECT " + GqlQuoteIdentifier(ids) + ".__gql_id AS " + GqlQuoteIdentifier(result.key_column);
			if (kind == "VERTEX") {
				result.relation_sql += ", [";
				for (idx_t label = 0; label < entry.table.static_labels.size(); label++) {
					if (label)
						result.relation_sql += ", ";
					result.relation_sql += GqlQuoteLiteral(entry.table.static_labels[label]);
				}
				result.relation_sql += "]::VARCHAR[] AS " + GqlQuoteIdentifier(result.label_column);
			} else {
				result.relation_sql +=
				    ", " + GqlQuoteLiteral(entry.table.static_labels[0]) +
				    " AS __gql_type, src.__gql_id AS __gql_source_id, dst.__gql_id AS __gql_target_id";
			}
			for (const auto &property : properties) {
				auto column = FindPropertyColumn(entry.table, property.first);
				result.relation_sql += ", " +
				                       (column ? GqlQuoteIdentifier(alias) + "." + GqlQuoteIdentifier(*column)
				                               : "CAST(NULL AS " + property.second + ")") +
				                       " AS " + GqlQuoteIdentifier(property.first);
			}
			auto key = entry.table.key_column.empty() ? string("rowid") : entry.table.key_column;
			result.relation_sql += " FROM " + QualifiedReferencedTable(entry.table) + " AS " +
			                       GqlQuoteIdentifier(alias) + " AT (VERSION => " + to_string(snapshot_id) + ")";
			result.relation_sql += " JOIN " + mapping(entry.table.element_table_id, ids) + " ON " +
			                       GqlQuoteIdentifier(alias) + "." + GqlQuoteIdentifier(key) + " = " +
			                       GqlQuoteIdentifier(ids) + ".source_key";
			if (kind == "EDGE") {
				result.relation_sql += " LEFT JOIN " + mapping(entry.source_table_id, "src") + " ON " +
				                       GqlQuoteIdentifier(alias) + "." + GqlQuoteIdentifier(entry.source_column) +
				                       " = src.source_key";
				result.relation_sql += " LEFT JOIN " + mapping(entry.target_table_id, "dst") + " ON " +
				                       GqlQuoteIdentifier(alias) + "." + GqlQuoteIdentifier(entry.target_column) +
				                       " = dst.source_key";
			}
			result.source_tables.push_back({entry.table.catalog_name, entry.table.schema_name, entry.table.table_name});
		}
		return result;
	}

	for (idx_t table_index = 0; table_index < tables.size(); table_index++) {
		auto &entry = tables[table_index];
		result.source_tables.push_back({entry.table.catalog_name, entry.table.schema_name, entry.table.table_name});
		if (entry.table.ownership != "REFERENCED") {
			throw InvalidInputException("Heterogeneous table-backed graphs require referenced element tables");
		}
		if (!result.relation_sql.empty()) {
			result.relation_sql += " UNION ALL ";
		}
		auto table_alias = "gql_referenced_" + StringUtil::Lower(kind) + "_" + to_string(table_index);
		result.relation_sql += "SELECT " + ReferencedDenseId(tables, table_index, table_alias, pinned, snapshot_id) +
		                       " AS " + GqlQuoteIdentifier(result.key_column);
		if (kind == "VERTEX") {
			result.relation_sql += ", [";
			for (idx_t label = 0; label < entry.table.static_labels.size(); label++) {
				if (label > 0) {
					result.relation_sql += ", ";
				}
				result.relation_sql += GqlQuoteLiteral(entry.table.static_labels[label]);
			}
			result.relation_sql += "]::VARCHAR[] AS " + GqlQuoteIdentifier(result.label_column);
		} else {
			if (entry.table.static_labels.size() != 1 || entry.source_column.empty() || entry.target_column.empty()) {
				throw InvalidInputException("Referenced edge table contains invalid type or endpoint metadata");
			}
			result.relation_sql += ", " + GqlQuoteLiteral(entry.table.static_labels[0]) + " AS " +
			                       GqlQuoteIdentifier(result.label_column) + ", " +
			                       GqlQuoteIdentifier("gql_referenced_source") + "." + GqlQuoteIdentifier("__gql_id") +
			                       " AS " + GqlQuoteIdentifier("__gql_source_id") + ", " +
			                       GqlQuoteIdentifier("gql_referenced_target") + "." + GqlQuoteIdentifier("__gql_id") +
			                       " AS " + GqlQuoteIdentifier("__gql_target_id");
		}
		for (const auto &property : properties) {
			auto column = FindPropertyColumn(entry.table, property.first);
			result.relation_sql += ", ";
			if (column) {
				result.relation_sql += GqlQuoteIdentifier(table_alias) + "." + GqlQuoteIdentifier(*column);
			} else {
				result.relation_sql += "CAST(NULL AS " + property.second + ")";
			}
			result.relation_sql += " AS " + GqlQuoteIdentifier(property.first);
		}
		result.relation_sql += " FROM " + QualifiedReferencedTable(entry.table, pinned, snapshot_id) + " AS " +
		                       GqlQuoteIdentifier(table_alias);
		if (kind == "EDGE") {
			auto source_index = FindReferencedVertex(vertices, entry.source_table_id);
			auto target_index = FindReferencedVertex(vertices, entry.target_table_id);
			result.relation_sql +=
			    " LEFT JOIN " +
			    ReferencedVertexLookup(vertices, source_index, "gql_referenced_source", pinned, snapshot_id) + " ON " +
			    GqlQuoteIdentifier(table_alias) + "." + GqlQuoteIdentifier(entry.source_column) + " = " +
			    GqlQuoteIdentifier("gql_referenced_source") + "." + GqlQuoteIdentifier("__gql_source_key") +
			    " LEFT JOIN " +
			    ReferencedVertexLookup(vertices, target_index, "gql_referenced_target", pinned, snapshot_id) + " ON " +
			    GqlQuoteIdentifier(table_alias) + "." + GqlQuoteIdentifier(entry.target_column) + " = " +
			    GqlQuoteIdentifier("gql_referenced_target") + "." + GqlQuoteIdentifier("__gql_source_key");
		}
	}
	return result;
}

static void LoadLabel(Connection &connection, GqlElementTableBinding &table) {
	auto result = GqlQuery(connection, "SELECT mapping_kind, column_name, label_name FROM "
	                                   "gql_internal.graph_label_mappings WHERE "
	                                   "element_table_id = " +
	                                       to_string(table.element_table_id));
	if (result->RowCount() == 0) {
		return;
	}
	for (idx_t row = 0; row < result->RowCount(); row++) {
		auto mapping_kind = result->GetValue(0, row).GetValue<string>();
		if (mapping_kind == "STATIC") {
			if (!table.label_column.empty() || result->GetValue(2, row).IsNull()) {
				throw InvalidInputException("Table-backed graph contains inconsistent static label metadata");
			}
			table.static_labels.push_back(result->GetValue(2, row).GetValue<string>());
			continue;
		}
		if ((mapping_kind != "SCALAR_COLUMN" && mapping_kind != "LIST_COLUMN") || result->RowCount() != 1 ||
		    !table.static_labels.empty()) {
			throw NotImplementedException("Table-backed MATCH does not support mixed label mapping kind '%s'",
			                              mapping_kind);
		}
		table.label_is_list = mapping_kind == "LIST_COLUMN";
		table.label_column = result->GetValue(1, row).GetValue<string>();
	}
}

// Binding a MATCH otherwise repeats a dozen catalog queries per statement. Only
// managed graphs are cached: referenced graphs must revalidate external sources.
struct GqlTableGraphCache : ObjectCacheEntry {
	static string ObjectType() {
		return "gql_table_graph_cache";
	}

	string GetObjectType() override {
		return ObjectType();
	}

	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx {};
	}

	struct Entry {
		uint64_t generation = 0;
		weak_ptr<AttachedDatabase> database;
		GqlTableGraphBinding binding;
	};

	mutex lock;
	uint64_t generation = 0;
	unordered_map<string, Entry> entries;
};

static shared_ptr<GqlTableGraphCache> GetTableGraphCache(ClientContext &context) {
	return ObjectCache::GetObjectCache(context).GetOrCreate<GqlTableGraphCache>("gql_table_graph_cache");
}

// Metadata connections resolve gql_internal in the instance default database.
static shared_ptr<AttachedDatabase> MetadataDatabase(ClientContext &context) {
	auto &manager = DatabaseManager::Get(context);
	return manager.GetDatabase(manager.GetDefaultDatabase());
}

static optional_ptr<const GqlTableGraphBinding> FindCachedTableGraph(const GqlTableGraphCache &cache,
                                                                     const string &graph_name,
                                                                     const shared_ptr<AttachedDatabase> &database) {
	auto entry = cache.entries.find(graph_name);
	if (entry == cache.entries.end() || entry->second.generation != cache.generation || !database ||
	    entry->second.database.lock() != database) {
		return nullptr;
	}
	return entry->second.binding;
}

void GqlInvalidateTableGraphCache(ClientContext &context) {
	auto cache = GetTableGraphCache(context);
	lock_guard<mutex> guard(cache->lock);
	cache->generation++;
	cache->entries.clear();
}

uint64_t GqlDatabaseWriteGeneration(ClientContext &context) {
	auto cache = GetTableGraphCache(context);
	lock_guard<mutex> guard(cache->lock);
	return cache->generation;
}

bool GqlTryGetCachedGraphVersion(ClientContext &context, const string &graph_name, uint64_t &graph_id,
                                 uint64_t &graph_version) {
	auto cache = GetTableGraphCache(context);
	auto database = MetadataDatabase(context);
	lock_guard<mutex> guard(cache->lock);
	auto binding = FindCachedTableGraph(*cache, graph_name, database);
	if (!binding) {
		return false;
	}
	graph_id = binding->graph_id;
	graph_version = binding->graph_version;
	return true;
}

static bool LoadTableGraph(ClientContext &context, const string &graph_name, GqlTableGraphBinding &result);

bool GqlTryLoadTableGraph(ClientContext &context, const string &graph_name, GqlTableGraphBinding &result) {
	auto cache = GetTableGraphCache(context);
	auto database = MetadataDatabase(context);
	uint64_t generation;
	{
		lock_guard<mutex> guard(cache->lock);
		auto binding = FindCachedTableGraph(*cache, graph_name, database);
		if (binding) {
			result = *binding;
			return true;
		}
		// Captured before reading: a commit racing the load then expires this entry.
		generation = cache->generation;
	}
	if (!LoadTableGraph(context, graph_name, result)) {
		return false;
	}
	if (result.source_kind.empty() && StringUtil::CIEquals(result.vertex.ownership, "MANAGED") &&
	    StringUtil::CIEquals(result.edge.ownership, "MANAGED")) {
		lock_guard<mutex> guard(cache->lock);
		if (generation == cache->generation) {
			cache->entries[graph_name] = {generation, database, result};
		}
	}
	return true;
}

static bool LoadTableGraph(ClientContext &context, const string &graph_name, GqlTableGraphBinding &result) {
	Connection connection(*context.db);
	auto storage =
	    GqlQuery(connection, "SELECT g.graph_id, gs.storage_mode, g.graph_version FROM gql_internal.graphs g JOIN "
	                         "gql_internal.graph_storage gs USING (graph_id) WHERE g.graph_name = " +
	                             GqlQuoteLiteral(graph_name));
	if (storage->RowCount() == 0) {
		throw InvalidInputException("Graph '%s' does not exist", graph_name);
	}
	result.graph_id = storage->GetValue(0, 0).GetValue<uint64_t>();
	result.graph_version = storage->GetValue(2, 0).GetValue<uint64_t>();
	if (storage->GetValue(1, 0).GetValue<string>() != "TABLE_BACKED") {
		return false;
	}

	auto tables =
	    GqlQuery(connection, "SELECT element_table_id, element_kind, catalog_name, schema_name, "
	                         "table_name, "
	                         "key_columns, ownership FROM gql_internal.graph_element_tables WHERE graph_id = " +
	                             to_string(result.graph_id) + " ORDER BY element_kind, element_table_id");
	vector<ReferencedPhysicalBinding> vertex_tables;
	vector<ReferencedPhysicalBinding> edge_tables;
	for (idx_t row = 0; row < tables->RowCount(); row++) {
		auto kind = tables->GetValue(1, row).GetValue<string>();
		if (kind != "VERTEX" && kind != "EDGE") {
			throw InvalidInputException("Table-backed graph '%s' contains invalid element table metadata", graph_name);
		}
		ReferencedPhysicalBinding physical;
		auto *target = &physical.table;
		target->element_table_id = tables->GetValue(0, row).GetValue<uint64_t>();
		target->catalog_name = tables->GetValue(2, row).GetValue<string>();
		target->schema_name = tables->GetValue(3, row).GetValue<string>();
		target->table_name = tables->GetValue(4, row).GetValue<string>();
		target->ownership = tables->GetValue(6, row).GetValue<string>();
		// Managed tables live beside this metadata, whose catalog is named after the file.
		if (target->ownership == "MANAGED") {
			target->catalog_name = MetadataDatabase(context)->GetName().GetIdentifierName();
		}
		auto key_columns_value = tables->GetValue(5, row);
		const auto &key_columns = ListValue::GetChildren(key_columns_value);
		if (kind == "EDGE" && target->ownership == "REFERENCED" && key_columns.empty()) {
			target->key_column.clear();
		} else {
			target->key_column = ReadSingleColumn(tables->GetValue(5, row), "element key");
		}
		LoadLabel(connection, *target);
		LoadProperties(connection, *target);
		LoadPropertyIndexes(connection, *target);
		if (kind == "EDGE") {
			auto endpoints = GqlQuery(connection, "SELECT source_vertex_table_id, target_vertex_table_id, "
			                                      "source_columns, target_columns FROM "
			                                      "gql_internal.graph_edge_endpoints WHERE edge_table_id = " +
			                                          to_string(target->element_table_id));
			if (endpoints->RowCount() != 1) {
				throw InvalidInputException("Table-backed graph '%s' has invalid endpoint metadata", graph_name);
			}
			physical.source_table_id = endpoints->GetValue(0, 0).GetValue<uint64_t>();
			physical.target_table_id = endpoints->GetValue(1, 0).GetValue<uint64_t>();
			physical.source_column = ReadSingleColumn(endpoints->GetValue(2, 0), "edge source");
			physical.target_column = ReadSingleColumn(endpoints->GetValue(3, 0), "edge destination");
			edge_tables.push_back(std::move(physical));
		} else {
			vertex_tables.push_back(std::move(physical));
		}
	}
	if (vertex_tables.empty() || edge_tables.empty()) {
		throw InvalidInputException("Table-backed graph '%s' must contain vertex and edge tables", graph_name);
	}
	unordered_set<uint64_t> vertex_table_ids;
	for (const auto &vertex : vertex_tables) {
		vertex_table_ids.insert(vertex.table.element_table_id);
	}
	for (const auto &edge : edge_tables) {
		if (vertex_table_ids.find(edge.source_table_id) == vertex_table_ids.end() ||
		    vertex_table_ids.find(edge.target_table_id) == vertex_table_ids.end()) {
			throw InvalidInputException("Table-backed graph '%s' has invalid endpoint table metadata", graph_name);
		}
	}
	auto source = GqlQuery(connection, "SELECT source_kind, source_catalog, snapshot_policy, pinned_snapshot_id, "
	                                   "access_mode, schema_fingerprint FROM "
	                                   "gql_internal.graph_sources WHERE graph_id = " +
	                                       to_string(result.graph_id));
	if (source->RowCount() == 1) {
		result.source_kind = source->GetValue(0, 0).GetValue<string>();
		result.source_catalog = source->GetValue(1, 0).GetValue<string>();
		result.snapshot_policy = source->GetValue(2, 0).GetValue<string>();
		if (!source->GetValue(3, 0).IsNull()) {
			result.has_pinned_snapshot = true;
			result.pinned_snapshot_id = source->GetValue(3, 0).GetValue<uint64_t>();
		}
		result.access_mode = source->GetValue(4, 0).GetValue<string>();
		auto registered_fingerprint = source->GetValue(5, 0).GetValue<string>();
		try {
			if (StringUtil::CIEquals(result.snapshot_policy, "PINNED")) {
				if (!StringUtil::CIEquals(result.source_kind, "DUCKLAKE") || !result.has_pinned_snapshot) {
					throw InvalidInputException("Pinned graph '%s' has incomplete DuckLake snapshot metadata",
					                            graph_name);
				}
				auto observed = ReadReferencedDuckLakeSnapshot(connection, result.source_catalog);
				if (observed != result.pinned_snapshot_id) {
					throw InvalidInputException(
					    "Pinned graph '%s' requires DuckLake catalog '%s' attached with SNAPSHOT_VERSION %llu; "
					    "observed snapshot %llu",
					    graph_name, result.source_catalog, result.pinned_snapshot_id, observed);
				}
			}
			auto current_fingerprint =
			    BuildReferencedSchemaFingerprint(connection, result.graph_id, result.source_kind);
			if (registered_fingerprint != current_fingerprint) {
				throw InvalidInputException("Referenced graph '%s' source schema or table identity changed; recreate "
				                            "the graph mapping",
				                            graph_name);
			}
		} catch (const InvalidInputException &) {
			throw;
		} catch (const std::exception &error) {
			throw InvalidInputException("Referenced graph '%s' source schema validation failed: %s", graph_name,
			                            error.what());
		}
	} else if (source->RowCount() != 0) {
		throw InvalidInputException("Table-backed graph '%s' contains duplicate source metadata", graph_name);
	}
	if (StringUtil::CIEquals(result.source_kind, "DUCKLAKE") && context.transaction.IsAutoCommit()) {
		auto type =
		    GqlQuery(connection,
		             "SELECT lower(type), database_oid::UBIGINT FROM duckdb_databases() WHERE lower(database_name)=" +
		                 GqlQuoteLiteral(StringUtil::Lower(result.source_catalog)));
		if (type->RowCount() == 1 && type->GetValue(0, 0).GetValue<string>() == "ducklake") {
			result.cached_reference_ids = true;
			result.source_catalog_id = type->GetValue(1, 0).GetValue<uint64_t>();
			result.source_snapshot_id = ReadReferencedDuckLakeSnapshot(connection, result.source_catalog);
		}
	}

	const bool referenced = vertex_tables[0].table.ownership == "REFERENCED";
	if (!referenced && vertex_tables.size() == 1 && edge_tables.size() == 1) {
		result.vertex = std::move(vertex_tables[0].table);
		result.edge = std::move(edge_tables[0].table);
		result.edge_source_column = edge_tables[0].source_column;
		result.edge_target_column = edge_tables[0].target_column;
	} else {
		result.vertex = BuildReferencedUnion(connection, result.graph_id, "VERTEX", vertex_tables, vertex_tables,
		                                     graph_name, false, result.source_snapshot_id, result.cached_reference_ids);
		result.edge = BuildReferencedUnion(connection, result.graph_id, "EDGE", edge_tables, vertex_tables, graph_name,
		                                   false, result.source_snapshot_id, result.cached_reference_ids);
		if (result.cached_reference_ids) {
			result.vertex.referenced_fetch_sql =
			    BuildReferencedUnion(connection, result.graph_id, "VERTEX", vertex_tables, vertex_tables, graph_name,
			                         true, result.source_snapshot_id)
			        .relation_sql;
		}
		result.edge_source_column = "__gql_source_id";
		result.edge_target_column = "__gql_target_id";
	}
	return true;
}

bool GqlReferencedKeyIsUnique(const GqlElementTableBinding &table, const string &property) {
	if (table.referenced_sources.size() != 1)
		return false;
	const auto &source = table.referenced_sources[0];
	for (const auto &entry : source.property_columns) {
		if (StringUtil::CIEquals(entry.first, property) && StringUtil::CIEquals(entry.second, source.key_column))
			return true;
	}
	return false;
}

} // namespace duckdb
