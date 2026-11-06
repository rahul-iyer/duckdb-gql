#include "gql_power.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"

#include <cmath>
#include <type_traits>

namespace duckdb {

static double CheckedPower(double base, double exponent, bool integral, bool odd) {
	if (!std::isfinite(base) || !std::isfinite(exponent)) {
		throw OutOfRangeException("GQL POWER: numeric value out of range (22003)");
	}
	if ((base == 0 && exponent < 0) || (base < 0 && !integral)) {
		throw InvalidInputException("GQL POWER: invalid argument for power function (2201F)");
	}
	auto value = std::pow(std::abs(base), exponent);
	if (!std::isfinite(value)) {
		throw OutOfRangeException("GQL POWER: numeric value out of range (22003)");
	}
	return base < 0 && odd ? -value : value;
}

template <class EXPONENT>
static void Power(DataChunk &args, ExpressionState &, Vector &result) {
	BinaryExecutor::Execute<double, EXPONENT, double>(
	    args.data[0], args.data[1], result, args.size(), [](double base, EXPONENT exponent) {
		    auto numeric_exponent = static_cast<double>(exponent);
		    bool odd;
		    if constexpr (std::is_integral<EXPONENT>::value) {
			    // Determine parity before DOUBLE conversion, which rounds large odd integers.
			    odd = exponent % 2 != 0;
		    } else {
			    odd = std::fmod(exponent, 2.0) != 0;
		    }
		    return CheckedPower(base, numeric_exponent, numeric_exponent == std::trunc(numeric_exponent), odd);
	    });
}

template <class EXPONENT>
static void DecimalPower(DataChunk &args, ExpressionState &, Vector &result) {
	auto divisor = Hugeint::POWERS_OF_TEN[DecimalType::GetScale(args.data[1].GetType())];
	BinaryExecutor::Execute<double, EXPONENT, double>(
	    args.data[0], args.data[1], result, args.size(), [&](double base, EXPONENT exponent) {
		    const hugeint_t exact = exponent;
		    const bool integral = exact % divisor == 0;
		    const bool odd = (exact / divisor) % 2 != 0;
		    auto numeric_exponent = Hugeint::Cast<double>(exact) / Hugeint::Cast<double>(divisor);
		    return CheckedPower(base, numeric_exponent, integral, odd);
	    });
}

static unique_ptr<FunctionData> BindDecimalPower(BindScalarFunctionInput &input) {
	auto &function = input.GetBoundFunction();
	auto type = function.GetLogicalArguments()[1];
	if (type.id() != LogicalTypeId::DECIMAL) {
		function.GetArguments()[1] = LogicalType::DOUBLE;
		function.SetFunctionCallback(Power<double>);
		return nullptr;
	}
	function.GetArguments()[1] = type;
	switch (type.InternalType()) {
	case PhysicalType::INT16:
		function.SetFunctionCallback(DecimalPower<int16_t>);
		break;
	case PhysicalType::INT32:
		function.SetFunctionCallback(DecimalPower<int32_t>);
		break;
	case PhysicalType::INT64:
		function.SetFunctionCallback(DecimalPower<int64_t>);
		break;
	case PhysicalType::INT128:
		function.SetFunctionCallback(DecimalPower<hugeint_t>);
		break;
	default:
		throw InternalException("GQL POWER received an invalid decimal exponent type");
	}
	return nullptr;
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
	auto exact = ScalarFunction("gql_power", {LogicalType::DOUBLE, LogicalTypeId::DECIMAL}, LogicalType::DOUBLE,
	                            nullptr, BindDecimalPower);
	exact.SetFallible();
	functions.AddFunction(std::move(exact));
	return functions;
}

} // namespace duckdb
