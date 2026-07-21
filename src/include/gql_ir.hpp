#pragma once

#include "gql_ast.hpp"

#include "duckdb/common/types/value.hpp"

namespace duckdb {

enum class GqlTypeId : uint8_t {
  UNKNOWN,
  NULL_VALUE,
  BOOLEAN,
  INTEGER,
  ELEMENT_ID,
  DECIMAL,
  DOUBLE,
  STRING,
  NODE,
  EDGE,
  PATH,
  PROPERTY_VALUE
};

struct GqlType {
  GqlTypeId id = GqlTypeId::UNKNOWN;
  bool nullable = true;
};

struct GqlBinding {
  string name;
  idx_t index = 0;
  GqlType type;
  GqlSourceRange source;
};

struct GqlBoundExpression {
  GqlExpressionType expression_type = GqlExpressionType::LITERAL;
  GqlType result_type;
  idx_t binding_index = DConstants::INVALID_INDEX;
  GqlLiteral literal;
  string property;
  GqlUnaryOperator unary_operator = GqlUnaryOperator::PLUS;
  GqlBinaryOperator binary_operator = GqlBinaryOperator::EQUAL;
  string function_name;
  bool aggregate = false;
  bool distinct = false;
  bool negated = false;
  shared_ptr<GqlBoundExpression> left;
  shared_ptr<GqlBoundExpression> right;
  vector<shared_ptr<GqlBoundExpression>> arguments;
  GqlSourceRange source;
};

struct GqlBoundPatternElement {
  GqlPatternElementType type = GqlPatternElementType::VERTEX;
  GqlEdgeDirection edge_direction = GqlEdgeDirection::RIGHT;
  bool quantified = false;
  bool unbounded = false;
  idx_t minimum_repetitions = 1;
  idx_t maximum_repetitions = 1;
  idx_t binding_index = DConstants::INVALID_INDEX;
  vector<string> labels;
  GqlSourceRange source;
};

struct GqlBoundPattern {
  vector<GqlBoundPatternElement> elements;
  bool optional = false;
  idx_t optional_stage = 0;
  GqlSourceRange source;
};

struct GqlBoundProjection {
  shared_ptr<GqlBoundExpression> expression;
  string name;
  GqlSourceRange source;
};

struct GqlBoundOrderBy {
  shared_ptr<GqlBoundExpression> expression;
  idx_t projection_index = DConstants::INVALID_INDEX;
  bool descending = false;
  bool nulls_first = false;
  bool null_order_specified = false;
  GqlSourceRange source;
};

struct GqlBoundMutation {
  GqlMutationType type = GqlMutationType::SET_PROPERTY;
  idx_t binding_index = DConstants::INVALID_INDEX;
  GqlType binding_type;
  string name;
  shared_ptr<GqlBoundExpression> value;
  bool detach = false;
  GqlSourceRange source;
};

enum class GqlLogicalOperatorType : uint8_t { MATCH, FILTER, PROJECT };

class GqlLogicalOperator {
public:
  explicit GqlLogicalOperator(GqlLogicalOperatorType type_p) : type(type_p) {}
  virtual ~GqlLogicalOperator() {}

  template <class TARGET> const TARGET &Cast() const {
    auto result = dynamic_cast<const TARGET *>(this);
    if (!result) {
      throw InternalException("Invalid GQL logical operator cast");
    }
    return *result;
  }

  GqlLogicalOperatorType type;
  shared_ptr<GqlLogicalOperator> child;
};

class GqlLogicalMatch final : public GqlLogicalOperator {
public:
  GqlLogicalMatch() : GqlLogicalOperator(GqlLogicalOperatorType::MATCH) {}

  vector<GqlBoundPattern> patterns;
  vector<shared_ptr<GqlBoundExpression>> optional_predicates;
  vector<idx_t> optional_predicate_stages;
  idx_t binding_count = 0;
  bool optional = false;
};

class GqlLogicalFilter final : public GqlLogicalOperator {
public:
  explicit GqlLogicalFilter(shared_ptr<GqlBoundExpression> predicate_p)
      : GqlLogicalOperator(GqlLogicalOperatorType::FILTER),
        predicate(std::move(predicate_p)) {}

  shared_ptr<GqlBoundExpression> predicate;
};

class GqlLogicalProject final : public GqlLogicalOperator {
public:
  GqlLogicalProject() : GqlLogicalOperator(GqlLogicalOperatorType::PROJECT) {}

  vector<GqlBoundProjection> projections;
  vector<GqlBoundOrderBy> order_by;
  bool distinct = false;
  bool has_offset = false;
  bool has_limit = false;
  idx_t offset = 0;
  idx_t limit = 0;
};

struct GqlLogicalPlan {
  vector<GqlBinding> bindings;
  shared_ptr<GqlLogicalOperator> root;
  vector<GqlBoundMutation> mutations;
  GqlExecutionMode execution_mode = GqlExecutionMode::NATIVE;
};

struct GqlExpressionProgram {
  vector<uint8_t> node_types;
  vector<uint8_t> result_types;
  vector<uint64_t> binding_indices;
  vector<uint8_t> operators;
  vector<string> values;
  vector<string> properties;
  vector<uint8_t> child_counts;
  vector<bool> aggregate;
  vector<bool> distinct;

  bool IsEmpty() const { return node_types.empty(); }
};

LogicalType GqlExpressionProgramType();
Value GqlSerializeExpression(const GqlBoundExpression &expression);
GqlExpressionProgram GqlDeserializeExpression(const Value &value);
LogicalType GqlDuckType(const GqlType &type);

} // namespace duckdb
