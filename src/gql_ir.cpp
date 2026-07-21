#include "gql_ir.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"

namespace duckdb {

static LogicalType PropertyValueType() {
	return LogicalType::UNION({{"bool_value", LogicalType::BOOLEAN},
	                           {"int_value", LogicalType::BIGINT},
	                           {"uint_value", LogicalType::UBIGINT},
	                           {"decimal_value", LogicalType::DECIMAL(38, 18)},
	                           {"double_value", LogicalType::DOUBLE},
	                           {"string_value", LogicalType::VARCHAR},
	                           {"blob_value", LogicalType::BLOB},
	                           {"date_value", LogicalType::DATE},
	                           {"time_value", LogicalType::TIME},
	                           {"timestamp_value", LogicalType::TIMESTAMP},
	                           {"timestamptz_value", LogicalType::TIMESTAMP_TZ},
	                           {"interval_value", LogicalType::INTERVAL}});
}

LogicalType GqlDuckType(const GqlType &type) {
	switch (type.id) {
	case GqlTypeId::NULL_VALUE:
		return LogicalType::SQLNULL;
	case GqlTypeId::BOOLEAN:
		return LogicalType::BOOLEAN;
	case GqlTypeId::INTEGER:
		return LogicalType::BIGINT;
	case GqlTypeId::ELEMENT_ID:
		return LogicalType::UBIGINT;
	case GqlTypeId::DECIMAL:
		return LogicalType::DECIMAL(38, 18);
	case GqlTypeId::DOUBLE:
		return LogicalType::DOUBLE;
	case GqlTypeId::STRING:
		return LogicalType::VARCHAR;
	case GqlTypeId::PROPERTY_VALUE:
		return PropertyValueType();
	case GqlTypeId::UNKNOWN:
	case GqlTypeId::NODE:
	case GqlTypeId::EDGE:
	case GqlTypeId::PATH:
		break;
	}
	throw NotImplementedException("GQL value type cannot be returned by the current backend");
}

LogicalType GqlExpressionProgramType() {
	return LogicalType::STRUCT({{"node_types", LogicalType::LIST(LogicalType::UTINYINT)},
	                            {"result_types", LogicalType::LIST(LogicalType::UTINYINT)},
	                            {"binding_indices", LogicalType::LIST(LogicalType::UBIGINT)},
	                            {"operators", LogicalType::LIST(LogicalType::UTINYINT)},
	                            {"values", LogicalType::LIST(LogicalType::VARCHAR)},
	                            {"properties", LogicalType::LIST(LogicalType::VARCHAR)},
	                            {"child_counts", LogicalType::LIST(LogicalType::UTINYINT)},
	                            {"aggregate", LogicalType::LIST(LogicalType::BOOLEAN)},
	                            {"distinct", LogicalType::LIST(LogicalType::BOOLEAN)}});
}

static void SerializeNode(const GqlBoundExpression &expression, GqlExpressionProgram &program) {
	program.node_types.push_back(static_cast<uint8_t>(expression.expression_type));
	program.result_types.push_back(static_cast<uint8_t>(expression.result_type.id));
	program.binding_indices.push_back(expression.binding_index == DConstants::INVALID_INDEX
	                                      ? NumericLimits<uint64_t>::Maximum()
	                                      : NumericCast<uint64_t>(expression.binding_index));
	uint8_t operation = 0;
	switch (expression.expression_type) {
	case GqlExpressionType::LITERAL:
		operation = static_cast<uint8_t>(expression.literal.type);
		break;
	case GqlExpressionType::UNARY:
		operation = static_cast<uint8_t>(expression.unary_operator);
		break;
	case GqlExpressionType::BINARY:
		operation = static_cast<uint8_t>(expression.binary_operator);
		break;
	case GqlExpressionType::IS_NULL:
		operation = expression.negated ? 1 : 0;
		break;
	case GqlExpressionType::LABELED:
		operation = expression.negated ? 1 : 0;
		break;
	case GqlExpressionType::VARIABLE_REFERENCE:
	case GqlExpressionType::PROPERTY_REFERENCE:
	case GqlExpressionType::ELEMENT_ID:
	case GqlExpressionType::FUNCTION:
		break;
	}
	program.operators.push_back(operation);
	program.values.push_back(expression.expression_type == GqlExpressionType::FUNCTION ? expression.function_name
	                                                                                   : expression.literal.value);
	program.properties.push_back(expression.property);
	program.child_counts.push_back(NumericCast<uint8_t>(expression.arguments.size()));
	program.aggregate.push_back(expression.aggregate);
	program.distinct.push_back(expression.distinct);
	if (expression.left) {
		SerializeNode(*expression.left, program);
	}
	if (expression.right) {
		SerializeNode(*expression.right, program);
	}
	for (const auto &argument : expression.arguments) {
		SerializeNode(*argument, program);
	}
}

template <class T>
static Value NumericList(const vector<T> &input, const LogicalType &type) {
	vector<Value> values;
	values.reserve(input.size());
	for (const auto entry : input) {
		if (type == LogicalType::UTINYINT) {
			values.push_back(Value::UTINYINT(NumericCast<uint8_t>(entry)));
		} else {
			values.push_back(Value::UBIGINT(NumericCast<uint64_t>(entry)));
		}
	}
	return Value::LIST(type, std::move(values));
}

static Value StringList(const vector<string> &input) {
	vector<Value> values;
	values.reserve(input.size());
	for (const auto &entry : input) {
		values.emplace_back(entry);
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(values));
}

Value GqlSerializeExpression(const GqlBoundExpression &expression) {
	GqlExpressionProgram program;
	SerializeNode(expression, program);
	vector<Value> children;
	children.push_back(NumericList(program.node_types, LogicalType::UTINYINT));
	children.push_back(NumericList(program.result_types, LogicalType::UTINYINT));
	children.push_back(NumericList(program.binding_indices, LogicalType::UBIGINT));
	children.push_back(NumericList(program.operators, LogicalType::UTINYINT));
	children.push_back(StringList(program.values));
	children.push_back(StringList(program.properties));
	children.push_back(NumericList(program.child_counts, LogicalType::UTINYINT));
	vector<Value> aggregate;
	vector<Value> distinct;
	for (const auto entry : program.aggregate) {
		aggregate.emplace_back(entry);
	}
	for (const auto entry : program.distinct) {
		distinct.emplace_back(entry);
	}
	children.push_back(Value::LIST(LogicalType::BOOLEAN, std::move(aggregate)));
	children.push_back(Value::LIST(LogicalType::BOOLEAN, std::move(distinct)));
	return Value::STRUCT(GqlExpressionProgramType(), std::move(children));
}

static vector<uint8_t> ReadByteList(const Value &value) {
	vector<uint8_t> result;
	for (const auto &entry : ListValue::GetChildren(value)) {
		result.push_back(entry.GetValue<uint8_t>());
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

static vector<string> ReadStringList(const Value &value) {
	vector<string> result;
	for (const auto &entry : ListValue::GetChildren(value)) {
		result.push_back(entry.GetValue<string>());
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

GqlExpressionProgram GqlDeserializeExpression(const Value &value) {
	if (value.type() != GqlExpressionProgramType()) {
		throw BinderException("Invalid GQL expression program type");
	}
	const auto &children = StructValue::GetChildren(value);
	if (children.size() != 9) {
		throw BinderException("Invalid GQL expression program");
	}
	GqlExpressionProgram result;
	result.node_types = ReadByteList(children[0]);
	result.result_types = ReadByteList(children[1]);
	result.binding_indices = ReadIndexList(children[2]);
	result.operators = ReadByteList(children[3]);
	result.values = ReadStringList(children[4]);
	result.properties = ReadStringList(children[5]);
	result.child_counts = ReadByteList(children[6]);
	result.aggregate = ReadBooleanList(children[7]);
	result.distinct = ReadBooleanList(children[8]);
	auto count = result.node_types.size();
	if (count == 0 || result.result_types.size() != count || result.binding_indices.size() != count ||
	    result.operators.size() != count || result.values.size() != count || result.properties.size() != count ||
	    result.child_counts.size() != count || result.aggregate.size() != count || result.distinct.size() != count) {
		throw BinderException("Invalid GQL expression program vectors");
	}
	return result;
}

} // namespace duckdb
