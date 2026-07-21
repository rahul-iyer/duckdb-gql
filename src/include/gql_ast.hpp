#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

struct GqlSourceRange {
  idx_t start_offset = 0;
  idx_t end_offset = 0;
  idx_t start_line = 0;
  idx_t start_column = 0;
  idx_t end_line = 0;
  idx_t end_column = 0;
};

struct GqlIdentifier {
  string value;
  bool delimited = false;
  GqlSourceRange source;

  bool IsEmpty() const { return value.empty(); }
};

enum class GqlLiteralType : uint8_t {
  NULL_VALUE,
  BOOLEAN,
  INTEGER,
  DECIMAL,
  DOUBLE,
  STRING
};

struct GqlLiteral {
  GqlLiteralType type = GqlLiteralType::STRING;
  string value;
  GqlSourceRange source;
};

struct GqlPropertyAssignment {
  GqlIdentifier name;
  GqlLiteral value;
  GqlSourceRange source;
};

struct GqlInsertElement {
  GqlIdentifier variable;
  vector<GqlIdentifier> labels;
  vector<GqlPropertyAssignment> properties;
  GqlSourceRange source;
};

struct GqlInsertEdge : GqlInsertElement {
  idx_t source_vertex = 0;
  idx_t target_vertex = 0;
};

enum class GqlPatternElementType : uint8_t { VERTEX, EDGE };
enum class GqlEdgeDirection : uint8_t { RIGHT, LEFT, ANY };
enum class GqlExecutionMode : uint8_t { NATIVE, CSR };

struct GqlPatternElement {
  GqlPatternElementType type = GqlPatternElementType::VERTEX;
  GqlEdgeDirection edge_direction = GqlEdgeDirection::RIGHT;
  bool quantified = false;
  bool unbounded = false;
  idx_t minimum_repetitions = 1;
  idx_t maximum_repetitions = 1;
  GqlIdentifier variable;
  vector<GqlIdentifier> labels;
  GqlSourceRange source;
  GqlSourceRange quantifier_source;
};

struct GqlPattern {
  vector<GqlPatternElement> elements;
  GqlIdentifier variable;
  bool optional = false;
  idx_t optional_stage = 0;
  GqlSourceRange source;
};

enum class GqlExpressionType : uint8_t {
  LITERAL,
  VARIABLE_REFERENCE,
  PROPERTY_REFERENCE,
  ELEMENT_ID,
  FUNCTION,
  UNARY,
  BINARY,
  IS_NULL,
  LABELED
};

enum class GqlUnaryOperator : uint8_t { PLUS, MINUS, NOT };

enum class GqlBinaryOperator : uint8_t {
  MULTIPLY,
  DIVIDE,
  ADD,
  SUBTRACT,
  CONCATENATE,
  EQUAL,
  NOT_EQUAL,
  LESS_THAN,
  GREATER_THAN,
  LESS_THAN_OR_EQUAL,
  GREATER_THAN_OR_EQUAL,
  AND,
  OR,
  XOR
};

struct GqlExpression {
  GqlExpressionType type = GqlExpressionType::LITERAL;
  GqlLiteral literal;
  GqlIdentifier variable;
  GqlIdentifier property;
  GqlUnaryOperator unary_operator = GqlUnaryOperator::PLUS;
  GqlBinaryOperator binary_operator = GqlBinaryOperator::EQUAL;
  string function_name;
  bool aggregate = false;
  bool distinct = false;
  bool negated = false;
  shared_ptr<GqlExpression> left;
  shared_ptr<GqlExpression> right;
  vector<shared_ptr<GqlExpression>> arguments;
  GqlSourceRange source;
};

struct GqlProjection {
  shared_ptr<GqlExpression> expression;
  GqlIdentifier alias;
  GqlSourceRange source;
};

struct GqlOrderBy {
  shared_ptr<GqlExpression> expression;
  bool descending = false;
  bool nulls_first = false;
  bool null_order_specified = false;
  GqlSourceRange source;
};

enum class GqlMutationType : uint8_t {
  SET_PROPERTY,
  SET_LABEL,
  CLEAR_PROPERTIES,
  REMOVE_PROPERTY,
  REMOVE_LABEL,
  DELETE_ELEMENT
};

struct GqlMutation {
  GqlMutationType type = GqlMutationType::SET_PROPERTY;
  GqlIdentifier variable;
  GqlIdentifier name;
  shared_ptr<GqlExpression> value;
  bool detach = false;
  GqlSourceRange source;
};

enum class GqlStatementType : uint8_t {
  CREATE_GRAPH,
  COPY_GRAPH,
  DROP_GRAPH,
  SESSION_SET_GRAPH,
  INSERT,
  MERGE,
  MATCH,
  UNSUPPORTED
};

class GqlStatement {
public:
  GqlStatement(GqlStatementType type_p, GqlSourceRange source_p)
      : type(type_p), source(std::move(source_p)) {}
  virtual ~GqlStatement() {}

  template <class TARGET> const TARGET &Cast() const {
    auto result = dynamic_cast<const TARGET *>(this);
    if (!result) {
      throw InternalException("Invalid GQL AST statement cast");
    }
    return *result;
  }

  GqlStatementType type;
  GqlSourceRange source;
};

class GqlCreateGraphStatement final : public GqlStatement {
public:
  GqlCreateGraphStatement(GqlSourceRange source_p, GqlIdentifier graph_name_p,
                          bool if_not_exists_p)
      : GqlStatement(GqlStatementType::CREATE_GRAPH, std::move(source_p)),
        graph_name(std::move(graph_name_p)), if_not_exists(if_not_exists_p) {}

  GqlIdentifier graph_name;
  bool if_not_exists;
};

class GqlCopyGraphStatement final : public GqlStatement {
public:
  GqlCopyGraphStatement(GqlSourceRange source_p, GqlIdentifier graph_name_p,
                        string vertex_path_p, string edge_path_p,
                        bool validate_p)
      : GqlStatement(GqlStatementType::COPY_GRAPH, std::move(source_p)),
        graph_name(std::move(graph_name_p)),
        vertex_path(std::move(vertex_path_p)),
        edge_path(std::move(edge_path_p)), validate(validate_p) {}

  GqlIdentifier graph_name;
  string vertex_path;
  string edge_path;
  bool validate;
};

class GqlDropGraphStatement final : public GqlStatement {
public:
  GqlDropGraphStatement(GqlSourceRange source_p, GqlIdentifier graph_name_p,
                        bool if_exists_p)
      : GqlStatement(GqlStatementType::DROP_GRAPH, std::move(source_p)),
        graph_name(std::move(graph_name_p)), if_exists(if_exists_p) {}

  GqlIdentifier graph_name;
  bool if_exists;
};

class GqlSessionSetGraphStatement final : public GqlStatement {
public:
  GqlSessionSetGraphStatement(GqlSourceRange source_p,
                              GqlIdentifier graph_name_p)
      : GqlStatement(GqlStatementType::SESSION_SET_GRAPH, std::move(source_p)),
        graph_name(std::move(graph_name_p)) {}

  GqlIdentifier graph_name;
};

class GqlInsertStatement final : public GqlStatement {
public:
  explicit GqlInsertStatement(GqlSourceRange source_p)
      : GqlStatement(GqlStatementType::INSERT, std::move(source_p)) {}

  vector<GqlInsertElement> vertices;
  vector<GqlInsertEdge> edges;
};

// MERGE is a project-owned Cypher compatibility extension. It is deliberately
// kept separate from the ISO GQL INSERT AST because ISO/IEC 39075:2024 does not
// define a MERGE statement.
class GqlMergeStatement final : public GqlStatement {
public:
  explicit GqlMergeStatement(GqlSourceRange source_p)
      : GqlStatement(GqlStatementType::MERGE, std::move(source_p)) {}

  GqlInsertElement vertex;
};

class GqlMatchStatement final : public GqlStatement {
public:
  explicit GqlMatchStatement(GqlSourceRange source_p)
      : GqlStatement(GqlStatementType::MATCH, std::move(source_p)) {}

  vector<GqlPattern> patterns;
  vector<shared_ptr<GqlExpression>> predicates;
  vector<idx_t> predicate_optional_stages;
  vector<GqlProjection> projections;
  vector<GqlOrderBy> order_by;
  vector<GqlIdentifier> group_by_variables;
  vector<GqlMutation> mutations;
  bool has_mutation = false;
  bool optional = false;
  bool distinct = false;
  bool has_offset = false;
  bool has_limit = false;
  idx_t offset = 0;
  idx_t limit = 0;
};

class GqlUnsupportedStatement final : public GqlStatement {
public:
  GqlUnsupportedStatement(GqlSourceRange source_p, string feature_p)
      : GqlStatement(GqlStatementType::UNSUPPORTED, std::move(source_p)),
        feature(std::move(feature_p)) {}

  string feature;
};

} // namespace duckdb
