#pragma once

#include "gql_ir.hpp"

namespace duckdb {

class GqlBinder {
public:
	GqlLogicalPlan Bind(const GqlMatchStatement &statement);
	vector<GqlLogicalPlan> BindAlternatives(const GqlMatchStatement &statement);

private:
	shared_ptr<GqlBoundExpression> BindExpression(const GqlExpression &expression);
	GqlBinding &Resolve(const GqlIdentifier &identifier);
	string ProjectionName(const GqlProjection &projection, const GqlBoundExpression &expression) const;

	vector<GqlBinding> bindings;
	unordered_map<string, idx_t> binding_map;
};

} // namespace duckdb
