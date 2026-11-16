#include "gql_layout.hpp"

#include "gql_catalog.hpp"
#include "gql_sql_utils.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

#include <algorithm>

namespace duckdb {

namespace {

constexpr idx_t MAX_LABEL_GROUPS = 4096;
//! A label test over more ranges than this costs more than it saves.
constexpr idx_t MAX_LABEL_RANGES = 8;
constexpr idx_t MAX_RECOMPUTE_BACKOFF = 1024;

vector<string> SplitLabels(const string &labels) {
	vector<string> result;
	for (auto &label : StringUtil::Split(labels, ';')) {
		// Compared exactly, as the label column's list_contains test compares them.
		if (!label.empty()) {
			result.push_back(label);
		}
	}
	return result;
}

bool HasLabels(const GqlGraphLayout::Group &group, const vector<string> &required) {
	for (const auto &label : required) {
		if (std::find(group.labels.begin(), group.labels.end(), label) == group.labels.end()) {
			return false;
		}
	}
	return true;
}

//! The share of a segment's vertices that have the required labels.
double SegmentShare(const GqlGraphLayout &layout, int64_t segment_index, const vector<string> &required) {
	if (segment_index < 0) {
		return required.empty() ? 1 : 0;
	}
	const auto &segment = layout.segments[NumericCast<idx_t>(segment_index)];
	double matching = 0;
	for (idx_t index = 0; index < segment.groups.size(); index++) {
		if (HasLabels(layout.groups[segment.groups[index]], required)) {
			matching += segment.shares[index];
		}
	}
	return matching;
}

} // namespace

bool GqlGraphLayout::LabelRanges(const string &labels, vector<GqlIdRange> &ranges) const {
	ranges.clear();
	const auto required = SplitLabels(labels);
	for (const auto &block : blocks) {
		vector<idx_t> matching;
		for (auto group : block.groups) {
			if (HasLabels(groups[group], required)) {
				matching.push_back(group);
			}
		}
		if (matching.empty()) {
			continue;
		}
		if (matching.size() == block.groups.size()) {
			if (!block.dense) {
				return false;
			}
			ranges.push_back({block.low, block.high});
			continue;
		}
		for (auto group : matching) {
			if (!groups[group].dense) {
				return false;
			}
			ranges.push_back({groups[group].low, groups[group].high});
		}
	}
	std::sort(ranges.begin(), ranges.end(), [](const GqlIdRange &a, const GqlIdRange &b) { return a.low < b.low; });
	vector<GqlIdRange> merged;
	for (const auto &range : ranges) {
		if (!merged.empty() && merged.back().high + 1 == range.low) {
			merged.back().high = range.high;
		} else {
			merged.push_back(range);
		}
	}
	ranges = std::move(merged);
	return ranges.size() <= MAX_LABEL_RANGES;
}

bool GqlGraphLayout::LabelBounds(const string &labels, vector<GqlIdRange> &ranges) const {
	ranges.clear();
	const auto required = SplitLabels(labels);
	if (required.empty()) {
		return false;
	}
	for (const auto &group : groups) {
		if (!HasLabels(group, required)) {
			continue;
		}
		// Groups are in order of their lowest ID.
		if (!ranges.empty() && group.low <= ranges.back().high) {
			ranges.back().high = MaxValue(ranges.back().high, group.high);
		} else {
			ranges.push_back({group.low, group.high});
		}
	}
	if (ranges.size() > MAX_LABEL_RANGES) {
		uint64_t high = 0;
		for (const auto &range : ranges) {
			high = MaxValue(high, range.high);
		}
		ranges = {{ranges[0].low, high}};
	}
	return true;
}

double GqlGraphLayout::LabelPopulation(const string &labels) const {
	const auto required = SplitLabels(labels);
	double result = 0;
	for (const auto &group : groups) {
		if (HasLabels(group, required)) {
			result += static_cast<double>(group.count);
		}
	}
	return result;
}

GqlEdgeScanEstimate GqlGraphLayout::EdgeEstimate(const string &type, const string &source_labels,
                                                 const string &target_labels, bool source_exact,
                                                 bool target_exact) const {
	GqlEdgeScanEstimate result;
	auto entry = triples.find(type);
	if (entry == triples.end()) {
		return result;
	}
	const auto source_required = SplitLabels(source_labels), target_required = SplitLabels(target_labels);
	vector<GqlIdRange> source_bounds, target_bounds;
	source_exact = source_exact || !LabelBounds(source_labels, source_bounds);
	target_exact = target_exact || !LabelBounds(target_labels, target_bounds);
	// Every vertex of a segment that the bounds reach counts, whatever its labels.
	auto bounded_share = [&](int64_t segment_index, const vector<GqlIdRange> &bounds) {
		if (segment_index < 0) {
			return 0.0;
		}
		const auto &segment = segments[NumericCast<idx_t>(segment_index)];
		for (const auto &range : bounds) {
			if (range.low <= segment.high && segment.low <= range.high) {
				return 1.0;
			}
		}
		return 0.0;
	};
	for (const auto &triple : entry->second) {
		const auto source_share = source_exact ? SegmentShare(*this, triple.source_segment, source_required)
		                                       : bounded_share(triple.source_segment, source_bounds);
		const auto target_share = target_exact ? SegmentShare(*this, triple.target_segment, target_required)
		                                       : bounded_share(triple.target_segment, target_bounds);
		result.edges += static_cast<double>(triple.edges) * source_share * target_share;
		// An endpoint keeps at least one edge under the other end's labels only at best.
		result.sources += static_cast<double>(triple.sources) * source_share * (target_share > 0 ? 1 : 0);
		result.targets += static_cast<double>(triple.targets) * target_share * (source_share > 0 ? 1 : 0);
		if (source_share > 0 && target_share > 0) {
			result.scan_rows += static_cast<double>(triple.row_groups) * DEFAULT_ROW_GROUP_SIZE;
		}
	}
	result.scan_rows = MinValue(result.scan_rows, static_cast<double>(edge_count));
	result.sources = MinValue(result.sources, result.edges);
	result.targets = MinValue(result.targets, result.edges);
	if (source_exact) {
		result.sources = MinValue(result.sources, LabelPopulation(source_labels));
	}
	if (target_exact) {
		result.targets = MinValue(result.targets, LabelPopulation(target_labels));
	}
	return result;
}

namespace {

struct LayoutKey {
	uint64_t graph_version = 0;
	GqlGraphWriteToken writes;
	idx_t vertex_oid = 0;
	idx_t edge_oid = 0;
	idx_t vertex_rows = 0;
	idx_t edge_rows = 0;

	bool operator==(const LayoutKey &other) const {
		return graph_version == other.graph_version && writes == other.writes && vertex_oid == other.vertex_oid &&
		       edge_oid == other.edge_oid && vertex_rows == other.vertex_rows && edge_rows == other.edge_rows;
	}
};

struct GqlLayoutCache : ObjectCacheEntry {
	static string ObjectType() {
		return "gql_graph_layout_cache";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx {};
	}

	struct Entry {
		//! Null when the tables could not be described; that answer is kept for its key too.
		shared_ptr<const GqlGraphLayout> layout;
		bool read = false;
		LayoutKey key;
		//! Statements that began before this cannot use the layout: it may describe rows they do not see.
		transaction_t snapshot_start = 0;
		//! Reading the tables again costs a scan of both, so a graph that keeps changing
		//! waits for more and more statements to find it unchanged before it is read.
		LayoutKey stale_key;
		idx_t stale_binds = 0;
		idx_t backoff = 1;
		idx_t uses = 0;
	};
	mutex lock;
	unordered_map<uint64_t, Entry> entries;
};

optional_ptr<DuckTableEntry> NativeTable(ClientContext &context, const GqlElementTableBinding &table) {
	if (!StringUtil::CIEquals(table.ownership, "MANAGED") || !table.relation_sql.empty()) {
		return nullptr;
	}
	auto &catalog = Catalog::GetCatalog(context, Identifier(table.catalog_name));
	auto entry = catalog.GetEntry(context, CatalogType::TABLE_ENTRY, Identifier(table.schema_name),
	                              Identifier(table.table_name), OnEntryNotFound::RETURN_NULL);
	if (!entry || !entry->Cast<TableCatalogEntry>().IsDuckTable()) {
		return nullptr;
	}
	return &entry->Cast<DuckTableEntry>();
}

bool HasUnsignedColumn(const DuckTableEntry &table, const string &column) {
	return table.ColumnExists(Identifier(column)) && table.GetColumn(Identifier(column)).Type() == LogicalType::UBIGINT;
}

string QualifiedName(const GqlElementTableBinding &table) {
	return GqlQuoteIdentifier(table.catalog_name) + "." + GqlQuoteIdentifier(table.schema_name) + "." +
	       GqlQuoteIdentifier(table.table_name);
}

//! Maps an ID to the block holding it by binary search, or to -1.
string SegmentOf(const GqlGraphLayout &layout, const string &column, idx_t first, idx_t last) {
	if (layout.segments.empty()) {
		return "-1";
	}
	if (first == last) {
		return "CASE WHEN " + column + " BETWEEN " + to_string(layout.segments[first].low) + " AND " +
		       to_string(layout.segments[first].high) + " THEN " + to_string(first) + " ELSE -1 END";
	}
	const auto middle = (first + last) / 2;
	return "CASE WHEN " + column + " < " + to_string(layout.segments[middle + 1].low) + " THEN " +
	       SegmentOf(layout, column, first, middle) + " ELSE " + SegmentOf(layout, column, middle + 1, last) + " END";
}

// Null when the layout cannot describe every vertex: a missing range would read as an empty label.
shared_ptr<GqlGraphLayout> ReadLayout(Connection &connection, const GqlTableGraphBinding &graph) {
	auto layout = make_shared_ptr<GqlGraphLayout>();
	const auto key = GqlQuoteIdentifier(graph.vertex.key_column);
	// A count can prove a range full only of distinct IDs: {1, 1, 4, 4} has the length and sum of 1..4.
	auto unique = GqlQuery(connection, "SELECT count(" + key + ") = count(DISTINCT " + key + ") FROM " +
	                                       QualifiedName(graph.vertex));
	if (unique->GetValue(0, 0).IsNull() || !unique->GetValue(0, 0).GetValue<bool>()) {
		return nullptr;
	}
	// The sum separates a full range from one where a repeated ID hides a missing one.
	auto rows = GqlQuery(connection, "SELECT " + GqlQuoteIdentifier(graph.vertex.label_column) + ", min(" + key +
	                                     "), max(" + key + "), count(*)::UBIGINT, count(*) = max(" + key + ") - min(" +
	                                     key + ") + 1 AND sum(" + key + "::HUGEINT) * 2 = (min(" + key +
	                                     ")::HUGEINT + max(" + key + ")::HUGEINT) * count(*) FROM " +
	                                     QualifiedName(graph.vertex) + " GROUP BY 1");
	if (rows->RowCount() > MAX_LABEL_GROUPS) {
		return nullptr;
	}
	for (idx_t row = 0; row < rows->RowCount(); row++) {
		GqlGraphLayout::Group group;
		auto labels = rows->GetValue(0, row);
		if (!labels.IsNull() && labels.type().id() == LogicalTypeId::LIST) {
			for (const auto &label : ListValue::GetChildren(labels)) {
				if (!label.IsNull()) {
					group.labels.push_back(label.ToString());
				}
			}
		} else if (!labels.IsNull()) {
			group.labels.push_back(labels.ToString());
		}
		group.count = rows->GetValue(3, row).GetValue<uint64_t>();
		layout->vertex_count += group.count;
		// Rows without an ID belong to no range.
		if (rows->GetValue(1, row).IsNull()) {
			continue;
		}
		group.low = rows->GetValue(1, row).GetValue<uint64_t>();
		group.high = rows->GetValue(2, row).GetValue<uint64_t>();
		group.dense = !rows->GetValue(4, row).IsNull() && rows->GetValue(4, row).GetValue<bool>();
		layout->groups.push_back(std::move(group));
	}
	std::sort(layout->groups.begin(), layout->groups.end(),
	          [](const GqlGraphLayout::Group &a, const GqlGraphLayout::Group &b) { return a.low < b.low; });
	for (idx_t index = 0; index < layout->groups.size(); index++) {
		const auto &group = layout->groups[index];
		if (layout->blocks.empty() || group.low > layout->blocks.back().high) {
			layout->blocks.push_back({group.low, group.high, 0, false, {}});
		}
		auto &block = layout->blocks.back();
		block.high = MaxValue(block.high, group.high);
		block.count += group.count;
		block.groups.push_back(index);
	}
	for (auto &block : layout->blocks) {
		// Unique IDs fill a range exactly when there are as many of them as the range is long.
		bool dense = block.count == block.high - block.low + 1;
		if (block.groups.size() == 1) {
			dense = layout->groups[block.groups[0]].dense;
		}
		block.dense = dense;
	}

	const auto source = GqlQuoteIdentifier(graph.edge_source_column), target = GqlQuoteIdentifier(graph.edge_target_column);
	// Every end of a group's range starts a new segment.
	vector<uint64_t> starts;
	for (const auto &group : layout->groups) {
		starts.push_back(group.low);
		if (group.high != NumericLimits<uint64_t>::Maximum()) {
			starts.push_back(group.high + 1);
		}
	}
	std::sort(starts.begin(), starts.end());
	starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
	for (idx_t index = 0; index < starts.size(); index++) {
		GqlGraphLayout::Segment segment;
		segment.low = starts[index];
		segment.high = index + 1 < starts.size() ? starts[index + 1] - 1 : NumericLimits<uint64_t>::Maximum();
		const auto length = static_cast<double>(segment.high - segment.low) + 1;
		double total = 0;
		for (idx_t group_index = 0; group_index < layout->groups.size(); group_index++) {
			const auto &group = layout->groups[group_index];
			if (group.low > segment.low) {
				break;
			}
			if (group.high < segment.high) {
				continue;
			}
			if (group.dense) {
				// Its range has no ID left for another group.
				segment.groups = {group_index};
				segment.shares = {1};
				total = 1;
				break;
			}
			// Its vertices are taken to lie evenly over its range.
			const auto share =
			    static_cast<double>(group.count) * length / (static_cast<double>(group.high - group.low) + 1);
			segment.groups.push_back(group_index);
			segment.shares.push_back(share);
			total += share;
		}
		if (segment.groups.empty()) {
			continue;
		}
		for (auto &share : segment.shares) {
			share /= total;
		}
		layout->segments.push_back(std::move(segment));
	}
	const auto last = layout->segments.empty() ? 0 : layout->segments.size() - 1;
	auto edges = GqlQuery(connection,
	                      "SELECT " + GqlQuoteIdentifier(graph.edge.label_column) + ", " +
	                          SegmentOf(*layout, source, 0, last) + ", " + SegmentOf(*layout, target, 0, last) +
	                          ", count(*)::UBIGINT, approx_count_distinct(" + source +
	                          ")::UBIGINT, approx_count_distinct(" + target +
	                          ")::UBIGINT, approx_count_distinct(rowid // " + to_string(DEFAULT_ROW_GROUP_SIZE) +
	                          ")::UBIGINT FROM " + QualifiedName(graph.edge) + " GROUP BY 1, 2, 3");
	for (idx_t row = 0; row < edges->RowCount(); row++) {
		layout->edge_count += edges->GetValue(3, row).GetValue<uint64_t>();
		if (edges->GetValue(0, row).IsNull()) {
			continue;
		}
		layout->triples[edges->GetValue(0, row).ToString()].push_back(
		    {edges->GetValue(1, row).GetValue<int64_t>(), edges->GetValue(2, row).GetValue<int64_t>(),
		     edges->GetValue(3, row).GetValue<uint64_t>(), edges->GetValue(4, row).GetValue<uint64_t>(),
		     edges->GetValue(5, row).GetValue<uint64_t>(), edges->GetValue(6, row).GetValue<uint64_t>()});
	}
	return layout;
}

} // namespace

shared_ptr<const GqlGraphLayout> GqlTryGetGraphLayout(ClientContext &context, const GqlTableGraphBinding &graph) {
	Value enabled;
	if (context.TryGetCurrentSetting(Identifier("gql_graph_layout"), enabled) && !enabled.IsNull() &&
	    !enabled.GetValue<bool>()) {
		return nullptr;
	}
	// The tables are read through another connection, which cannot see this transaction's writes.
	if (!context.transaction.IsAutoCommit() || graph.vertex.label_column.empty() || !graph.vertex.static_labels.empty() ||
	    graph.edge.label_column.empty() || graph.edge.label_is_list || !graph.edge.static_labels.empty() ||
	    !StringUtil::CIEquals(graph.vertex.catalog_name, graph.edge.catalog_name)) {
		return nullptr;
	}
	auto vertex_table = NativeTable(context, graph.vertex), edge_table = NativeTable(context, graph.edge);
	if (!vertex_table || !edge_table || !HasUnsignedColumn(*vertex_table, graph.vertex.key_column) ||
	    !HasUnsignedColumn(*edge_table, graph.edge_source_column) ||
	    !HasUnsignedColumn(*edge_table, graph.edge_target_column)) {
		return nullptr;
	}
	auto database = DatabaseManager::Get(context).GetDatabase(context, Identifier(graph.vertex.catalog_name));
	if (!database || !database->GetCatalog().IsDuckCatalog()) {
		return nullptr;
	}
	auto read_key = [&]() {
		LayoutKey key;
		key.graph_version = graph.graph_version;
		key.writes = GqlReadGraphWriteToken(context, graph);
		key.vertex_oid = vertex_table->oid;
		key.edge_oid = edge_table->oid;
		key.vertex_rows = vertex_table->GetStorage().GetTotalRows();
		key.edge_rows = edge_table->GetStorage().GetTotalRows();
		return key;
	};
	const auto key = read_key();
	if (key.writes.writes_in_flight > 0) {
		return nullptr;
	}
	auto cache = ObjectCache::GetObjectCache(context).GetOrCreate<GqlLayoutCache>(GqlLayoutCache::ObjectType());
	{
		lock_guard<mutex> guard(cache->lock);
		auto &entry = cache->entries[graph.graph_id];
		if (entry.read && entry.key == key) {
			if (!entry.layout) {
				return nullptr;
			}
			if (DuckTransaction::Get(context, *database).start_time < entry.snapshot_start &&
			    !GqlNoWriteSinceQueryBegin(context)) {
				return nullptr;
			}
			entry.uses++;
			return entry.layout;
		}
		if (!(entry.stale_key == key)) {
			entry.stale_key = key;
			entry.stale_binds = 0;
		}
		if (++entry.stale_binds < entry.backoff) {
			return nullptr;
		}
	}
	shared_ptr<GqlGraphLayout> layout;
	transaction_t snapshot_start;
	{
		Connection connection(*context.db);
		connection.BeginTransaction();
		layout = ReadLayout(connection, graph);
		snapshot_start = DuckTransaction::Get(*connection.context, *database).start_time;
		connection.Commit();
	}
	// A write planned while the tables were being read may or may not be in what was read.
	if (!(read_key() == key)) {
		return nullptr;
	}
	lock_guard<mutex> guard(cache->lock);
	auto &entry = cache->entries[graph.graph_id];
	if (entry.read) {
		entry.backoff = entry.uses < 4 * entry.backoff ? MinValue(entry.backoff * 2, MAX_RECOMPUTE_BACKOFF)
		                                               : MaxValue<idx_t>(entry.backoff / 2, 1);
	}
	entry.layout = layout;
	entry.read = true;
	entry.key = key;
	entry.snapshot_start = snapshot_start;
	entry.uses = 0;
	entry.stale_binds = 0;
	// This statement began before the tables were read, so it sees the same rows only if no write intervened.
	if (!layout || !GqlNoWriteSinceQueryBegin(context)) {
		return nullptr;
	}
	return layout;
}

namespace {

struct ElementScanBindData : TableScanBindData {
	explicit ElementScanBindData(TableCatalogEntry &table) : TableScanBindData(table) {
	}

	idx_t rows = 0;
	//! Logical column index to the distinct values the scan returns in it.
	unordered_map<idx_t, idx_t> distinct;

	bool Equals(const FunctionData &other_p) const override {
		auto other = dynamic_cast<const ElementScanBindData *>(&other_p);
		return other && TableScanBindData::Equals(other_p) && other->rows == rows && other->distinct == distinct;
	}
	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<ElementScanBindData>(table);
		result->is_index_scan = is_index_scan;
		result->is_create_index = is_create_index;
		result->column_ids = column_ids;
		result->order_options = order_options ? make_uniq<RowGroupOrderOptions>(*order_options) : nullptr;
		result->partitions_to_scan =
		    partitions_to_scan ? make_uniq<unordered_set<idx_t>>(*partitions_to_scan) : nullptr;
		result->rows = rows;
		result->distinct = distinct;
		return std::move(result);
	}
};

unique_ptr<FunctionData> ElementScanBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &types, vector<Identifier> &names) {
	auto &catalog = Catalog::GetCatalog(context, Identifier(input.inputs[0].GetValue<string>()));
	auto entry = catalog.GetEntry(context, CatalogType::TABLE_ENTRY, Identifier(input.inputs[1].GetValue<string>()),
	                              Identifier(input.inputs[2].GetValue<string>()), OnEntryNotFound::THROW_EXCEPTION);
	auto &table = entry->Cast<TableCatalogEntry>();
	if (!table.IsDuckTable()) {
		throw BinderException("gql_element_scan reads native tables only");
	}
	auto result = make_uniq<ElementScanBindData>(table);
	result->rows = LossyNumericCast<idx_t>(MaxValue<double>(input.inputs[3].GetValue<double>(), 0));
	const auto &columns = ListValue::GetChildren(input.inputs[4]);
	const auto &distinct = ListValue::GetChildren(input.inputs[5]);
	if (columns.size() != distinct.size()) {
		throw BinderException("gql_element_scan needs one distinct count per column");
	}
	for (auto &column : table.GetColumns().Logical()) {
		types.push_back(column.Type());
		names.push_back(column.Name());
	}
	for (idx_t index = 0; index < columns.size(); index++) {
		auto &column = table.GetColumn(Identifier(columns[index].GetValue<string>()));
		result->distinct[column.Logical().index] =
		    MaxValue<idx_t>(LossyNumericCast<idx_t>(MaxValue<double>(distinct[index].GetValue<double>(), 1)), 1);
	}
	return std::move(result);
}

unique_ptr<NodeStatistics> ElementScanCardinality(ClientContext &context, const FunctionData *bind_data) {
	auto native = TableScanFunction::GetFunction().cardinality(context, bind_data);
	auto rows = bind_data->Cast<ElementScanBindData>().rows;
	if (!native || !native->has_max_cardinality) {
		return make_uniq<NodeStatistics>(rows);
	}
	return make_uniq<NodeStatistics>(rows, MaxValue(rows, native->max_cardinality));
}

unique_ptr<BaseStatistics> ElementScanStatistics(ClientContext &context, TableFunctionGetStatisticsInput &input) {
	// Bounds and null counts stay the table's own, so they hold for every row the scan can return.
	auto result = TableScanFunction::GetFunction().statistics_extended(context, input);
	auto &bind_data = input.bind_data->Cast<ElementScanBindData>();
	auto entry = bind_data.distinct.find(input.column_index.GetPrimaryIndex());
	if (result && entry != bind_data.distinct.end()) {
		result->SetDistinctCount(entry->second);
	}
	return result;
}

} // namespace

TableFunction GqlElementScanFunction() {
	// DuckDB's own table scan under another name: only what it tells the planner differs.
	auto native = TableScanFunction::GetFunction();
	TableFunction result("gql_element_scan",
	                     {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::DOUBLE,
	                      LogicalType::LIST(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::DOUBLE)},
	                     native.function, ElementScanBind, native.init_global, native.init_local);
	result.statistics_extended = ElementScanStatistics;
	result.cardinality = ElementScanCardinality;
	// A plan with this scan is not one DuckDB can write down and read back, which also keeps its
	// common subplan optimizer from sharing a scan between joins on different columns of it.
	result.verify_serialization = false;
	result.dependency = native.dependency;
	result.get_metrics = native.get_metrics;
	result.to_string = native.to_string;
	result.table_scan_progress = native.table_scan_progress;
	result.get_partition_data = native.get_partition_data;
	result.get_partition_stats = native.get_partition_stats;
	result.get_table_entry = native.get_table_entry;
	result.projection_pushdown = true;
	result.filter_pushdown = true;
	result.filter_prune = true;
	result.pushdown_expression = native.pushdown_expression;
	result.get_virtual_columns = native.get_virtual_columns;
	result.get_row_id_columns = native.get_row_id_columns;
	result.supports_pushdown_extract = native.supports_pushdown_extract;
	return result;
}

namespace {

struct LayoutBindData : TableFunctionData {
	string graph_name;
};
struct LayoutScanState : GlobalTableFunctionState {
	shared_ptr<const GqlGraphLayout> layout;
	vector<vector<Value>> rows;
	idx_t position = 0;
};

unique_ptr<FunctionData> LayoutBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &types,
                                    vector<Identifier> &names) {
	auto result = make_uniq<LayoutBindData>();
	result->graph_name = input.inputs[0].GetValue<string>();
	names = {"kind",         "name",         "low",     "high",    "rows",      "id_ranges",
	         "source_block", "target_block", "sources", "targets", "row_groups"};
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::UBIGINT, LogicalType::UBIGINT,
	         LogicalType::UBIGINT, LogicalType::BOOLEAN, LogicalType::BIGINT,  LogicalType::BIGINT,
	         LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT};
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> LayoutInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<LayoutScanState>();
	GqlTableGraphBinding graph;
	const auto &name = input.bind_data->Cast<LayoutBindData>().graph_name;
	if (!GqlTryLoadTableGraph(context, name, graph)) {
		throw InvalidInputException("Graph '%s' has no table storage", name);
	}
	result->layout = GqlTryGetGraphLayout(context, graph);
	if (!result->layout) {
		return std::move(result);
	}
	const auto &layout = *result->layout;
	for (const auto &group : layout.groups) {
		vector<GqlIdRange> ranges;
		const auto labels = StringUtil::Join(group.labels, ";");
		result->rows.push_back({Value("labels"), Value(labels), Value::UBIGINT(group.low), Value::UBIGINT(group.high),
		                        Value::UBIGINT(group.count), Value::BOOLEAN(layout.LabelRanges(labels, ranges)),
		                        Value(LogicalType::BIGINT), Value(LogicalType::BIGINT), Value(LogicalType::UBIGINT),
		                        Value(LogicalType::UBIGINT), Value(LogicalType::UBIGINT)});
	}
	for (const auto &type : layout.triples) {
		for (const auto &triple : type.second) {
			result->rows.push_back({Value("edges"), Value(type.first), Value(LogicalType::UBIGINT),
			                        Value(LogicalType::UBIGINT), Value::UBIGINT(triple.edges),
			                        Value(LogicalType::BOOLEAN), Value::BIGINT(triple.source_segment),
			                        Value::BIGINT(triple.target_segment), Value::UBIGINT(triple.sources),
			                        Value::UBIGINT(triple.targets), Value::UBIGINT(triple.row_groups)});
		}
	}
	return std::move(result);
}

void LayoutRead(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<LayoutScanState>();
	auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.position);
	for (idx_t row = 0; row < count; row++) {
		const auto &values = state.rows[state.position++];
		for (idx_t column = 0; column < values.size(); column++) {
			output.SetValue(column, row, values[column]);
		}
	}
	output.SetChildCardinality(count);
}

} // namespace

TableFunction GqlGraphLayoutFunction() {
	return TableFunction("gql_graph_layout", {LogicalType::VARCHAR}, LayoutRead, LayoutBind, LayoutInit);
}

} // namespace duckdb
