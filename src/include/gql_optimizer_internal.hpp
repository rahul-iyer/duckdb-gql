#pragma once

// Helpers shared by the access-path planners. Not part of the extension's interface.
#include "gql_import.hpp"
#include "gql_optimizer.hpp"

namespace duckdb {

struct GqlLiteralEquality {
	string property_name;
	GqlLiteralType literal_type;
	string literal_value;
};

//! Conjuncts `binding.property = literal` of the predicate rooted at node.
void CollectLiteralEqualityConjuncts(const GqlExpressionProgram &program, idx_t node, idx_t binding_index,
                                     vector<GqlLiteralEquality> &result);
bool TryFindCaseInsensitive(const unordered_map<string, string> &entries, const string &requested, string &value);
bool ProgramReadsBindingProperty(const GqlExpressionProgram &program, idx_t binding_index);
//! Whether a projection returns the binding as part of a whole edge or path value.
bool ProgramProjectsGraphValue(const GqlExpressionProgram &program, idx_t binding_index);
bool ContainsLabel(const vector<string> &labels, const string &candidate);
const GqlCsrEdgeLabelStats *TryEdgeLabelStats(const GqlFanoutStatistics &statistics, const string &label);
//! Rounds an estimate up, saturating below INVALID_INDEX.
idx_t ClampCardinality(long double value);

} // namespace duckdb
