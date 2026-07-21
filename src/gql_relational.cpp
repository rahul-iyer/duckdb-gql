#include "gql_relational.hpp"

#include "gql_ast.hpp"
#include "gql_catalog.hpp"
#include "gql_ir.hpp"
#include "gql_storage.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

namespace duckdb {

struct RelationalPatternElement {
  GqlPatternElementType type;
  idx_t binding_index;
  string label;
  bool reverse = false;
};

struct RelationalPattern {
  vector<RelationalPatternElement> elements;
  bool optional = false;
  idx_t optional_stage = 0;
};

struct RelationalResultModifiers {
  bool optional = false;
  bool distinct = false;
  vector<idx_t> order_indices;
  vector<bool> order_descending;
  vector<uint8_t> order_nulls;
  bool has_limit = false;
  idx_t limit = 0;
  bool has_offset = false;
  idx_t offset = 0;
};

struct RelationalMatchInput {
  vector<RelationalPattern> patterns;
  vector<GqlPatternElementType> binding_types;
  vector<GqlExpressionProgram> projections;
  vector<string> projection_names;
  vector<GqlExpressionProgram> predicates;
  vector<GqlExpressionProgram> optional_predicates;
  vector<idx_t> optional_predicate_stages;
  RelationalResultModifiers modifiers;
};

struct RecursiveMatchInput {
  idx_t binding_count;
  idx_t source_binding;
  idx_t edge_binding;
  idx_t target_binding;
  vector<string> source_labels;
  vector<string> edge_labels;
  vector<string> target_labels;
  bool reverse;
  idx_t minimum_repetitions;
  bool use_csr;
  vector<GqlExpressionProgram> projections;
  vector<string> projection_names;
  vector<GqlExpressionProgram> predicates;
  RelationalResultModifiers modifiers;
};

struct RelationalPropertyAccess {
  string table_alias;
  string column_name;
};

struct RelationalIdentityAccess {
  string table_alias;
  string column_name;
};

using RelationalPropertyMap = unordered_map<string, RelationalPropertyAccess>;

static vector<string> ReadStringList(const Value &value) {
  vector<string> result;
  for (const auto &entry : ListValue::GetChildren(value)) {
    result.push_back(entry.GetValue<string>());
  }
  return result;
}

static vector<uint64_t> ReadIndexList(const Value &value) {
  vector<uint64_t> result;
  for (const auto &entry : ListValue::GetChildren(value)) {
    result.push_back(entry.GetValue<uint64_t>());
  }
  return result;
}

static vector<uint8_t> ReadByteList(const Value &value) {
  vector<uint8_t> result;
  for (const auto &entry : ListValue::GetChildren(value)) {
    result.push_back(entry.GetValue<uint8_t>());
  }
  return result;
}

static vector<bool> ReadBooleanList(const Value &value) {
  vector<bool> result;
  for (const auto &entry : ListValue::GetChildren(value)) {
    result.push_back(entry.GetValue<bool>());
  }
  return result;
}

static RelationalResultModifiers
ReadResultModifiers(TableFunctionBindInput &input, idx_t offset) {
  RelationalResultModifiers result;
  result.optional = input.inputs[offset].GetValue<bool>();
  result.distinct = input.inputs[offset + 1].GetValue<bool>();
  for (const auto index : ReadIndexList(input.inputs[offset + 2])) {
    result.order_indices.push_back(NumericCast<idx_t>(index));
  }
  result.order_descending = ReadBooleanList(input.inputs[offset + 3]);
  result.order_nulls = ReadByteList(input.inputs[offset + 4]);
  result.has_limit = input.inputs[offset + 5].GetValue<bool>();
  result.limit =
      NumericCast<idx_t>(input.inputs[offset + 6].GetValue<uint64_t>());
  result.has_offset = input.inputs[offset + 7].GetValue<bool>();
  result.offset =
      NumericCast<idx_t>(input.inputs[offset + 8].GetValue<uint64_t>());
  if (result.order_indices.size() != result.order_descending.size() ||
      result.order_indices.size() != result.order_nulls.size()) {
    throw BinderException("Invalid GQL result modifiers");
  }
  return result;
}

static void ValidateProgram(const GqlExpressionProgram &program,
                            idx_t variable_count, bool predicate) {
  for (const auto index : program.binding_indices) {
    if (index != NumericLimits<uint64_t>::Maximum() &&
        index >= variable_count) {
      throw BinderException("Invalid GQL MATCH expression variable");
    }
  }
  if (predicate) {
    auto result_type = static_cast<GqlTypeId>(program.result_types[0]);
    if (result_type != GqlTypeId::BOOLEAN &&
        result_type != GqlTypeId::PROPERTY_VALUE) {
      throw BinderException("Invalid GQL MATCH predicate type");
    }
  }
}

static RelationalMatchInput ReadMatchInput(TableFunctionBindInput &input) {
  if (input.inputs.size() != 21) {
    throw BinderException("Invalid GQL relational MATCH input");
  }
  RelationalMatchInput result;
  auto pattern_sizes = ReadIndexList(input.inputs[0]);
  auto element_types = ReadByteList(input.inputs[1]);
  auto binding_indices = ReadIndexList(input.inputs[2]);
  auto labels = ReadStringList(input.inputs[3]);
  auto reverses = ReadBooleanList(input.inputs[4]);
  auto optionals = ReadBooleanList(input.inputs[5]);
  auto optional_stages = ReadIndexList(input.inputs[6]);
  result.projection_names = ReadStringList(input.inputs[8]);
  for (const auto &program : ListValue::GetChildren(input.inputs[7])) {
    result.projections.push_back(GqlDeserializeExpression(program));
  }
  for (const auto &program : ListValue::GetChildren(input.inputs[9])) {
    result.predicates.push_back(GqlDeserializeExpression(program));
  }
  for (const auto &program : ListValue::GetChildren(input.inputs[10])) {
    result.optional_predicates.push_back(GqlDeserializeExpression(program));
  }
  result.optional_predicate_stages = ReadIndexList(input.inputs[11]);
  result.modifiers = ReadResultModifiers(input, 12);
  if (result.projections.empty() ||
      result.projections.size() != result.projection_names.size()) {
    throw BinderException("Invalid GQL MATCH projections");
  }
  if (pattern_sizes.empty() || optionals.size() != pattern_sizes.size() ||
      optional_stages.size() != pattern_sizes.size() ||
      result.optional_predicates.size() !=
          result.optional_predicate_stages.size() ||
      element_types.size() != binding_indices.size() ||
      element_types.size() != labels.size() ||
      element_types.size() != reverses.size()) {
    throw BinderException("Invalid GQL MATCH patterns");
  }

  idx_t offset = 0;
  idx_t binding_count = 0;
  for (idx_t pattern_index = 0; pattern_index < pattern_sizes.size();
       pattern_index++) {
    auto pattern_size = NumericCast<idx_t>(pattern_sizes[pattern_index]);
    if (pattern_size == 0 || pattern_size % 2 == 0 ||
        offset + pattern_size > element_types.size()) {
      throw BinderException("Invalid GQL fixed MATCH pattern");
    }
    RelationalPattern pattern;
    pattern.optional = optionals[pattern_index];
    pattern.optional_stage =
        NumericCast<idx_t>(optional_stages[pattern_index]);
    if ((!pattern.optional && pattern.optional_stage > 0) ||
        (pattern.optional && pattern.optional_stage == 0 &&
         !result.modifiers.optional)) {
      throw BinderException("Invalid GQL optional MATCH stage");
    }
    for (idx_t element_index = 0; element_index < pattern_size;
         element_index++) {
      auto flat_index = offset + element_index;
      auto type = static_cast<GqlPatternElementType>(element_types[flat_index]);
      if ((element_index % 2 == 0 && type != GqlPatternElementType::VERTEX) ||
          (element_index % 2 == 1 && type != GqlPatternElementType::EDGE)) {
        throw BinderException("Invalid GQL fixed MATCH topology");
      }
      auto binding_index = NumericCast<idx_t>(binding_indices[flat_index]);
      binding_count = MaxValue<idx_t>(binding_count, binding_index + 1);
      pattern.elements.push_back(
          {type, binding_index, labels[flat_index], reverses[flat_index]});
    }
    result.patterns.push_back(std::move(pattern));
    offset += pattern_size;
  }
  if (offset != element_types.size()) {
    throw BinderException("Invalid GQL MATCH pattern elements");
  }

  result.binding_types.resize(binding_count);
  vector<bool> binding_seen(binding_count, false);
  for (const auto &pattern : result.patterns) {
    for (const auto &element : pattern.elements) {
      if (binding_seen[element.binding_index] &&
          result.binding_types[element.binding_index] != element.type) {
        throw BinderException(
            "GQL MATCH binding has incompatible element types");
      }
      result.binding_types[element.binding_index] = element.type;
      binding_seen[element.binding_index] = true;
    }
  }
  for (const auto seen : binding_seen) {
    if (!seen) {
      throw BinderException("Invalid GQL MATCH binding slots");
    }
  }
  for (const auto &program : result.projections) {
    ValidateProgram(program, binding_count, false);
  }
  for (const auto &program : result.predicates) {
    ValidateProgram(program, binding_count, true);
  }
  for (const auto &program : result.optional_predicates) {
    ValidateProgram(program, binding_count, true);
  }
  for (const auto index : result.modifiers.order_indices) {
    if (index >= result.projections.size()) {
      throw BinderException("Invalid GQL ORDER BY projection");
    }
  }
  return result;
}

static RecursiveMatchInput
ReadRecursiveMatchInput(TableFunctionBindInput &input) {
  if (input.inputs.size() != 22) {
    throw BinderException("Invalid GQL recursive MATCH input");
  }
  RecursiveMatchInput result;
  result.binding_count =
      NumericCast<idx_t>(input.inputs[0].GetValue<uint64_t>());
  result.source_binding =
      NumericCast<idx_t>(input.inputs[1].GetValue<uint64_t>());
  result.edge_binding =
      NumericCast<idx_t>(input.inputs[2].GetValue<uint64_t>());
  result.target_binding =
      NumericCast<idx_t>(input.inputs[3].GetValue<uint64_t>());
  result.source_labels = ReadStringList(input.inputs[4]);
  result.edge_labels = ReadStringList(input.inputs[5]);
  result.target_labels = ReadStringList(input.inputs[6]);
  result.reverse = input.inputs[7].GetValue<bool>();
  result.minimum_repetitions =
      NumericCast<idx_t>(input.inputs[8].GetValue<uint64_t>());
  result.use_csr = input.inputs[9].GetValue<bool>();
  result.projection_names = ReadStringList(input.inputs[11]);
  for (const auto &program : ListValue::GetChildren(input.inputs[10])) {
    result.projections.push_back(GqlDeserializeExpression(program));
  }
  for (const auto &program : ListValue::GetChildren(input.inputs[12])) {
    result.predicates.push_back(GqlDeserializeExpression(program));
  }
  result.modifiers = ReadResultModifiers(input, 13);
  if (result.binding_count == 0 ||
      result.source_binding >= result.binding_count ||
      result.edge_binding >= result.binding_count ||
      result.target_binding >= result.binding_count ||
      result.edge_binding == result.source_binding ||
      result.edge_binding == result.target_binding) {
    throw BinderException("Invalid GQL recursive MATCH binding slots");
  }
  if (result.projections.empty() ||
      result.projections.size() != result.projection_names.size()) {
    throw BinderException("Invalid GQL recursive MATCH projections");
  }
  for (const auto &program : result.projections) {
    ValidateProgram(program, result.binding_count, false);
  }
  for (const auto &program : result.predicates) {
    ValidateProgram(program, result.binding_count, true);
  }
  for (const auto index : result.modifiers.order_indices) {
    if (index >= result.projections.size()) {
      throw BinderException("Invalid GQL ORDER BY projection");
    }
  }
  return result;
}

static unique_ptr<ParsedExpression> Column(const string &table,
                                           const string &column) {
  return make_uniq<ColumnRefExpression>(column, table);
}

static unique_ptr<ParsedExpression> Constant(Value value) {
  return make_uniq<ConstantExpression>(std::move(value));
}

static unique_ptr<ParsedExpression>
Compare(ExpressionType type, unique_ptr<ParsedExpression> left,
        unique_ptr<ParsedExpression> right) {
  return make_uniq<ComparisonExpression>(type, std::move(left),
                                         std::move(right));
}

static unique_ptr<ParsedExpression> Equal(unique_ptr<ParsedExpression> left,
                                          unique_ptr<ParsedExpression> right) {
  return Compare(ExpressionType::COMPARE_EQUAL, std::move(left),
                 std::move(right));
}

static unique_ptr<ParsedExpression>
NotEqual(unique_ptr<ParsedExpression> left,
         unique_ptr<ParsedExpression> right) {
  return Compare(ExpressionType::COMPARE_NOTEQUAL, std::move(left),
                 std::move(right));
}

static unique_ptr<ParsedExpression>
And(vector<unique_ptr<ParsedExpression>> expressions) {
  if (expressions.empty()) {
    return nullptr;
  }
  if (expressions.size() == 1) {
    return std::move(expressions[0]);
  }
  return make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
                                          std::move(expressions));
}

static unique_ptr<TableRef> NamedTable(const string &name,
                                       const string &alias) {
  auto result = make_uniq<BaseTableRef>();
  result->table_name = name;
  result->alias = alias;
  return std::move(result);
}

static unique_ptr<TableRef> ElementTable(const GqlElementTableBinding &table,
                                         const string &alias) {
  auto result = make_uniq<BaseTableRef>();
  result->catalog_name = table.catalog_name;
  result->schema_name = table.schema_name;
  result->table_name = table.table_name;
  result->alias = alias;
  return std::move(result);
}

static unique_ptr<ParsedExpression>
Aliased(unique_ptr<ParsedExpression> expression, const string &alias) {
  expression->SetAlias(alias);
  return expression;
}

static void AppendJoin(unique_ptr<TableRef> &root, unique_ptr<TableRef> right,
                       JoinType type,
                       vector<unique_ptr<ParsedExpression>> conditions) {
  auto join = make_uniq<JoinRef>(JoinRefType::REGULAR);
  join->left = std::move(root);
  join->right = std::move(right);
  join->type = type;
  join->condition = And(std::move(conditions));
  root = std::move(join);
}

static Value LiteralValue(GqlLiteralType type, const string &text) {
  switch (type) {
  case GqlLiteralType::NULL_VALUE:
    return Value();
  case GqlLiteralType::BOOLEAN:
    return Value::BOOLEAN(text == "true");
  case GqlLiteralType::INTEGER:
    return Value::BIGINT(std::stoll(text));
  case GqlLiteralType::DECIMAL:
    return Value(text).DefaultCastAs(LogicalType::DECIMAL(38, 18));
  case GqlLiteralType::DOUBLE:
    return Value::DOUBLE(std::stod(text));
  case GqlLiteralType::STRING:
    return Value(text);
  }
  throw InternalException("Unknown GQL literal program type");
}

static string PropertyKey(uint64_t binding_index, const string &property) {
  return to_string(binding_index) + "\x1f" + property;
}

static const string GQL_LABEL_ACCESS = "\x1egql_label";

static void CollectProperties(const GqlExpressionProgram &program,
                              RelationalPropertyMap &aliases) {
  for (idx_t node = 0; node < program.node_types.size(); node++) {
    if (static_cast<GqlExpressionType>(program.node_types[node]) !=
            GqlExpressionType::PROPERTY_REFERENCE &&
        static_cast<GqlExpressionType>(program.node_types[node]) !=
            GqlExpressionType::LABELED) {
      continue;
    }
    auto key = PropertyKey(
        program.binding_indices[node],
        static_cast<GqlExpressionType>(program.node_types[node]) ==
                GqlExpressionType::LABELED
            ? GQL_LABEL_ACCESS
            : program.properties[node]);
    if (aliases.find(key) == aliases.end()) {
      auto alias = "gql_op_" + to_string(aliases.size());
      aliases.emplace(std::move(key),
                      RelationalPropertyAccess{std::move(alias), "value"});
    }
  }
}

static ExpressionType ComparisonType(GqlBinaryOperator operation) {
  switch (operation) {
  case GqlBinaryOperator::EQUAL:
    return ExpressionType::COMPARE_EQUAL;
  case GqlBinaryOperator::NOT_EQUAL:
    return ExpressionType::COMPARE_NOTEQUAL;
  case GqlBinaryOperator::LESS_THAN:
    return ExpressionType::COMPARE_LESSTHAN;
  case GqlBinaryOperator::GREATER_THAN:
    return ExpressionType::COMPARE_GREATERTHAN;
  case GqlBinaryOperator::LESS_THAN_OR_EQUAL:
    return ExpressionType::COMPARE_LESSTHANOREQUALTO;
  case GqlBinaryOperator::GREATER_THAN_OR_EQUAL:
    return ExpressionType::COMPARE_GREATERTHANOREQUALTO;
  case GqlBinaryOperator::MULTIPLY:
  case GqlBinaryOperator::DIVIDE:
  case GqlBinaryOperator::ADD:
  case GqlBinaryOperator::SUBTRACT:
  case GqlBinaryOperator::CONCATENATE:
  case GqlBinaryOperator::AND:
  case GqlBinaryOperator::OR:
  case GqlBinaryOperator::XOR:
    break;
  }
  throw InternalException("GQL operation is not a comparison");
}

static string ArithmeticName(GqlBinaryOperator operation) {
  switch (operation) {
  case GqlBinaryOperator::MULTIPLY:
    return "*";
  case GqlBinaryOperator::DIVIDE:
    return "/";
  case GqlBinaryOperator::ADD:
    return "+";
  case GqlBinaryOperator::SUBTRACT:
    return "-";
  default:
    break;
  }
  throw InternalException("GQL operation is not arithmetic");
}

static idx_t ExpressionEnd(const GqlExpressionProgram &program, idx_t node) {
  if (node >= program.node_types.size()) {
    throw InternalException("Truncated GQL expression program");
  }
  auto type = static_cast<GqlExpressionType>(program.node_types[node]);
  auto cursor = node + 1;
  switch (type) {
  case GqlExpressionType::LITERAL:
  case GqlExpressionType::VARIABLE_REFERENCE:
    return cursor;
  case GqlExpressionType::PROPERTY_REFERENCE:
  case GqlExpressionType::ELEMENT_ID:
  case GqlExpressionType::UNARY:
  case GqlExpressionType::IS_NULL:
  case GqlExpressionType::LABELED:
    return ExpressionEnd(program, cursor);
  case GqlExpressionType::BINARY:
    return ExpressionEnd(program, ExpressionEnd(program, cursor));
  case GqlExpressionType::FUNCTION:
    for (idx_t child = 0; child < program.child_counts[node]; child++) {
      cursor = ExpressionEnd(program, cursor);
    }
    return cursor;
  }
  throw InternalException("Unknown GQL expression program node");
}

static unique_ptr<ParsedExpression>
Function(const string &name, vector<unique_ptr<ParsedExpression>> arguments) {
  return make_uniq<FunctionExpression>(name, std::move(arguments));
}

static unique_ptr<ParsedExpression>
VertexHasLabel(const string &table_alias, const string &label_column,
               const string &label) {
  vector<unique_ptr<ParsedExpression>> split_arguments;
  split_arguments.push_back(Column(table_alias, label_column));
  split_arguments.push_back(Constant(Value(";")));
  vector<unique_ptr<ParsedExpression>> contains_arguments;
  contains_arguments.push_back(
      Function("string_split", std::move(split_arguments)));
  contains_arguments.push_back(Constant(Value(label)));
  auto contains = Function("list_contains", std::move(contains_arguments));
  auto result = make_uniq<CaseExpression>();
  CaseCheck missing;
  missing.when_expr = make_uniq<OperatorExpression>(
      ExpressionType::OPERATOR_IS_NULL, contains->Copy());
  missing.then_expr = Constant(Value(false));
  result->case_checks.push_back(std::move(missing));
  result->else_expr = std::move(contains);
  return std::move(result);
}

static unique_ptr<ParsedExpression>
LowerExpression(const GqlExpressionProgram &program, idx_t &cursor,
                const RelationalPropertyMap &property_aliases,
                const vector<RelationalIdentityAccess> &identities,
                GqlTypeId desired_type = GqlTypeId::UNKNOWN) {
  if (cursor >= program.node_types.size()) {
    throw InternalException("Truncated GQL expression program");
  }
  auto node = cursor++;
  auto expression_type =
      static_cast<GqlExpressionType>(program.node_types[node]);
  auto operation = program.operators[node];
  switch (expression_type) {
  case GqlExpressionType::LITERAL:
    return Constant(LiteralValue(static_cast<GqlLiteralType>(operation),
                                 program.values[node]));
  case GqlExpressionType::VARIABLE_REFERENCE:
    if (program.binding_indices[node] >= identities.size()) {
      throw InternalException("GQL identity binding is missing");
    }
    return Column(identities[program.binding_indices[node]].table_alias,
                  identities[program.binding_indices[node]].column_name);
  case GqlExpressionType::PROPERTY_REFERENCE: {
    auto receiver =
        LowerExpression(program, cursor, property_aliases, identities);
    (void)receiver;
    auto key =
        PropertyKey(program.binding_indices[node], program.properties[node]);
    auto entry = property_aliases.find(key);
    if (entry == property_aliases.end()) {
      throw InternalException("GQL property join was not created");
    }
    if (entry->second.column_name.empty()) {
      return Constant(Value());
    }
    return Column(entry->second.table_alias, entry->second.column_name);
  }
  case GqlExpressionType::ELEMENT_ID:
    return LowerExpression(program, cursor, property_aliases, identities);
  case GqlExpressionType::FUNCTION: {
    auto name = program.values[node];
    auto lowered_name = name == "count" && program.child_counts[node] == 0
                            ? "count_star"
                            : name;
    vector<unique_ptr<ParsedExpression>> arguments;
    for (idx_t child = 0; child < program.child_counts[node]; child++) {
      auto desired = GqlTypeId::UNKNOWN;
      if (name == "lower" || name == "upper" || name == "trim" ||
          name == "ltrim" || name == "rtrim" || name == "left" ||
          name == "right" || name == "char_length" || name == "length" ||
          name == "nfc_normalize") {
        desired = child == 0 ? GqlTypeId::STRING : GqlTypeId::INTEGER;
      }
      arguments.push_back(LowerExpression(program, cursor, property_aliases,
                                          identities, desired));
    }
    auto result =
        make_uniq<FunctionExpression>(lowered_name, std::move(arguments));
    result->distinct = program.distinct[node];
    return std::move(result);
  }
  case GqlExpressionType::UNARY: {
    auto input = LowerExpression(program, cursor, property_aliases, identities);
    auto unary = static_cast<GqlUnaryOperator>(operation);
    if (unary == GqlUnaryOperator::NOT) {
      return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT,
                                           std::move(input));
    }
    vector<unique_ptr<ParsedExpression>> children;
    children.push_back(std::move(input));
    auto result = make_uniq<FunctionExpression>(
        unary == GqlUnaryOperator::PLUS ? "+" : "-", std::move(children));
    result->is_operator = true;
    return std::move(result);
  }
  case GqlExpressionType::IS_NULL: {
    auto input = LowerExpression(program, cursor, property_aliases, identities,
                                 GqlTypeId::PROPERTY_VALUE);
    return make_uniq<OperatorExpression>(
        operation ? ExpressionType::OPERATOR_IS_NOT_NULL
                  : ExpressionType::OPERATOR_IS_NULL,
        std::move(input));
  }
  case GqlExpressionType::LABELED: {
    auto input = LowerExpression(program, cursor, property_aliases, identities);
    (void)input;
    auto entry = property_aliases.find(
        PropertyKey(program.binding_indices[node], GQL_LABEL_ACCESS));
    if (entry == property_aliases.end() || entry->second.column_name.empty()) {
      return Constant(Value(program.operators[node] != 0));
    }
    vector<unique_ptr<ParsedExpression>> predicates;
    for (const auto &label : StringUtil::Split(program.properties[node], ';')) {
      predicates.push_back(VertexHasLabel(entry->second.table_alias,
                                          entry->second.column_name, label));
    }
    auto result = And(std::move(predicates));
    if (program.operators[node] != 0) {
      return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT,
                                           std::move(result));
    }
    return result;
  }
  case GqlExpressionType::BINARY: {
    auto left_root = cursor;
    auto right_root = ExpressionEnd(program, left_root);
    auto left_type = static_cast<GqlTypeId>(program.result_types[left_root]);
    auto right_type = static_cast<GqlTypeId>(program.result_types[right_root]);
    auto left_desired = left_type == GqlTypeId::PROPERTY_VALUE
                            ? right_type
                            : GqlTypeId::UNKNOWN;
    auto right_desired = right_type == GqlTypeId::PROPERTY_VALUE
                             ? left_type
                             : GqlTypeId::UNKNOWN;
    auto left = LowerExpression(program, cursor, property_aliases, identities,
                                left_desired);
    auto right = LowerExpression(program, cursor, property_aliases, identities,
                                 right_desired);
    auto binary = static_cast<GqlBinaryOperator>(operation);
    if (binary == GqlBinaryOperator::AND || binary == GqlBinaryOperator::OR) {
      return make_uniq<ConjunctionExpression>(
          binary == GqlBinaryOperator::AND ? ExpressionType::CONJUNCTION_AND
                                           : ExpressionType::CONJUNCTION_OR,
          std::move(left), std::move(right));
    }
    if (binary == GqlBinaryOperator::XOR) {
      auto left_copy = left->Copy();
      auto right_copy = right->Copy();
      auto not_right = make_uniq<OperatorExpression>(
          ExpressionType::OPERATOR_NOT, std::move(right));
      auto not_left = make_uniq<OperatorExpression>(
          ExpressionType::OPERATOR_NOT, std::move(left_copy));
      auto left_branch = make_uniq<ConjunctionExpression>(
          ExpressionType::CONJUNCTION_AND, std::move(left),
          std::move(not_right));
      auto right_branch = make_uniq<ConjunctionExpression>(
          ExpressionType::CONJUNCTION_AND, std::move(not_left),
          std::move(right_copy));
      return make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_OR,
                                              std::move(left_branch),
                                              std::move(right_branch));
    }
    if (binary == GqlBinaryOperator::MULTIPLY ||
        binary == GqlBinaryOperator::DIVIDE ||
        binary == GqlBinaryOperator::ADD ||
        binary == GqlBinaryOperator::SUBTRACT) {
      vector<unique_ptr<ParsedExpression>> children;
      children.push_back(std::move(left));
      children.push_back(std::move(right));
      auto result = make_uniq<FunctionExpression>(ArithmeticName(binary),
                                                  std::move(children));
      result->is_operator = true;
      return std::move(result);
    }
    if (binary == GqlBinaryOperator::CONCATENATE) {
      vector<unique_ptr<ParsedExpression>> children;
      children.push_back(std::move(left));
      children.push_back(std::move(right));
      auto result = make_uniq<FunctionExpression>("||", std::move(children));
      result->is_operator = true;
      return std::move(result);
    }
    return Compare(ComparisonType(binary), std::move(left), std::move(right));
  }
  }
  throw InternalException("Unknown GQL expression program node");
}

static unique_ptr<ParsedExpression>
LowerExpression(const GqlExpressionProgram &program,
                const RelationalPropertyMap &property_aliases,
                const vector<RelationalIdentityAccess> &identities,
                GqlTypeId desired_type = GqlTypeId::UNKNOWN) {
  idx_t cursor = 0;
  auto result = LowerExpression(program, cursor, property_aliases, identities,
                                desired_type);
  if (cursor != program.node_types.size()) {
    throw InternalException("GQL expression program has trailing nodes");
  }
  return result;
}

static bool ContainsAggregate(const GqlExpressionProgram &program) {
  for (const auto aggregate : program.aggregate) {
    if (aggregate) {
      return true;
    }
  }
  return false;
}

static void AppendStructField(vector<unique_ptr<ParsedExpression>> &fields,
                              unique_ptr<ParsedExpression> expression,
                              const string &name) {
  expression->SetAlias(name);
  fields.push_back(std::move(expression));
}

static unique_ptr<ParsedExpression>
GraphElementValueAt(const GqlExpressionProgram &program, idx_t node,
                    const GqlTableGraphBinding &graph,
                    const vector<GqlPatternElementType> &binding_types,
                    const vector<RelationalIdentityAccess> &identities) {
  if (node >= program.node_types.size() ||
      static_cast<GqlExpressionType>(program.node_types[node]) !=
          GqlExpressionType::VARIABLE_REFERENCE) {
    throw NotImplementedException(
        "GQL graph values currently require a direct element variable");
  }
  auto binding_index = NumericCast<idx_t>(program.binding_indices[node]);
  if (binding_index >= identities.size() ||
      binding_index >= binding_types.size()) {
    throw InternalException("GQL graph-value binding is missing");
  }
  auto type = static_cast<GqlTypeId>(program.result_types[node]);
  auto expected = type == GqlTypeId::NODE ? GqlPatternElementType::VERTEX
                  : type == GqlTypeId::EDGE ? GqlPatternElementType::EDGE
                                            : throw InternalException(
                                                  "Invalid GQL graph-value type");
  if (binding_types[binding_index] != expected) {
    throw InternalException("GQL graph-value binding type is inconsistent");
  }

  const auto &table = expected == GqlPatternElementType::VERTEX
                          ? graph.vertex
                          : graph.edge;
  const auto &alias = identities[binding_index].table_alias;
  vector<unique_ptr<ParsedExpression>> fields;
  AppendStructField(fields, Column(alias, table.key_column), "__gql_id");
  if (expected == GqlPatternElementType::VERTEX) {
    AppendStructField(fields,
                      table.label_column.empty()
                          ? Constant(Value())
                          : Column(alias, table.label_column),
                      "__gql_labels");
  } else {
    AppendStructField(fields,
                      table.label_column.empty()
                          ? Constant(Value())
                          : Column(alias, table.label_column),
                      "__gql_type");
    AppendStructField(fields, Column(alias, graph.edge_source_column),
                      "__gql_source");
    AppendStructField(fields, Column(alias, graph.edge_target_column),
                      "__gql_target");
  }
  vector<pair<string, string>> properties(table.property_columns.begin(),
                                          table.property_columns.end());
  std::sort(properties.begin(), properties.end(),
            [](const auto &left, const auto &right) {
              return StringUtil::CILessThan(left.first, right.first);
            });
  for (const auto &property : properties) {
    AppendStructField(fields, Column(alias, property.second), property.first);
  }

  auto packed = Function("struct_pack", std::move(fields));
  auto result = make_uniq<CaseExpression>();
  CaseCheck missing;
  missing.when_expr = make_uniq<OperatorExpression>(
      ExpressionType::OPERATOR_IS_NULL, Column(alias, table.key_column));
  missing.then_expr = Constant(Value());
  result->case_checks.push_back(std::move(missing));
  result->else_expr = std::move(packed);
  return std::move(result);
}

static unique_ptr<ParsedExpression>
GraphElementValue(const GqlExpressionProgram &program,
                  const GqlTableGraphBinding &graph,
                  const vector<GqlPatternElementType> &binding_types,
                  const vector<RelationalIdentityAccess> &identities) {
  if (program.node_types.empty()) {
    throw InternalException("GQL graph-value expression is empty");
  }
  return GraphElementValueAt(program, 0, graph, binding_types, identities);
}

static unique_ptr<ParsedExpression>
GraphPathValue(const GqlExpressionProgram &program,
               const GqlTableGraphBinding &graph,
               const vector<GqlPatternElementType> &binding_types,
               const vector<RelationalIdentityAccess> &identities) {
  if (program.node_types.empty() ||
      static_cast<GqlExpressionType>(program.node_types[0]) !=
          GqlExpressionType::FUNCTION ||
      program.values[0] != "__gql_path" ||
      program.child_counts[0] + 1 != program.node_types.size()) {
    throw NotImplementedException(
        "GQL path values currently require a named fixed path");
  }

  vector<unique_ptr<ParsedExpression>> nodes;
  vector<unique_ptr<ParsedExpression>> edges;
  vector<unique_ptr<ParsedExpression>> missing_elements;
  for (idx_t node = 1; node < program.node_types.size(); node++) {
    if (program.child_counts[node] != 0) {
      throw InternalException("GQL fixed path contains a nested expression");
    }
    auto binding_index = NumericCast<idx_t>(program.binding_indices[node]);
    if (binding_index >= binding_types.size() ||
        binding_index >= identities.size()) {
      throw InternalException("GQL fixed path binding is missing");
    }
    const auto &table = binding_types[binding_index] ==
                                GqlPatternElementType::EDGE
                            ? graph.edge
                            : graph.vertex;
    missing_elements.push_back(make_uniq<OperatorExpression>(
        ExpressionType::OPERATOR_IS_NULL,
        Column(identities[binding_index].table_alias, table.key_column)));
    auto value = GraphElementValueAt(program, node, graph, binding_types,
                                     identities);
    if (binding_types[binding_index] == GqlPatternElementType::EDGE) {
      edges.push_back(std::move(value));
    } else {
      nodes.push_back(std::move(value));
    }
  }
  if (nodes.empty()) {
    throw InternalException("GQL fixed path has no nodes");
  }

  vector<unique_ptr<ParsedExpression>> fields;
  AppendStructField(fields, Function("list_value", std::move(nodes)), "nodes");
  auto edge_list = edges.empty()
                       ? Constant(Value::LIST(LogicalType::SQLNULL, {}))
                       : Function("list_value", std::move(edges));
  AppendStructField(fields, std::move(edge_list), "edges");
  auto packed = Function("struct_pack", std::move(fields));

  unique_ptr<ParsedExpression> missing;
  if (missing_elements.size() == 1) {
    missing = std::move(missing_elements[0]);
  } else {
    missing = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_OR,
                                               std::move(missing_elements));
  }
  auto result = make_uniq<CaseExpression>();
  CaseCheck missing_path;
  missing_path.when_expr = std::move(missing);
  missing_path.then_expr = Constant(Value());
  result->case_checks.push_back(std::move(missing_path));
  result->else_expr = std::move(packed);
  return std::move(result);
}

static void
AppendProjections(SelectNode &select,
                  const vector<GqlExpressionProgram> &projections,
                  const vector<string> &projection_names,
                  const RelationalPropertyMap &property_aliases,
                  const vector<RelationalIdentityAccess> &identities,
                  const GqlTableGraphBinding &graph,
                  const vector<GqlPatternElementType> &binding_types) {
  bool has_aggregate = false;
  for (const auto &projection : projections) {
    has_aggregate = has_aggregate || ContainsAggregate(projection);
  }
  GroupingSet grouping_set;
  for (idx_t index = 0; index < projections.size(); index++) {
    auto mutation_value =
        StringUtil::StartsWith(projection_names[index], "gql_mutation_value_");
    auto desired_type =
        mutation_value ? GqlTypeId::PROPERTY_VALUE : GqlTypeId::UNKNOWN;
    auto projection_type =
        static_cast<GqlTypeId>(projections[index].result_types[0]);
    unique_ptr<ParsedExpression> expression;
    if (projection_type == GqlTypeId::NODE ||
        projection_type == GqlTypeId::EDGE) {
      expression = GraphElementValue(projections[index], graph, binding_types,
                                     identities);
    } else if (projection_type == GqlTypeId::PATH) {
      expression = GraphPathValue(projections[index], graph, binding_types,
                                  identities);
    } else {
      expression = LowerExpression(projections[index], property_aliases,
                                   identities, desired_type);
    }
    if (has_aggregate && !ContainsAggregate(projections[index])) {
      grouping_set.insert(select.groups.group_expressions.size());
      select.groups.group_expressions.push_back(expression->Copy());
    }
    expression->SetAlias(projection_names[index]);
    select.select_list.push_back(std::move(expression));
  }
  if (!grouping_set.empty()) {
    select.groups.grouping_sets.push_back(std::move(grouping_set));
  }
}

static void AppendOrderExpression(OrderModifier &order,
                                  const RelationalResultModifiers &modifiers,
                                  idx_t index,
                                  unique_ptr<ParsedExpression> expression) {
  auto null_order = OrderByNullType::ORDER_DEFAULT;
  if (modifiers.order_nulls[index] == 1) {
    null_order = OrderByNullType::NULLS_FIRST;
  } else if (modifiers.order_nulls[index] == 2) {
    null_order = OrderByNullType::NULLS_LAST;
  }
  order.orders.emplace_back(modifiers.order_descending[index]
                                ? OrderType::DESCENDING
                                : OrderType::ASCENDING,
                            null_order, std::move(expression));
}

static void
AppendResultModifiers(SelectNode &select,
                      const vector<GqlExpressionProgram> &projections,
                      const vector<string> &projection_names,
                      const RelationalResultModifiers &modifiers,
                      bool normalized_property_values = true) {
  if (modifiers.distinct) {
    select.modifiers.push_back(make_uniq<DistinctModifier>());
  }
  if (!modifiers.order_indices.empty()) {
    auto order = make_uniq<OrderModifier>();
    for (idx_t index = 0; index < modifiers.order_indices.size(); index++) {
      auto projection_index = modifiers.order_indices[index];
      unique_ptr<ParsedExpression> column =
          make_uniq<ColumnRefExpression>(projection_names[projection_index]);
      auto result_type =
          static_cast<GqlTypeId>(projections[projection_index].result_types[0]);
      if (normalized_property_values &&
          result_type == GqlTypeId::PROPERTY_VALUE) {
        // DuckDB VARIANT is intentionally not directly orderable. ANY graphs do
        // not provide a static property type, so use its scalar rendering as a
        // stable native sort key instead of allowing Top-N to raise an internal
        // error.
        column =
            make_uniq<CastExpression>(LogicalType::VARCHAR, std::move(column));
      }
      AppendOrderExpression(*order, modifiers, index, std::move(column));
    }
    select.modifiers.push_back(std::move(order));
  }
  if (modifiers.has_limit || modifiers.has_offset) {
    auto limit = make_uniq<LimitModifier>();
    if (modifiers.has_limit) {
      limit->limit = Constant(Value::UBIGINT(modifiers.limit));
    }
    if (modifiers.has_offset) {
      limit->offset = Constant(Value::UBIGINT(modifiers.offset));
    }
    select.modifiers.push_back(std::move(limit));
  }
}

static unique_ptr<TableRef>
FinalizeMatchSelect(unique_ptr<SelectNode> select,
                    const vector<GqlExpressionProgram> &projections,
                    const vector<string> &projection_names,
                    const RelationalResultModifiers &modifiers,
                    bool normalized_property_values = true) {
  if (!modifiers.optional) {
    AppendResultModifiers(*select, projections, projection_names, modifiers,
                          normalized_property_values);
    auto statement = make_uniq<SelectStatement>();
    statement->node = std::move(select);
    return make_uniq<SubqueryRef>(std::move(statement));
  }
  auto match_statement = make_uniq<SelectStatement>();
  match_statement->node = std::move(select);
  unique_ptr<TableRef> from;
  {
    auto seed = make_uniq<SelectNode>();
    seed->from_table = make_uniq<EmptyTableRef>();
    seed->select_list.push_back(Aliased(Constant(Value(true)), "present"));
    auto seed_statement = make_uniq<SelectStatement>();
    seed_statement->node = std::move(seed);
    from =
        make_uniq<SubqueryRef>(std::move(seed_statement), "gql_optional_seed");
  }
  vector<unique_ptr<ParsedExpression>> conditions;
  conditions.push_back(Constant(Value(true)));
  AppendJoin(
      from,
      make_uniq<SubqueryRef>(std::move(match_statement), "gql_optional_result"),
      JoinType::LEFT, std::move(conditions));
  auto optional = make_uniq<SelectNode>();
  optional->from_table = std::move(from);
  for (const auto &name : projection_names) {
    optional->select_list.push_back(
        Aliased(Column("gql_optional_result", name), name));
  }
  AppendResultModifiers(*optional, projections, projection_names, modifiers,
                        normalized_property_values);
  auto statement = make_uniq<SelectStatement>();
  statement->node = std::move(optional);
  return make_uniq<SubqueryRef>(std::move(statement));
}

static const string &FindPropertyColumn(const GqlElementTableBinding &table,
                                        const string &property);

static bool TryFindPropertyColumn(const GqlElementTableBinding &table,
                                  const string &property, string &column) {
  for (const auto &entry : table.property_columns) {
    if (StringUtil::CIEquals(entry.first, property)) {
      column = entry.second;
      return true;
    }
  }
  column.clear();
  return false;
}

struct CsrStartRestriction {
  bool constrained = false;
  vector<Value> ids;
};

static bool CsrSourceProperty(const GqlExpressionProgram &program, idx_t node,
                              idx_t source_binding, string &property) {
  if (node + 1 >= program.node_types.size() ||
      static_cast<GqlExpressionType>(program.node_types[node]) !=
          GqlExpressionType::PROPERTY_REFERENCE ||
      static_cast<GqlExpressionType>(program.node_types[node + 1]) !=
          GqlExpressionType::VARIABLE_REFERENCE ||
      program.binding_indices[node] != source_binding ||
      program.binding_indices[node + 1] != source_binding ||
      ExpressionEnd(program, node) != node + 2) {
    return false;
  }
  property = program.properties[node];
  return true;
}

static bool CsrLiteral(const GqlExpressionProgram &program, idx_t node,
                       Value &value) {
  if (node >= program.node_types.size() ||
      static_cast<GqlExpressionType>(program.node_types[node]) !=
          GqlExpressionType::LITERAL ||
      ExpressionEnd(program, node) != node + 1) {
    return false;
  }
  value = LiteralValue(static_cast<GqlLiteralType>(program.operators[node]),
                       program.values[node]);
  return true;
}

static bool ExtractCsrSourceEquality(const GqlExpressionProgram &program,
                                     idx_t source_binding, string &property,
                                     Value &value) {
  if (program.node_types.empty() ||
      static_cast<GqlExpressionType>(program.node_types[0]) !=
          GqlExpressionType::BINARY ||
      static_cast<GqlBinaryOperator>(program.operators[0]) !=
          GqlBinaryOperator::EQUAL ||
      ExpressionEnd(program, 0) != program.node_types.size()) {
    return false;
  }
  const idx_t left = 1;
  const idx_t right = ExpressionEnd(program, left);
  return (CsrSourceProperty(program, left, source_binding, property) &&
          CsrLiteral(program, right, value)) ||
         (CsrLiteral(program, left, value) &&
          CsrSourceProperty(program, right, source_binding, property));
}

static string QuoteCsrIdentifier(const string &value) {
  return KeywordHelper::WriteQuoted(value, '"');
}

static CsrStartRestriction
ResolveCsrStartRestriction(ClientContext &context,
                           const GqlTableGraphBinding &graph,
                           const RecursiveMatchInput &match) {
  CsrStartRestriction restriction;
  string property;
  Value literal;
  for (const auto &predicate : match.predicates) {
    if (!ExtractCsrSourceEquality(predicate, match.source_binding, property,
                                  literal)) {
      continue;
    }
    restriction.constrained = true;
    if (literal.IsNull()) {
      return restriction;
    }
    const auto &column = FindPropertyColumn(graph.vertex, property);
    auto qualified = QuoteCsrIdentifier(graph.vertex.catalog_name) + "." +
                     QuoteCsrIdentifier(graph.vertex.schema_name) + "." +
                     QuoteCsrIdentifier(graph.vertex.table_name);
    auto sql = "SELECT CAST(" + QuoteCsrIdentifier(graph.vertex.key_column) +
               " AS UBIGINT) FROM " + qualified + " WHERE " +
               QuoteCsrIdentifier(column) + " = " + literal.ToSQLString();
    Connection connection(*context.db);
    auto result = connection.Query(sql);
    if (result->HasError()) {
      throw BinderException("Failed to resolve CSR source predicate: %s",
                            result->GetError());
    }
    for (idx_t row = 0; row < result->RowCount(); row++) {
      restriction.ids.push_back(
          Value::UBIGINT(result->GetValue(0, row).GetValue<uint64_t>()));
    }
    return restriction;
  }
  return restriction;
}

static bool ReferencesOnlyBinding(const GqlExpressionProgram &program, idx_t binding_index) {
	for (const auto index : program.binding_indices) {
		if (index != NumericLimits<uint64_t>::Maximum() && index != binding_index) {
			return false;
		}
	}
	return true;
}

static bool ReferencesBinding(const GqlExpressionProgram &program, idx_t binding_index) {
	for (const auto index : program.binding_indices) {
		if (index == binding_index) {
			return true;
		}
	}
	return false;
}

static unique_ptr<TableRef> TableBackedNativeRecursiveMatch(const GqlTableGraphBinding &graph,
                                                            const RecursiveMatchInput &match) {
	if (match.edge_labels.size() > 1) {
		throw NotImplementedException("Table-backed native paths currently support "
		                              "at most one scalar edge label");
	}
	for (const auto &program : match.projections) {
		for (const auto binding_index : program.binding_indices) {
			if (binding_index == match.edge_binding) {
				throw BinderException("Quantified GQL edge group variables are not supported");
			}
		}
	}
	for (const auto &program : match.predicates) {
		for (const auto binding_index : program.binding_indices) {
			if (binding_index == match.edge_binding) {
				throw BinderException("Quantified GQL edge group variables are not supported");
			}
		}
	}

	vector<RelationalIdentityAccess> identities(match.binding_count);
	identities[match.source_binding] = {"gql_object_" + to_string(match.source_binding), graph.vertex.key_column};
	identities[match.target_binding] = {"gql_object_" + to_string(match.target_binding), graph.vertex.key_column};

	RelationalPropertyMap property_aliases;
	for (const auto &program : match.projections) {
		CollectProperties(program, property_aliases);
	}
	for (const auto &program : match.predicates) {
		CollectProperties(program, property_aliases);
	}
	for (auto &entry : property_aliases) {
		auto separator = entry.first.find('\x1f');
		if (separator == string::npos) {
			throw InternalException("Invalid GQL recursive property binding key");
		}
		auto binding_index = NumericCast<idx_t>(std::stoull(entry.first.substr(0, separator)));
		if (binding_index != match.source_binding && binding_index != match.target_binding) {
			throw NotImplementedException("Quantified GQL edge group variables are not supported");
		}
		auto property = entry.first.substr(separator + 1);
		entry.second.table_alias = identities[binding_index].table_alias;
		if (property == GQL_LABEL_ACCESS) {
			entry.second.column_name = graph.vertex.label_column;
		} else {
			TryFindPropertyColumn(graph.vertex, property,
			                      entry.second.column_name);
		}
	}

	// Seed native recursion from the mapped vertex table. Source-only predicates
	// are repeated here as well as above the CTE so a selective source property
	// can prevent database-wide expansion.
	const string anchor_alias = "gql_anchor_vertex";
	auto anchor = make_uniq<SelectNode>();
	anchor->from_table = ElementTable(graph.vertex, anchor_alias);
	vector<unique_ptr<ParsedExpression>> anchor_filters;
	for (const auto &label : match.source_labels) {
		if (graph.vertex.label_column.empty()) {
			anchor_filters.push_back(Constant(Value(false)));
		} else {
			anchor_filters.push_back(VertexHasLabel(anchor_alias, graph.vertex.label_column, label));
		}
	}
	vector<RelationalIdentityAccess> anchor_identities(match.binding_count);
	anchor_identities[match.source_binding] = {anchor_alias, graph.vertex.key_column};
	RelationalPropertyMap anchor_properties;
	for (const auto &entry : property_aliases) {
		auto separator = entry.first.find('\x1f');
		auto binding_index = NumericCast<idx_t>(std::stoull(entry.first.substr(0, separator)));
		if (binding_index != match.source_binding) {
			continue;
		}
		auto access = entry.second;
		access.table_alias = anchor_alias;
		anchor_properties.emplace(entry.first, std::move(access));
	}
	for (const auto &predicate : match.predicates) {
		if (ReferencesOnlyBinding(predicate, match.source_binding)) {
			anchor_filters.push_back(
			    LowerExpression(predicate, anchor_properties, anchor_identities, GqlTypeId::BOOLEAN));
		}
	}
	anchor->where_clause = And(std::move(anchor_filters));
	anchor->select_list.push_back(Aliased(Column(anchor_alias, graph.vertex.key_column), "start_id"));
	anchor->select_list.push_back(Aliased(Column(anchor_alias, graph.vertex.key_column), "end_id"));
	vector<Value> empty_edges;
	anchor->select_list.push_back(
	    Aliased(Constant(Value::LIST(LogicalType::VARCHAR, std::move(empty_edges))), "edge_ids"));
	anchor->select_list.push_back(Aliased(Constant(Value::UBIGINT(0)), "depth"));

	// Extend the current endpoint by one unused mapped edge. Casting the edge
	// key to VARCHAR keeps trail identity generic across scalar key types.
	unique_ptr<TableRef> step_from = NamedTable("gql_recursive_path", "gql_path_previous");
	const string edge_alias = "gql_path_edge";
	vector<unique_ptr<ParsedExpression>> edge_conditions;
	edge_conditions.push_back(
	    Equal(Column(edge_alias, match.reverse ? graph.edge_target_column : graph.edge_source_column),
	          Column("gql_path_previous", "end_id")));
	AppendJoin(step_from, ElementTable(graph.edge, edge_alias), JoinType::INNER, std::move(edge_conditions));

	auto edge_identity = make_uniq<CastExpression>(LogicalType::VARCHAR, Column(edge_alias, graph.edge.key_column));
	vector<unique_ptr<ParsedExpression>> contains_arguments;
	contains_arguments.push_back(Column("gql_path_previous", "edge_ids"));
	contains_arguments.push_back(edge_identity->Copy());
	vector<unique_ptr<ParsedExpression>> step_filters;
	step_filters.push_back(make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT,
	                                                     Function("list_contains", std::move(contains_arguments))));
	for (const auto &label : match.edge_labels) {
		if (graph.edge.label_column.empty()) {
			step_filters.push_back(Constant(Value(false)));
		} else {
			step_filters.push_back(Equal(Column(edge_alias, graph.edge.label_column), Constant(Value(label))));
		}
	}

	auto step = make_uniq<SelectNode>();
	step->from_table = std::move(step_from);
	step->where_clause = And(std::move(step_filters));
	step->select_list.push_back(Aliased(Column("gql_path_previous", "start_id"), "start_id"));
	step->select_list.push_back(
	    Aliased(Column(edge_alias, match.reverse ? graph.edge_source_column : graph.edge_target_column), "end_id"));
	vector<unique_ptr<ParsedExpression>> append_arguments;
	append_arguments.push_back(Column("gql_path_previous", "edge_ids"));
	append_arguments.push_back(std::move(edge_identity));
	step->select_list.push_back(Aliased(Function("list_append", std::move(append_arguments)), "edge_ids"));
	vector<unique_ptr<ParsedExpression>> depth_arguments;
	depth_arguments.push_back(Column("gql_path_previous", "depth"));
	depth_arguments.push_back(Constant(Value::UBIGINT(1)));
	auto next_depth = Function("+", std::move(depth_arguments));
	next_depth->Cast<FunctionExpression>().is_operator = true;
	step->select_list.push_back(Aliased(std::move(next_depth), "depth"));

	auto recursive = make_uniq<RecursiveCTENode>();
	recursive->ctename = "gql_recursive_path";
	recursive->union_all = true;
	recursive->aliases = {"start_id", "end_id", "edge_ids", "depth"};
	recursive->left = std::move(anchor);
	recursive->right = std::move(step);
	auto recursive_statement = make_uniq<SelectStatement>();
	recursive_statement->node = std::move(recursive);
	unique_ptr<TableRef> from = make_uniq<SubqueryRef>(std::move(recursive_statement), "gql_path");

	bool source_needed_above = match.source_binding == match.target_binding;
	for (const auto &projection : match.projections) {
		source_needed_above |= ReferencesBinding(projection, match.source_binding);
	}
	for (const auto &predicate : match.predicates) {
		source_needed_above |= ReferencesBinding(predicate, match.source_binding) &&
		                       !ReferencesOnlyBinding(predicate, match.source_binding);
	}
	if (source_needed_above) {
		vector<unique_ptr<ParsedExpression>> source_conditions;
		source_conditions.push_back(Equal(Column(identities[match.source_binding].table_alias, graph.vertex.key_column),
		                                  Column("gql_path", "start_id")));
		AppendJoin(from, ElementTable(graph.vertex, identities[match.source_binding].table_alias), JoinType::INNER,
		           std::move(source_conditions));
	}
	if (match.target_binding != match.source_binding) {
		vector<unique_ptr<ParsedExpression>> target_conditions;
		target_conditions.push_back(Equal(Column(identities[match.target_binding].table_alias, graph.vertex.key_column),
		                                  Column("gql_path", "end_id")));
		AppendJoin(from, ElementTable(graph.vertex, identities[match.target_binding].table_alias), JoinType::INNER,
		           std::move(target_conditions));
	}

	vector<unique_ptr<ParsedExpression>> filters;
	if (match.minimum_repetitions > 0) {
		filters.push_back(Compare(ExpressionType::COMPARE_GREATERTHANOREQUALTO, Column("gql_path", "depth"),
		                          Constant(Value::UBIGINT(match.minimum_repetitions))));
	}
	auto append_labels = [&](const vector<string> &labels, idx_t binding_index) {
		for (const auto &label : labels) {
			if (graph.vertex.label_column.empty()) {
				filters.push_back(Constant(Value(false)));
			} else {
				filters.push_back(VertexHasLabel(identities[binding_index].table_alias, graph.vertex.label_column, label));
			}
		}
	};
	append_labels(match.target_labels, match.target_binding);
	if (match.source_binding == match.target_binding) {
		filters.push_back(Equal(Column("gql_path", "start_id"), Column("gql_path", "end_id")));
	}
	for (const auto &predicate : match.predicates) {
		if (!ReferencesOnlyBinding(predicate, match.source_binding)) {
			filters.push_back(LowerExpression(predicate, property_aliases, identities, GqlTypeId::BOOLEAN));
		}
	}

	auto select = make_uniq<SelectNode>();
	select->from_table = std::move(from);
	select->where_clause = And(std::move(filters));
	vector<GqlPatternElementType> binding_types(match.binding_count,
	                                           GqlPatternElementType::VERTEX);
	binding_types[match.edge_binding] = GqlPatternElementType::EDGE;
	AppendProjections(*select, match.projections, match.projection_names,
	                  property_aliases, identities, graph, binding_types);
	return FinalizeMatchSelect(std::move(select), match.projections, match.projection_names, match.modifiers, false);
}

static unique_ptr<TableRef> CsrPathTable(const string &graph_name, const RecursiveMatchInput &match,
                                         CsrStartRestriction restriction) {
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(Constant(Value(graph_name)));
	arguments.push_back(Constant(Value(match.reverse)));
	arguments.push_back(Constant(Value::UBIGINT(match.minimum_repetitions)));
	arguments.push_back(Constant(Value(match.edge_labels.empty() ? string() : match.edge_labels[0])));
	arguments.push_back(Constant(restriction.constrained ? Value::LIST(LogicalType::UBIGINT, std::move(restriction.ids))
	                                                     : Value(LogicalType::LIST(LogicalType::UBIGINT))));
	auto result = make_uniq<TableFunctionRef>();
	result->function = make_uniq<FunctionExpression>("gql_csr_path_scan", std::move(arguments));
	result->alias = "gql_path";
	return std::move(result);
}

static unique_ptr<TableRef> TableBackedCsrRecursiveMatch(ClientContext &context, const GqlTableGraphBinding &graph,
                                                         const string &graph_name, const RecursiveMatchInput &match) {
	if (match.edge_labels.size() > 1) {
		throw NotImplementedException("Table-backed CSR paths currently support at "
		                              "most one scalar edge label");
	}
	auto start_restriction = ResolveCsrStartRestriction(context, graph, match);
	unique_ptr<TableRef> from = CsrPathTable(graph_name, match, std::move(start_restriction));
	vector<RelationalIdentityAccess> identities(match.binding_count);
	identities[match.source_binding] = {"gql_object_" + to_string(match.source_binding), graph.vertex.key_column};
	identities[match.target_binding] = {"gql_object_" + to_string(match.target_binding), graph.vertex.key_column};

	vector<unique_ptr<ParsedExpression>> source_conditions;
	source_conditions.push_back(Equal(Column(identities[match.source_binding].table_alias, graph.vertex.key_column),
	                                  Column("gql_path", "start_id")));
	AppendJoin(from, ElementTable(graph.vertex, identities[match.source_binding].table_alias), JoinType::INNER,
	           std::move(source_conditions));
	if (match.target_binding != match.source_binding) {
		vector<unique_ptr<ParsedExpression>> target_conditions;
		target_conditions.push_back(Equal(Column(identities[match.target_binding].table_alias, graph.vertex.key_column),
		                                  Column("gql_path", "end_id")));
		AppendJoin(from, ElementTable(graph.vertex, identities[match.target_binding].table_alias), JoinType::INNER,
		           std::move(target_conditions));
	}

	RelationalPropertyMap property_aliases;
	for (const auto &program : match.projections) {
		CollectProperties(program, property_aliases);
	}
	for (const auto &program : match.predicates) {
		CollectProperties(program, property_aliases);
	}
	for (auto &entry : property_aliases) {
		auto separator = entry.first.find('\x1f');
		if (separator == string::npos) {
			throw InternalException("Invalid GQL CSR property binding key");
		}
		auto binding_index = NumericCast<idx_t>(std::stoull(entry.first.substr(0, separator)));
		if (binding_index != match.source_binding && binding_index != match.target_binding) {
			throw NotImplementedException("Quantified GQL edge group variables are not supported");
		}
		auto property = entry.first.substr(separator + 1);
		entry.second.table_alias = identities[binding_index].table_alias;
		if (property == GQL_LABEL_ACCESS) {
			entry.second.column_name = graph.vertex.label_column;
		} else {
			TryFindPropertyColumn(graph.vertex, property,
			                      entry.second.column_name);
		}
	}

	vector<unique_ptr<ParsedExpression>> filters;
	auto append_labels = [&](const vector<string> &labels, idx_t binding_index) {
		for (const auto &label : labels) {
			if (graph.vertex.label_column.empty()) {
				filters.push_back(Constant(Value(false)));
			} else {
				filters.push_back(VertexHasLabel(identities[binding_index].table_alias, graph.vertex.label_column, label));
			}
		}
	};
	append_labels(match.source_labels, match.source_binding);
	append_labels(match.target_labels, match.target_binding);
	if (match.source_binding == match.target_binding) {
		filters.push_back(Equal(Column("gql_path", "start_id"), Column("gql_path", "end_id")));
	}
	for (const auto &predicate : match.predicates) {
		filters.push_back(LowerExpression(predicate, property_aliases, identities, GqlTypeId::BOOLEAN));
	}

	auto select = make_uniq<SelectNode>();
	select->from_table = std::move(from);
	select->where_clause = And(std::move(filters));
	vector<GqlPatternElementType> binding_types(match.binding_count,
	                                           GqlPatternElementType::VERTEX);
	binding_types[match.edge_binding] = GqlPatternElementType::EDGE;
	AppendProjections(*select, match.projections, match.projection_names,
	                  property_aliases, identities, graph, binding_types);
	return FinalizeMatchSelect(std::move(select), match.projections, match.projection_names, match.modifiers, false);
}

static unique_ptr<TableRef>
RecursiveMatchBindReplace(ClientContext &context,
                          TableFunctionBindInput &input) {
  auto graph_name = GqlGetSelectedGraph(context);
  if (graph_name.empty()) {
    throw InvalidInputException(
        "No graph selected; use SESSION SET GRAPH before MATCH");
  }
  auto match = ReadRecursiveMatchInput(input);
  GqlTableGraphBinding table_graph;
  if (!GqlTryLoadTableGraph(context, graph_name, table_graph)) {
    throw InvalidInputException(
        "Graph '%s' has no native tables; load it with COPY GRAPH before MATCH",
        graph_name);
  }
  if (match.use_csr) {
    return TableBackedCsrRecursiveMatch(context, table_graph, graph_name,
                                        match);
  }
  return TableBackedNativeRecursiveMatch(table_graph, match);
}

static const string &FindPropertyColumn(const GqlElementTableBinding &table,
                                        const string &property) {
  for (const auto &entry : table.property_columns) {
    if (StringUtil::CIEquals(entry.first, property)) {
      return entry.second;
    }
  }
  throw BinderException(
      "Property '%s' is not mapped for table-backed graph table %s.%s",
      property, table.schema_name, table.table_name);
}

static unique_ptr<TableRef>
TableBackedMatch(const GqlTableGraphBinding &graph,
                 const RelationalMatchInput &match) {
  vector<RelationalIdentityAccess> identities(match.binding_types.size());
  vector<bool> mandatory_bindings(match.binding_types.size(), false);
  for (const auto &pattern : match.patterns) {
    if (pattern.optional_stage > 0 && !match.modifiers.optional) {
      continue;
    }
    for (const auto &element : pattern.elements) {
      mandatory_bindings[element.binding_index] = true;
    }
  }
  unique_ptr<TableRef> from;
  for (idx_t index = 0; index < match.binding_types.size(); index++) {
    const auto &table =
        match.binding_types[index] == GqlPatternElementType::EDGE
            ? graph.edge
            : graph.vertex;
    auto alias = "gql_object_" + to_string(index);
    identities[index] = {alias, table.key_column};
    if (!mandatory_bindings[index]) {
      continue;
    }
    auto source = ElementTable(table, alias);
    if (!from) {
      from = std::move(source);
    } else {
      vector<unique_ptr<ParsedExpression>> conditions;
      conditions.push_back(Constant(Value(true)));
      AppendJoin(from, std::move(source), JoinType::INNER,
                 std::move(conditions));
    }
  }
  if (!from) {
    throw InternalException("Table-backed MATCH has no bindings");
  }

  RelationalPropertyMap property_aliases;
  for (const auto &program : match.projections) {
    CollectProperties(program, property_aliases);
  }
  for (const auto &program : match.predicates) {
    CollectProperties(program, property_aliases);
  }
  for (const auto &program : match.optional_predicates) {
    CollectProperties(program, property_aliases);
  }
  for (auto &entry : property_aliases) {
    auto separator = entry.first.find('\x1f');
    if (separator == string::npos) {
      throw InternalException("Invalid GQL property binding key");
    }
    auto binding_index =
        NumericCast<idx_t>(std::stoull(entry.first.substr(0, separator)));
    if (binding_index >= match.binding_types.size()) {
      throw InternalException("Invalid GQL property binding index");
    }
    auto property = entry.first.substr(separator + 1);
    const auto &table =
        match.binding_types[binding_index] == GqlPatternElementType::EDGE
            ? graph.edge
            : graph.vertex;
    entry.second.table_alias = identities[binding_index].table_alias;
    if (property == GQL_LABEL_ACCESS) {
      entry.second.column_name = table.label_column;
    } else {
      TryFindPropertyColumn(table, property, entry.second.column_name);
    }
  }

  vector<bool> available_bindings = mandatory_bindings;
  idx_t maximum_optional_stage = 0;
  for (const auto &pattern : match.patterns) {
    maximum_optional_stage =
        MaxValue(maximum_optional_stage, pattern.optional_stage);
  }
  for (const auto stage : match.optional_predicate_stages) {
    if (stage == 0 || stage > maximum_optional_stage) {
      throw BinderException("Invalid GQL OPTIONAL MATCH predicate stage");
    }
  }
  for (idx_t stage = 1; stage <= maximum_optional_stage; stage++) {
    vector<bool> stage_bindings(match.binding_types.size(), false);
    bool found_stage = false;
    for (const auto &pattern : match.patterns) {
      if (pattern.optional_stage != stage) {
        continue;
      }
      found_stage = true;
      for (const auto &element : pattern.elements) {
        if (!available_bindings[element.binding_index]) {
          stage_bindings[element.binding_index] = true;
        }
      }
    }
    if (!found_stage) {
      throw BinderException("Invalid non-contiguous GQL OPTIONAL MATCH stages");
    }

    unique_ptr<TableRef> optional_source;
    for (idx_t binding_index = 0; binding_index < stage_bindings.size();
         binding_index++) {
      if (!stage_bindings[binding_index]) {
        continue;
      }
      const auto &table =
          match.binding_types[binding_index] == GqlPatternElementType::EDGE
              ? graph.edge
              : graph.vertex;
      auto source =
          ElementTable(table, identities[binding_index].table_alias);
      if (!optional_source) {
        optional_source = std::move(source);
      } else {
        vector<unique_ptr<ParsedExpression>> cross_conditions;
        cross_conditions.push_back(Constant(Value(true)));
        AppendJoin(optional_source, std::move(source), JoinType::INNER,
                   std::move(cross_conditions));
      }
    }
    if (!optional_source) {
      throw NotImplementedException(
          "OPTIONAL MATCH stages that introduce no new bindings");
    }

    vector<unique_ptr<ParsedExpression>> conditions;
    vector<idx_t> stage_edges;
    for (const auto &pattern : match.patterns) {
      if (pattern.optional_stage != stage) {
        continue;
      }
      for (const auto &element : pattern.elements) {
        auto alias = identities[element.binding_index].table_alias;
        if (element.type == GqlPatternElementType::VERTEX) {
          for (const auto &label : StringUtil::Split(element.label, ';')) {
            if (!label.empty()) {
              conditions.push_back(VertexHasLabel(
                  alias, graph.vertex.label_column, label));
            }
          }
        } else {
          if (!element.label.empty()) {
            conditions.push_back(
                Equal(Column(alias, graph.edge.label_column),
                      Constant(Value(element.label))));
          }
          if (stage_bindings[element.binding_index] &&
              std::find(stage_edges.begin(), stage_edges.end(),
                        element.binding_index) == stage_edges.end()) {
            stage_edges.push_back(element.binding_index);
          }
        }
      }
      for (idx_t element_index = 1; element_index < pattern.elements.size();
           element_index += 2) {
        const auto &left = pattern.elements[element_index - 1];
        const auto &edge = pattern.elements[element_index];
        const auto &right = pattern.elements[element_index + 1];
        auto edge_alias = identities[edge.binding_index].table_alias;
        conditions.push_back(Equal(
            Column(edge_alias, edge.reverse ? graph.edge_target_column
                                            : graph.edge_source_column),
            Column(identities[left.binding_index].table_alias,
                   graph.vertex.key_column)));
        conditions.push_back(Equal(
            Column(edge_alias, edge.reverse ? graph.edge_source_column
                                            : graph.edge_target_column),
            Column(identities[right.binding_index].table_alias,
                   graph.vertex.key_column)));
      }
    }
    for (idx_t left = 0; left < stage_edges.size(); left++) {
      for (idx_t prior = 0; prior < match.binding_types.size(); prior++) {
        if (available_bindings[prior] &&
            match.binding_types[prior] == GqlPatternElementType::EDGE &&
            prior != stage_edges[left]) {
          conditions.push_back(NotEqual(
              Column(identities[stage_edges[left]].table_alias,
                     graph.edge.key_column),
              Column(identities[prior].table_alias, graph.edge.key_column)));
        }
      }
      for (idx_t right = left + 1; right < stage_edges.size(); right++) {
        conditions.push_back(NotEqual(
            Column(identities[stage_edges[left]].table_alias,
                   graph.edge.key_column),
            Column(identities[stage_edges[right]].table_alias,
                   graph.edge.key_column)));
      }
    }
    for (idx_t predicate_index = 0;
         predicate_index < match.optional_predicates.size();
         predicate_index++) {
      if (match.optional_predicate_stages[predicate_index] == stage) {
        conditions.push_back(LowerExpression(
            match.optional_predicates[predicate_index], property_aliases,
            identities, GqlTypeId::BOOLEAN));
      }
    }
    if (conditions.empty()) {
      conditions.push_back(Constant(Value(true)));
    }
    AppendJoin(from, std::move(optional_source), JoinType::LEFT,
               std::move(conditions));
    for (idx_t binding_index = 0; binding_index < stage_bindings.size();
         binding_index++) {
      available_bindings[binding_index] =
          available_bindings[binding_index] || stage_bindings[binding_index];
    }
  }

  vector<unique_ptr<ParsedExpression>> filters;
  for (const auto &pattern : match.patterns) {
    if (pattern.optional_stage > 0 && !match.modifiers.optional) {
      continue;
    }
    for (const auto &element : pattern.elements) {
      if (element.label.empty()) {
        continue;
      }
      const auto &table = element.type == GqlPatternElementType::EDGE
                              ? graph.edge
                              : graph.vertex;
      if (table.label_column.empty()) {
        filters.push_back(Constant(Value(false)));
      } else {
        if (element.type == GqlPatternElementType::VERTEX) {
          for (const auto &label : StringUtil::Split(element.label, ';')) {
            filters.push_back(VertexHasLabel(
                identities[element.binding_index].table_alias,
                table.label_column, label));
          }
        } else {
          filters.push_back(
              Equal(Column(identities[element.binding_index].table_alias,
                           table.label_column),
                    Constant(Value(element.label))));
        }
      }
    }
  }

  vector<idx_t> edge_bindings;
  for (idx_t binding_index = 0; binding_index < match.binding_types.size();
       binding_index++) {
    if (mandatory_bindings[binding_index] &&
        match.binding_types[binding_index] == GqlPatternElementType::EDGE) {
      edge_bindings.push_back(binding_index);
    }
  }
  for (idx_t left = 0; left < edge_bindings.size(); left++) {
    for (idx_t right = left + 1; right < edge_bindings.size(); right++) {
      filters.push_back(
          NotEqual(Column(identities[edge_bindings[left]].table_alias,
                          graph.edge.key_column),
                   Column(identities[edge_bindings[right]].table_alias,
                          graph.edge.key_column)));
    }
  }
  for (const auto &pattern : match.patterns) {
    if (pattern.optional_stage > 0 && !match.modifiers.optional) {
      continue;
    }
    for (idx_t element_index = 1; element_index < pattern.elements.size();
         element_index += 2) {
      const auto &left = pattern.elements[element_index - 1];
      const auto &edge = pattern.elements[element_index];
      const auto &right = pattern.elements[element_index + 1];
      auto edge_alias = identities[edge.binding_index].table_alias;
      filters.push_back(
          Equal(Column(edge_alias, edge.reverse ? graph.edge_target_column
                                                : graph.edge_source_column),
                Column(identities[left.binding_index].table_alias,
                       graph.vertex.key_column)));
      filters.push_back(
          Equal(Column(edge_alias, edge.reverse ? graph.edge_source_column
                                                : graph.edge_target_column),
                Column(identities[right.binding_index].table_alias,
                       graph.vertex.key_column)));
    }
  }
  for (const auto &predicate : match.predicates) {
    filters.push_back(LowerExpression(predicate, property_aliases, identities,
                                      GqlTypeId::BOOLEAN));
  }

  auto select = make_uniq<SelectNode>();
  select->from_table = std::move(from);
  select->where_clause = And(std::move(filters));
  AppendProjections(*select, match.projections, match.projection_names,
                    property_aliases, identities, graph, match.binding_types);
  return FinalizeMatchSelect(std::move(select), match.projections,
                             match.projection_names, match.modifiers, false);
}

static unique_ptr<TableRef>
RelationalMatchBindReplace(ClientContext &context,
                           TableFunctionBindInput &input) {
  auto graph_name = GqlGetSelectedGraph(context);
  if (graph_name.empty()) {
    throw InvalidInputException(
        "No graph selected; use SESSION SET GRAPH before MATCH");
  }
  auto match = ReadMatchInput(input);
  GqlTableGraphBinding table_graph;
  if (!GqlTryLoadTableGraph(context, graph_name, table_graph)) {
    throw InvalidInputException(
        "Graph '%s' has no native tables; load it with COPY GRAPH before MATCH",
        graph_name);
  }
  return TableBackedMatch(table_graph, match);
}

TableFunction GqlRelationalMatchFunction() {
  TableFunction function(
      "gql_match_relational",
      {LogicalType::LIST(LogicalType::UBIGINT),
       LogicalType::LIST(LogicalType::UTINYINT),
       LogicalType::LIST(LogicalType::UBIGINT),
       LogicalType::LIST(LogicalType::VARCHAR),
       LogicalType::LIST(LogicalType::BOOLEAN),
       LogicalType::LIST(LogicalType::BOOLEAN),
       LogicalType::LIST(LogicalType::UBIGINT),
       LogicalType::LIST(GqlExpressionProgramType()),
       LogicalType::LIST(LogicalType::VARCHAR),
       LogicalType::LIST(GqlExpressionProgramType()),
       LogicalType::LIST(GqlExpressionProgramType()),
       LogicalType::LIST(LogicalType::UBIGINT), LogicalType::BOOLEAN,
       LogicalType::BOOLEAN, LogicalType::LIST(LogicalType::UBIGINT),
       LogicalType::LIST(LogicalType::BOOLEAN),
       LogicalType::LIST(LogicalType::UTINYINT), LogicalType::BOOLEAN,
       LogicalType::UBIGINT, LogicalType::BOOLEAN, LogicalType::UBIGINT},
      nullptr, nullptr);
  function.bind_replace = RelationalMatchBindReplace;
  return function;
}

TableFunction GqlRecursiveMatchFunction() {
  TableFunction function("gql_match_recursive",
                         {LogicalType::UBIGINT,
                          LogicalType::UBIGINT,
                          LogicalType::UBIGINT,
                          LogicalType::UBIGINT,
                          LogicalType::LIST(LogicalType::VARCHAR),
                          LogicalType::LIST(LogicalType::VARCHAR),
                          LogicalType::LIST(LogicalType::VARCHAR),
                          LogicalType::BOOLEAN,
                          LogicalType::UBIGINT,
                          LogicalType::BOOLEAN,
                          LogicalType::LIST(GqlExpressionProgramType()),
                          LogicalType::LIST(LogicalType::VARCHAR),
                          LogicalType::LIST(GqlExpressionProgramType()),
                          LogicalType::BOOLEAN,
                          LogicalType::BOOLEAN,
                          LogicalType::LIST(LogicalType::UBIGINT),
                          LogicalType::LIST(LogicalType::BOOLEAN),
                          LogicalType::LIST(LogicalType::UTINYINT),
                          LogicalType::BOOLEAN,
                          LogicalType::UBIGINT,
                          LogicalType::BOOLEAN,
                          LogicalType::UBIGINT},
                         nullptr, nullptr);
  function.bind_replace = RecursiveMatchBindReplace;
  return function;
}

} // namespace duckdb
