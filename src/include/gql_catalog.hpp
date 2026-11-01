#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

class ClientContext;
class Connection;

string GqlTypedPropertyDuckType(const string &gql_type);

struct GqlSourceTable {
	string catalog_name;
	string schema_name;
	string table_name;
};

// Physical source identity is table-qualified; properties remain authoritative
// in this source. Cached ordinal maps contain keys only, never property rows.
struct GqlReferencedSource {
	uint64_t element_table_id = 0;
	string catalog_name;
	string schema_name;
	string table_name;
	string key_column;
	vector<string> labels;
	unordered_map<string, string> property_columns;
};

struct GqlElementTableBinding {
	uint64_t element_table_id = 0;
	string catalog_name;
	string schema_name;
	string table_name;
	string key_column;
	string ownership;
	string label_column;
	bool label_is_list = false;
	vector<string> static_labels;
	unordered_map<string, string> property_columns;
	unordered_map<string, string> property_indexes;
	//! Non-empty for a referenced graph. This SELECT is a canonical, zero-copy
	//! union over the physical element tables. Referenced source keys are
	//! type-qualified by their mapping; the relation assigns snapshot-local,
	//! graph-wide dense IDs used by MATCH, element_id(), and CSR.
	string relation_sql;
	//! Cached ordinal join for selective fetch and CSR preparation only.
	string referenced_fetch_sql;
	vector<GqlSourceTable> source_tables;
	vector<GqlReferencedSource> referenced_sources;
};

struct GqlTableGraphBinding {
	uint64_t graph_id = 0;
	uint64_t graph_version = 0;
	uint64_t source_catalog_id = 0;
	GqlElementTableBinding vertex;
	GqlElementTableBinding edge;
	string edge_source_column;
	string edge_target_column;
	string source_kind;
	string source_catalog;
	string snapshot_policy;
	bool has_pinned_snapshot = false;
	uint64_t pinned_snapshot_id = 0;
	string access_mode;
	bool cached_reference_ids = false;
	uint64_t source_snapshot_id = 0;
};

struct GqlPropertyColumnMapping {
	string source_column;
	string property_name;
};

struct GqlReferencedElementMapping {
	string kind;
	string table;
	string schema_type;
	string key;
	string source;
	string target;
	string source_schema_type;
	string target_schema_type;
	vector<GqlPropertyColumnMapping> properties;
};

struct GqlReferencedTableMapping {
	vector<GqlReferencedElementMapping> elements;
	string snapshot_policy = "LIVE";
	string access_mode = "READ_ONLY";
	bool validate = true;
};

//! Returns true when the graph has native table storage. Throws when native
//! metadata is incomplete or inconsistent.
bool GqlTryLoadTableGraph(ClientContext &context, const string &graph_name, GqlTableGraphBinding &result);

// A one-table referenced key is unique in its snapshot, unlike an arbitrary
// property. This proves selectivity only; it does not impose a result limit.
bool GqlReferencedKeyIsUnique(const GqlElementTableBinding &table, const string &property);

//! Attach graph-owned wide tables to an existing empty graph. The caller owns
//! the transaction and must create the tables before calling this function.
void GqlAttachManagedGraphTables(Connection &connection, const string &graph_name, const string &vertex_table,
                                 const string &vertex_key, const string &vertex_label, const string &edge_table,
                                 const string &edge_key, const string &edge_source, const string &edge_target,
                                 const string &edge_label, bool validate);

//! Attach read-only external tables to an existing typed graph. DuckGQL owns
//! only the mappings and never drops or mutates the referenced source tables.
void GqlAttachReferencedGraphTables(Connection &connection, const string &graph_name,
                                    const GqlReferencedTableMapping &mapping);

} // namespace duckdb
