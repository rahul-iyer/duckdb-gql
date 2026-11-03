#pragma once

#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/planner_extension.hpp"

namespace duckdb {

//! Records that this statement binds a MATCH, which makes it a candidate for plan reuse.
void GqlPlanReuseNoteMatch(ClientContext &context);
//! Whether the statement being planned binds a MATCH.
bool GqlPlanReuseStatementBindsMatch(ClientContext &context);
//! Records whether binding asked for a rebind, before the optimizer sets that flag itself.
void GqlPlanReuseObserveBind(PlannerExtensionInput &input, BoundStatement &statement);
//! Lets an eligible prepared MATCH keep its plan until something it depends on changes.
void GqlPlanReuseOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);

} // namespace duckdb
