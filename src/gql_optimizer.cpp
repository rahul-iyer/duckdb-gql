#include "gql_optimizer.hpp"

namespace duckdb {

static vector<bool> EmptyBindingSet(idx_t binding_count) {
  return vector<bool>(binding_count, false);
}

static void UnionInto(vector<bool> &target, const vector<bool> &source) {
  if (target.size() != source.size()) {
    throw InternalException("Incompatible GQL logical binding sets");
  }
  for (idx_t index = 0; index < target.size(); index++) {
    target[index] = target[index] || source[index];
  }
}

static vector<bool> Intersection(const vector<bool> &left,
                                 const vector<bool> &right) {
  if (left.size() != right.size()) {
    throw InternalException("Incompatible GQL logical binding sets");
  }
  vector<bool> result(left.size(), false);
  for (idx_t index = 0; index < left.size(); index++) {
    result[index] = left[index] && right[index];
  }
  return result;
}

static bool IsSubset(const vector<bool> &subset,
                     const vector<bool> &superset) {
  if (subset.size() != superset.size()) {
    throw InternalException("Incompatible GQL logical binding sets");
  }
  for (idx_t index = 0; index < subset.size(); index++) {
    if (subset[index] && !superset[index]) {
      return false;
    }
  }
  return true;
}

static vector<bool> ExpressionBindings(const GqlBoundExpression &expression,
                                       idx_t binding_count) {
  auto result = EmptyBindingSet(binding_count);
  auto collect = [&](auto &self, const GqlBoundExpression &entry) -> void {
    if (entry.binding_index != DConstants::INVALID_INDEX &&
        entry.binding_source == GqlBinding::Source::GRAPH) {
      if (entry.binding_index >= binding_count) {
        throw InternalException("GQL expression binding is outside the plan");
      }
      result[entry.binding_index] = true;
    }
    if (entry.left) {
      self(self, *entry.left);
    }
    if (entry.right) {
      self(self, *entry.right);
    }
    for (const auto &argument : entry.arguments) {
      if (!argument) {
        throw InternalException("GQL expression contains an empty argument");
      }
      self(self, *argument);
    }
  };
  collect(collect, expression);
  return result;
}

static const GqlLogicalProperties &
InferProperties(const shared_ptr<GqlLogicalOperator> &operation,
                idx_t binding_count) {
  if (!operation) {
    throw InternalException("Cannot infer properties for an empty GQL plan");
  }
  if (operation->properties_valid) {
    return operation->properties;
  }
  GqlLogicalProperties result;
  result.output_bindings = EmptyBindingSet(binding_count);
  result.required_bindings = EmptyBindingSet(binding_count);
  result.nullable_bindings = EmptyBindingSet(binding_count);
  result.correlated_bindings = EmptyBindingSet(binding_count);

  switch (operation->type) {
  case GqlLogicalOperatorType::UNIT:
    if (operation->child) {
      throw InternalException("GQL UNIT cannot have a child");
    }
    result.minimum_cardinality = 1;
    break;
  case GqlLogicalOperatorType::MATCH: {
    const auto &match = operation->Cast<GqlLogicalMatch>();
    if (operation->child || match.patterns.empty()) {
      throw InternalException("Invalid GQL MATCH stage");
    }
    for (const auto &pattern : match.patterns) {
      for (const auto &element : pattern.elements) {
        if (element.binding_index >= binding_count) {
          throw InternalException("GQL MATCH binding is outside the plan");
        }
        result.output_bindings[element.binding_index] = true;
      }
    }
    break;
  }
  case GqlLogicalOperatorType::FILTER: {
    const auto &filter = operation->Cast<GqlLogicalFilter>();
    if (!filter.predicate) {
      throw InternalException("GQL FILTER has no predicate");
    }
    const auto &child = InferProperties(filter.child, binding_count);
    result = child;
    auto dependencies = ExpressionBindings(*filter.predicate, binding_count);
    for (idx_t index = 0; index < binding_count; index++) {
      if (dependencies[index] && !child.output_bindings[index]) {
        result.required_bindings[index] = true;
      }
    }
    result.minimum_cardinality = 0;
    break;
  }
  case GqlLogicalOperatorType::INNER_APPLY:
  case GqlLogicalOperatorType::LEFT_APPLY: {
    const shared_ptr<GqlLogicalOperator> *right_ptr;
    if (operation->type == GqlLogicalOperatorType::INNER_APPLY) {
      right_ptr = &operation->Cast<GqlLogicalInnerApply>().right;
    } else {
      right_ptr = &operation->Cast<GqlLogicalLeftApply>().right;
    }
    const auto &left = InferProperties(operation->child, binding_count);
    const auto &right = InferProperties(*right_ptr, binding_count);
    auto right_dependencies = right.required_bindings;
    auto right_output_correlations =
        Intersection(left.output_bindings, right.output_bindings);
    auto right_required_correlations =
        Intersection(left.output_bindings, right_dependencies);
    result.correlated_bindings = std::move(right_output_correlations);
    UnionInto(result.correlated_bindings, right_required_correlations);
    for (idx_t index = 0; index < binding_count; index++) {
      if (right_dependencies[index] && !left.output_bindings[index]) {
        throw InternalException(
            "GQL APPLY right side requires an unavailable binding");
      }
    }
    result.output_bindings = left.output_bindings;
    UnionInto(result.output_bindings, right.output_bindings);
    result.required_bindings = left.required_bindings;
    result.nullable_bindings = left.nullable_bindings;
    UnionInto(result.nullable_bindings, right.nullable_bindings);
    if (operation->type == GqlLogicalOperatorType::LEFT_APPLY) {
      for (idx_t index = 0; index < binding_count; index++) {
        if (right.output_bindings[index] && !left.output_bindings[index]) {
          result.nullable_bindings[index] = true;
        }
      }
      result.minimum_cardinality = left.minimum_cardinality;
    }
    break;
  }
  case GqlLogicalOperatorType::CALL: {
    const auto &call = operation->Cast<GqlLogicalCall>();
    if (!call.child || call.output_names.empty() ||
        call.output_names.size() != call.output_types.size()) {
      throw InternalException("Invalid GQL CALL operator");
    }
    // CALL is a relation-replacement barrier. Its child is still part of this
    // plan and transaction, but graph bindings are not visible after a
    // blocking batch/no-input procedure unless a future contract explicitly
    // declares preservation.
    (void)InferProperties(call.child, binding_count);
    result.minimum_cardinality = 0;
    break;
  }
  case GqlLogicalOperatorType::PROJECT: {
    const auto &project = operation->Cast<GqlLogicalProject>();
    const auto &child = InferProperties(project.child, binding_count);
    result = child;
    for (const auto &projection : project.projections) {
      if (!projection.expression ||
          !IsSubset(ExpressionBindings(*projection.expression, binding_count),
                    child.output_bindings)) {
        throw InternalException("GQL projection requires an unavailable binding");
      }
    }
    for (const auto &order : project.order_by) {
      if (!order.expression ||
          !IsSubset(ExpressionBindings(*order.expression, binding_count),
                    child.output_bindings)) {
        throw InternalException("GQL ordering requires an unavailable binding");
      }
    }
    break;
  }
  }
  operation->properties = std::move(result);
  operation->properties_valid = true;
  return operation->properties;
}

static void ClearProperties(const shared_ptr<GqlLogicalOperator> &operation) {
  if (!operation) {
    return;
  }
  operation->properties_valid = false;
  ClearProperties(operation->child);
  if (operation->type == GqlLogicalOperatorType::INNER_APPLY) {
    ClearProperties(operation->Cast<GqlLogicalInnerApply>().right);
  } else if (operation->type == GqlLogicalOperatorType::LEFT_APPLY) {
    ClearProperties(operation->Cast<GqlLogicalLeftApply>().right);
  }
}

static shared_ptr<GqlLogicalOperator>
PushFilter(const shared_ptr<GqlLogicalOperator> &filter_operation,
           idx_t binding_count) {
  auto &filter = filter_operation->Cast<GqlLogicalFilter>();
  auto dependencies = ExpressionBindings(*filter.predicate, binding_count);
  auto child = filter.child;
  if (!child || (child->type != GqlLogicalOperatorType::INNER_APPLY &&
                 child->type != GqlLogicalOperatorType::LEFT_APPLY)) {
    return filter_operation;
  }

  shared_ptr<GqlLogicalOperator> right;
  if (child->type == GqlLogicalOperatorType::INNER_APPLY) {
    right = child->Cast<GqlLogicalInnerApply>().right;
  } else {
    right = child->Cast<GqlLogicalLeftApply>().right;
  }
  const auto &left_properties =
      InferProperties(child->child, binding_count);
  if (IsSubset(dependencies, left_properties.output_bindings)) {
    filter.child = child->child;
    filter_operation->properties_valid = false;
    child->child = PushFilter(filter_operation, binding_count);
    child->properties_valid = false;
    return child;
  }

  // A predicate that touches the null-extended side must remain above a
  // LEFT_APPLY. Moving it into the join condition would preserve rows that the
  // original post-join filter rejects.
  if (child->type == GqlLogicalOperatorType::LEFT_APPLY) {
    return filter_operation;
  }

  filter.child = right;
  filter_operation->properties_valid = false;
  child->Cast<GqlLogicalInnerApply>().right =
      PushFilter(filter_operation, binding_count);
  child->properties_valid = false;
  return child;
}

static shared_ptr<GqlLogicalOperator>
RewritePredicates(shared_ptr<GqlLogicalOperator> operation,
                  idx_t binding_count) {
  if (!operation) {
    throw InternalException("Cannot optimize an empty GQL logical operator");
  }
  operation->child = operation->child
                         ? RewritePredicates(operation->child, binding_count)
                         : nullptr;
  if (operation->type == GqlLogicalOperatorType::INNER_APPLY) {
    auto &apply = operation->Cast<GqlLogicalInnerApply>();
    apply.right = RewritePredicates(apply.right, binding_count);
  } else if (operation->type == GqlLogicalOperatorType::LEFT_APPLY) {
    auto &apply = operation->Cast<GqlLogicalLeftApply>();
    apply.right = RewritePredicates(apply.right, binding_count);
  }
  operation->properties_valid = false;
  if (operation->type != GqlLogicalOperatorType::FILTER) {
    return operation;
  }
  return PushFilter(operation, binding_count);
}

void GqlOptimize(GqlLogicalPlan &plan) {
  if (!plan.root || plan.binding_count == 0) {
    throw InternalException("Cannot optimize an empty GQL logical plan");
  }
  plan.root = RewritePredicates(std::move(plan.root), plan.binding_count);
  ClearProperties(plan.root);
  const auto &properties = InferProperties(plan.root, plan.binding_count);
  for (const auto required : properties.required_bindings) {
    if (required) {
      throw InternalException("GQL logical plan has an unresolved binding dependency");
    }
  }

  // Do not reorder mandatory MATCH stages here. Fixed table-backed regions are
  // lowered into one native DuckDB join graph, where DuckDB can use table and
  // column statistics for join enumeration. This pass owns graph semantics and
  // barriers such as LEFT_APPLY; graph-specific costing belongs only to
  // physical alternatives DuckDB cannot see, such as recursive path engines.
}

} // namespace duckdb
