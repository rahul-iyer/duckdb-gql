#include "gql_log10.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"

#include <cmath>

namespace duckdb {

static void Log10(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<double, double>(args.data[0], result, args.size(), [](double value) {
		if (value <= 0) {
			throw InvalidInputException("GQL LOG10: invalid argument for general logarithm function (2201Z)");
		}
		if (!std::isfinite(value)) {
			throw OutOfRangeException("GQL LOG10: numeric value out of range (22003)");
		}
		return std::log10(value);
	});
}

ScalarFunction GqlLog10Function() {
	auto function = ScalarFunction("gql_log10", {LogicalType::DOUBLE}, LogicalType::DOUBLE, Log10);
	function.SetFallible();
	return function;
}

} // namespace duckdb
