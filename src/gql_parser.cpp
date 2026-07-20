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
#include "gql_lowerer.hpp"
#include "gql_storage.hpp"
#include "gql_transformer.hpp"

namespace duckdb {

using antlr4::BaseErrorListener;
using antlr4::Recognizer;
using antlr4::Token;

unique_ptr<ParserExtensionParseData> GqlParseData::Copy() const {
	return make_uniq_base<ParserExtensionParseData, GqlParseData>(*this);
}

string GqlParseData::ToString() const {
	return query;
}

class GqlErrorListener final : public BaseErrorListener {
public:
	void syntaxError(Recognizer *recognizer, Token *offending_symbol, size_t line, size_t character_in_line,
	                 const string &message, std::exception_ptr exception) override {
		if (!error.empty()) {
			return;
		}
		error =
		    "GQL parser error at line " + to_string(line) + ", column " + to_string(character_in_line) + ": " + message;
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
	       StringUtil::StartsWith(normalized, "MATCH") || StringUtil::StartsWith(normalized, "OPTIONAL MATCH") ||
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

ParserExtensionParseResult GqlParse(ParserExtensionInfo *, const string &query) {
	if (!StartsWithGqlCommand(query)) {
		return ParserExtensionParseResult();
	}

	auto gql_query = StripTerminator(query);
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
	GqlTransformer transformer;
	parse_data->statement = transformer.Transform(*tree);
	if (!parse_data->statement) {
		throw InternalException("GQL transformer returned no statement");
	}
	return ParserExtensionParseResult(std::move(parse_data));
}

static string LiteralStorageTag(GqlLiteralType type) {
	switch (type) {
	case GqlLiteralType::NULL_VALUE:
		throw NotImplementedException("NULL-valued GQL INSERT properties");
	case GqlLiteralType::BOOLEAN:
		return "bool_value";
	case GqlLiteralType::INTEGER:
		return "int_value";
	case GqlLiteralType::DECIMAL:
		return "decimal_value";
	case GqlLiteralType::DOUBLE:
		return "double_value";
	case GqlLiteralType::STRING:
		return "string_value";
	}
	throw InternalException("Unknown GQL literal type");
}

static void AppendInsertElement(const GqlInsertElement &element, vector<Value> &labels_out, vector<Value> &names_out,
                                vector<Value> &tags_out, vector<Value> &literals_out) {
	vector<Value> labels;
	vector<Value> names;
	vector<Value> tags;
	vector<Value> literals;
	for (const auto &label : element.labels) {
		labels.emplace_back(label.value);
	}
	for (const auto &property : element.properties) {
		names.emplace_back(property.name.value);
		tags.emplace_back(LiteralStorageTag(property.value.type));
		literals.emplace_back(property.value.value);
	}
	labels_out.emplace_back(Value::LIST(LogicalType::VARCHAR, std::move(labels)));
	names_out.emplace_back(Value::LIST(LogicalType::VARCHAR, std::move(names)));
	tags_out.emplace_back(Value::LIST(LogicalType::VARCHAR, std::move(tags)));
	literals_out.emplace_back(Value::LIST(LogicalType::VARCHAR, std::move(literals)));
}

static ParserExtensionPlanResult PlanInsert(const GqlInsertStatement &insert) {
	ParserExtensionPlanResult result;
	result.requires_valid_transaction = true;
	result.return_type = StatementReturnType::QUERY_RESULT;
	if (insert.vertices.size() == 1 && insert.edges.empty()) {
		vector<Value> labels;
		vector<Value> property_names;
		vector<Value> property_tags;
		vector<Value> property_literals;
		for (const auto &label : insert.vertices[0].labels) {
			labels.emplace_back(label.value);
		}
		for (const auto &property : insert.vertices[0].properties) {
			property_names.emplace_back(property.name.value);
			property_tags.emplace_back(LiteralStorageTag(property.value.type));
			property_literals.emplace_back(property.value.value);
		}
		result.function = GqlInsertVertexFunction();
		result.parameters.emplace_back(Value::LIST(LogicalType::VARCHAR, std::move(labels)));
		result.parameters.emplace_back(Value::LIST(LogicalType::VARCHAR, std::move(property_names)));
		result.parameters.emplace_back(Value::LIST(LogicalType::VARCHAR, std::move(property_tags)));
		result.parameters.emplace_back(Value::LIST(LogicalType::VARCHAR, std::move(property_literals)));
		return result;
	}

	auto varchar_list_type = LogicalType::LIST(LogicalType::VARCHAR);
	vector<Value> vertex_labels;
	vector<Value> vertex_property_names;
	vector<Value> vertex_property_tags;
	vector<Value> vertex_property_literals;
	vector<Value> edge_sources;
	vector<Value> edge_targets;
	vector<Value> edge_labels;
	vector<Value> edge_property_names;
	vector<Value> edge_property_tags;
	vector<Value> edge_property_literals;
	for (const auto &vertex : insert.vertices) {
		AppendInsertElement(vertex, vertex_labels, vertex_property_names, vertex_property_tags,
		                    vertex_property_literals);
	}
	for (const auto &edge : insert.edges) {
		edge_sources.emplace_back(Value::UBIGINT(edge.source_vertex));
		edge_targets.emplace_back(Value::UBIGINT(edge.target_vertex));
		AppendInsertElement(edge, edge_labels, edge_property_names, edge_property_tags, edge_property_literals);
	}
	result.function = GqlInsertPathFunction();
	result.parameters.emplace_back(Value::LIST(varchar_list_type, std::move(vertex_labels)));
	result.parameters.emplace_back(Value::LIST(varchar_list_type, std::move(vertex_property_names)));
	result.parameters.emplace_back(Value::LIST(varchar_list_type, std::move(vertex_property_tags)));
	result.parameters.emplace_back(Value::LIST(varchar_list_type, std::move(vertex_property_literals)));
	result.parameters.emplace_back(Value::LIST(LogicalType::UBIGINT, std::move(edge_sources)));
	result.parameters.emplace_back(Value::LIST(LogicalType::UBIGINT, std::move(edge_targets)));
	result.parameters.emplace_back(Value::LIST(varchar_list_type, std::move(edge_labels)));
	result.parameters.emplace_back(Value::LIST(varchar_list_type, std::move(edge_property_names)));
	result.parameters.emplace_back(Value::LIST(varchar_list_type, std::move(edge_property_tags)));
	result.parameters.emplace_back(Value::LIST(varchar_list_type, std::move(edge_property_literals)));
	return result;
}

ParserExtensionPlanResult GqlPlan(ParserExtensionInfo *, ClientContext &,
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
		return PlanInsert(statement.Cast<GqlInsertStatement>());
	case GqlStatementType::MATCH: {
		GqlBinder binder;
		auto &match = statement.Cast<GqlMatchStatement>();
		auto alternatives = binder.BindAlternatives(match);
		if (match.has_mutation) {
			throw NotImplementedException("Matched GQL mutation requires DuckDB parser-override lowering");
		}
		if (alternatives.size() != 1) {
			throw NotImplementedException("Finite ranged GQL MATCH requires DuckDB parser-override lowering");
		}
		return GqlLower(alternatives[0]);
	}
	case GqlStatementType::UNSUPPORTED: {
		auto &unsupported = statement.Cast<GqlUnsupportedStatement>();
		auto message = unsupported.feature + " (line " + to_string(unsupported.source.start_line) + ", column " +
		               to_string(unsupported.source.start_column) + ")";
		throw NotImplementedException("GQL feature not implemented: %s", message);
	}
	}
	throw InternalException("Unknown GQL statement type");
}

ParserOverrideResult GqlParserOverride(ParserExtensionInfo *, const string &query, ParserOptions &) {
	auto normalized = query;
	StringUtil::Trim(normalized);
	normalized = StringUtil::Upper(normalized);
	if (!StringUtil::StartsWith(normalized, "MATCH") && !StringUtil::StartsWith(normalized, "OPTIONAL MATCH")) {
		return ParserOverrideResult();
	}
	auto parsed = GqlParse(nullptr, query);
	if (parsed.type != ParserExtensionResultType::PARSE_SUCCESSFUL) {
		return ParserOverrideResult();
	}
	auto gql_ptr = dynamic_cast<GqlParseData *>(parsed.parse_data.get());
	if (!gql_ptr || !gql_ptr->statement || gql_ptr->statement->type != GqlStatementType::MATCH) {
		return ParserOverrideResult();
	}

	try {
		GqlBinder binder;
		auto &match = gql_ptr->statement->Cast<GqlMatchStatement>();
		auto plans = binder.BindAlternatives(match);
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
