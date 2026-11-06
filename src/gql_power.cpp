#include "gql_power.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"

#include <cmath>
#include <type_traits>

namespace duckdb {

template <class EXPONENT>
static void Power(DataChunk &args, ExpressionState &, Vector &result) {
	BinaryExecutor::Execute<double, EXPONENT, double>(
	    args.data[0], args.data[1], result, args.size(), [](double base, EXPONENT exponent) {
		    auto numeric_exponent = static_cast<double>(exponent);
		    if (!std::isfinite(base) || !std::isfinite(numeric_exponent)) {
			    throw OutOfRangeException("GQL POWER: numeric value out of range (22003)");
		    }
		    if ((base == 0 && exponent < 0) || (base < 0 && numeric_exponent != std::trunc(numeric_exponent))) {
			    throw InvalidInputException("GQL POWER: invalid argument for power function (2201F)");
		    }
		    auto value = std::pow(std::abs(base), numeric_exponent);
		    if (!std::isfinite(value)) {
			    throw OutOfRangeException("GQL POWER: numeric value out of range (22003)");
		    }
		    bool odd;
		    if constexpr (std::is_integral<EXPONENT>::value) {
			    // Determine parity before DOUBLE conversion, which rounds large odd integers.
			    odd = exponent % 2 != 0;
		    } else {
			    odd = std::fmod(exponent, 2.0) != 0;
		    }
		    return base < 0 && odd ? -value : value;
	    });
}

ScalarFunctionSet GqlPowerFunction() {
	ScalarFunctionSet functions("gql_power");
	auto integers =
	    ScalarFunction("gql_power", {LogicalType::DOUBLE, LogicalType::BIGINT}, LogicalType::DOUBLE, Power<int64_t>);
	integers.SetFallible();
	functions.AddFunction(std::move(integers));
	auto approximate =
	    ScalarFunction("gql_power", {LogicalType::DOUBLE, LogicalType::DOUBLE}, LogicalType::DOUBLE, Power<double>);
	approximate.SetFallible();
	functions.AddFunction(std::move(approximate));
	return functions;
}

} // namespace duckdb
