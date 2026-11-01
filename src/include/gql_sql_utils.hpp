#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/main/query_result.hpp"

namespace duckdb {

class BaseQueryResult;
class Connection;
// Random access to the small metadata results used by the graph catalog.
// Cache the row view once rather than materializing it for each GetValue call.
class GqlQueryResult {
public:
	explicit GqlQueryResult(unique_ptr<QueryResult> result_p) : result(std::move(result_p)) {
	}
	Value GetValue(idx_t column, idx_t row) {
		if (!rows) {
			rows = make_uniq<ColumnDataRowCollection>(result->Collection().GetRows());
		}
		return rows->GetValue(column, row);
	}
	idx_t RowCount() {
		return result->RowCount();
	}
	const vector<LogicalType> &GetTypes() const {
		return result->GetTypes();
	}
	const vector<Identifier> &GetNames() const {
		return result->GetNames();
	}

private:
	unique_ptr<QueryResult> result;
	unique_ptr<ColumnDataRowCollection> rows;
};

string GqlQuoteLiteral(const string &value);
string GqlQuoteIdentifier(const string &value);
void GqlThrowOnError(const BaseQueryResult &result, const string &context = "DuckGQL internal query failed");
unique_ptr<GqlQueryResult> GqlQuery(Connection &connection, const string &sql,
                                    const string &context = "DuckGQL internal query failed");

} // namespace duckdb
