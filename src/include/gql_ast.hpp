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

struct GqlExpression;

struct GqlPropertyAssignment {
  GqlIdentifier name;
  GqlLiteral value;
  shared_ptr<GqlExpression> expression;
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
  LABELED,
  LIST_CONSTRUCTOR,
  RECORD_CONSTRUCTOR
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
  vector<GqlIdentifier> field_names;
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

struct GqlYieldItem {
  GqlIdentifier field;
  GqlIdentifier alias;
  GqlSourceRange source;
};

// A procedure invocation embedded in a linear query. Unlike the standalone
// CALL statement, arguments are expressions so a procedure can declare that
// selected arguments are evaluated over the complete upstream row set.
struct GqlProcedureCall {
  GqlIdentifier procedure_namespace;
  GqlIdentifier procedure_name;
  vector<shared_ptr<GqlExpression>> arguments;
  vector<GqlYieldItem> yield_items;
  GqlSourceRange source;
};

// Linear queries are represented as an ordered clause stream. Clause payloads
// reference the statement-owned AST pools by index so copying a statement for
// a physical alternative does not duplicate or invalidate its query shape.
// This is the compatibility seam for migrating the remaining flat fields below
// into dedicated clause payload arenas.
enum class GqlQueryClauseType : uint8_t { MATCH, LET, FILTER, CALL, RETURN };

struct GqlQueryClause {
  GqlQueryClauseType type = GqlQueryClauseType::MATCH;
  GqlSourceRange source;

  // MATCH payload: a contiguous range in GqlMatchStatement::patterns and any
  // graph-pattern WHERE predicates owned by this MATCH.
  idx_t pattern_begin = 0;
  idx_t pattern_count = 0;
  vector<idx_t> predicate_indices;
  bool optional = false;
  idx_t optional_stage = 0;

  // FILTER payload: one entry in GqlMatchStatement::predicates.
  idx_t predicate_index = DConstants::INVALID_INDEX;

  // LET payload: a contiguous range in GqlMatchStatement::let_bindings.
  idx_t let_begin = 0;
  idx_t let_count = 0;

  // CALL payload: one entry in GqlMatchStatement::procedure_calls.
  idx_t procedure_call_index = DConstants::INVALID_INDEX;
};

struct GqlLetBinding {
  GqlIdentifier variable;
  shared_ptr<GqlExpression> expression;
  GqlSourceRange source;
};

enum class GqlMutationType : uint8_t {
  SET_PROPERTY,
  SET_PROPERTIES,
  SET_LABEL,
  CLEAR_PROPERTIES,
  MERGE_PROPERTIES,
  REMOVE_PROPERTY,
  REMOVE_LABEL,
  DELETE_ELEMENT
};

struct GqlMutation {
  GqlMutationType type = GqlMutationType::SET_PROPERTY;
  GqlIdentifier variable;
  GqlIdentifier name;
  shared_ptr<GqlExpression> target;
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
  CALL,
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

class GqlCallStatement final : public GqlStatement {
public:
  explicit GqlCallStatement(GqlSourceRange source_p)
      : GqlStatement(GqlStatementType::CALL, std::move(source_p)) {}

  GqlIdentifier procedure_namespace;
  GqlIdentifier procedure_name;
  vector<GqlLiteral> arguments;
  // Empty entries are positional. Non-empty entries preserve project-owned
  // DuckDB-style `name := value` configuration arguments for CALL pipelines;
  // ISO GQL's imported grammar only models positional procedure arguments.
  vector<GqlIdentifier> argument_names;
  vector<GqlYieldItem> yield_items;
  vector<GqlProjection> projections;
  vector<GqlOrderBy> order_by;
  bool distinct = false;
  bool has_offset = false;
  bool has_limit = false;
  idx_t offset = 0;
  idx_t limit = 0;
};

class GqlMatchStatement final : public GqlStatement {
public:
  explicit GqlMatchStatement(GqlSourceRange source_p)
      : GqlStatement(GqlStatementType::MATCH, std::move(source_p)) {}

  vector<GqlQueryClause> clauses;
  vector<GqlPattern> patterns;
  vector<shared_ptr<GqlExpression>> predicates;
  vector<GqlLetBinding> let_bindings;
  vector<GqlProcedureCall> procedure_calls;
  vector<GqlProjection> projections;
  vector<GqlOrderBy> order_by;
  vector<GqlIdentifier> group_by_variables;
  vector<GqlMutation> mutations;
  shared_ptr<GqlInsertStatement> insertion;
  bool has_mutation = false;
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
