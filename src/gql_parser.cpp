#include "gql_parser.hpp"

#include "GQLLexer.h"
#include "GQLParser.h"
#include "antlr4-runtime.h"

#ifdef INVALID_INDEX
#undef INVALID_INDEX
#endif

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "gql_binder.hpp"
#include "gql_csr.hpp"
#include "gql_import.hpp"
#include "gql_lowerer.hpp"
#include "gql_mutation.hpp"
#include "gql_storage.hpp"
#include "gql_transformer.hpp"

#include <cctype>

namespace duckdb {

using antlr4::BaseErrorListener;
using antlr4::Recognizer;
using antlr4::Token;

static bool StartsWithMergePattern(const string &query) {
  idx_t offset = 0;
  while (offset < query.size() &&
         std::isspace(static_cast<unsigned char>(query[offset]))) {
    offset++;
  }
  if (offset + 5 > query.size() ||
      !StringUtil::CIEquals(query.substr(offset, 5), "MERGE")) {
    return false;
  }
  offset += 5;
  if (offset < query.size() &&
      (std::isalnum(static_cast<unsigned char>(query[offset])) ||
       query[offset] == '_')) {
    return false;
  }
  while (offset < query.size() &&
         std::isspace(static_cast<unsigned char>(query[offset]))) {
    offset++;
  }
  return offset < query.size() && query[offset] == '(';
}

unique_ptr<ParserExtensionParseData> GqlParseData::Copy() const {
  return make_uniq_base<ParserExtensionParseData, GqlParseData>(*this);
}

string GqlParseData::ToString() const { return query; }

class GqlErrorListener final : public BaseErrorListener {
public:
  void syntaxError(Recognizer *recognizer, Token *offending_symbol, size_t line,
                   size_t character_in_line, const string &message,
                   std::exception_ptr exception) override {
    if (!error.empty()) {
      return;
    }
    error = "GQL parser error at line " + to_string(line) + ", column " +
            to_string(character_in_line) + ": " + message;
    location = character_in_line;
  }

  string error;
  idx_t location = 0;
};

static bool StartsWithGqlCommand(const string &query) {
  auto normalized = query;
  StringUtil::Trim(normalized);
  normalized = StringUtil::Upper(normalized);
  return StringUtil::StartsWith(normalized, "CREATE GRAPH") ||
         StringUtil::StartsWith(normalized, "CREATE PROPERTY GRAPH") ||
         StringUtil::StartsWith(normalized, "DROP GRAPH") ||
         StringUtil::StartsWith(normalized, "DROP PROPERTY GRAPH") ||
         StringUtil::StartsWith(normalized, "SESSION SET GRAPH") ||
         StringUtil::StartsWith(normalized, "SESSION SET PROPERTY GRAPH") ||
         StringUtil::StartsWith(normalized, "COPY GRAPH") ||
         StartsWithMergePattern(query) ||
         StringUtil::StartsWith(normalized, "MATCH") ||
         StringUtil::StartsWith(normalized, "OPTIONAL MATCH") ||
         StringUtil::StartsWith(normalized, "INSERT (");
}

static string StripTerminator(const string &query) {
  auto trimmed = query;
  StringUtil::Trim(trimmed);
  if (!trimmed.empty() && trimmed.back() == ';') {
    trimmed.pop_back();
    StringUtil::Trim(trimmed);
  }
  return trimmed;
}

class CopyGraphParser {
public:
  explicit CopyGraphParser(const string &query_p) : query(query_p) {}

  shared_ptr<GqlStatement> Parse() {
    ExpectKeyword("COPY");
    ExpectKeyword("GRAPH");
    auto graph_name = ParseIdentifier();
    ExpectKeyword("FROM");
    ExpectCharacter('(');
    ExpectKeyword("VERTICES");
    auto vertex_path = ParseString();
    ExpectCharacter(',');
    ExpectKeyword("EDGES");
    auto edge_path = ParseString();
    ExpectCharacter(')');
    ExpectKeyword("FORMAT");
    ExpectKeyword("NEO4J");
    bool validate = true;
    SkipWhitespace();
    if (!AtEnd()) {
      ExpectKeyword("OPTIONS");
      ExpectCharacter('(');
      ExpectKeyword("VALIDATE");
      validate = ParseBoolean();
      ExpectCharacter(')');
      SkipWhitespace();
    }
    if (!AtEnd()) {
      Error("unexpected trailing input");
    }
    GqlSourceRange source;
    source.end_offset = query.size();
    source.start_line = 1;
    source.end_line = 1;
    source.end_column = query.size();
    return make_shared_ptr<GqlCopyGraphStatement>(
        source, std::move(graph_name), std::move(vertex_path),
        std::move(edge_path), validate);
  }

private:
  void SkipWhitespace() {
    while (offset < query.size() &&
           std::isspace(static_cast<unsigned char>(query[offset]))) {
      offset++;
    }
  }

  bool AtEnd() {
    SkipWhitespace();
    return offset == query.size();
  }

  [[noreturn]] void Error(const string &message) const {
    throw ParserException("COPY GRAPH parser error at byte %llu: %s",
                          static_cast<unsigned long long>(offset), message);
  }

  bool ConsumeKeyword(const string &keyword) {
    SkipWhitespace();
    if (offset + keyword.size() > query.size()) {
      return false;
    }
    auto candidate = query.substr(offset, keyword.size());
    if (!StringUtil::CIEquals(candidate, keyword)) {
      return false;
    }
    auto end = offset + keyword.size();
    if (end < query.size() &&
        (std::isalnum(static_cast<unsigned char>(query[end])) ||
         query[end] == '_')) {
      return false;
    }
    offset = end;
    return true;
  }

  void ExpectKeyword(const string &keyword) {
    if (!ConsumeKeyword(keyword)) {
      Error("expected " + keyword);
    }
  }

  void ExpectCharacter(char character) {
    SkipWhitespace();
    if (offset >= query.size() || query[offset] != character) {
      Error("expected '" + string(1, character) + "'");
    }
    offset++;
  }

  GqlIdentifier ParseIdentifier() {
    SkipWhitespace();
    auto start = offset;
    if (offset >= query.size() ||
        !(std::isalpha(static_cast<unsigned char>(query[offset])) ||
          query[offset] == '_')) {
      Error("expected a regular graph name");
    }
    offset++;
    while (offset < query.size() &&
           (std::isalnum(static_cast<unsigned char>(query[offset])) ||
            query[offset] == '_')) {
      offset++;
    }
    GqlIdentifier result;
    result.value = StringUtil::Lower(query.substr(start, offset - start));
    result.source.start_offset = start;
    result.source.end_offset = offset;
    result.source.start_line = 1;
    result.source.end_line = 1;
    result.source.start_column = start;
    result.source.end_column = offset;
    return result;
  }

  string ParseString() {
    SkipWhitespace();
    if (offset >= query.size() || query[offset] != '\'') {
      Error("expected a single-quoted file path");
    }
    offset++;
    string result;
    while (offset < query.size()) {
      auto character = query[offset++];
      if (character != '\'') {
        result += character;
        continue;
      }
      if (offset < query.size() && query[offset] == '\'') {
        result += '\'';
        offset++;
        continue;
      }
      if (result.empty()) {
        Error("file paths cannot be empty");
      }
      return result;
    }
    Error("unterminated file path");
  }

  bool ParseBoolean() {
    if (ConsumeKeyword("TRUE")) {
      return true;
    }
    if (ConsumeKeyword("FALSE")) {
      return false;
    }
    Error("expected TRUE or FALSE");
  }

  const string &query;
  idx_t offset = 0;
};

class MergeParser {
public:
  explicit MergeParser(const string &query_p) : query(query_p) {}

  shared_ptr<GqlStatement> Parse() {
    ExpectKeyword("MERGE");
    ExpectCharacter('(');
    auto result = make_shared_ptr<GqlMergeStatement>(Range(0, query.size()));
    SkipWhitespace();
    if (PeekIdentifier()) {
      result->vertex.variable = ParseIdentifier();
    }
    SkipWhitespace();
    if (ConsumeCharacter(':')) {
      result->vertex.labels.push_back(ParseIdentifier());
      while (ConsumeCharacter('&')) {
        result->vertex.labels.push_back(ParseIdentifier());
      }
    }
    SkipWhitespace();
    if (ConsumeCharacter('{')) {
      ParseProperties(result->vertex.properties);
    }
    ExpectCharacter(')');
    SkipWhitespace();
    if (!AtEnd()) {
      Error("only a single vertex pattern is supported; ON CREATE, ON MATCH, "
            "paths, and trailing clauses are not yet supported");
    }
    if (result->vertex.labels.empty() && result->vertex.properties.empty()) {
      Error("MERGE requires a label or at least one property");
    }
    result->vertex.source = Range(pattern_start, offset);
    return result;
  }

private:
  void SkipWhitespace() {
    while (offset < query.size() &&
           std::isspace(static_cast<unsigned char>(query[offset]))) {
      offset++;
    }
  }

  bool AtEnd() {
    SkipWhitespace();
    return offset == query.size();
  }

  [[noreturn]] void Error(const string &message) const {
    throw ParserException("MERGE parser error at byte %llu: %s",
                          static_cast<unsigned long long>(offset), message);
  }

  bool ConsumeKeyword(const string &keyword) {
    SkipWhitespace();
    if (offset + keyword.size() > query.size() ||
        !StringUtil::CIEquals(query.substr(offset, keyword.size()), keyword)) {
      return false;
    }
    auto end = offset + keyword.size();
    if (end < query.size() &&
        (std::isalnum(static_cast<unsigned char>(query[end])) ||
         query[end] == '_')) {
      return false;
    }
    offset = end;
    return true;
  }

  void ExpectKeyword(const string &keyword) {
    if (!ConsumeKeyword(keyword)) {
      Error("expected " + keyword);
    }
  }

  bool ConsumeCharacter(char character) {
    SkipWhitespace();
    if (offset >= query.size() || query[offset] != character) {
      return false;
    }
    if (character == '(') {
      pattern_start = offset;
    }
    offset++;
    return true;
  }

  void ExpectCharacter(char character) {
    if (!ConsumeCharacter(character)) {
      Error("expected '" + string(1, character) + "'");
    }
  }

  bool PeekIdentifier() {
    SkipWhitespace();
    return offset < query.size() &&
           (std::isalpha(static_cast<unsigned char>(query[offset])) ||
            query[offset] == '_');
  }

  GqlIdentifier ParseIdentifier() {
    SkipWhitespace();
    auto start = offset;
    if (!PeekIdentifier()) {
      Error("expected a regular identifier");
    }
    offset++;
    while (offset < query.size() &&
           (std::isalnum(static_cast<unsigned char>(query[offset])) ||
            query[offset] == '_')) {
      offset++;
    }
    GqlIdentifier result;
    result.value = StringUtil::Lower(query.substr(start, offset - start));
    result.source = Range(start, offset);
    return result;
  }

  GqlLiteral ParseLiteral() {
    SkipWhitespace();
    auto start = offset;
    GqlLiteral result;
    if (offset < query.size() && query[offset] == '\'') {
      offset++;
      string value;
      while (offset < query.size()) {
        auto character = query[offset++];
        if (character != '\'') {
          value += character;
          continue;
        }
        if (offset < query.size() && query[offset] == '\'') {
          value += '\'';
          offset++;
          continue;
        }
        result.type = GqlLiteralType::STRING;
        result.value = std::move(value);
        result.source = Range(start, offset);
        return result;
      }
      Error("unterminated string literal");
    }
    if (ConsumeKeyword("TRUE") || ConsumeKeyword("FALSE")) {
      result.type = GqlLiteralType::BOOLEAN;
      result.value = StringUtil::Lower(query.substr(start, offset - start));
      result.source = Range(start, offset);
      return result;
    }
    if (ConsumeKeyword("NULL")) {
      Error("NULL properties are not supported in MERGE patterns");
    }
    offset = start;
    if (offset < query.size() &&
        (query[offset] == '+' || query[offset] == '-')) {
      offset++;
    }
    auto digits_start = offset;
    while (offset < query.size() &&
           std::isdigit(static_cast<unsigned char>(query[offset]))) {
      offset++;
    }
    if (digits_start == offset) {
      Error("expected a scalar literal");
    }
    bool decimal = false;
    bool approximate = false;
    if (offset < query.size() && query[offset] == '.') {
      decimal = true;
      offset++;
      auto fraction_start = offset;
      while (offset < query.size() &&
             std::isdigit(static_cast<unsigned char>(query[offset]))) {
        offset++;
      }
      if (fraction_start == offset) {
        Error("expected digits after decimal point");
      }
    }
    if (offset < query.size() &&
        (query[offset] == 'e' || query[offset] == 'E')) {
      approximate = true;
      offset++;
      if (offset < query.size() &&
          (query[offset] == '+' || query[offset] == '-')) {
        offset++;
      }
      auto exponent_start = offset;
      while (offset < query.size() &&
             std::isdigit(static_cast<unsigned char>(query[offset]))) {
        offset++;
      }
      if (exponent_start == offset) {
        Error("expected exponent digits");
      }
    }
    result.type = approximate
                      ? GqlLiteralType::DOUBLE
                      : decimal ? GqlLiteralType::DECIMAL
                                : GqlLiteralType::INTEGER;
    result.value = query.substr(start, offset - start);
    result.source = Range(start, offset);
    return result;
  }

  void ParseProperties(vector<GqlPropertyAssignment> &properties) {
    SkipWhitespace();
    if (ConsumeCharacter('}')) {
      Error("MERGE property maps cannot be empty");
    }
    unordered_set<string> names;
    while (true) {
      GqlPropertyAssignment property;
      auto start = offset;
      property.name = ParseIdentifier();
      if (!names.insert(property.name.value).second) {
        Error("duplicate MERGE property '" + property.name.value + "'");
      }
      ExpectCharacter(':');
      property.value = ParseLiteral();
      property.source = Range(start, offset);
      properties.push_back(std::move(property));
      SkipWhitespace();
      if (ConsumeCharacter('}')) {
        return;
      }
      ExpectCharacter(',');
    }
  }

  GqlSourceRange Range(idx_t start, idx_t end) const {
    GqlSourceRange result;
    result.start_offset = start;
    result.end_offset = end;
    result.start_line = 1;
    result.start_column = start;
    result.end_line = 1;
    result.end_column = end;
    for (idx_t index = 0; index < end; index++) {
      if (query[index] != '\n') {
        continue;
      }
      result.end_line++;
      result.end_column = end - index - 1;
      if (index < start) {
        result.start_line++;
        result.start_column = start - index - 1;
      }
    }
    return result;
  }

  const string &query;
  idx_t offset = 0;
  idx_t pattern_start = 0;
};

static GqlExecutionMode ReadExecutionMode(const string &query) {
  auto trimmed = query;
  StringUtil::Trim(trimmed);
  auto upper = StringUtil::Upper(trimmed);
  idx_t offset;
  if (StringUtil::StartsWith(upper, "OPTIONAL MATCH")) {
    offset = string("OPTIONAL MATCH").size();
  } else if (StringUtil::StartsWith(upper, "MATCH")) {
    offset = string("MATCH").size();
  } else {
    return GqlExecutionMode::NATIVE;
  }
  while (offset < trimmed.size() &&
         std::isspace(static_cast<unsigned char>(trimmed[offset]))) {
    offset++;
  }
  if (offset + 3 > trimmed.size() || trimmed.compare(offset, 3, "/*+") != 0) {
    return GqlExecutionMode::NATIVE;
  }
  auto end = trimmed.find("*/", offset + 3);
  if (end == string::npos) {
    throw ParserException("Unterminated GQL execution hint");
  }
  auto hint = trimmed.substr(offset + 3, end - offset - 3);
  StringUtil::Trim(hint);
  hint = StringUtil::Upper(hint);
  if (hint == "CSR" || hint == "GQL_CSR") {
    return GqlExecutionMode::CSR;
  }
  if (hint == "NATIVE" || hint == "GQL_NATIVE") {
    return GqlExecutionMode::NATIVE;
  }
  throw ParserException(
      "Unknown GQL execution hint '%s'; expected CSR or NATIVE", hint);
}

ParserExtensionParseResult GqlParse(ParserExtensionInfo *,
                                    const string &query) {
  if (!StartsWithGqlCommand(query)) {
    return ParserExtensionParseResult();
  }

  auto gql_query = StripTerminator(query);
  auto normalized = StringUtil::Upper(gql_query);
  StringUtil::Trim(normalized);
  if (StringUtil::StartsWith(normalized, "COPY GRAPH")) {
    auto parse_data = make_uniq<GqlParseData>();
    parse_data->query = gql_query;
    parse_data->statement = CopyGraphParser(gql_query).Parse();
    return ParserExtensionParseResult(std::move(parse_data));
  }
  if (StartsWithMergePattern(gql_query)) {
    auto parse_data = make_uniq<GqlParseData>();
    parse_data->query = gql_query;
    parse_data->statement = MergeParser(gql_query).Parse();
    return ParserExtensionParseResult(std::move(parse_data));
  }
  antlr4::ANTLRInputStream input(gql_query);
  GQLLexer lexer(&input);
  antlr4::CommonTokenStream tokens(&lexer);
  GQLParser parser(&tokens);
  GqlErrorListener errors;
  lexer.removeErrorListeners();
  parser.removeErrorListeners();
  lexer.addErrorListener(&errors);
  parser.addErrorListener(&errors);
  auto tree = parser.gqlProgram();
  if (!errors.error.empty()) {
    ParserExtensionParseResult result(errors.error);
    result.error_location = errors.location;
    return result;
  }

  auto parse_data = make_uniq<GqlParseData>();
  parse_data->query = gql_query;
  parse_data->execution_mode = ReadExecutionMode(gql_query);
  GqlTransformer transformer;
  parse_data->statement = transformer.Transform(*tree);
  if (!parse_data->statement) {
    throw InternalException("GQL transformer returned no statement");
  }
  return ParserExtensionParseResult(std::move(parse_data));
}

ParserExtensionPlanResult
GqlPlan(ParserExtensionInfo *, ClientContext &,
        unique_ptr<ParserExtensionParseData> parse_data) {
  auto gql_ptr = dynamic_cast<GqlParseData *>(parse_data.get());
  if (!gql_ptr || !gql_ptr->statement) {
    throw InternalException("Invalid GQL parser data");
  }
  auto &statement = *gql_ptr->statement;
  ParserExtensionPlanResult result;
  result.requires_valid_transaction = true;
  result.return_type = StatementReturnType::QUERY_RESULT;

  switch (statement.type) {
  case GqlStatementType::CREATE_GRAPH: {
    auto &create = statement.Cast<GqlCreateGraphStatement>();
    result.function = GqlCreateGraphFunction();
    result.parameters.emplace_back(create.graph_name.value);
    result.parameters.emplace_back(create.if_not_exists);
    return result;
  }
  case GqlStatementType::COPY_GRAPH: {
    auto &copy = statement.Cast<GqlCopyGraphStatement>();
    result.function = GqlCopyGraphFunction();
    result.parameters.emplace_back(copy.graph_name.value);
    result.parameters.emplace_back(copy.vertex_path);
    result.parameters.emplace_back(copy.edge_path);
    result.parameters.emplace_back(copy.validate);
    return result;
  }
  case GqlStatementType::DROP_GRAPH: {
    auto &drop = statement.Cast<GqlDropGraphStatement>();
    result.function = GqlDropGraphFunction();
    result.parameters.emplace_back(drop.graph_name.value);
    result.parameters.emplace_back(drop.if_exists);
    return result;
  }
  case GqlStatementType::SESSION_SET_GRAPH: {
    auto &set_graph = statement.Cast<GqlSessionSetGraphStatement>();
    result.function = GqlSetGraphFunction();
    result.parameters.emplace_back(set_graph.graph_name.value);
    return result;
  }
  case GqlStatementType::INSERT:
    throw NotImplementedException(
        "GQL INSERT on native graph tables is not implemented yet");
  case GqlStatementType::MERGE:
    throw InternalException("MERGE requires DuckDB parser-override lowering");
  case GqlStatementType::MATCH: {
    GqlBinder binder;
    auto &match = statement.Cast<GqlMatchStatement>();
    auto alternatives = binder.BindAlternatives(match);
    for (auto &alternative : alternatives) {
      alternative.execution_mode = gql_ptr->execution_mode;
    }
    if (match.has_mutation) {
      throw NotImplementedException(
          "SET, REMOVE, and DELETE on native graph tables are not implemented yet");
    }
    if (alternatives.size() != 1) {
      throw NotImplementedException(
          "Finite ranged GQL MATCH requires DuckDB parser-override lowering");
    }
    return GqlLower(alternatives[0]);
  }
  case GqlStatementType::UNSUPPORTED: {
    auto &unsupported = statement.Cast<GqlUnsupportedStatement>();
    auto message = unsupported.feature + " (line " +
                   to_string(unsupported.source.start_line) + ", column " +
                   to_string(unsupported.source.start_column) + ")";
    throw NotImplementedException("GQL feature not implemented: %s", message);
  }
  }
  throw InternalException("Unknown GQL statement type");
}

ParserOverrideResult GqlParserOverride(ParserExtensionInfo *,
                                       const string &query, ParserOptions &) {
  auto normalized = query;
  StringUtil::Trim(normalized);
  normalized = StringUtil::Upper(normalized);
  if (!StartsWithMergePattern(query) &&
      !StringUtil::StartsWith(normalized, "MATCH") &&
      !StringUtil::StartsWith(normalized, "OPTIONAL MATCH")) {
    return ParserOverrideResult();
  }
  auto parsed = GqlParse(nullptr, query);
  if (parsed.type != ParserExtensionResultType::PARSE_SUCCESSFUL) {
    return ParserOverrideResult();
  }
  auto gql_ptr = dynamic_cast<GqlParseData *>(parsed.parse_data.get());
  if (!gql_ptr || !gql_ptr->statement) {
    return ParserOverrideResult();
  }

  try {
    if (gql_ptr->statement->type == GqlStatementType::MERGE) {
      auto statements =
          GqlLowerMerge(gql_ptr->statement->Cast<GqlMergeStatement>());
      for (auto &statement : statements) {
        statement->query = query;
        statement->stmt_location = 0;
        statement->stmt_length = query.size();
      }
      return ParserOverrideResult(std::move(statements));
    }
    if (gql_ptr->statement->type != GqlStatementType::MATCH) {
      return ParserOverrideResult();
    }
    GqlBinder binder;
    auto &match = gql_ptr->statement->Cast<GqlMatchStatement>();
    auto plans = binder.BindAlternatives(match);
    for (auto &plan : plans) {
      plan.execution_mode = gql_ptr->execution_mode;
    }
    vector<unique_ptr<SQLStatement>> statements;
    if (match.has_mutation) {
      statements = GqlLowerMutation(plans);
    } else {
      statements.push_back(GqlLowerSelect(std::move(plans)));
    }
    for (auto &statement : statements) {
      statement->query = query;
      statement->stmt_location = 0;
      statement->stmt_length = query.size();
    }
    return ParserOverrideResult(std::move(statements));
  } catch (std::exception &error) {
    return ParserOverrideResult(error);
  }
}

GqlParserExtension::GqlParserExtension() : ParserExtension() {
  parse_function = GqlParse;
  plan_function = GqlPlan;
  parser_override = GqlParserOverride;
  parser_info = make_shared_ptr<GqlParserExtensionInfo>();
}

} // namespace duckdb
