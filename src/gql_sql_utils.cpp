#include "gql_sql_utils.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

string GqlQuoteLiteral(const string &value) {
	return KeywordHelper::WriteQuoted(value, '\'');
}

string GqlQuoteIdentifier(const string &value) {
	return KeywordHelper::WriteQuoted(value, '"');
}

void GqlThrowOnError(const BaseQueryResult &result, const string &context) {
	if (result.HasError()) {
		throw InvalidInputException("%s: %s", context, result.GetError());
	}
}

unique_ptr<GqlQueryResult> GqlQuery(Connection &connection, const string &sql, const string &context) {
	auto result = connection.Query(sql);
	GqlThrowOnError(*result, context);
	return make_uniq<GqlQueryResult>(std::move(result));
}

} // namespace duckdb
