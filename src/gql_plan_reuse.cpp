#include "gql_plan_reuse.hpp"

#include "gql_catalog.hpp"
#include "gql_csr.hpp"
#include "gql_storage.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/bound_parameter_map.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_prepare.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

namespace duckdb {

// DuckDB rebinds every prepared statement that reads data, because optimizers embed
// database state in the plan: statistics can prune a filter and an index lookup can
// fix row IDs. For a prepared MATCH that means compiling on each execution. Such a
// plan stays correct while no write has changed that state, so it may be reused as
// long as everything below is unchanged. Any difference falls back to the rebind.
struct GqlPlanReuseToken {
	uint64_t write_generation = 0;
	string selected_graph;
	string optimizer;
	uint64_t csr_builds = 0;

	bool operator==(const GqlPlanReuseToken &other) const {
		return write_generation == other.write_generation && selected_graph == other.selected_graph &&
		       optimizer == other.optimizer && csr_builds == other.csr_builds;
	}
};

static bool CanTrackDatabase(ClientContext &context, const Identifier &name, bool preparing) {
	auto database = DatabaseManager::Get(context).GetDatabase(context, name);
	if (!database || !database->GetCatalog().IsDuckCatalog()) {
		return false;
	}
	// A commit after this transaction began is invisible to the plan being captured,
	// which would then be older than the generation recorded with it. Every commit
	// advances this position, including read-only ones, so it cannot itself be the token.
	return !preparing ||
	       DuckTransactionManager::Get(*database).GetLastCommit() <= DuckTransaction::Get(context, *database).start_time;
}

static bool TryBuildToken(ClientContext &context, const StatementProperties &properties, bool preparing,
                          GqlPlanReuseToken &token) {
	for (const auto &entry : properties.read_databases) {
		if (!CanTrackDatabase(context, entry.first, preparing)) {
			return false;
		}
	}
	// Graph metadata and CSR validity are read through the default database.
	if (!CanTrackDatabase(context, DatabaseManager::Get(context).GetDefaultDatabase(), preparing)) {
		return false;
	}
	token.write_generation = GqlDatabaseWriteGeneration(context);
	token.selected_graph = GqlGetSelectedGraph(context);
	token.csr_builds = GqlCsrBuildCount(context);
	Value optimizer;
	if (context.TryGetCurrentSetting(Identifier("gql_optimizer"), optimizer)) {
		token.optimizer = optimizer.ToString();
	}
	return true;
}

struct GqlPlanReuseState : ClientContextState {
	struct Entry {
		weak_ptr<PreparedStatementData> prepared;
		GqlPlanReuseToken token;
	};

	// Per statement.
	bool match_bound = false;
	bool rebind_required = false;
	// Prepared statements keeping their plans. An absent entry means an ordinary rebind.
	unordered_map<const PreparedStatementData *, Entry> entries;

	void QueryEnd() override {
		match_bound = false;
		rebind_required = false;
	}

	RebindQueryInfo OnRebindPreparedStatement(ClientContext &context, BindPreparedStatementCallbackInfo &info,
	                                          RebindQueryInfo current_rebind) override {
		auto entry = entries.find(&info.prepared_statement);
		if (entry == entries.end()) {
			return current_rebind;
		}
		GqlPlanReuseToken current;
		// CSR access is unavailable in an explicit transaction, which can also hold
		// uncommitted writes. Plan those executions from scratch.
		if (!context.transaction.IsAutoCommit() ||
		    !TryBuildToken(context, info.prepared_statement.properties, false, current) ||
		    !(current == entry->second.token)) {
			return RebindQueryInfo::ATTEMPT_TO_REBIND;
		}
		return current_rebind;
	}
};

static shared_ptr<GqlPlanReuseState> GetPlanReuseState(ClientContext &context) {
	return context.registered_state->GetOrCreate<GqlPlanReuseState>("gql_plan_reuse_state");
}

void GqlPlanReuseNoteMatch(ClientContext &context) {
	GetPlanReuseState(context)->match_bound = true;
}

// DuckDB rebinds any parameter that occurs twice, in case a cast hides an occurrence
// still of unknown type. A generic plan is safe once every occurrence has the type its
// bound value will be cast to, which also rules out one parameter used at two types.
static bool ParametersFullyTyped(LogicalOperator &op) {
	bool typed = true;
	std::function<void(Expression &)> visit = [&](Expression &expression) {
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY) {
			typed = false;
			return;
		}
		if (expression.GetExpressionClass() == ExpressionClass::BOUND_PARAMETER) {
			auto &parameter = expression.Cast<BoundParameterExpression>();
			typed = typed && parameter.GetReturnType() != LogicalType::UNKNOWN &&
			        parameter.GetReturnType() == parameter.ParameterData()->return_type;
		}
		ExpressionIterator::EnumerateChildren(expression, visit);
	};
	LogicalOperatorVisitor::EnumerateExpressions(op, [&](unique_ptr<Expression> *expression) { visit(**expression); });
	for (auto &child : op.children) {
		typed = typed && ParametersFullyTyped(*child);
	}
	return typed;
}

void GqlPlanReuseObserveBind(PlannerExtensionInput &input, BoundStatement &statement) {
	auto state = GetPlanReuseState(input.context);
	if (input.binder.GetStatementProperties().always_require_rebind) {
		state->rebind_required = true;
	}
	auto parameters = input.binder.GetParameters();
	if (state->match_bound && parameters && parameters->rebind && statement.plan &&
	    statement.plan->type != LogicalOperatorType::LOGICAL_PREPARE && ParametersFullyTyped(*statement.plan)) {
		parameters->rebind = false;
	}
}

// Only sources whose every change passes through a write to a native database.
static bool SourcesFollowCommits(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = op.Cast<LogicalGet>();
		const auto &name = get.function.GetName().GetIdentifierName();
		auto table = get.GetTable();
		const bool native_table = table && table->IsDuckTable();
		if (!native_table && !StringUtil::StartsWith(name, "gql_csr_") && !StringUtil::StartsWith(name, "gql_vertex_") &&
		    !StringUtil::StartsWith(name, "gql_edge_")) {
			return false;
		}
	}
	for (const auto &child : op.children) {
		if (!SourcesFollowCommits(*child)) {
			return false;
		}
	}
	return true;
}

void GqlPlanReuseOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	if (plan->type != LogicalOperatorType::LOGICAL_PREPARE) {
		return;
	}
	auto &context = input.context;
	auto state = GetPlanReuseState(context);
	auto &prepare = plan->Cast<LogicalPrepare>();
	auto &properties = prepare.prepared->properties;
	GqlPlanReuseToken token;
	if (!state->match_bound || state->rebind_required || plan->children.empty() || !properties.bound_all_parameters ||
	    !properties.IsReadOnly() || !context.transaction.IsAutoCommit() || !SourcesFollowCommits(*plan->children[0]) ||
	    !TryBuildToken(context, properties, true, token)) {
		return;
	}
	for (auto entry = state->entries.begin(); entry != state->entries.end();) {
		entry = entry->second.prepared.expired() ? state->entries.erase(entry) : std::next(entry);
	}
	properties.always_require_rebind = false;
	state->entries[prepare.prepared.get()] = {prepare.prepared, std::move(token)};
}

} // namespace duckdb
