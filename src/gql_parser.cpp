#include "gql_parser.hpp"

#include "GQLLexer.h"
#include "GQLParser.h"
#include "antlr4-runtime.h"

#ifdef INVALID_INDEX
#undef INVALID_INDEX
#endif

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/simplified_token.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "gql_binder.hpp"
#include "gql_import.hpp"
#include "gql_lowerer.hpp"
#include "gql_mutation.hpp"
#include "gql_storage.hpp"
#include "gql_transformer.hpp"

#include <cctype>
#include <initializer_list>

namespace duckdb {

using antlr4::BaseErrorListener;
using antlr4::Recognizer;
using antlr4::Token;

static idx_t SkipGqlTrivia(const string &query, idx_t offset = 0) {
	while (offset < query.size()) {
		if (std::isspace(static_cast<unsigned char>(query[offset]))) {
			offset++;
			continue;
		}
		if (offset + 1 < query.size() && ((query[offset] == '-' && query[offset + 1] == '-') ||
		                                  (query[offset] == '/' && query[offset + 1] == '/'))) {
			offset += 2;
			while (offset < query.size() && query[offset] != '\n') {
				offset++;
			}
			continue;
		}
		if (offset + 1 < query.size() && query[offset] == '/' && query[offset + 1] == '*') {
			auto end = query.find("*/", offset + 2);
			if (end == string::npos) {
				return query.size();
			}
			offset = end + 2;
			continue;
		}
		break;
	}
	return offset;
}

static bool ConsumeGqlKeyword(const string &query, idx_t &offset, const string &keyword) {
	offset = SkipGqlTrivia(query, offset);
	if (offset + keyword.size() > query.size() ||
	    !StringUtil::CIEquals(query.substr(offset, keyword.size()), keyword)) {
		return false;
	}
	auto end = offset + keyword.size();
	if (end < query.size() && (std::isalnum(static_cast<unsigned char>(query[end])) || query[end] == '_')) {
		return false;
	}
	offset = end;
	return true;
}

static bool StartsWithGqlKeywords(const string &query, std::initializer_list<const char *> keywords,
                                  idx_t *end_offset = nullptr) {
	idx_t offset = 0;
	for (const auto keyword : keywords) {
		if (!ConsumeGqlKeyword(query, offset, keyword)) {
			return false;
		}
	}
	if (end_offset) {
		*end_offset = offset;
	}
	return true;
}

static bool StartsWithGqlKeywordsAndCharacter(const string &query, std::initializer_list<const char *> keywords,
                                              char character) {
	idx_t offset;
	if (!StartsWithGqlKeywords(query, keywords, &offset)) {
		return false;
	}
	offset = SkipGqlTrivia(query, offset);
	return offset < query.size() && query[offset] == character;
}

static vector<string> SplitGqlOverrideQueries(const string &query) {
	vector<string> queries;
	auto tokens = Parser::Tokenize(query);
	idx_t last_split = 0;
	for (const auto &token : tokens) {
		if (token.type != SimplifiedTokenType::SIMPLIFIED_TOKEN_OPERATOR || query[token.start] != ';') {
			continue;
		}
		auto segment = query.substr(last_split, token.start - last_split);
		StringUtil::Trim(segment);
		if (!segment.empty()) {
			segment.push_back(';');
			queries.push_back(std::move(segment));
		}
		last_split = token.start + 1;
	}
	auto final_segment = query.substr(last_split);
	StringUtil::Trim(final_segment);
	if (!final_segment.empty()) {
		queries.push_back(std::move(final_segment));
	}
	return queries;
}

static bool StartsWithMergePattern(const string &query) {
	idx_t offset;
	if (!StartsWithGqlKeywords(query, {"MERGE"}, &offset)) {
		return false;
	}
	offset = SkipGqlTrivia(query, offset);
	return offset < query.size() && query[offset] == '(';
}

static bool FindAlgorithmCall(const string &query, idx_t &algorithm_offset) {
	idx_t offset;
	if (!StartsWithGqlKeywords(query, {"CALL"}, &offset)) {
		return false;
	}
	offset = SkipGqlTrivia(query, offset);
	if (offset + 5 > query.size() || !StringUtil::CIEquals(query.substr(offset, 5), "algo.")) {
		return false;
	}
	algorithm_offset = offset;
	return true;
}

static bool StartsWithAlgorithmCall(const string &query) {
	idx_t algorithm_offset;
	return FindAlgorithmCall(query, algorithm_offset);
}

static bool ContainsUnquotedKeyword(const string &query, const string &keyword) {
	char quote = '\0';
	bool line_comment = false;
	bool block_comment = false;
	for (idx_t offset = 0; offset < query.size(); offset++) {
		auto character = query[offset];
		auto next = offset + 1 < query.size() ? query[offset + 1] : '\0';
		if (line_comment) {
			line_comment = character != '\n';
			continue;
		}
		if (block_comment) {
			if (character == '*' && next == '/') {
				block_comment = false;
				offset++;
			}
			continue;
		}
		if (quote) {
			if (character != quote) {
				continue;
			}
			if (next == quote) {
				offset++;
				continue;
			}
			quote = '\0';
			continue;
		}
		if (character == '-' && next == '-') {
			line_comment = true;
			offset++;
			continue;
		}
		if (character == '/' && next == '*') {
			block_comment = true;
			offset++;
			continue;
		}
		if (character == '\'' || character == '"' || character == '`') {
			quote = character;
			continue;
		}
		if (!(std::isalpha(static_cast<unsigned char>(character)) || character == '_')) {
			continue;
		}
		auto start = offset++;
		while (offset < query.size() &&
		       (std::isalnum(static_cast<unsigned char>(query[offset])) || query[offset] == '_')) {
			offset++;
		}
		if (StringUtil::CIEquals(query.substr(start, offset - start), keyword)) {
			return true;
		}
		offset--;
	}
	return false;
}

unique_ptr<ParserExtensionParseData> GqlParseData::Copy() const {
	return make_uniq_base<ParserExtensionParseData, GqlParseData>(*this);
}

string GqlParseData::ToString() const {
	return query;
}

class GqlErrorListener final : public BaseErrorListener {
public:
	explicit GqlErrorListener(const string &query_p) : query(query_p) {
	}

	void syntaxError(Recognizer *recognizer, Token *offending_symbol, size_t line, size_t character_in_line,
	                 const string &message, std::exception_ptr exception) override {
		if (!error.empty()) {
			return;
		}
		error =
		    "GQL parser error at line " + to_string(line) + ", column " + to_string(character_in_line) + ": " + message;
		if (offending_symbol && offending_symbol->getStartIndex() <= query.size()) {
			location = offending_symbol->getStartIndex();
			return;
		}
		location = 0;
		for (idx_t current_line = 1; current_line < line && location < query.size(); current_line++) {
			auto newline = query.find('\n', location);
			if (newline == string::npos) {
				location = query.size();
				break;
			}
			location = newline + 1;
		}
		location = MinValue<idx_t>(query.size(), location + character_in_line);
	}

	const string &query;
	string error;
	idx_t location = 0;
};

static bool StartsWithGqlCommand(const string &query) {
	return StartsWithGqlKeywords(query, {"CREATE", "GRAPH"}) ||
	       StartsWithGqlKeywords(query, {"CREATE", "PROPERTY", "GRAPH"}) ||
	       StartsWithGqlKeywords(query, {"DROP", "GRAPH"}) ||
	       StartsWithGqlKeywords(query, {"DROP", "PROPERTY", "GRAPH"}) ||
	       StartsWithGqlKeywords(query, {"SESSION", "SET", "GRAPH"}) ||
	       StartsWithGqlKeywords(query, {"SESSION", "SET", "PROPERTY", "GRAPH"}) ||
	       StartsWithGqlKeywords(query, {"COPY", "GRAPH"}) || StartsWithMergePattern(query) ||
	       StartsWithGqlKeywords(query, {"MATCH"}) || StartsWithGqlKeywords(query, {"OPTIONAL", "MATCH"}) ||
	       StartsWithGqlKeywordsAndCharacter(query, {"INSERT"}, '(') || StartsWithAlgorithmCall(query);
}

static bool RewriteAlgorithmCall(const string &query, string &rewritten) {
	idx_t offset;
	if (!FindAlgorithmCall(query, offset) || ContainsUnquotedKeyword(query, "YIELD")) {
		return false;
	}
	rewritten = query;
	rewritten.insert(offset, "system.");
	return true;
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

// The imported ISO grammar intentionally has no Cypher-style += token. Accept
// this project-owned compatibility form without forking the generated parser:
// blank only the '+' in an unquoted += pair, preserving every source offset.
// The transformer inspects the original query to distinguish merge from
// replacement semantics.
static string RewritePropertyMapMergeSyntax(const string &query) {
	auto result = query;
	char quote = '\0';
	bool line_comment = false;
	bool block_comment = false;
	for (idx_t index = 0; index < result.size(); index++) {
		auto character = result[index];
		auto next = index + 1 < result.size() ? result[index + 1] : '\0';
		if (line_comment) {
			line_comment = character != '\n';
			continue;
		}
		if (block_comment) {
			if (character == '*' && next == '/') {
				block_comment = false;
				index++;
			}
			continue;
		}
		if (quote) {
			if (character != quote) {
				continue;
			}
			if (next == quote) {
				index++;
				continue;
			}
			quote = '\0';
			continue;
		}
		if (character == '-' && next == '-') {
			line_comment = true;
			index++;
			continue;
		}
		if (character == '/' && next == '*') {
			block_comment = true;
			index++;
			continue;
		}
		if (character == '\'' || character == '"' || character == '`') {
			quote = character;
			continue;
		}
		if (character == '+' && next == '=') {
			auto map_offset = index + 2;
			while (map_offset < result.size()) {
				if (std::isspace(static_cast<unsigned char>(result[map_offset]))) {
					map_offset++;
					continue;
				}
				if (map_offset + 1 < result.size() && result[map_offset] == '-' && result[map_offset + 1] == '-') {
					map_offset += 2;
					while (map_offset < result.size() && result[map_offset] != '\n') {
						map_offset++;
					}
					continue;
				}
				if (map_offset + 1 < result.size() && result[map_offset] == '/' && result[map_offset + 1] == '*') {
					auto end = result.find("*/", map_offset + 2);
					if (end == string::npos) {
						break;
					}
					map_offset = end + 2;
					continue;
				}
				break;
			}
			if (map_offset < result.size() &&
			    (result[map_offset] == '{' || result[map_offset] == '`' || result[map_offset] == '"' ||
			     std::isalpha(static_cast<unsigned char>(result[map_offset])) || result[map_offset] == '_')) {
				result[index] = ' ';
				index++;
			}
		}
	}
	return result;
}

// DuckDB-style named arguments are part of the public algo CALL surface, but
// the imported ISO grammar only accepts positional procedure arguments. Blank
// `name :=` outside quotes/comments while preserving byte offsets; the
// transformer recovers the name from the original query using the value's
// unchanged source position.
static string RewriteCallNamedArgumentSyntax(const string &query) {
	if (!StartsWithAlgorithmCall(query) || !ContainsUnquotedKeyword(query, "YIELD")) {
		return query;
	}
	auto result = query;
	char quote = '\0';
	bool line_comment = false;
	bool block_comment = false;
	for (idx_t index = 0; index + 1 < result.size(); index++) {
		auto character = result[index];
		auto next = result[index + 1];
		if (line_comment) {
			line_comment = character != '\n';
			continue;
		}
		if (block_comment) {
			if (character == '*' && next == '/') {
				block_comment = false;
				index++;
			}
			continue;
		}
		if (quote) {
			if (character != quote) {
				continue;
			}
			if (next == quote) {
				index++;
				continue;
			}
			quote = '\0';
			continue;
		}
		if (character == '-' && next == '-') {
			line_comment = true;
			index++;
			continue;
		}
		if (character == '/' && next == '*') {
			block_comment = true;
			index++;
			continue;
		}
		if (character == '\'' || character == '"' || character == '`') {
			quote = character;
			continue;
		}
		if (character != ':' || next != '=') {
			continue;
		}
		auto name_end = index;
		while (name_end > 0 && std::isspace(static_cast<unsigned char>(result[name_end - 1]))) {
			name_end--;
		}
		auto name_start = name_end;
		while (name_start > 0 &&
		       (std::isalnum(static_cast<unsigned char>(result[name_start - 1])) || result[name_start - 1] == '_')) {
			name_start--;
		}
		if (name_start == name_end ||
		    !(std::isalpha(static_cast<unsigned char>(result[name_start])) || result[name_start] == '_')) {
			continue;
		}
		std::fill(result.begin() + name_start, result.begin() + index + 2, ' ');
		index++;
	}
	return result;
}

// The ISO SET grammar accepts only an inline property-map constructor. LET is
// the runtime value-producing seam used by the compiler, so accept a single
// LET variable on the right hand side as a compatibility form. Replace only
// that identifier with an empty map in the parser input; the transformer reads
// the original identifier and the binder expands its record fields into the
// ordinary typed mutation program. The rewritten query retains its byte count.
static string RewritePropertyMapExpressionSyntax(const string &query) {
	auto result = query;
	auto skip_trivia = [&](idx_t offset) -> idx_t {
		while (offset < query.size()) {
			if (std::isspace(static_cast<unsigned char>(query[offset]))) {
				offset++;
				continue;
			}
			if (offset + 1 < query.size() && query[offset] == '-' && query[offset + 1] == '-') {
				offset += 2;
				while (offset < query.size() && query[offset] != '\n') {
					offset++;
				}
				continue;
			}
			if (offset + 1 < query.size() && query[offset] == '/' && query[offset + 1] == '*') {
				auto end = query.find("*/", offset + 2);
				if (end == string::npos) {
					return query.size();
				}
				offset = end + 2;
				continue;
			}
			break;
		}
		return offset;
	};
	auto read_identifier = [&](idx_t offset, idx_t &end) {
		if (offset >= query.size()) {
			return false;
		}
		if (query[offset] == '`' || query[offset] == '"') {
			auto quote = query[offset++];
			while (offset < query.size()) {
				if (query[offset] != quote) {
					offset++;
					continue;
				}
				if (offset + 1 < query.size() && query[offset + 1] == quote) {
					offset += 2;
					continue;
				}
				end = offset + 1;
				return true;
			}
			return false;
		}
		if (!(std::isalpha(static_cast<unsigned char>(query[offset])) || query[offset] == '_')) {
			return false;
		}
		offset++;
		while (offset < query.size() &&
		       (std::isalnum(static_cast<unsigned char>(query[offset])) || query[offset] == '_')) {
			offset++;
		}
		end = offset;
		return true;
	};
	auto item_end = [&](idx_t offset) -> idx_t {
		idx_t parentheses = 0;
		idx_t brackets = 0;
		idx_t braces = 0;
		char quote = '\0';
		bool line_comment = false;
		bool block_comment = false;
		for (; offset < query.size(); offset++) {
			auto character = query[offset];
			auto next = offset + 1 < query.size() ? query[offset + 1] : '\0';
			if (line_comment) {
				line_comment = character != '\n';
				continue;
			}
			if (block_comment) {
				if (character == '*' && next == '/') {
					block_comment = false;
					offset++;
				}
				continue;
			}
			if (quote) {
				if (character == quote) {
					if (next == quote) {
						offset++;
					} else {
						quote = '\0';
					}
				}
				continue;
			}
			if (character == '-' && next == '-') {
				line_comment = true;
				offset++;
				continue;
			}
			if (character == '/' && next == '*') {
				block_comment = true;
				offset++;
				continue;
			}
			if (character == '\'' || character == '"' || character == '`') {
				quote = character;
				continue;
			}
			switch (character) {
			case '(':
				parentheses++;
				break;
			case ')':
				if (parentheses > 0) {
					parentheses--;
				}
				break;
			case '[':
				brackets++;
				break;
			case ']':
				if (brackets > 0) {
					brackets--;
				}
				break;
			case '{':
				braces++;
				break;
			case '}':
				if (braces > 0) {
					braces--;
				}
				break;
			case ',':
				if (parentheses == 0 && brackets == 0 && braces == 0) {
					return offset;
				}
				break;
			default:
				break;
			}
		}
		return query.size();
	};

	idx_t set_offset = DConstants::INVALID_INDEX;
	char quote = '\0';
	bool line_comment = false;
	bool block_comment = false;
	for (idx_t index = 0; index < query.size(); index++) {
		auto character = query[index];
		auto next = index + 1 < query.size() ? query[index + 1] : '\0';
		if (line_comment) {
			line_comment = character != '\n';
			continue;
		}
		if (block_comment) {
			if (character == '*' && next == '/') {
				block_comment = false;
				index++;
			}
			continue;
		}
		if (quote) {
			if (character == quote) {
				if (next == quote) {
					index++;
				} else {
					quote = '\0';
				}
			}
			continue;
		}
		if (character == '-' && next == '-') {
			line_comment = true;
			index++;
			continue;
		}
		if (character == '/' && next == '*') {
			block_comment = true;
			index++;
			continue;
		}
		if (character == '\'' || character == '"' || character == '`') {
			quote = character;
			continue;
		}
		if (!(std::isalpha(static_cast<unsigned char>(character)) || character == '_')) {
			continue;
		}
		auto word_start = index;
		while (index + 1 < query.size() &&
		       (std::isalnum(static_cast<unsigned char>(query[index + 1])) || query[index + 1] == '_')) {
			index++;
		}
		if (StringUtil::CIEquals(query.substr(word_start, index - word_start + 1), "SET")) {
			set_offset = index + 1;
			break;
		}
	}
	if (set_offset == DConstants::INVALID_INDEX) {
		return result;
	}

	for (auto item = set_offset; item < query.size();) {
		auto end = item_end(item);
		auto cursor = skip_trivia(item);
		idx_t target_end;
		if (!read_identifier(cursor, target_end)) {
			break;
		}
		cursor = skip_trivia(target_end);
		if (cursor >= end || query[cursor] != '=') {
			item = end < query.size() ? end + 1 : end;
			continue;
		}
		auto rhs_start = skip_trivia(cursor + 1);
		idx_t rhs_end;
		if (rhs_start >= end || query[rhs_start] == '{' || !read_identifier(rhs_start, rhs_end) ||
		    skip_trivia(rhs_end) < end) {
			item = end < query.size() ? end + 1 : end;
			continue;
		}
		auto rewrite_start = rhs_start;
		if (rhs_end - rhs_start < 2) {
			if (rhs_start == cursor + 1 || !std::isspace(static_cast<unsigned char>(query[rhs_start - 1]))) {
				item = end < query.size() ? end + 1 : end;
				continue;
			}
			rewrite_start--;
		}
		for (idx_t offset = rewrite_start; offset < rhs_end; offset++) {
			result[offset] = ' ';
		}
		result[rewrite_start] = '{';
		result[rewrite_start + 1] = '}';
		item = end < query.size() ? end + 1 : end;
	}
	return result;
}

// Compact Cypher-style label chains are a project-owned compatibility form.
// The ISO grammar accepts one label per SET/REMOVE item, so blank the second
// and later suffixes before parsing. The transformer reads those suffixes from
// the untouched original query and emits one typed mutation per label. Keeping
// the rewritten string byte-for-byte the same size preserves source ranges.
static string RewriteChainedLabelSyntax(const string &query) {
	auto result = query;
	auto skip_trivia = [&](idx_t offset) -> idx_t {
		while (offset < query.size()) {
			if (std::isspace(static_cast<unsigned char>(query[offset]))) {
				offset++;
				continue;
			}
			if (offset + 1 < query.size() && query[offset] == '-' && query[offset + 1] == '-') {
				offset += 2;
				while (offset < query.size() && query[offset] != '\n') {
					offset++;
				}
				continue;
			}
			if (offset + 1 < query.size() && query[offset] == '/' && query[offset + 1] == '*') {
				auto end = query.find("*/", offset + 2);
				if (end == string::npos) {
					return query.size();
				}
				offset = end + 2;
				continue;
			}
			break;
		}
		return offset;
	};
	auto read_identifier = [&](idx_t offset, idx_t &end) {
		if (offset >= query.size()) {
			return false;
		}
		if (query[offset] == '`' || query[offset] == '"') {
			auto quote = query[offset++];
			while (offset < query.size()) {
				if (query[offset] != quote) {
					offset++;
					continue;
				}
				if (offset + 1 < query.size() && query[offset + 1] == quote) {
					offset += 2;
					continue;
				}
				end = offset + 1;
				return true;
			}
			return false;
		}
		if (!(std::isalpha(static_cast<unsigned char>(query[offset])) || query[offset] == '_')) {
			return false;
		}
		offset++;
		while (offset < query.size() &&
		       (std::isalnum(static_cast<unsigned char>(query[offset])) || query[offset] == '_')) {
			offset++;
		}
		end = offset;
		return true;
	};
	auto next_item = [&](idx_t offset) -> idx_t {
		idx_t parentheses = 0;
		idx_t brackets = 0;
		idx_t braces = 0;
		char quote = '\0';
		bool line_comment = false;
		bool block_comment = false;
		for (; offset < query.size(); offset++) {
			auto character = query[offset];
			auto next = offset + 1 < query.size() ? query[offset + 1] : '\0';
			if (line_comment) {
				line_comment = character != '\n';
				continue;
			}
			if (block_comment) {
				if (character == '*' && next == '/') {
					block_comment = false;
					offset++;
				}
				continue;
			}
			if (quote) {
				if (character == quote) {
					if (next == quote) {
						offset++;
					} else {
						quote = '\0';
					}
				}
				continue;
			}
			if (character == '-' && next == '-') {
				line_comment = true;
				offset++;
				continue;
			}
			if (character == '/' && next == '*') {
				block_comment = true;
				offset++;
				continue;
			}
			if (character == '\'' || character == '"' || character == '`') {
				quote = character;
				continue;
			}
			switch (character) {
			case '(':
				parentheses++;
				break;
			case ')':
				if (parentheses > 0) {
					parentheses--;
				}
				break;
			case '[':
				brackets++;
				break;
			case ']':
				if (brackets > 0) {
					brackets--;
				}
				break;
			case '{':
				braces++;
				break;
			case '}':
				if (braces > 0) {
					braces--;
				}
				break;
			case ',':
				if (parentheses == 0 && brackets == 0 && braces == 0) {
					return offset + 1;
				}
				break;
			default:
				break;
			}
		}
		return query.size();
	};

	idx_t mutation_offset = DConstants::INVALID_INDEX;
	char quote = '\0';
	bool line_comment = false;
	bool block_comment = false;
	for (idx_t index = 0; index < query.size(); index++) {
		auto character = query[index];
		auto next = index + 1 < query.size() ? query[index + 1] : '\0';
		if (line_comment) {
			line_comment = character != '\n';
			continue;
		}
		if (block_comment) {
			if (character == '*' && next == '/') {
				block_comment = false;
				index++;
			}
			continue;
		}
		if (quote) {
			if (character == quote) {
				if (next == quote) {
					index++;
				} else {
					quote = '\0';
				}
			}
			continue;
		}
		if (character == '-' && next == '-') {
			line_comment = true;
			index++;
			continue;
		}
		if (character == '/' && next == '*') {
			block_comment = true;
			index++;
			continue;
		}
		if (character == '\'' || character == '"' || character == '`') {
			quote = character;
			continue;
		}
		if (!(std::isalpha(static_cast<unsigned char>(character)) || character == '_')) {
			continue;
		}
		auto word_start = index;
		while (index + 1 < query.size() &&
		       (std::isalnum(static_cast<unsigned char>(query[index + 1])) || query[index + 1] == '_')) {
			index++;
		}
		auto word = query.substr(word_start, index - word_start + 1);
		if (StringUtil::CIEquals(word, "SET") || StringUtil::CIEquals(word, "REMOVE")) {
			mutation_offset = index + 1;
			break;
		}
	}
	if (mutation_offset == DConstants::INVALID_INDEX) {
		return result;
	}

	for (auto item = mutation_offset; item < query.size();) {
		auto cursor = skip_trivia(item);
		idx_t identifier_end;
		if (!read_identifier(cursor, identifier_end)) {
			break;
		}
		cursor = skip_trivia(identifier_end);
		if (cursor >= query.size() || query[cursor] != ':') {
			item = next_item(cursor);
			continue;
		}
		cursor = skip_trivia(cursor + 1);
		if (!read_identifier(cursor, identifier_end)) {
			item = next_item(cursor);
			continue;
		}
		cursor = identifier_end;
		while (true) {
			auto colon = skip_trivia(cursor);
			if (colon >= query.size() || query[colon] != ':') {
				break;
			}
			auto label_start = skip_trivia(colon + 1);
			idx_t label_end;
			if (!read_identifier(label_start, label_end)) {
				break;
			}
			for (idx_t index = colon; index < label_end; index++) {
				if (result[index] != '\n' && result[index] != '\r') {
					result[index] = ' ';
				}
			}
			cursor = label_end;
		}
		item = next_item(cursor);
	}
	return result;
}

class CopyGraphParser {
public:
	explicit CopyGraphParser(const string &query_p) : query(query_p) {
	}

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
		ExpectKeyword("GRAPH");
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
		return make_shared_ptr<GqlCopyGraphStatement>(source, std::move(graph_name), std::move(vertex_path),
		                                              std::move(edge_path), validate);
	}

private:
	void SkipWhitespace() {
		offset = SkipGqlTrivia(query, offset);
	}

	bool AtEnd() {
		SkipWhitespace();
		return offset == query.size();
	}

	[[noreturn]] void Error(const string &message) const {
		throw ParserException("COPY GRAPH parser error at byte %llu: %s", static_cast<unsigned long long>(offset),
		                      message);
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
		if (end < query.size() && (std::isalnum(static_cast<unsigned char>(query[end])) || query[end] == '_')) {
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
		    !(std::isalpha(static_cast<unsigned char>(query[offset])) || query[offset] == '_')) {
			Error("expected a regular graph name");
		}
		offset++;
		while (offset < query.size() &&
		       (std::isalnum(static_cast<unsigned char>(query[offset])) || query[offset] == '_')) {
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
	explicit MergeParser(const string &query_p) : query(query_p) {
	}

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
		offset = SkipGqlTrivia(query, offset);
	}

	bool AtEnd() {
		SkipWhitespace();
		return offset == query.size();
	}

	[[noreturn]] void Error(const string &message) const {
		throw ParserException("MERGE parser error at byte %llu: %s", static_cast<unsigned long long>(offset), message);
	}

	bool ConsumeKeyword(const string &keyword) {
		SkipWhitespace();
		if (offset + keyword.size() > query.size() ||
		    !StringUtil::CIEquals(query.substr(offset, keyword.size()), keyword)) {
			return false;
		}
		auto end = offset + keyword.size();
		if (end < query.size() && (std::isalnum(static_cast<unsigned char>(query[end])) || query[end] == '_')) {
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
		       (std::isalpha(static_cast<unsigned char>(query[offset])) || query[offset] == '_');
	}

	GqlIdentifier ParseIdentifier() {
		SkipWhitespace();
		auto start = offset;
		if (!PeekIdentifier()) {
			Error("expected a regular identifier");
		}
		offset++;
		while (offset < query.size() &&
		       (std::isalnum(static_cast<unsigned char>(query[offset])) || query[offset] == '_')) {
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
		if (offset < query.size() && (query[offset] == '+' || query[offset] == '-')) {
			offset++;
		}
		auto digits_start = offset;
		while (offset < query.size() && std::isdigit(static_cast<unsigned char>(query[offset]))) {
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
			while (offset < query.size() && std::isdigit(static_cast<unsigned char>(query[offset]))) {
				offset++;
			}
			if (fraction_start == offset) {
				Error("expected digits after decimal point");
			}
		}
		if (offset < query.size() && (query[offset] == 'e' || query[offset] == 'E')) {
			approximate = true;
			offset++;
			if (offset < query.size() && (query[offset] == '+' || query[offset] == '-')) {
				offset++;
			}
			auto exponent_start = offset;
			while (offset < query.size() && std::isdigit(static_cast<unsigned char>(query[offset]))) {
				offset++;
			}
			if (exponent_start == offset) {
				Error("expected exponent digits");
			}
		}
		result.type = approximate ? GqlLiteralType::DOUBLE
		              : decimal   ? GqlLiteralType::DECIMAL
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

static void ValidateExecutionHint(const string &query) {
	idx_t offset;
	if (StartsWithGqlKeywords(query, {"OPTIONAL", "MATCH"}, &offset)) {
		// Offset already follows MATCH.
	} else if (StartsWithGqlKeywords(query, {"MATCH"}, &offset)) {
		// Offset already follows MATCH.
	} else {
		return;
	}
	while (offset < query.size() && std::isspace(static_cast<unsigned char>(query[offset]))) {
		offset++;
	}
	if (offset + 3 > query.size() || query.compare(offset, 3, "/*+") != 0) {
		return;
	}
	auto end = query.find("*/", offset + 3);
	if (end == string::npos) {
		throw ParserException("Unterminated GQL execution hint");
	}
	auto hint = query.substr(offset + 3, end - offset - 3);
	StringUtil::Trim(hint);
	hint = StringUtil::Upper(hint);
	if (hint == "CSR" || hint == "GQL_CSR") {
		throw ParserException("CSR is reserved for graph algorithms invoked with "
		                      "CALL; MATCH always uses native execution");
	}
	if (hint == "NATIVE" || hint == "GQL_NATIVE") {
		return;
	}
	throw ParserException("Unknown GQL execution hint '%s'; expected NATIVE", hint);
}

ParserExtensionParseResult GqlParse(ParserExtensionInfo *, const string &query) {
	if (!StartsWithGqlCommand(query)) {
		return ParserExtensionParseResult();
	}

	auto gql_query = StripTerminator(query);
	if (StartsWithGqlKeywords(gql_query, {"COPY", "GRAPH"})) {
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
	auto parser_query = RewriteCallNamedArgumentSyntax(
	    RewriteChainedLabelSyntax(RewritePropertyMapExpressionSyntax(RewritePropertyMapMergeSyntax(gql_query))));
	antlr4::ANTLRInputStream input(parser_query);
	GQLLexer lexer(&input);
	antlr4::CommonTokenStream tokens(&lexer);
	GQLParser parser(&tokens);
	GqlErrorListener errors(parser_query);
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
	ValidateExecutionHint(gql_query);
	GqlTransformer transformer(gql_query);
	parse_data->statement = transformer.Transform(*tree);
	if (!parse_data->statement) {
		throw InternalException("GQL transformer returned no statement");
	}
	return ParserExtensionParseResult(std::move(parse_data));
}

static string QuoteSqlIdentifier(const string &identifier) {
	string result = "\"";
	for (auto character : identifier) {
		if (character == '"') {
			result += '"';
		}
		result += character;
	}
	result += '"';
	return result;
}

static string QuoteSqlString(const string &value) {
	string result = "'";
	for (auto character : value) {
		if (character == '\'') {
			result += '\'';
		}
		result += character;
	}
	result += '\'';
	return result;
}

static void AppendSqlStringList(string &result, const vector<string> &values) {
	result += "[";
	for (idx_t index = 0; index < values.size(); index++) {
		if (index > 0) {
			result += ", ";
		}
		result += QuoteSqlString(values[index]);
	}
	result += "]";
}

static string LowerAlgorithmCall(const GqlCallStatement &call) {
	string result = "SELECT ";
	if (call.distinct) {
		result += "DISTINCT ";
	}
	for (idx_t index = 0; index < call.projections.size(); index++) {
		if (index > 0) {
			result += ", ";
		}
		const auto &projection = call.projections[index];
		result += QuoteSqlIdentifier(projection.expression->variable.value);
		if (!projection.alias.IsEmpty()) {
			result += " AS " + QuoteSqlIdentifier(projection.alias.value);
		}
	}
	result += " FROM (SELECT ";
	for (idx_t index = 0; index < call.yield_items.size(); index++) {
		if (index > 0) {
			result += ", ";
		}
		const auto &yield_item = call.yield_items[index];
		result += QuoteSqlIdentifier(yield_item.field.value);
		if (!yield_item.alias.IsEmpty()) {
			result += " AS " + QuoteSqlIdentifier(yield_item.alias.value);
		}
	}
	result += " FROM gql_algorithm_result(" + QuoteSqlString(call.procedure_name.value) + ", ";
	vector<string> argument_values;
	vector<string> argument_names;
	for (idx_t index = 0; index < call.arguments.size(); index++) {
		argument_values.push_back(call.arguments[index].value);
		argument_names.push_back(index < call.argument_names.size() && !call.argument_names[index].IsEmpty()
		                             ? call.argument_names[index].value
		                             : string());
	}
	AppendSqlStringList(result, argument_values);
	result += ", [";
	for (idx_t index = 0; index < call.arguments.size(); index++) {
		if (index > 0) {
			result += ", ";
		}
		result += to_string(static_cast<uint8_t>(call.arguments[index].type));
	}
	result += "]::UTINYINT[], ";
	AppendSqlStringList(result, argument_names);
	result += ", [";
	for (idx_t index = 0; index < call.yield_items.size(); index++) {
		if (index > 0) {
			result += ", ";
		}
		result += QuoteSqlString(call.yield_items[index].field.value);
	}
	result += "])) AS __gql_yield";
	if (!call.order_by.empty()) {
		result += " ORDER BY ";
		for (idx_t index = 0; index < call.order_by.size(); index++) {
			if (index > 0) {
				result += ", ";
			}
			const auto &order = call.order_by[index];
			result += QuoteSqlIdentifier(order.expression->variable.value);
			result += order.descending ? " DESC" : " ASC";
			if (order.null_order_specified) {
				result += order.nulls_first ? " NULLS FIRST" : " NULLS LAST";
			}
		}
	}
	if (call.has_limit) {
		result += " LIMIT " + to_string(call.limit);
	}
	if (call.has_offset) {
		result += " OFFSET " + to_string(call.offset);
	}
	return result;
}

struct GqlExplainInput {
	string query;
	ExplainType type = ExplainType::EXPLAIN_STANDARD;
	ExplainFormat format = ExplainFormat::DEFAULT;
};

static idx_t SkipWhitespace(const string &query, idx_t offset) {
	return SkipGqlTrivia(query, offset);
}

static bool ConsumeKeyword(const string &query, idx_t &offset, const string &keyword) {
	if (offset + keyword.size() > query.size()) {
		return false;
	}
	for (idx_t index = 0; index < keyword.size(); index++) {
		auto query_character = query[offset + index];
		auto keyword_character = keyword[index];
		if (query_character >= 'a' && query_character <= 'z') {
			query_character = static_cast<char>(query_character - ('a' - 'A'));
		}
		if (keyword_character >= 'a' && keyword_character <= 'z') {
			keyword_character = static_cast<char>(keyword_character - ('a' - 'A'));
		}
		if (query_character != keyword_character) {
			return false;
		}
	}
	auto end = offset + keyword.size();
	if (end < query.size() && (std::isalnum(static_cast<unsigned char>(query[end])) || query[end] == '_')) {
		return false;
	}
	offset = end;
	return true;
}

static idx_t ExplainOptionsEnd(const string &query, idx_t offset) {
	if (offset >= query.size() || query[offset] != '(') {
		return offset;
	}
	idx_t depth = 0;
	char quote = '\0';
	for (; offset < query.size(); offset++) {
		auto character = query[offset];
		if (quote) {
			if (character != quote) {
				continue;
			}
			if (offset + 1 < query.size() && query[offset + 1] == quote) {
				offset++;
				continue;
			}
			quote = '\0';
			continue;
		}
		if (character == '\'' || character == '"') {
			quote = character;
		} else if (character == '(') {
			depth++;
		} else if (character == ')' && --depth == 0) {
			return offset + 1;
		}
	}
	throw ParserException("Unterminated EXPLAIN options");
}

static bool TryParseGqlExplain(const string &query, ParserOptions &options, GqlExplainInput &result) {
	auto explain_start = SkipWhitespace(query, 0);
	auto inner_offset = explain_start;
	if (!ConsumeKeyword(query, inner_offset, "EXPLAIN")) {
		return false;
	}
	inner_offset = SkipWhitespace(query, inner_offset);
	if (inner_offset < query.size() && query[inner_offset] == '(') {
		inner_offset = ExplainOptionsEnd(query, inner_offset);
	} else {
		if (ConsumeKeyword(query, inner_offset, "ANALYZE")) {
			inner_offset = SkipWhitespace(query, inner_offset);
		}
		if (ConsumeKeyword(query, inner_offset, "VERBOSE")) {
			inner_offset = SkipWhitespace(query, inner_offset);
		}
	}
	inner_offset = SkipWhitespace(query, inner_offset);
	if (inner_offset >= query.size()) {
		throw ParserException("EXPLAIN requires a query");
	}

	auto inner_query = query.substr(inner_offset);
	auto is_match =
	    StartsWithGqlKeywords(inner_query, {"MATCH"}) || StartsWithGqlKeywords(inner_query, {"OPTIONAL", "MATCH"});
	if (!is_match && !StartsWithAlgorithmCall(inner_query)) {
		if (StartsWithGqlCommand(inner_query)) {
			throw NotImplementedException("DuckGQL EXPLAIN currently supports MATCH and CALL algo.* queries");
		}
		return false;
	}

	Parser parser(options);
	parser.ParseQuery(query.substr(explain_start, inner_offset - explain_start) + "SELECT 1");
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::EXPLAIN_STATEMENT) {
		throw InternalException("DuckGQL failed to parse EXPLAIN options");
	}
	auto &explain = parser.statements[0]->Cast<ExplainStatement>();
	result.query = std::move(inner_query);
	result.type = explain.explain_type;
	result.format = explain.explain_format;
	return true;
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
		vector<Value> element_kinds;
		vector<Value> type_names;
		vector<Value> local_aliases;
		vector<Value> source_aliases;
		vector<Value> target_aliases;
		vector<Value> directions;
		vector<Value> label_element_indices;
		vector<Value> label_names;
		vector<Value> property_element_indices;
		vector<Value> property_names;
		vector<Value> property_types;
		vector<Value> property_nullables;
		for (idx_t element_index = 0; element_index < create.schema.elements.size(); element_index++) {
			auto &element = create.schema.elements[element_index];
			element_kinds.emplace_back(element.kind == GqlPatternElementType::VERTEX ? "NODE" : "EDGE");
			type_names.emplace_back(element.type_name.value);
			local_aliases.emplace_back(element.local_alias.value);
			source_aliases.emplace_back(element.source_alias.value);
			target_aliases.emplace_back(element.target_alias.value);
			directions.emplace_back(element.kind == GqlPatternElementType::VERTEX  ? "NONE"
			                        : element.direction == GqlEdgeDirection::RIGHT ? "RIGHT"
			                        : element.direction == GqlEdgeDirection::LEFT  ? "LEFT"
			                                                                       : "ANY");
			for (auto &label : element.labels) {
				label_element_indices.emplace_back(Value::UBIGINT(element_index));
				label_names.emplace_back(label.value);
			}
			for (auto &property : element.properties) {
				property_element_indices.emplace_back(Value::UBIGINT(element_index));
				property_names.emplace_back(property.name.value);
				property_types.emplace_back(property.gql_type);
				property_nullables.emplace_back(property.nullable);
			}
		}
		result.function = GqlCreateGraphFunction();
		result.parameters.emplace_back(create.graph_name.value);
		result.parameters.emplace_back(create.if_not_exists);
		result.parameters.emplace_back(create.schema.kind == GqlGraphSchemaKind::ANY ? "ANY" : "INLINE");
		result.parameters.emplace_back(create.schema.typed);
		result.parameters.emplace_back(Value::LIST(LogicalType::VARCHAR, std::move(element_kinds)));
		result.parameters.emplace_back(Value::LIST(LogicalType::VARCHAR, std::move(type_names)));
		result.parameters.emplace_back(Value::LIST(LogicalType::VARCHAR, std::move(local_aliases)));
		result.parameters.emplace_back(Value::LIST(LogicalType::VARCHAR, std::move(source_aliases)));
		result.parameters.emplace_back(Value::LIST(LogicalType::VARCHAR, std::move(target_aliases)));
		result.parameters.emplace_back(Value::LIST(LogicalType::VARCHAR, std::move(directions)));
		result.parameters.emplace_back(Value::LIST(LogicalType::UBIGINT, std::move(label_element_indices)));
		result.parameters.emplace_back(Value::LIST(LogicalType::VARCHAR, std::move(label_names)));
		result.parameters.emplace_back(Value::LIST(LogicalType::UBIGINT, std::move(property_element_indices)));
		result.parameters.emplace_back(Value::LIST(LogicalType::VARCHAR, std::move(property_names)));
		result.parameters.emplace_back(Value::LIST(LogicalType::VARCHAR, std::move(property_types)));
		result.parameters.emplace_back(Value::LIST(LogicalType::BOOLEAN, std::move(property_nullables)));
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
		throw NotImplementedException("GQL INSERT on native graph tables is not implemented yet");
	case GqlStatementType::MERGE:
		throw InternalException("MERGE requires DuckDB parser-override lowering");
	case GqlStatementType::CALL:
		throw InternalException("GQL CALL pipeline requires DuckDB parser-override lowering");
	case GqlStatementType::MATCH: {
		GqlBinder binder;
		auto &match = statement.Cast<GqlMatchStatement>();
		auto alternatives = binder.BindAlternatives(match);
		if (match.has_mutation) {
			throw NotImplementedException("SET, REMOVE, and DELETE on native graph "
			                              "tables are not implemented yet");
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

ParserOverrideResult GqlParserOverride(ParserExtensionInfo *, const string &query, ParserOptions &options) {
	auto queries = SplitGqlOverrideQueries(query);
	if (queries.size() > 1) {
		try {
			vector<unique_ptr<SQLStatement>> statements;
			for (const auto &single_query : queries) {
				Parser parser(options);
				parser.ParseQuery(single_query);
				for (auto &statement : parser.statements) {
					statements.push_back(std::move(statement));
				}
			}
			return ParserOverrideResult(std::move(statements));
		} catch (std::exception &error) {
			return ParserOverrideResult(error);
		}
	}

	try {
		GqlExplainInput explain;
		if (TryParseGqlExplain(query, options, explain)) {
			auto lowered = GqlParserOverride(nullptr, explain.query, options);
			if (lowered.type != ParserExtensionResultType::PARSE_SUCCESSFUL) {
				return lowered;
			}
			if (lowered.statements.size() != 1 || lowered.statements[0]->type != StatementType::SELECT_STATEMENT) {
				throw NotImplementedException("DuckGQL EXPLAIN requires one read-only query");
			}
			vector<unique_ptr<SQLStatement>> statements;
			auto statement =
			    make_uniq<ExplainStatement>(std::move(lowered.statements[0]), explain.type, explain.format);
			statement->query = query;
			statement->stmt_location = 0;
			statement->stmt_length = query.size();
			statements.push_back(std::move(statement));
			return ParserOverrideResult(std::move(statements));
		}
	} catch (std::exception &error) {
		return ParserOverrideResult(error);
	}

	string rewritten;
	if (RewriteAlgorithmCall(query, rewritten)) {
		Parser parser(options);
		parser.ParseQuery(rewritten);
		for (auto &statement : parser.statements) {
			statement->query = query;
			statement->stmt_location = 0;
			statement->stmt_length = query.size();
		}
		return ParserOverrideResult(std::move(parser.statements));
	}
	if (!StartsWithMergePattern(query) && !StartsWithGqlKeywordsAndCharacter(query, {"INSERT"}, '(') &&
	    !StartsWithGqlKeywords(query, {"MATCH"}) && !StartsWithGqlKeywords(query, {"OPTIONAL", "MATCH"}) &&
	    !StartsWithAlgorithmCall(query)) {
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
		if (gql_ptr->statement->type == GqlStatementType::INSERT) {
			auto statements = GqlLowerInsert(gql_ptr->statement->Cast<GqlInsertStatement>());
			for (auto &statement : statements) {
				statement->query = query;
				statement->stmt_location = 0;
				statement->stmt_length = query.size();
			}
			return ParserOverrideResult(std::move(statements));
		}
		if (gql_ptr->statement->type == GqlStatementType::MERGE) {
			auto statements = GqlLowerMerge(gql_ptr->statement->Cast<GqlMergeStatement>());
			for (auto &statement : statements) {
				statement->query = query;
				statement->stmt_location = 0;
				statement->stmt_length = query.size();
			}
			return ParserOverrideResult(std::move(statements));
		}
		if (gql_ptr->statement->type == GqlStatementType::CALL) {
			auto &call = gql_ptr->statement->Cast<GqlCallStatement>();
			Parser parser(options);
			parser.ParseQuery(LowerAlgorithmCall(call));
			for (auto &statement : parser.statements) {
				statement->query = query;
				statement->stmt_location = 0;
				statement->stmt_length = query.size();
			}
			return ParserOverrideResult(std::move(parser.statements));
		}
		if (gql_ptr->statement->type != GqlStatementType::MATCH) {
			return ParserOverrideResult();
		}
		GqlBinder binder;
		auto &match = gql_ptr->statement->Cast<GqlMatchStatement>();
		auto plans = binder.BindAlternatives(match);
		vector<unique_ptr<SQLStatement>> statements;
		if (match.has_mutation) {
			statements = match.insertion ? GqlLowerMatchInsert(plans) : GqlLowerMutation(plans);
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
