#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

class ClientContext;
class Connection;

struct GqlElementTableBinding {
	uint64_t element_table_id = 0;
	string catalog_name;
	string schema_name;
	string table_name;
	string key_column;
	string ownership;
	string label_column;
	unordered_map<string, string> property_columns;
};

struct GqlTableGraphBinding {
	uint64_t graph_id = 0;
	GqlElementTableBinding vertex;
	GqlElementTableBinding edge;
	string edge_source_column;
	string edge_target_column;
};

//! Returns true when the graph has native table storage. Throws when native
//! metadata is incomplete or inconsistent.
bool GqlTryLoadTableGraph(ClientContext &context, const string &graph_name, GqlTableGraphBinding &result);

//! Attach graph-owned wide tables to an existing empty graph. The caller owns
//! the transaction and must create the tables before calling this function.
void GqlAttachManagedGraphTables(Connection &connection, const string &graph_name, const string &vertex_table,
                                 const string &vertex_key, const string &vertex_label, const string &edge_table,
                                 const string &edge_key, const string &edge_source, const string &edge_target,
                                 const string &edge_label, bool validate);

} // namespace duckdb
