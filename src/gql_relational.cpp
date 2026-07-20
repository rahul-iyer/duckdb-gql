#include "gql_relational.hpp"

#include "gql_ast.hpp"
#include "gql_ir.hpp"
#include "gql_storage.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"

namespace duckdb {

struct RelationalPatternElement {
	GqlPatternElementType type;
	idx_t binding_index;
	string label;
	bool reverse = false;
};

struct RelationalPattern {
	vector<RelationalPatternElement> elements;
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
	vector<GqlExpressionProgram> projections;
	vector<string> projection_names;
	vector<GqlExpressionProgram> predicates;
	RelationalResultModifiers modifiers;
};

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

static RelationalResultModifiers ReadResultModifiers(TableFunctionBindInput &input, idx_t offset) {
	RelationalResultModifiers result;
	result.optional = input.inputs[offset].GetValue<bool>();
	result.distinct = input.inputs[offset + 1].GetValue<bool>();
	for (const auto index : ReadIndexList(input.inputs[offset + 2])) {
		result.order_indices.push_back(NumericCast<idx_t>(index));
	}
	result.order_descending = ReadBooleanList(input.inputs[offset + 3]);
	result.order_nulls = ReadByteList(input.inputs[offset + 4]);
	result.has_limit = input.inputs[offset + 5].GetValue<bool>();
	result.limit = NumericCast<idx_t>(input.inputs[offset + 6].GetValue<uint64_t>());
	result.has_offset = input.inputs[offset + 7].GetValue<bool>();
	result.offset = NumericCast<idx_t>(input.inputs[offset + 8].GetValue<uint64_t>());
	if (result.order_indices.size() != result.order_descending.size() ||
	    result.order_indices.size() != result.order_nulls.size()) {
		throw BinderException("Invalid GQL result modifiers");
	}
	return result;
}

static void ValidateProgram(const GqlExpressionProgram &program, idx_t variable_count, bool predicate) {
	for (const auto index : program.binding_indices) {
		if (index != NumericLimits<uint64_t>::Maximum() && index >= variable_count) {
			throw BinderException("Invalid GQL MATCH expression variable");
		}
	}
	if (predicate) {
		auto result_type = static_cast<GqlTypeId>(program.result_types[0]);
		if (result_type != GqlTypeId::BOOLEAN && result_type != GqlTypeId::PROPERTY_VALUE) {
			throw BinderException("Invalid GQL MATCH predicate type");
		}
	}
}

static RelationalMatchInput ReadMatchInput(TableFunctionBindInput &input) {
	if (input.inputs.size() != 17) {
		throw BinderException("Invalid GQL relational MATCH input");
	}
	RelationalMatchInput result;
	auto pattern_sizes = ReadIndexList(input.inputs[0]);
	auto element_types = ReadByteList(input.inputs[1]);
	auto binding_indices = ReadIndexList(input.inputs[2]);
	auto labels = ReadStringList(input.inputs[3]);
	auto reverses = ReadBooleanList(input.inputs[4]);
	result.projection_names = ReadStringList(input.inputs[6]);
	for (const auto &program : ListValue::GetChildren(input.inputs[5])) {
		result.projections.push_back(GqlDeserializeExpression(program));
	}
	for (const auto &program : ListValue::GetChildren(input.inputs[7])) {
		result.predicates.push_back(GqlDeserializeExpression(program));
	}
	result.modifiers = ReadResultModifiers(input, 8);
	if (result.projections.empty() || result.projections.size() != result.projection_names.size()) {
		throw BinderException("Invalid GQL MATCH projections");
	}
	if (pattern_sizes.empty() || element_types.size() != binding_indices.size() ||
	    element_types.size() != labels.size() || element_types.size() != reverses.size()) {
		throw BinderException("Invalid GQL MATCH patterns");
	}

	idx_t offset = 0;
	idx_t binding_count = 0;
	for (idx_t pattern_index = 0; pattern_index < pattern_sizes.size(); pattern_index++) {
		auto pattern_size = NumericCast<idx_t>(pattern_sizes[pattern_index]);
		if (pattern_size == 0 || pattern_size % 2 == 0 || offset + pattern_size > element_types.size()) {
			throw BinderException("Invalid GQL fixed MATCH pattern");
		}
		RelationalPattern pattern;
		for (idx_t element_index = 0; element_index < pattern_size; element_index++) {
			auto flat_index = offset + element_index;
			auto type = static_cast<GqlPatternElementType>(element_types[flat_index]);
			if ((element_index % 2 == 0 && type != GqlPatternElementType::VERTEX) ||
			    (element_index % 2 == 1 && type != GqlPatternElementType::EDGE)) {
				throw BinderException("Invalid GQL fixed MATCH topology");
			}
			auto binding_index = NumericCast<idx_t>(binding_indices[flat_index]);
			binding_count = MaxValue<idx_t>(binding_count, binding_index + 1);
			pattern.elements.push_back({type, binding_index, labels[flat_index], reverses[flat_index]});
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
			if (binding_seen[element.binding_index] && result.binding_types[element.binding_index] != element.type) {
				throw BinderException("GQL MATCH binding has incompatible element types");
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
	for (const auto index : result.modifiers.order_indices) {
		if (index >= result.projections.size()) {
			throw BinderException("Invalid GQL ORDER BY projection");
		}
	}
	return result;
}

static RecursiveMatchInput ReadRecursiveMatchInput(TableFunctionBindInput &input) {
	if (input.inputs.size() != 21) {
		throw BinderException("Invalid GQL recursive MATCH input");
	}
	RecursiveMatchInput result;
	result.binding_count = NumericCast<idx_t>(input.inputs[0].GetValue<uint64_t>());
	result.source_binding = NumericCast<idx_t>(input.inputs[1].GetValue<uint64_t>());
	result.edge_binding = NumericCast<idx_t>(input.inputs[2].GetValue<uint64_t>());
	result.target_binding = NumericCast<idx_t>(input.inputs[3].GetValue<uint64_t>());
	result.source_labels = ReadStringList(input.inputs[4]);
	result.edge_labels = ReadStringList(input.inputs[5]);
	result.target_labels = ReadStringList(input.inputs[6]);
	result.reverse = input.inputs[7].GetValue<bool>();
	result.minimum_repetitions = NumericCast<idx_t>(input.inputs[8].GetValue<uint64_t>());
	result.projection_names = ReadStringList(input.inputs[10]);
	for (const auto &program : ListValue::GetChildren(input.inputs[9])) {
		result.projections.push_back(GqlDeserializeExpression(program));
	}
	for (const auto &program : ListValue::GetChildren(input.inputs[11])) {
		result.predicates.push_back(GqlDeserializeExpression(program));
	}
	result.modifiers = ReadResultModifiers(input, 12);
	if (result.binding_count == 0 || result.source_binding >= result.binding_count ||
	    result.edge_binding >= result.binding_count || result.target_binding >= result.binding_count ||
	    result.edge_binding == result.source_binding || result.edge_binding == result.target_binding) {
		throw BinderException("Invalid GQL recursive MATCH binding slots");
	}
	if (result.projections.empty() || result.projections.size() != result.projection_names.size()) {
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

static unique_ptr<ParsedExpression> Column(const string &table, const string &column) {
	return make_uniq<ColumnRefExpression>(column, table);
}

static unique_ptr<ParsedExpression> Constant(Value value) {
	return make_uniq<ConstantExpression>(std::move(value));
}

static unique_ptr<ParsedExpression> Compare(ExpressionType type, unique_ptr<ParsedExpression> left,
                                            unique_ptr<ParsedExpression> right) {
	return make_uniq<ComparisonExpression>(type, std::move(left), std::move(right));
}

static unique_ptr<ParsedExpression> Equal(unique_ptr<ParsedExpression> left, unique_ptr<ParsedExpression> right) {
	return Compare(ExpressionType::COMPARE_EQUAL, std::move(left), std::move(right));
}

static unique_ptr<ParsedExpression> NotEqual(unique_ptr<ParsedExpression> left, unique_ptr<ParsedExpression> right) {
	return Compare(ExpressionType::COMPARE_NOTEQUAL, std::move(left), std::move(right));
}

static unique_ptr<ParsedExpression> And(vector<unique_ptr<ParsedExpression>> expressions) {
	if (expressions.empty()) {
		return nullptr;
	}
	if (expressions.size() == 1) {
		return std::move(expressions[0]);
	}
	return make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(expressions));
}

static unique_ptr<TableRef> Table(const string &name, const string &alias) {
	auto result = make_uniq<BaseTableRef>();
	result->schema_name = "gql_internal";
	result->table_name = name;
	result->alias = alias;
	return std::move(result);
}

static unique_ptr<TableRef> NamedTable(const string &name, const string &alias) {
	auto result = make_uniq<BaseTableRef>();
	result->table_name = name;
	result->alias = alias;
	return std::move(result);
}

static unique_ptr<ParsedExpression> Aliased(unique_ptr<ParsedExpression> expression, const string &alias) {
	expression->SetAlias(alias);
	return expression;
}

static void AppendJoin(unique_ptr<TableRef> &root, unique_ptr<TableRef> right, JoinType type,
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

static void CollectProperties(const GqlExpressionProgram &program, unordered_map<string, string> &aliases) {
	for (idx_t node = 0; node < program.node_types.size(); node++) {
		if (static_cast<GqlExpressionType>(program.node_types[node]) != GqlExpressionType::PROPERTY_REFERENCE) {
			continue;
		}
		auto key = PropertyKey(program.binding_indices[node], program.properties[node]);
		if (aliases.find(key) == aliases.end()) {
			aliases.emplace(std::move(key), "gql_op_" + to_string(aliases.size()));
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

static string PropertyMember(GqlTypeId type) {
	switch (type) {
	case GqlTypeId::BOOLEAN:
		return "bool_value";
	case GqlTypeId::INTEGER:
		return "int_value";
	case GqlTypeId::DECIMAL:
		return "decimal_value";
	case GqlTypeId::DOUBLE:
		return "double_value";
	case GqlTypeId::STRING:
		return "string_value";
	case GqlTypeId::NULL_VALUE:
	case GqlTypeId::ELEMENT_ID:
	case GqlTypeId::PROPERTY_VALUE:
	case GqlTypeId::UNKNOWN:
	case GqlTypeId::NODE:
	case GqlTypeId::EDGE:
	case GqlTypeId::PATH:
		break;
	}
	return string();
}

static unique_ptr<ParsedExpression> Function(const string &name, vector<unique_ptr<ParsedExpression>> arguments) {
	return make_uniq<FunctionExpression>(name, std::move(arguments));
}

static unique_ptr<ParsedExpression> PropertyTypeName(const ParsedExpression &value) {
	static const pair<const char *, const char *> TYPE_NAMES[] = {{"bool_value", "BOOLEAN"},
	                                                              {"int_value", "BIGINT"},
	                                                              {"uint_value", "UBIGINT"},
	                                                              {"decimal_value", "DECIMAL"},
	                                                              {"double_value", "DOUBLE"},
	                                                              {"string_value", "VARCHAR"},
	                                                              {"blob_value", "BLOB"},
	                                                              {"date_value", "DATE"},
	                                                              {"time_value", "TIME"},
	                                                              {"timestamp_value", "TIMESTAMP"},
	                                                              {"timestamptz_value", "TIMESTAMPTZ"},
	                                                              {"interval_value", "INTERVAL"}};
	auto result = make_uniq<CaseExpression>();
	for (const auto &entry : TYPE_NAMES) {
		vector<unique_ptr<ParsedExpression>> tag_arguments;
		tag_arguments.push_back(value.Copy());
		CaseCheck check;
		check.when_expr = Equal(Function("union_tag", std::move(tag_arguments)), Constant(Value(entry.first)));
		check.then_expr = Constant(Value(entry.second));
		result->case_checks.push_back(std::move(check));
	}
	result->else_expr = Constant(Value("PROPERTY_VALUE"));
	return std::move(result);
}

static unique_ptr<ParsedExpression> BooleanProperty(unique_ptr<ParsedExpression> value) {
	auto result = make_uniq<CaseExpression>();
	CaseCheck missing;
	missing.when_expr = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NULL, value->Copy());
	missing.then_expr = Constant(Value());
	result->case_checks.push_back(std::move(missing));

	vector<unique_ptr<ParsedExpression>> tag_arguments;
	tag_arguments.push_back(value->Copy());
	CaseCheck boolean;
	boolean.when_expr = Equal(Function("union_tag", std::move(tag_arguments)), Constant(Value("bool_value")));
	vector<unique_ptr<ParsedExpression>> extract_arguments;
	extract_arguments.push_back(value->Copy());
	extract_arguments.push_back(Constant(Value("bool_value")));
	boolean.then_expr = Function("union_extract", std::move(extract_arguments));
	result->case_checks.push_back(std::move(boolean));

	vector<unique_ptr<ParsedExpression>> message_arguments;
	message_arguments.push_back(Constant(Value("GQL predicate expected BOOLEAN, found ")));
	message_arguments.push_back(PropertyTypeName(*value));
	vector<unique_ptr<ParsedExpression>> error_arguments;
	error_arguments.push_back(Function("concat", std::move(message_arguments)));
	result->else_expr = Function("error", std::move(error_arguments));
	return std::move(result);
}

static unique_ptr<ParsedExpression> LowerExpression(const GqlExpressionProgram &program, idx_t &cursor,
                                                    const unordered_map<string, string> &property_aliases,
                                                    GqlTypeId desired_type = GqlTypeId::UNKNOWN) {
	if (cursor >= program.node_types.size()) {
		throw InternalException("Truncated GQL expression program");
	}
	auto node = cursor++;
	auto expression_type = static_cast<GqlExpressionType>(program.node_types[node]);
	auto operation = program.operators[node];
	switch (expression_type) {
	case GqlExpressionType::LITERAL:
		return Constant(LiteralValue(static_cast<GqlLiteralType>(operation), program.values[node]));
	case GqlExpressionType::VARIABLE_REFERENCE:
		return Column("gql_object_" + to_string(program.binding_indices[node]), "object_id");
	case GqlExpressionType::PROPERTY_REFERENCE: {
		auto receiver = LowerExpression(program, cursor, property_aliases);
		(void)receiver;
		auto key = PropertyKey(program.binding_indices[node], program.properties[node]);
		auto entry = property_aliases.find(key);
		if (entry == property_aliases.end()) {
			throw InternalException("GQL property join was not created");
		}
		auto value = Column(entry->second, "value");
		if (desired_type == GqlTypeId::PROPERTY_VALUE) {
			return value;
		}
		auto member = PropertyMember(desired_type);
		if (!member.empty()) {
			if (desired_type == GqlTypeId::BOOLEAN) {
				return BooleanProperty(std::move(value));
			}
			vector<unique_ptr<ParsedExpression>> arguments;
			arguments.push_back(std::move(value));
			arguments.push_back(Constant(Value(member)));
			return make_uniq<FunctionExpression>("union_extract", std::move(arguments));
		}
		return make_uniq<CastExpression>(LogicalType::VARIANT(), std::move(value));
	}
	case GqlExpressionType::ELEMENT_ID:
		return LowerExpression(program, cursor, property_aliases);
	case GqlExpressionType::FUNCTION: {
		auto name = program.values[node];
		auto lowered_name = name == "count" && program.child_counts[node] == 0 ? "count_star" : name;
		vector<unique_ptr<ParsedExpression>> arguments;
		for (idx_t child = 0; child < program.child_counts[node]; child++) {
			auto desired = GqlTypeId::UNKNOWN;
			if (name == "lower" || name == "upper" || name == "trim" || name == "ltrim" || name == "rtrim" ||
			    name == "left" || name == "right" || name == "char_length" || name == "length" ||
			    name == "nfc_normalize") {
				desired = child == 0 ? GqlTypeId::STRING : GqlTypeId::INTEGER;
			}
			arguments.push_back(LowerExpression(program, cursor, property_aliases, desired));
		}
		auto result = make_uniq<FunctionExpression>(lowered_name, std::move(arguments));
		result->distinct = program.distinct[node];
		return std::move(result);
	}
	case GqlExpressionType::UNARY: {
		auto input = LowerExpression(program, cursor, property_aliases);
		auto unary = static_cast<GqlUnaryOperator>(operation);
		if (unary == GqlUnaryOperator::NOT) {
			return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(input));
		}
		vector<unique_ptr<ParsedExpression>> children;
		children.push_back(std::move(input));
		auto result = make_uniq<FunctionExpression>(unary == GqlUnaryOperator::PLUS ? "+" : "-", std::move(children));
		result->is_operator = true;
		return std::move(result);
	}
	case GqlExpressionType::IS_NULL: {
		auto input = LowerExpression(program, cursor, property_aliases, GqlTypeId::PROPERTY_VALUE);
		return make_uniq<OperatorExpression>(
		    operation ? ExpressionType::OPERATOR_IS_NOT_NULL : ExpressionType::OPERATOR_IS_NULL, std::move(input));
	}
	case GqlExpressionType::BINARY: {
		auto left_root = cursor;
		auto right_root = ExpressionEnd(program, left_root);
		auto left_type = static_cast<GqlTypeId>(program.result_types[left_root]);
		auto right_type = static_cast<GqlTypeId>(program.result_types[right_root]);
		auto left_desired = left_type == GqlTypeId::PROPERTY_VALUE ? right_type : GqlTypeId::UNKNOWN;
		auto right_desired = right_type == GqlTypeId::PROPERTY_VALUE ? left_type : GqlTypeId::UNKNOWN;
		auto left = LowerExpression(program, cursor, property_aliases, left_desired);
		auto right = LowerExpression(program, cursor, property_aliases, right_desired);
		auto binary = static_cast<GqlBinaryOperator>(operation);
		if (binary == GqlBinaryOperator::AND || binary == GqlBinaryOperator::OR) {
			return make_uniq<ConjunctionExpression>(binary == GqlBinaryOperator::AND ? ExpressionType::CONJUNCTION_AND
			                                                                         : ExpressionType::CONJUNCTION_OR,
			                                        std::move(left), std::move(right));
		}
		if (binary == GqlBinaryOperator::XOR) {
			auto left_copy = left->Copy();
			auto right_copy = right->Copy();
			auto not_right = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(right));
			auto not_left = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(left_copy));
			auto left_branch = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(left),
			                                                    std::move(not_right));
			auto right_branch = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(not_left),
			                                                     std::move(right_copy));
			return make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_OR, std::move(left_branch),
			                                        std::move(right_branch));
		}
		if (binary == GqlBinaryOperator::MULTIPLY || binary == GqlBinaryOperator::DIVIDE ||
		    binary == GqlBinaryOperator::ADD || binary == GqlBinaryOperator::SUBTRACT) {
			vector<unique_ptr<ParsedExpression>> children;
			children.push_back(std::move(left));
			children.push_back(std::move(right));
			auto result = make_uniq<FunctionExpression>(ArithmeticName(binary), std::move(children));
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

static unique_ptr<ParsedExpression> LowerExpression(const GqlExpressionProgram &program,
                                                    const unordered_map<string, string> &property_aliases,
                                                    GqlTypeId desired_type = GqlTypeId::UNKNOWN) {
	idx_t cursor = 0;
	auto result = LowerExpression(program, cursor, property_aliases, desired_type);
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

static void AppendProjections(SelectNode &select, const vector<GqlExpressionProgram> &projections,
                              const vector<string> &projection_names,
                              const unordered_map<string, string> &property_aliases) {
	bool has_aggregate = false;
	for (const auto &projection : projections) {
		has_aggregate = has_aggregate || ContainsAggregate(projection);
	}
	GroupingSet grouping_set;
	for (idx_t index = 0; index < projections.size(); index++) {
		auto mutation_value = StringUtil::StartsWith(projection_names[index], "gql_mutation_value_");
		auto desired_type = mutation_value ? GqlTypeId::PROPERTY_VALUE : GqlTypeId::UNKNOWN;
		auto expression = LowerExpression(projections[index], property_aliases, desired_type);
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

static void AppendOrderExpression(OrderModifier &order, const RelationalResultModifiers &modifiers, idx_t index,
                                  unique_ptr<ParsedExpression> expression) {
	auto null_order = OrderByNullType::ORDER_DEFAULT;
	if (modifiers.order_nulls[index] == 1) {
		null_order = OrderByNullType::NULLS_FIRST;
	} else if (modifiers.order_nulls[index] == 2) {
		null_order = OrderByNullType::NULLS_LAST;
	}
	order.orders.emplace_back(modifiers.order_descending[index] ? OrderType::DESCENDING : OrderType::ASCENDING,
	                          null_order, std::move(expression));
}

static void AppendResultModifiers(SelectNode &select, const vector<GqlExpressionProgram> &projections,
                                  const vector<string> &projection_names, const RelationalResultModifiers &modifiers) {
	if (modifiers.distinct) {
		select.modifiers.push_back(make_uniq<DistinctModifier>());
	}
	if (!modifiers.order_indices.empty()) {
		auto order = make_uniq<OrderModifier>();
		for (idx_t index = 0; index < modifiers.order_indices.size(); index++) {
			auto projection_index = modifiers.order_indices[index];
			unique_ptr<ParsedExpression> column = make_uniq<ColumnRefExpression>(projection_names[projection_index]);
			auto result_type = static_cast<GqlTypeId>(projections[projection_index].result_types[0]);
			if (result_type == GqlTypeId::PROPERTY_VALUE) {
				// DuckDB VARIANT is intentionally not directly orderable. ANY graphs do not
				// provide a static property type, so use its scalar rendering as a stable
				// native sort key instead of allowing Top-N to raise an internal error.
				column = make_uniq<CastExpression>(LogicalType::VARCHAR, std::move(column));
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

static unique_ptr<TableRef> FinalizeMatchSelect(unique_ptr<SelectNode> select,
                                                const vector<GqlExpressionProgram> &projections,
                                                const vector<string> &projection_names,
                                                const RelationalResultModifiers &modifiers) {
	if (!modifiers.optional) {
		AppendResultModifiers(*select, projections, projection_names, modifiers);
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
		from = make_uniq<SubqueryRef>(std::move(seed_statement), "gql_optional_seed");
	}
	vector<unique_ptr<ParsedExpression>> conditions;
	conditions.push_back(Constant(Value(true)));
	AppendJoin(from, make_uniq<SubqueryRef>(std::move(match_statement), "gql_optional_result"), JoinType::LEFT,
	           std::move(conditions));
	auto optional = make_uniq<SelectNode>();
	optional->from_table = std::move(from);
	for (const auto &name : projection_names) {
		optional->select_list.push_back(Aliased(Column("gql_optional_result", name), name));
	}
	AppendResultModifiers(*optional, projections, projection_names, modifiers);
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(optional);
	return make_uniq<SubqueryRef>(std::move(statement));
}

static void AppendLabelJoins(unique_ptr<TableRef> &from, const vector<string> &labels, const string &graph_alias,
                             const string &object_alias, const string &alias_prefix) {
	for (idx_t index = 0; index < labels.size(); index++) {
		auto suffix = alias_prefix + "_" + to_string(index);
		auto object_label_alias = "gql_object_label_" + suffix;
		auto label_alias = "gql_label_" + suffix;
		vector<unique_ptr<ParsedExpression>> object_label_conditions;
		object_label_conditions.push_back(
		    Equal(Column(object_label_alias, "graph_id"), Column(graph_alias, "graph_id")));
		object_label_conditions.push_back(
		    Equal(Column(object_label_alias, "object_id"), Column(object_alias, "object_id")));
		AppendJoin(from, Table("object_labels", object_label_alias), JoinType::INNER,
		           std::move(object_label_conditions));

		vector<unique_ptr<ParsedExpression>> label_conditions;
		label_conditions.push_back(Equal(Column(label_alias, "graph_id"), Column(graph_alias, "graph_id")));
		label_conditions.push_back(Equal(Column(label_alias, "label_id"), Column(object_label_alias, "label_id")));
		label_conditions.push_back(Equal(Column(label_alias, "label_name"), Constant(Value(labels[index]))));
		AppendJoin(from, Table("labels", label_alias), JoinType::INNER, std::move(label_conditions));
	}
}

static unique_ptr<TableRef> RecursiveMatchBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto graph_name = GqlGetSelectedGraph(context);
	if (graph_name.empty()) {
		throw InvalidInputException("No graph selected; use SESSION SET GRAPH before MATCH");
	}
	auto match = ReadRecursiveMatchInput(input);
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

	// The non-recursive term seeds one zero-length path for every vertex in the
	// selected graph.
	unique_ptr<TableRef> anchor_from = Table("graphs", "gql_anchor_graph");
	vector<unique_ptr<ParsedExpression>> anchor_object_conditions;
	anchor_object_conditions.push_back(
	    Equal(Column("gql_anchor_object", "graph_id"), Column("gql_anchor_graph", "graph_id")));
	anchor_object_conditions.push_back(Equal(Column("gql_anchor_object", "kind"), Constant(Value::UTINYINT(0))));
	AppendJoin(anchor_from, Table("objects", "gql_anchor_object"), JoinType::INNER,
	           std::move(anchor_object_conditions));
	auto anchor = make_uniq<SelectNode>();
	anchor->from_table = std::move(anchor_from);
	anchor->where_clause = Equal(Column("gql_anchor_graph", "graph_name"), Constant(Value(std::move(graph_name))));
	anchor->select_list.push_back(Aliased(Column("gql_anchor_graph", "graph_id"), "graph_id"));
	anchor->select_list.push_back(Aliased(Column("gql_anchor_object", "object_id"), "start_id"));
	anchor->select_list.push_back(Aliased(Column("gql_anchor_object", "object_id"), "end_id"));
	vector<Value> empty_edges;
	anchor->select_list.push_back(
	    Aliased(Constant(Value::LIST(LogicalType::UBIGINT, std::move(empty_edges))), "edge_ids"));
	anchor->select_list.push_back(Aliased(Constant(Value::UBIGINT(0)), "depth"));

	// Each recursive step extends the current endpoint by one unused edge.
	// Keeping edge IDs in the recursive state implements the default
	// different-edge (trail) semantics and guarantees termination.
	unique_ptr<TableRef> step_from = NamedTable("gql_recursive_path", "gql_path_previous");
	vector<unique_ptr<ParsedExpression>> edge_conditions;
	edge_conditions.push_back(Equal(Column("gql_path_edge", "graph_id"), Column("gql_path_previous", "graph_id")));
	edge_conditions.push_back(Equal(Column("gql_path_edge", "kind"), Constant(Value::UTINYINT(1))));
	edge_conditions.push_back(Equal(Column("gql_path_edge", match.reverse ? "target_id" : "source_id"),
	                                Column("gql_path_previous", "end_id")));
	AppendJoin(step_from, Table("objects", "gql_path_edge"), JoinType::INNER, std::move(edge_conditions));
	AppendLabelJoins(step_from, match.edge_labels, "gql_path_previous", "gql_path_edge", "recursive_edge");

	vector<unique_ptr<ParsedExpression>> contains_arguments;
	contains_arguments.push_back(Column("gql_path_previous", "edge_ids"));
	contains_arguments.push_back(Column("gql_path_edge", "object_id"));
	auto step = make_uniq<SelectNode>();
	step->from_table = std::move(step_from);
	step->where_clause = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT,
	                                                   Function("list_contains", std::move(contains_arguments)));
	step->select_list.push_back(Aliased(Column("gql_path_previous", "graph_id"), "graph_id"));
	step->select_list.push_back(Aliased(Column("gql_path_previous", "start_id"), "start_id"));
	step->select_list.push_back(Aliased(Column("gql_path_edge", match.reverse ? "source_id" : "target_id"), "end_id"));
	vector<unique_ptr<ParsedExpression>> append_arguments;
	append_arguments.push_back(Column("gql_path_previous", "edge_ids"));
	append_arguments.push_back(Column("gql_path_edge", "object_id"));
	step->select_list.push_back(Aliased(Function("list_append", std::move(append_arguments)), "edge_ids"));
	vector<unique_ptr<ParsedExpression>> depth_arguments;
	depth_arguments.push_back(Column("gql_path_previous", "depth"));
	depth_arguments.push_back(Constant(Value::UBIGINT(1)));
	auto next_depth = Function("+", std::move(depth_arguments));
	next_depth->Cast<FunctionExpression>().is_operator = true;
	step->select_list.push_back(Aliased(std::move(next_depth), "depth"));

	auto recursive = make_uniq<RecursiveCTENode>();
	recursive->ctename = "gql_recursive_path";
	// DuckDB's UNION form also prevents the cumulative working table from
	// re-emitting an identical frontier state forever. Distinct path states
	// remain distinct because edge_ids records the complete ordered edge sequence
	// (including parallel-edge identity).
	recursive->union_all = false;
	recursive->aliases = {"graph_id", "start_id", "end_id", "edge_ids", "depth"};
	recursive->left = std::move(anchor);
	recursive->right = std::move(step);
	auto recursive_statement = make_uniq<SelectStatement>();
	recursive_statement->node = std::move(recursive);
	unique_ptr<TableRef> from = make_uniq<SubqueryRef>(std::move(recursive_statement), "gql_path");

	// Bind the path endpoints back to ordinary object rows so the existing
	// expression lowering can be reused.
	vector<unique_ptr<ParsedExpression>> source_conditions;
	source_conditions.push_back(
	    Equal(Column("gql_object_" + to_string(match.source_binding), "graph_id"), Column("gql_path", "graph_id")));
	source_conditions.push_back(
	    Equal(Column("gql_object_" + to_string(match.source_binding), "object_id"), Column("gql_path", "start_id")));
	source_conditions.push_back(
	    Equal(Column("gql_object_" + to_string(match.source_binding), "kind"), Constant(Value::UTINYINT(0))));
	AppendJoin(from, Table("objects", "gql_object_" + to_string(match.source_binding)), JoinType::INNER,
	           std::move(source_conditions));
	if (match.target_binding != match.source_binding) {
		vector<unique_ptr<ParsedExpression>> target_conditions;
		target_conditions.push_back(
		    Equal(Column("gql_object_" + to_string(match.target_binding), "graph_id"), Column("gql_path", "graph_id")));
		target_conditions.push_back(
		    Equal(Column("gql_object_" + to_string(match.target_binding), "object_id"), Column("gql_path", "end_id")));
		target_conditions.push_back(
		    Equal(Column("gql_object_" + to_string(match.target_binding), "kind"), Constant(Value::UTINYINT(0))));
		AppendJoin(from, Table("objects", "gql_object_" + to_string(match.target_binding)), JoinType::INNER,
		           std::move(target_conditions));
	}
	AppendLabelJoins(from, match.source_labels, "gql_path", "gql_object_" + to_string(match.source_binding), "source");
	AppendLabelJoins(from, match.target_labels, "gql_path", "gql_object_" + to_string(match.target_binding), "target");

	unordered_map<string, string> property_aliases;
	for (const auto &program : match.projections) {
		CollectProperties(program, property_aliases);
	}
	for (const auto &program : match.predicates) {
		CollectProperties(program, property_aliases);
	}
	for (const auto &entry : property_aliases) {
		auto separator = entry.first.find('\x1f');
		if (separator == string::npos) {
			throw InternalException("Invalid GQL property join key");
		}
		auto binding_index = std::stoull(entry.first.substr(0, separator));
		auto property = entry.first.substr(separator + 1);
		auto property_alias = entry.second;
		auto key_alias = "gql_pk_" + property_alias.substr(7);
		vector<unique_ptr<ParsedExpression>> key_conditions;
		key_conditions.push_back(Equal(Column(key_alias, "graph_id"), Column("gql_path", "graph_id")));
		key_conditions.push_back(Equal(Column(key_alias, "key_name"), Constant(Value(property))));
		AppendJoin(from, Table("property_keys", key_alias), JoinType::LEFT, std::move(key_conditions));
		vector<unique_ptr<ParsedExpression>> property_conditions;
		property_conditions.push_back(Equal(Column(property_alias, "graph_id"), Column("gql_path", "graph_id")));
		property_conditions.push_back(Equal(Column(property_alias, "key_id"), Column(key_alias, "key_id")));
		property_conditions.push_back(
		    Equal(Column(property_alias, "object_id"), Column("gql_object_" + to_string(binding_index), "object_id")));
		AppendJoin(from, Table("object_properties", property_alias), JoinType::LEFT, std::move(property_conditions));
	}

	vector<unique_ptr<ParsedExpression>> filters;
	filters.push_back(Compare(ExpressionType::COMPARE_GREATERTHANOREQUALTO, Column("gql_path", "depth"),
	                          Constant(Value::UBIGINT(match.minimum_repetitions))));
	if (match.source_binding == match.target_binding) {
		filters.push_back(Equal(Column("gql_path", "start_id"), Column("gql_path", "end_id")));
	}
	for (const auto &predicate : match.predicates) {
		filters.push_back(LowerExpression(predicate, property_aliases, GqlTypeId::BOOLEAN));
	}

	auto select = make_uniq<SelectNode>();
	select->from_table = std::move(from);
	select->where_clause = And(std::move(filters));
	AppendProjections(*select, match.projections, match.projection_names, property_aliases);
	return FinalizeMatchSelect(std::move(select), match.projections, match.projection_names, match.modifiers);
}

static unique_ptr<TableRef> RelationalMatchBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto graph_name = GqlGetSelectedGraph(context);
	if (graph_name.empty()) {
		throw InvalidInputException("No graph selected; use SESSION SET GRAPH before MATCH");
	}
	auto match = ReadMatchInput(input);

	unordered_map<string, string> property_aliases;
	for (const auto &program : match.projections) {
		CollectProperties(program, property_aliases);
	}
	for (const auto &program : match.predicates) {
		CollectProperties(program, property_aliases);
	}

	unique_ptr<TableRef> from = Table("graphs", "gql_graph");
	for (idx_t index = 0; index < match.binding_types.size(); index++) {
		vector<unique_ptr<ParsedExpression>> conditions;
		conditions.push_back(
		    Equal(Column("gql_object_" + to_string(index), "graph_id"), Column("gql_graph", "graph_id")));
		auto kind = match.binding_types[index] == GqlPatternElementType::EDGE ? 1 : 0;
		conditions.push_back(Equal(Column("gql_object_" + to_string(index), "kind"), Constant(Value::UTINYINT(kind))));
		AppendJoin(from, Table("objects", "gql_object_" + to_string(index)), JoinType::INNER, std::move(conditions));
	}

	idx_t occurrence_index = 0;
	for (const auto &pattern : match.patterns) {
		for (const auto &element : pattern.elements) {
			if (!element.label.empty()) {
				auto object_label_alias = "gql_object_label_" + to_string(occurrence_index);
				auto label_alias = "gql_label_" + to_string(occurrence_index);
				vector<unique_ptr<ParsedExpression>> object_label_conditions;
				object_label_conditions.push_back(
				    Equal(Column(object_label_alias, "graph_id"), Column("gql_graph", "graph_id")));
				object_label_conditions.push_back(
				    Equal(Column(object_label_alias, "object_id"),
				          Column("gql_object_" + to_string(element.binding_index), "object_id")));
				AppendJoin(from, Table("object_labels", object_label_alias), JoinType::INNER,
				           std::move(object_label_conditions));

				vector<unique_ptr<ParsedExpression>> label_conditions;
				label_conditions.push_back(Equal(Column(label_alias, "graph_id"), Column("gql_graph", "graph_id")));
				label_conditions.push_back(
				    Equal(Column(label_alias, "label_id"), Column(object_label_alias, "label_id")));
				label_conditions.push_back(Equal(Column(label_alias, "label_name"), Constant(Value(element.label))));
				AppendJoin(from, Table("labels", label_alias), JoinType::INNER, std::move(label_conditions));
			}
			occurrence_index++;
		}
	}

	for (const auto &entry : property_aliases) {
		auto separator = entry.first.find('\x1f');
		if (separator == string::npos) {
			throw InternalException("Invalid GQL property join key");
		}
		auto binding_index = std::stoull(entry.first.substr(0, separator));
		auto property = entry.first.substr(separator + 1);
		auto property_alias = entry.second;
		auto key_alias = "gql_pk_" + property_alias.substr(7);

		vector<unique_ptr<ParsedExpression>> key_conditions;
		key_conditions.push_back(Equal(Column(key_alias, "graph_id"), Column("gql_graph", "graph_id")));
		key_conditions.push_back(Equal(Column(key_alias, "key_name"), Constant(Value(property))));
		AppendJoin(from, Table("property_keys", key_alias), JoinType::LEFT, std::move(key_conditions));

		vector<unique_ptr<ParsedExpression>> property_conditions;
		property_conditions.push_back(Equal(Column(property_alias, "graph_id"), Column("gql_graph", "graph_id")));
		property_conditions.push_back(Equal(Column(property_alias, "key_id"), Column(key_alias, "key_id")));
		property_conditions.push_back(
		    Equal(Column(property_alias, "object_id"), Column("gql_object_" + to_string(binding_index), "object_id")));
		AppendJoin(from, Table("object_properties", property_alias), JoinType::LEFT, std::move(property_conditions));
	}

	vector<unique_ptr<ParsedExpression>> filters;
	filters.push_back(Equal(Column("gql_graph", "graph_name"), Constant(Value(graph_name))));
	vector<idx_t> edge_bindings;
	for (idx_t binding_index = 0; binding_index < match.binding_types.size(); binding_index++) {
		if (match.binding_types[binding_index] == GqlPatternElementType::EDGE) {
			edge_bindings.push_back(binding_index);
		}
	}
	for (idx_t left = 0; left < edge_bindings.size(); left++) {
		for (idx_t right = left + 1; right < edge_bindings.size(); right++) {
			filters.push_back(NotEqual(Column("gql_object_" + to_string(edge_bindings[left]), "object_id"),
			                           Column("gql_object_" + to_string(edge_bindings[right]), "object_id")));
		}
	}
	for (const auto &pattern : match.patterns) {
		for (idx_t element_index = 1; element_index < pattern.elements.size(); element_index += 2) {
			const auto &left = pattern.elements[element_index - 1];
			const auto &edge = pattern.elements[element_index];
			const auto &right = pattern.elements[element_index + 1];
			filters.push_back(
			    Equal(Column("gql_object_" + to_string(edge.binding_index), edge.reverse ? "target_id" : "source_id"),
			          Column("gql_object_" + to_string(left.binding_index), "object_id")));
			filters.push_back(
			    Equal(Column("gql_object_" + to_string(edge.binding_index), edge.reverse ? "source_id" : "target_id"),
			          Column("gql_object_" + to_string(right.binding_index), "object_id")));
		}
	}
	for (const auto &predicate : match.predicates) {
		filters.push_back(LowerExpression(predicate, property_aliases, GqlTypeId::BOOLEAN));
	}

	auto select = make_uniq<SelectNode>();
	select->from_table = std::move(from);
	select->where_clause = And(std::move(filters));
	AppendProjections(*select, match.projections, match.projection_names, property_aliases);
	return FinalizeMatchSelect(std::move(select), match.projections, match.projection_names, match.modifiers);
}

TableFunction GqlRelationalMatchFunction() {
	TableFunction function("gql_match_relational",
	                       {LogicalType::LIST(LogicalType::UBIGINT), LogicalType::LIST(LogicalType::UTINYINT),
	                        LogicalType::LIST(LogicalType::UBIGINT), LogicalType::LIST(LogicalType::VARCHAR),
	                        LogicalType::LIST(LogicalType::BOOLEAN), LogicalType::LIST(GqlExpressionProgramType()),
	                        LogicalType::LIST(LogicalType::VARCHAR), LogicalType::LIST(GqlExpressionProgramType()),
	                        LogicalType::BOOLEAN, LogicalType::BOOLEAN, LogicalType::LIST(LogicalType::UBIGINT),
	                        LogicalType::LIST(LogicalType::BOOLEAN), LogicalType::LIST(LogicalType::UTINYINT),
	                        LogicalType::BOOLEAN, LogicalType::UBIGINT, LogicalType::BOOLEAN, LogicalType::UBIGINT},
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
