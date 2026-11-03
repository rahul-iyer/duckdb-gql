#pragma once

#include "gql_ir.hpp"
#include "gql_cost_model.hpp"

namespace duckdb {

class TableCatalogEntry;

class ClientContext;
class TableFunction;
struct GqlTableGraphBinding;

// Applies semantics-preserving logical rewrites and annotates every operator
// with binding, correlation, nullability, and cardinality properties.
void GqlOptimize(GqlLogicalPlan &plan);

// Runtime graph capabilities are not available while the parser builds the
// semantic logical plan. The relational MATCH bind replacement therefore runs
// this second optimizer phase after the selected graph and its current CSR
// snapshot are known.
struct GqlAccessPatternElement {
	GqlPatternElementType type;
	idx_t binding_index;
	string label;
	bool reverse = false;
	bool quantified = false;
	bool unbounded = false;
	idx_t minimum_repetitions = 1;
	idx_t maximum_repetitions = 1;
	//! ALL SHORTEST between two given vertices.
	bool all_shortest = false;
};

struct GqlAccessPattern {
	vector<GqlAccessPatternElement> elements;
};

struct GqlAccessMatchStage {
	vector<GqlAccessPattern> patterns;
};

struct GqlAccessLogicalNode {
	GqlLogicalOperatorType type;
	idx_t child = DConstants::INVALID_INDEX;
	idx_t right = DConstants::INVALID_INDEX;
	idx_t payload = DConstants::INVALID_INDEX;
};

struct GqlAccessPathInput {
	vector<GqlAccessMatchStage> match_stages;
	vector<GqlAccessLogicalNode> nodes;
	idx_t root = DConstants::INVALID_INDEX;
	vector<GqlPatternElementType> binding_types;
	vector<GqlExpressionProgram> projections;
	vector<GqlExpressionProgram> predicates;
	// The final result removes duplicates, with no multiplicity-sensitive or
	// potentially volatile expressions between MATCH and that DISTINCT.
	bool duplicate_insensitive = false;
	vector<vector<string>> vertex_fetch_columns;
};

enum class GqlBindingAccessPathType : uint8_t {
	TABLE_SCAN,
	PROPERTY_INDEX_LOOKUP,
	CSR_EXPANSION,
	CSR_EDGE_PROPERTY_EXPANSION,
	CSR_PATH_EXPANSION,
	RELATIONAL_PATH_EXPANSION,
	BATCHED_ELEMENT_FETCH,
	LABEL_POSTING_FETCH,
	REFERENCED_ELEMENT_FETCH
};

struct GqlBindingAccessPath {
	GqlBindingAccessPathType type = GqlBindingAccessPathType::TABLE_SCAN;
	idx_t expansion_vertex_binding = DConstants::INVALID_INDEX;
	string expansion_direction;
	string edge_label;
	idx_t minimum_repetitions = 1;
	idx_t maximum_repetitions = 1;
	bool unbounded = false;
	bool distinct_endpoints = false;
	bool fetch_edge_properties = false;
	idx_t fetch_id_binding = DConstants::INVALID_INDEX;
	string fetch_id_column;
	vector<string> fetch_columns;
	string property_name;
	string property_column;
	string property_index_name;
	GqlLiteralType literal_type = GqlLiteralType::NULL_VALUE;
	string literal_value;
	vector<string> label_postings;
	GqlCardinalityEstimate cardinality;
	GqlPlanCost cost;
	bool native_index_filter = false;
	idx_t candidate_rows = DConstants::INVALID_INDEX;
	//! Distribution used for a fixed expansion estimate, never a semantic bound.
	string fanout_source;
	idx_t fanout_population = DConstants::INVALID_INDEX;
	idx_t fanout_edges = DConstants::INVALID_INDEX;
	idx_t endpoint_edges = DConstants::INVALID_INDEX;
	//! Estimated joined tuple rows after this edge, distinct from physical expansion rows.
	idx_t joined_rows = DConstants::INVALID_INDEX;
};

struct GqlStageAccessPlan {
	vector<bool> introduced;
	vector<idx_t> source_order;
	vector<GqlBindingAccessPath> bindings;
	GqlPlanCost cost;
	idx_t alternatives_considered = 0;
	bool search_bounded = false;
	bool costed = false;
};

struct GqlAccessPathPlan {
	vector<GqlStageAccessPlan> stages;
	string optimizer_mode = "cost";
};

// Chooses graph-specific physical access paths that DuckDB's relational
// optimizer cannot invent. Relational lowering must consume this plan without
// reapplying eligibility or selectivity policy.
GqlAccessPathPlan GqlOptimizeAccessPaths(ClientContext &context, const string &graph_name,
                                         const GqlTableGraphBinding &graph, const GqlAccessPathInput &input);

// Row IDs of an indexed property value, or false when no usable index answers it.
// These are candidates: the caller's fetch applies visibility and must recheck the value.
bool GqlProbePropertyIndex(ClientContext &context, TableCatalogEntry &table, const string &column, const Value &literal,
                           idx_t limit, vector<idx_t> &rowids);

// Inspect the last graph access plan bound on this connection, including EXPLAIN.
// This reads diagnostics only; it never executes the graph query or builds CSR.
TableFunction GqlOptimizerPlanFunction();

} // namespace duckdb
