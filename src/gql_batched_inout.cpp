#include "gql_batched_inout.hpp"

#include "gql_csr.hpp"

#include "duckdb/execution/operator/projection/physical_tableinout_function.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

// A decorrelated lateral function keeps its correlated columns in projected_input,
// which makes PhysicalTableInOutFunction call it once per input row and produce one
// chunk per row. This operator has the same output as that LogicalGet, but plans
// it without projected input: the function sees whole chunks and emits those columns.
struct LogicalGqlBatchedInOut : LogicalExtensionOperator {
	explicit LogicalGqlBatchedInOut(unique_ptr<LogicalGet> get_p) : get(std::move(get_p)) {
		estimated_cardinality = get->estimated_cardinality;
		has_estimated_cardinality = get->has_estimated_cardinality;
		children.push_back(std::move(get->children[0]));
		get->children.clear();
	}

	string GetName() const override {
		return "GQL_BATCHED_IN_OUT";
	}

	InsertionOrderPreservingMap<string> ParamsToString() const override {
		InsertionOrderPreservingMap<string> result;
		result["Function"] = get->function.GetName().GetIdentifierName();
		return result;
	}

	bool SupportSerialization() const override {
		return false;
	}

	vector<ColumnBinding> GetColumnBindings() override {
		vector<ColumnBinding> result;
		for (auto index : ProjectionIndex::GetIndexes(get->GetColumnIds().size())) {
			result.emplace_back(get->table_index, ProjectionIndex(index));
		}
		auto child_bindings = children[0]->GetColumnBindings();
		for (auto entry : get->projected_input) {
			result.emplace_back(child_bindings[entry]);
		}
		return result;
	}

	PhysicalOperator &CreatePlan(ClientContext &, PhysicalPlanGenerator &planner) override {
		auto &child = planner.CreatePlan(*children[0]);
		auto &result = planner.Make<PhysicalTableInOutFunction>(types, get->function, std::move(get->bind_data),
		                                                        get->GetColumnIds(), vector<idx_t>(), nullptr,
		                                                        estimated_cardinality, vector<column_t>());
		result.children.push_back(child);
		return result;
	}

protected:
	void ResolveTypes() override {
		for (const auto &index : get->GetColumnIds()) {
			types.push_back(get->GetColumnType(index));
		}
		for (auto entry : get->projected_input) {
			types.push_back(children[0]->types[entry]);
		}
	}

private:
	unique_ptr<LogicalGet> get;
};

static bool CanBatch(const LogicalGet &get) {
	const auto &name = get.function.GetName().GetIdentifierName();
	if (name != "gql_csr_expand" && name != "gql_vertex_fetch" && name != "gql_vertex_fetch_projected" &&
	    name != "gql_edge_fetch") {
		return false;
	}
	// Anything else this plan shape would have to reproduce stays row by row.
	if (get.children.size() != 1 || get.projected_input.empty() || !get.projection_ids.empty() ||
	    get.ordinality_idx.IsValid() || get.table_filters.HasFilters() || get.table_filters.HasMultiColumnFilters()) {
		return false;
	}
	const auto &child_types = get.children[0]->types;
	if (child_types.size() < get.input_table_types.size()) {
		return false;
	}
	for (idx_t index = 0; index < get.input_table_types.size(); index++) {
		if (child_types[index] != get.input_table_types[index]) {
			return false;
		}
	}
	return true;
}

void GqlBatchInOutFunctions(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	for (auto &child : plan->children) {
		GqlBatchInOutFunctions(input, child);
	}
	if (plan->type != LogicalOperatorType::LOGICAL_GET) {
		return;
	}
	auto &get = plan->Cast<LogicalGet>();
	if (!get.children.empty() && get.children[0]->types.empty()) {
		get.children[0]->ResolveOperatorTypes();
	}
	if (!CanBatch(get)) {
		return;
	}
	get.bind_data->Cast<GqlBatchedInOutBindData>().passthrough_input = get.projected_input;
	plan = make_uniq<LogicalGqlBatchedInOut>(unique_ptr_cast<LogicalOperator, LogicalGet>(std::move(plan)));
	plan->ResolveOperatorTypes();
}

} // namespace duckdb
