#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/function/table_function.hpp"

#include "gql_csr.hpp"

namespace duckdb {

class ClientContext;
struct GqlTableGraphBinding;

//! An inclusive range of vertex IDs.
struct GqlIdRange {
	uint64_t low;
	uint64_t high;
};

//! What a scan of one relationship type between two sets of vertex labels returns.
struct GqlEdgeScanEstimate {
	double edges = 0;
	double sources = 0;
	double targets = 0;
	//! Rows the scan reads to find those edges: every row group that holds one of them.
	double scan_rows = 0;
};

// Where a managed graph keeps each label's vertices, and how many edges of each type run between them.
//
// Bulk loads store the vertices of one label set together, under consecutive IDs. A vertex ID then
// names its labels, so a pattern can test an edge's endpoint against ID ranges instead of joining
// the vertex table, and each (source labels, type, target labels) combination is a relation with a
// known size. Ranges decide results and are exact for the tables as read; counts only guide
// planning.
struct GqlGraphLayout {
	//! Vertices sharing one label set.
	struct Group {
		vector<string> labels;
		uint64_t low;
		uint64_t high;
		uint64_t count;
		//! Whether these vertices hold every ID in [low, high].
		bool dense;
	};
	//! Groups whose ID ranges overlap, merged into one range that no other group shares.
	struct Block {
		uint64_t low;
		uint64_t high;
		uint64_t count;
		bool dense;
		vector<idx_t> groups;
	};
	//! Edges of one type between two blocks. A block of -1 holds the IDs outside every block.
	struct Triple {
		int64_t source_block;
		int64_t target_block;
		uint64_t edges;
		uint64_t sources;
		uint64_t targets;
		//! Row groups of the edge table that hold these edges.
		uint64_t row_groups;
	};

	vector<Group> groups;
	vector<Block> blocks;
	uint64_t vertex_count = 0;
	uint64_t edge_count = 0;
	unordered_map<string, vector<Triple>> triples;

	//! The ID ranges holding exactly the vertices that have every one of these ';'-separated labels.
	//! False when IDs do not decide those labels; a vertex table join must test them then.
	bool LabelRanges(const string &labels, vector<GqlIdRange> &ranges) const;
	//! How many vertices have every one of these labels.
	double LabelPopulation(const string &labels) const;
	GqlEdgeScanEstimate EdgeEstimate(const string &type, const string &source_labels,
	                                 const string &target_labels) const;
};

//! The layout of a graph's tables as this statement sees them, or null when it is unknown,
//! disabled, or cannot be trusted for this statement.
shared_ptr<const GqlGraphLayout> GqlTryGetGraphLayout(ClientContext &context, const GqlTableGraphBinding &graph);

//! A scan of a managed element table that reports the given size and distinct counts to the planner.
//! Arguments: catalog, schema, table, rows, column names, distinct counts.
TableFunction GqlElementScanFunction();
//! One row per label group and one per relationship-type combination of a graph's current layout.
TableFunction GqlGraphLayoutFunction();

} // namespace duckdb
