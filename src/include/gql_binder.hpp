#pragma once

#include "gql_ir.hpp"

namespace duckdb {

class GqlBinder {
public:
	GqlLogicalPlan Bind(const GqlMatchStatement &statement);
	vector<GqlLogicalPlan> BindAlternatives(const GqlMatchStatement &statement);

private:
	struct BindingScope {
		idx_t parent = DConstants::INVALID_INDEX;
		unordered_map<string, idx_t> bindings;
	};

	shared_ptr<GqlBoundExpression> BindExpression(const GqlExpression &expression);
	const GqlExpression &ResolveValueExpression(const GqlExpression &expression) const;
	void ValidateLetExpression(const GqlExpression &expression);
	GqlBinding &Resolve(const GqlIdentifier &identifier);
	idx_t ResolveIndex(const GqlIdentifier &identifier) const;
	idx_t FindBindingIndex(const string &name) const;
	idx_t DefineBinding(GqlBinding binding);
	void ResetScopes();
	void PushScope();
	string ProjectionName(const GqlProjection &projection, const GqlBoundExpression &expression) const;

	vector<GqlBinding> bindings;
	vector<BindingScope> scopes;
	idx_t current_scope = DConstants::INVALID_INDEX;
	unordered_map<idx_t, vector<GqlBoundPatternElement>> path_bindings;
	unordered_map<string, const GqlExpression *> let_bindings;
};

} // namespace duckdb
