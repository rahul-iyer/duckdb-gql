#pragma once

#include "gql_ast.hpp"

#include "duckdb/common/types/value.hpp"

namespace duckdb {

// Version of the compact logical-node arena passed through DuckDB's
// parser-extension Value boundary for ordinary fixed MATCH queries.
static constexpr uint8_t GQL_LOGICAL_PROGRAM_VERSION = 1;

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
  enum class Source : uint8_t { GRAPH, PROCEDURE };

  string name;
  idx_t index = 0;
  GqlType type;
  GqlSourceRange source;
  Source binding_source = Source::GRAPH;
};

struct GqlBoundExpression {
  GqlExpressionType expression_type = GqlExpressionType::LITERAL;
  GqlType result_type;
  idx_t binding_index = DConstants::INVALID_INDEX;
  GqlBinding::Source binding_source = GqlBinding::Source::GRAPH;
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

struct GqlBoundInsertProperty {
  string name;
  string value_column;
  GqlSourceRange source;
};

struct GqlBoundInsertVertex {
  bool existing = false;
  bool create = false;
  idx_t binding_index = DConstants::INVALID_INDEX;
  idx_t allocation_index = DConstants::INVALID_INDEX;
  string existing_id_column;
  vector<string> labels;
  vector<GqlBoundInsertProperty> properties;
  GqlSourceRange source;
};

struct GqlBoundInsertEdge {
  idx_t allocation_index = DConstants::INVALID_INDEX;
  idx_t source_vertex = DConstants::INVALID_INDEX;
  idx_t target_vertex = DConstants::INVALID_INDEX;
  vector<string> labels;
  vector<GqlBoundInsertProperty> properties;
  GqlSourceRange source;
};

struct GqlBoundInsert {
  vector<GqlBoundInsertVertex> vertices;
  vector<GqlBoundInsertEdge> edges;
  idx_t new_vertex_count = 0;
};

enum class GqlLogicalOperatorType : uint8_t {
  UNIT,
  MATCH,
  FILTER,
  INNER_APPLY,
  LEFT_APPLY,
  CALL,
  PROJECT
};

// Procedure input is an execution contract, not an algorithm-specific parser
// rule. NONE consumes the child to stay in the same plan/transaction but runs
// once. BATCH evaluates its declared input expressions for every child row and
// invokes the procedure once with that batch. ROW is reserved for lateral
// row-at-a-time procedures.
enum class GqlProcedureInputMode : uint8_t { NONE, ROW, BATCH };

struct GqlLogicalProperties {
  // Dense binding bitmaps make dependency checks deterministic and cheap.
  vector<bool> output_bindings;
  vector<bool> required_bindings;
  vector<bool> nullable_bindings;
  vector<bool> correlated_bindings;

  // This lower bound is exact for UNIT and preserved by LEFT_APPLY. A future
  // statistics pass can add a cost estimate without changing operator shape.
  idx_t minimum_cardinality = 0;
};

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

  template <class TARGET> TARGET &Cast() {
    auto result = dynamic_cast<TARGET *>(this);
    if (!result) {
      throw InternalException("Invalid GQL logical operator cast");
    }
    return *result;
  }

  GqlLogicalOperatorType type;
  shared_ptr<GqlLogicalOperator> child;
  GqlLogicalProperties properties;
  bool properties_valid = false;
};

// The identity relation: one row with no bindings. This makes a leading
// OPTIONAL MATCH an ordinary LEFT_APPLY instead of a special property of a
// MATCH node.
class GqlLogicalUnit final : public GqlLogicalOperator {
public:
  GqlLogicalUnit() : GqlLogicalOperator(GqlLogicalOperatorType::UNIT) {}
};

class GqlLogicalMatch final : public GqlLogicalOperator {
public:
  GqlLogicalMatch() : GqlLogicalOperator(GqlLogicalOperatorType::MATCH) {}

  vector<GqlBoundPattern> patterns;
};

class GqlLogicalFilter final : public GqlLogicalOperator {
public:
  explicit GqlLogicalFilter(shared_ptr<GqlBoundExpression> predicate_p)
      : GqlLogicalOperator(GqlLogicalOperatorType::FILTER),
        predicate(std::move(predicate_p)) {}

  shared_ptr<GqlBoundExpression> predicate;
};

// APPLY is the correlation boundary between an already-bound left pipeline
// and one MATCH stage on the right. INNER_APPLY extends successful bindings;
// LEFT_APPLY also preserves a left row when its right stage has no match.
class GqlLogicalInnerApply final : public GqlLogicalOperator {
public:
  GqlLogicalInnerApply()
      : GqlLogicalOperator(GqlLogicalOperatorType::INNER_APPLY) {}

  shared_ptr<GqlLogicalOperator> right;
};

class GqlLogicalLeftApply final : public GqlLogicalOperator {
public:
  explicit GqlLogicalLeftApply(idx_t optional_stage_p)
      : GqlLogicalOperator(GqlLogicalOperatorType::LEFT_APPLY),
        optional_stage(optional_stage_p) {}

  shared_ptr<GqlLogicalOperator> right;
  idx_t optional_stage;
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

class GqlLogicalCall final : public GqlLogicalOperator {
public:
  GqlLogicalCall() : GqlLogicalOperator(GqlLogicalOperatorType::CALL) {}

  string procedure_namespace;
  string procedure_name;
  GqlProcedureInputMode input_mode = GqlProcedureInputMode::NONE;
  vector<GqlLiteral> configuration_arguments;
  vector<string> output_names;
  vector<GqlType> output_types;
};

struct GqlLogicalPlan {
  vector<GqlBinding> bindings;
  shared_ptr<GqlLogicalOperator> root;
  idx_t binding_count = 0;
  vector<GqlBoundMutation> mutations;
  shared_ptr<GqlBoundInsert> insertion;
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
