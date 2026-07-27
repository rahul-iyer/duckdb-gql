#include "gql_relational.hpp"

#include "gql_ast.hpp"
#include "gql_catalog.hpp"
#include "gql_ir.hpp"
#include "gql_optimizer.hpp"
#include "gql_storage.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/common_table_expression_info.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

namespace duckdb {

struct RelationalPatternElement {
	GqlPatternElementType type;
	idx_t binding_index;
	string label;
	bool reverse = false;
	bool quantified = false;
	bool unbounded = false;
	idx_t minimum_repetitions = 1;
	idx_t maximum_repetitions = 1;
};

struct RelationalPattern {
	vector<RelationalPatternElement> elements;
};

struct RelationalMatchStage {
	vector<RelationalPattern> patterns;
};

struct RelationalLogicalNode {
	GqlLogicalOperatorType type;
	idx_t child = DConstants::INVALID_INDEX;
	idx_t right = DConstants::INVALID_INDEX;
	idx_t payload = DConstants::INVALID_INDEX;
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
	vector<RelationalMatchStage> match_stages;
	vector<RelationalLogicalNode> nodes;
	idx_t root = DConstants::INVALID_INDEX;
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

struct RelationalPropertyAccess {
	string table_alias;
	string column_name;
	bool is_list = false;
};

struct RelationalIdentityAccess {
	string table_alias;
	string column_name;
};

using RelationalPropertyMap = unordered_map<string, RelationalPropertyAccess>;

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

static RelationalResultModifiers ReadResultModifiers(TableFunctionBindInput &input, idx_t offset,
                                                     bool includes_optional = true) {
	RelationalResultModifiers result;
	if (includes_optional) {
		result.optional = input.inputs[offset++].GetValue<bool>();
	}
	result.distinct = input.inputs[offset].GetValue<bool>();
	for (const auto index : ReadIndexList(input.inputs[offset + 1])) {
		result.order_indices.push_back(NumericCast<idx_t>(index));
	}
	result.order_descending = ReadBooleanList(input.inputs[offset + 2]);
	result.order_nulls = ReadByteList(input.inputs[offset + 3]);
	result.has_limit = input.inputs[offset + 4].GetValue<bool>();
	result.limit = NumericCast<idx_t>(input.inputs[offset + 5].GetValue<uint64_t>());
	result.has_offset = input.inputs[offset + 6].GetValue<bool>();
	result.offset = NumericCast<idx_t>(input.inputs[offset + 7].GetValue<uint64_t>());
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
	if (input.inputs.size() != 27) {
		throw BinderException("Invalid GQL relational MATCH input");
	}
	auto program_version = input.inputs[0].GetValue<uint8_t>();
	if (program_version != GQL_LOGICAL_PROGRAM_VERSION) {
		throw BinderException("Invalid GQL relational MATCH input");
	}
	RelationalMatchInput result;
	auto pattern_sizes = ReadIndexList(input.inputs[1]);
	auto element_types = ReadByteList(input.inputs[2]);
	auto binding_indices = ReadIndexList(input.inputs[3]);
	auto labels = ReadStringList(input.inputs[4]);
	auto reverses = ReadBooleanList(input.inputs[5]);
	auto quantified = ReadBooleanList(input.inputs[6]);
	auto unbounded = ReadBooleanList(input.inputs[7]);
	auto minimum_repetitions = ReadIndexList(input.inputs[8]);
	auto maximum_repetitions = ReadIndexList(input.inputs[9]);
	auto match_pattern_counts = ReadIndexList(input.inputs[10]);
	auto node_types = ReadByteList(input.inputs[11]);
	auto child_indices = ReadIndexList(input.inputs[12]);
	auto right_indices = ReadIndexList(input.inputs[13]);
	auto payload_indices = ReadIndexList(input.inputs[14]);
	result.root = NumericCast<idx_t>(input.inputs[15].GetValue<uint64_t>());
	result.projection_names = ReadStringList(input.inputs[17]);
	for (const auto &program : ListValue::GetChildren(input.inputs[16])) {
		result.projections.push_back(GqlDeserializeExpression(program));
	}
	for (const auto &program : ListValue::GetChildren(input.inputs[18])) {
		result.predicates.push_back(GqlDeserializeExpression(program));
	}
	result.modifiers = ReadResultModifiers(input, 19, false);
	if (result.projections.empty() || result.projections.size() != result.projection_names.size()) {
		throw BinderException("Invalid GQL MATCH projections");
	}
	if (pattern_sizes.empty() || match_pattern_counts.empty() || element_types.size() != binding_indices.size() ||
	    element_types.size() != labels.size() || element_types.size() != reverses.size() ||
	    element_types.size() != quantified.size() || element_types.size() != unbounded.size() ||
	    element_types.size() != minimum_repetitions.size() || element_types.size() != maximum_repetitions.size() ||
	    node_types.empty() || node_types.size() != child_indices.size() || node_types.size() != right_indices.size() ||
	    node_types.size() != payload_indices.size() || result.root >= node_types.size()) {
		throw BinderException("Invalid GQL MATCH patterns");
	}

	idx_t offset = 0;
	idx_t binding_count = 0;
	vector<RelationalPattern> patterns;
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
			auto minimum = NumericCast<idx_t>(minimum_repetitions[flat_index]);
			auto maximum = NumericCast<idx_t>(maximum_repetitions[flat_index]);
			if (quantified[flat_index] &&
			    (type != GqlPatternElementType::EDGE || (!unbounded[flat_index] && minimum == 0) ||
			     (!unbounded[flat_index] && minimum > maximum))) {
				throw BinderException("Invalid GQL quantified MATCH factor");
			}
			pattern.elements.push_back({type, binding_index, labels[flat_index], reverses[flat_index],
			                            quantified[flat_index], unbounded[flat_index], minimum, maximum});
		}
		patterns.push_back(std::move(pattern));
		offset += pattern_size;
	}
	if (offset != element_types.size()) {
		throw BinderException("Invalid GQL MATCH pattern elements");
	}

	idx_t pattern_offset = 0;
	for (const auto count_value : match_pattern_counts) {
		auto count = NumericCast<idx_t>(count_value);
		if (count == 0 || count > patterns.size() - pattern_offset) {
			throw BinderException("Invalid GQL MATCH stage pattern range");
		}
		RelationalMatchStage stage;
		for (idx_t index = 0; index < count; index++) {
			stage.patterns.push_back(std::move(patterns[pattern_offset++]));
		}
		result.match_stages.push_back(std::move(stage));
	}
	if (pattern_offset != patterns.size()) {
		throw BinderException("Unowned GQL MATCH patterns");
	}

	auto read_node_index = [&](uint64_t value) {
		return value == NumericLimits<uint64_t>::Maximum() ? DConstants::INVALID_INDEX : NumericCast<idx_t>(value);
	};
	for (idx_t index = 0; index < node_types.size(); index++) {
		auto type = static_cast<GqlLogicalOperatorType>(node_types[index]);
		if (type > GqlLogicalOperatorType::PROJECT) {
			throw BinderException("Invalid GQL logical operator type");
		}
		result.nodes.push_back({type, read_node_index(child_indices[index]), read_node_index(right_indices[index]),
		                        read_node_index(payload_indices[index])});
	}

	vector<uint8_t> visit_state(result.nodes.size(), 0);
	vector<bool> used_match_stages(result.match_stages.size(), false);
	vector<bool> used_predicates(result.predicates.size(), false);
	auto validate_node = [&](auto &self, idx_t node_index) -> void {
		if (node_index >= result.nodes.size()) {
			throw BinderException("Invalid GQL logical operator child");
		}
		if (visit_state[node_index] != 0) {
			throw BinderException(visit_state[node_index] == 1 ? "Cyclic GQL logical operator program"
			                                                   : "GQL logical operator program is not a tree");
		}
		visit_state[node_index] = 1;
		const auto &node = result.nodes[node_index];
		switch (node.type) {
		case GqlLogicalOperatorType::UNIT:
			if (node.child != DConstants::INVALID_INDEX || node.right != DConstants::INVALID_INDEX ||
			    node.payload != DConstants::INVALID_INDEX) {
				throw BinderException("Invalid GQL UNIT operator");
			}
			break;
		case GqlLogicalOperatorType::MATCH:
			if (node.child != DConstants::INVALID_INDEX || node.right != DConstants::INVALID_INDEX ||
			    node.payload >= result.match_stages.size() || used_match_stages[node.payload]) {
				throw BinderException("Invalid GQL MATCH operator");
			}
			used_match_stages[node.payload] = true;
			break;
		case GqlLogicalOperatorType::FILTER:
			if (node.child == DConstants::INVALID_INDEX || node.right != DConstants::INVALID_INDEX ||
			    node.payload >= result.predicates.size() || used_predicates[node.payload]) {
				throw BinderException("Invalid GQL FILTER operator");
			}
			used_predicates[node.payload] = true;
			self(self, node.child);
			break;
		case GqlLogicalOperatorType::INNER_APPLY:
			if (node.child == DConstants::INVALID_INDEX || node.right == DConstants::INVALID_INDEX ||
			    node.payload != DConstants::INVALID_INDEX) {
				throw BinderException("Invalid GQL INNER_APPLY operator");
			}
			self(self, node.child);
			self(self, node.right);
			break;
		case GqlLogicalOperatorType::LEFT_APPLY:
			if (node.child == DConstants::INVALID_INDEX || node.right == DConstants::INVALID_INDEX ||
			    node.payload != DConstants::INVALID_INDEX) {
				throw BinderException("Invalid GQL LEFT_APPLY operator");
			}
			self(self, node.child);
			self(self, node.right);
			break;
		case GqlLogicalOperatorType::PROJECT:
		case GqlLogicalOperatorType::CALL:
			throw BinderException("Nested GQL PROJECT operator");
		}
		visit_state[node_index] = 2;
	};
	validate_node(validate_node, result.root);
	for (const auto state : visit_state) {
		if (state != 2) {
			throw BinderException("Unreachable GQL logical operator");
		}
	}
	idx_t unit_index = DConstants::INVALID_INDEX;
	for (idx_t index = 0; index < result.nodes.size(); index++) {
		if (result.nodes[index].type != GqlLogicalOperatorType::UNIT) {
			continue;
		}
		if (unit_index != DConstants::INVALID_INDEX) {
			throw BinderException("Multiple GQL UNIT operators");
		}
		unit_index = index;
	}
	if (unit_index != DConstants::INVALID_INDEX) {
		idx_t leading_optional_inputs = 0;
		for (const auto &node : result.nodes) {
			if (node.type == GqlLogicalOperatorType::LEFT_APPLY && node.child == unit_index) {
				leading_optional_inputs++;
			}
		}
		if (leading_optional_inputs != 1) {
			throw BinderException("GQL UNIT is not a leading LEFT_APPLY input");
		}
	}
	for (const auto used : used_match_stages) {
		if (!used) {
			throw BinderException("Unreachable GQL MATCH stage");
		}
	}
	for (const auto used : used_predicates) {
		if (!used) {
			throw BinderException("Unreachable GQL predicate");
		}
	}

	result.binding_types.resize(binding_count);
	vector<bool> binding_seen(binding_count, false);
	for (const auto &stage : result.match_stages) {
		for (const auto &pattern : stage.patterns) {
			for (const auto &element : pattern.elements) {
				if (binding_seen[element.binding_index] &&
				    result.binding_types[element.binding_index] != element.type) {
					throw BinderException("GQL MATCH binding has incompatible element types");
				}
				result.binding_types[element.binding_index] = element.type;
				binding_seen[element.binding_index] = true;
			}
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

static unique_ptr<TableRef> NamedTable(const string &name, const string &alias) {
	auto result = make_uniq<BaseTableRef>();
	result->table_name = name;
	result->alias = alias;
	return std::move(result);
}

static unique_ptr<TableRef> ElementTable(const GqlElementTableBinding &table, const string &alias) {
	auto result = make_uniq<BaseTableRef>();
	result->catalog_name = table.catalog_name;
	result->schema_name = table.schema_name;
	result->table_name = table.table_name;
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

static const string GQL_LABEL_ACCESS = "\x1egql_label";

static void CollectProperties(const GqlExpressionProgram &program, RelationalPropertyMap &aliases) {
	for (idx_t node = 0; node < program.node_types.size(); node++) {
		if (static_cast<GqlExpressionType>(program.node_types[node]) != GqlExpressionType::PROPERTY_REFERENCE &&
		    static_cast<GqlExpressionType>(program.node_types[node]) != GqlExpressionType::LABELED) {
			continue;
		}
		auto key = PropertyKey(program.binding_indices[node],
		                       static_cast<GqlExpressionType>(program.node_types[node]) == GqlExpressionType::LABELED
		                           ? GQL_LABEL_ACCESS
		                           : program.properties[node]);
		if (aliases.find(key) == aliases.end()) {
			auto alias = "gql_op_" + to_string(aliases.size());
			aliases.emplace(std::move(key), RelationalPropertyAccess {std::move(alias), "value", false});
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
	case GqlExpressionType::LIST_CONSTRUCTOR:
	case GqlExpressionType::RECORD_CONSTRUCTOR:
		throw InternalException("GQL collection constructor reached relational expression lowering");
	case GqlExpressionType::PROPERTY_REFERENCE:
	case GqlExpressionType::ELEMENT_ID:
	case GqlExpressionType::UNARY:
	case GqlExpressionType::IS_NULL:
	case GqlExpressionType::LABELED:
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

static unique_ptr<ParsedExpression> Function(const string &name, vector<unique_ptr<ParsedExpression>> arguments) {
	return make_uniq<FunctionExpression>(name, std::move(arguments));
}

static unique_ptr<ParsedExpression> ElementHasLabel(const string &table_alias, const string &label_column,
                                                    bool label_is_list, const string &label) {
	unique_ptr<ParsedExpression> predicate;
	if (label_is_list) {
		vector<unique_ptr<ParsedExpression>> contains_arguments;
		contains_arguments.push_back(Column(table_alias, label_column));
		contains_arguments.push_back(Constant(Value(label)));
		predicate = Function("list_contains", std::move(contains_arguments));
	} else {
		predicate = Equal(Column(table_alias, label_column), Constant(Value(label)));
	}
	vector<unique_ptr<ParsedExpression>> coalesce_arguments;
	coalesce_arguments.push_back(std::move(predicate));
	coalesce_arguments.push_back(Constant(Value(false)));
	return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_COALESCE, std::move(coalesce_arguments));
}

static unique_ptr<ParsedExpression> LowerExpression(const GqlExpressionProgram &program, idx_t &cursor,
                                                    const RelationalPropertyMap &property_aliases,
                                                    const vector<RelationalIdentityAccess> &identities,
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
	case GqlExpressionType::LIST_CONSTRUCTOR:
	case GqlExpressionType::RECORD_CONSTRUCTOR:
		throw InternalException("GQL collection constructor reached relational expression lowering");
	case GqlExpressionType::VARIABLE_REFERENCE:
		if (program.binding_indices[node] >= identities.size()) {
			throw InternalException("GQL identity binding is missing");
		}
		return Column(identities[program.binding_indices[node]].table_alias,
		              identities[program.binding_indices[node]].column_name);
	case GqlExpressionType::PROPERTY_REFERENCE: {
		auto receiver = LowerExpression(program, cursor, property_aliases, identities);
		(void)receiver;
		auto key = PropertyKey(program.binding_indices[node], program.properties[node]);
		auto entry = property_aliases.find(key);
		if (entry == property_aliases.end()) {
			throw InternalException("GQL property join was not created");
		}
		if (entry->second.column_name.empty()) {
			return Constant(Value());
		}
		return Column(entry->second.table_alias, entry->second.column_name);
	}
	case GqlExpressionType::ELEMENT_ID:
		return LowerExpression(program, cursor, property_aliases, identities);
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
			arguments.push_back(LowerExpression(program, cursor, property_aliases, identities, desired));
		}
		if (name == "coalesce") {
			return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_COALESCE, std::move(arguments));
		}
		auto result = make_uniq<FunctionExpression>(lowered_name, std::move(arguments));
		result->distinct = program.distinct[node];
		return std::move(result);
	}
	case GqlExpressionType::UNARY: {
		auto input = LowerExpression(program, cursor, property_aliases, identities);
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
		auto input = LowerExpression(program, cursor, property_aliases, identities, GqlTypeId::PROPERTY_VALUE);
		return make_uniq<OperatorExpression>(
		    operation ? ExpressionType::OPERATOR_IS_NOT_NULL : ExpressionType::OPERATOR_IS_NULL, std::move(input));
	}
	case GqlExpressionType::LABELED: {
		auto input = LowerExpression(program, cursor, property_aliases, identities);
		(void)input;
		auto entry = property_aliases.find(PropertyKey(program.binding_indices[node], GQL_LABEL_ACCESS));
		if (entry == property_aliases.end() || entry->second.column_name.empty()) {
			return Constant(Value(program.operators[node] != 0));
		}
		vector<unique_ptr<ParsedExpression>> predicates;
		for (const auto &label : StringUtil::Split(program.properties[node], ';')) {
			predicates.push_back(
			    ElementHasLabel(entry->second.table_alias, entry->second.column_name, entry->second.is_list, label));
		}
		auto result = And(std::move(predicates));
		if (program.operators[node] != 0) {
			return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(result));
		}
		return result;
	}
	case GqlExpressionType::BINARY: {
		auto left_root = cursor;
		auto right_root = ExpressionEnd(program, left_root);
		auto left_type = static_cast<GqlTypeId>(program.result_types[left_root]);
		auto right_type = static_cast<GqlTypeId>(program.result_types[right_root]);
		auto left_desired = left_type == GqlTypeId::PROPERTY_VALUE ? right_type : GqlTypeId::UNKNOWN;
		auto right_desired = right_type == GqlTypeId::PROPERTY_VALUE ? left_type : GqlTypeId::UNKNOWN;
		auto left = LowerExpression(program, cursor, property_aliases, identities, left_desired);
		auto right = LowerExpression(program, cursor, property_aliases, identities, right_desired);
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
                                                    const RelationalPropertyMap &property_aliases,
                                                    const vector<RelationalIdentityAccess> &identities,
                                                    GqlTypeId desired_type = GqlTypeId::UNKNOWN) {
	idx_t cursor = 0;
	auto result = LowerExpression(program, cursor, property_aliases, identities, desired_type);
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

static void AppendStructField(vector<unique_ptr<ParsedExpression>> &fields, unique_ptr<ParsedExpression> expression,
                              const string &name) {
	expression->SetAlias(name);
	fields.push_back(std::move(expression));
}

static unique_ptr<TableRef> CsrExpansionTable(const string &graph_name, const string &vertex_alias,
                                              const string &vertex_key, const string &direction,
                                              const string &edge_label, const string &edge_alias) {
	vector<unique_ptr<ParsedExpression>> fields;
	AppendStructField(fields, Constant(Value(graph_name)), "graph_name");
	AppendStructField(fields, Column(vertex_alias, vertex_key), "vertex_id");
	AppendStructField(fields, Constant(Value(direction)), "direction");
	AppendStructField(fields, Constant(Value(edge_label)), "edge_label");
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(Function("struct_pack", std::move(fields)));
	auto result = make_uniq<TableFunctionRef>();
	result->function = make_uniq<FunctionExpression>("gql_csr_expand", std::move(arguments));
	result->alias = edge_alias;
	return std::move(result);
}

static unique_ptr<TableRef> ElementFetchTable(const string &graph_name, const string &element_kind,
                                              unique_ptr<ParsedExpression> element_id, const string &alias) {
	(void)graph_name;
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(std::move(element_id));
	auto result = make_uniq<TableFunctionRef>();
	result->function = make_uniq<FunctionExpression>(element_kind == "vertex" ? "gql_vertex_fetch" : "gql_edge_fetch",
	                                                 std::move(arguments));
	result->alias = alias;
	return std::move(result);
}

static unique_ptr<TableRef> CsrEdgePropertyExpansionTable(const GqlTableGraphBinding &graph, const string &graph_name,
                                                          const string &vertex_alias, const string &vertex_key,
                                                          const GqlBindingAccessPath &path, const string &edge_alias) {
	auto csr_alias = edge_alias + "_csr";
	auto result =
	    CsrExpansionTable(graph_name, vertex_alias, vertex_key, path.expansion_direction, path.edge_label, csr_alias);
	if (path.fetch_edge_properties) {
		vector<unique_ptr<ParsedExpression>> conditions;
		conditions.push_back(Constant(Value(true)));
		AppendJoin(result, ElementFetchTable(graph_name, "edge", Column(csr_alias, "__gql_edge_id"), edge_alias),
		           JoinType::INNER, std::move(conditions));
		return result;
	}
	vector<unique_ptr<ParsedExpression>> conditions;
	conditions.push_back(Equal(Column(csr_alias, "__gql_edge_id"), Column(edge_alias, graph.edge.key_column)));
	AppendJoin(result, ElementTable(graph.edge, edge_alias), JoinType::INNER, std::move(conditions));
	return result;
}

static unique_ptr<TableRef> CsrPathExpansionTable(const string &graph_name, const string &vertex_alias,
                                                  const string &vertex_key, const GqlBindingAccessPath &path,
                                                  const string &edge_alias) {
	vector<unique_ptr<ParsedExpression>> fields;
	AppendStructField(fields, Constant(Value(graph_name)), "graph_name");
	AppendStructField(fields, Column(vertex_alias, vertex_key), "vertex_id");
	AppendStructField(fields, Constant(Value(path.expansion_direction)), "direction");
	AppendStructField(fields, Constant(Value(path.edge_label)), "edge_label");
	AppendStructField(fields, Constant(Value::UBIGINT(path.minimum_repetitions)), "minimum_repetitions");
	AppendStructField(fields, Constant(Value::UBIGINT(path.maximum_repetitions)), "maximum_repetitions");
	AppendStructField(fields, Constant(Value(path.unbounded)), "unbounded");
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(Function("struct_pack", std::move(fields)));
	auto result = make_uniq<TableFunctionRef>();
	result->function = make_uniq<FunctionExpression>("gql_csr_path_expand", std::move(arguments));
	result->alias = edge_alias;
	return std::move(result);
}

static unique_ptr<TableRef> RelationalPathExpansionTable(const GqlTableGraphBinding &graph, const string &vertex_alias,
                                                         const string &vertex_key, const GqlBindingAccessPath &path,
                                                         const string &edge_alias) {
	if (path.unbounded || path.minimum_repetitions == 0 || path.minimum_repetitions > path.maximum_repetitions) {
		throw InternalException("Invalid bounded relational path expansion");
	}
	const bool outgoing = path.expansion_direction == "out";
	const auto &start_column = outgoing ? graph.edge_source_column : graph.edge_target_column;
	const auto &end_column = outgoing ? graph.edge_target_column : graph.edge_source_column;

	vector<unique_ptr<QueryNode>> branches;
	for (idx_t depth = path.minimum_repetitions; depth <= path.maximum_repetitions; depth++) {
		auto select = make_uniq<SelectNode>();
		unique_ptr<TableRef> from;
		vector<unique_ptr<ParsedExpression>> filters;
		vector<string> aliases;
		for (idx_t hop = 0; hop < depth; hop++) {
			auto alias = "gql_path_edge_" + to_string(hop);
			if (!from) {
				from = ElementTable(graph.edge, alias);
				filters.push_back(Equal(Column(alias, start_column), Column(vertex_alias, vertex_key)));
			} else {
				vector<unique_ptr<ParsedExpression>> conditions;
				conditions.push_back(Equal(Column(aliases.back(), end_column), Column(alias, start_column)));
				AppendJoin(from, ElementTable(graph.edge, alias), JoinType::INNER, std::move(conditions));
			}
			if (graph.edge.label_column.empty()) {
				filters.push_back(Constant(Value(false)));
			} else {
				filters.push_back(
				    ElementHasLabel(alias, graph.edge.label_column, graph.edge.label_is_list, path.edge_label));
			}
			for (const auto &prior_alias : aliases) {
				filters.push_back(
				    NotEqual(Column(alias, graph.edge.key_column), Column(prior_alias, graph.edge.key_column)));
			}
			aliases.push_back(std::move(alias));
		}
		select->from_table = std::move(from);
		select->where_clause = And(std::move(filters));
		const auto &last_alias = aliases.back();
		select->select_list.push_back(Aliased(Column(last_alias, graph.edge.key_column), "__gql_edge_id"));
		select->select_list.push_back(
		    Aliased(outgoing ? Column(vertex_alias, vertex_key) : Column(last_alias, end_column), "__gql_source_id"));
		select->select_list.push_back(
		    Aliased(outgoing ? Column(last_alias, end_column) : Column(vertex_alias, vertex_key), "__gql_target_id"));
		select->select_list.push_back(Aliased(Constant(Value(path.edge_label)), "__gql_type"));
		branches.push_back(std::move(select));
	}

	unique_ptr<QueryNode> root;
	if (branches.size() == 1) {
		root = std::move(branches[0]);
	} else {
		auto union_node = make_uniq<SetOperationNode>();
		union_node->setop_type = SetOperationType::UNION;
		union_node->setop_all = true;
		for (auto &branch : branches) {
			union_node->children.push_back(std::move(branch));
		}
		root = std::move(union_node);
	}
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(root);
	return make_uniq<SubqueryRef>(std::move(statement), edge_alias);
}

static unique_ptr<TableRef> CsrVerticesTable(const string &graph_name, const string &label, const string &alias) {
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(Constant(Value(graph_name)));
	arguments.push_back(Constant(Value(label)));
	auto result = make_uniq<TableFunctionRef>();
	result->function = make_uniq<FunctionExpression>("gql_csr_vertices", std::move(arguments));
	result->alias = alias;
	return std::move(result);
}

static unique_ptr<TableRef> PropertyIndexTable(const GqlElementTableBinding &table, const string &alias,
                                               const GqlBindingAccessPath &path, idx_t stage_index,
                                               idx_t binding_index) {
	auto base_alias = "gql_property_index_base_" + to_string(stage_index) + "_" + to_string(binding_index);
	auto candidates = make_uniq<SelectNode>();
	candidates->from_table = ElementTable(table, base_alias);
	candidates->select_list.push_back(make_uniq<StarExpression>());
	candidates->where_clause =
	    Equal(Column(base_alias, path.property_column), Constant(LiteralValue(path.literal_type, path.literal_value)));
	auto candidate_statement = make_uniq<SelectStatement>();
	candidate_statement->node = std::move(candidates);

	auto cte = make_uniq<CommonTableExpressionInfo>();
	cte->query = std::move(candidate_statement);
	cte->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	auto cte_name = "gql_property_index_candidates_" + to_string(stage_index) + "_" + to_string(binding_index);

	auto materialized = make_uniq<SelectNode>();
	materialized->cte_map.map[cte_name] = std::move(cte);
	materialized->from_table = NamedTable(cte_name, cte_name);
	materialized->select_list.push_back(make_uniq<StarExpression>());
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(materialized);
	return make_uniq<SubqueryRef>(std::move(statement), alias);
}

static unique_ptr<ParsedExpression> GraphElementValueAt(const GqlExpressionProgram &program, idx_t node,
                                                        const GqlTableGraphBinding &graph,
                                                        const vector<GqlPatternElementType> &binding_types,
                                                        const vector<RelationalIdentityAccess> &identities) {
	if (node >= program.node_types.size() ||
	    static_cast<GqlExpressionType>(program.node_types[node]) != GqlExpressionType::VARIABLE_REFERENCE) {
		throw NotImplementedException("GQL graph values currently require a direct element variable");
	}
	auto binding_index = NumericCast<idx_t>(program.binding_indices[node]);
	if (binding_index >= identities.size() || binding_index >= binding_types.size()) {
		throw InternalException("GQL graph-value binding is missing");
	}
	auto type = static_cast<GqlTypeId>(program.result_types[node]);
	auto expected = type == GqlTypeId::NODE   ? GqlPatternElementType::VERTEX
	                : type == GqlTypeId::EDGE ? GqlPatternElementType::EDGE
	                                          : throw InternalException("Invalid GQL graph-value type");
	if (binding_types[binding_index] != expected) {
		throw InternalException("GQL graph-value binding type is inconsistent");
	}

	const auto &table = expected == GqlPatternElementType::VERTEX ? graph.vertex : graph.edge;
	const auto &alias = identities[binding_index].table_alias;
	vector<unique_ptr<ParsedExpression>> fields;
	AppendStructField(fields, Column(alias, table.key_column),
	                  expected == GqlPatternElementType::VERTEX ? "vertex_id" : "edge_id");
	if (expected == GqlPatternElementType::VERTEX) {
		unique_ptr<ParsedExpression> labels =
		    table.label_column.empty() ? Constant(Value()) : Column(alias, table.label_column);
		if (!table.label_column.empty() && table.label_is_list) {
			vector<unique_ptr<ParsedExpression>> arguments;
			arguments.push_back(std::move(labels));
			arguments.push_back(Constant(Value(";")));
			labels = Function("array_to_string", std::move(arguments));
		}
		AppendStructField(fields, std::move(labels), "__gql_labels");
	} else {
		AppendStructField(fields, table.label_column.empty() ? Constant(Value()) : Column(alias, table.label_column),
		                  "__gql_type");
		AppendStructField(fields, Column(alias, graph.edge_source_column), "__gql_source");
		AppendStructField(fields, Column(alias, graph.edge_target_column), "__gql_target");
	}
	vector<pair<string, string>> properties(table.property_columns.begin(), table.property_columns.end());
	std::sort(properties.begin(), properties.end(),
	          [](const auto &left, const auto &right) { return StringUtil::CILessThan(left.first, right.first); });
	for (const auto &property : properties) {
		AppendStructField(fields, Column(alias, property.second), property.first);
	}

	auto packed = Function("struct_pack", std::move(fields));
	auto result = make_uniq<CaseExpression>();
	CaseCheck missing;
	missing.when_expr =
	    make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NULL, Column(alias, table.key_column));
	missing.then_expr = Constant(Value());
	result->case_checks.push_back(std::move(missing));
	result->else_expr = std::move(packed);
	return std::move(result);
}

static unique_ptr<ParsedExpression> GraphElementValue(const GqlExpressionProgram &program,
                                                      const GqlTableGraphBinding &graph,
                                                      const vector<GqlPatternElementType> &binding_types,
                                                      const vector<RelationalIdentityAccess> &identities) {
	if (program.node_types.empty()) {
		throw InternalException("GQL graph-value expression is empty");
	}
	return GraphElementValueAt(program, 0, graph, binding_types, identities);
}

static unique_ptr<ParsedExpression> GraphPathValue(const GqlExpressionProgram &program,
                                                   const GqlTableGraphBinding &graph,
                                                   const vector<GqlPatternElementType> &binding_types,
                                                   const vector<RelationalIdentityAccess> &identities) {
	if (program.node_types.empty() ||
	    static_cast<GqlExpressionType>(program.node_types[0]) != GqlExpressionType::FUNCTION ||
	    program.values[0] != "__gql_path" || program.child_counts[0] + 1 != program.node_types.size()) {
		throw NotImplementedException("GQL path values currently require a named fixed path");
	}

	vector<unique_ptr<ParsedExpression>> nodes;
	vector<unique_ptr<ParsedExpression>> edges;
	vector<unique_ptr<ParsedExpression>> missing_elements;
	for (idx_t node = 1; node < program.node_types.size(); node++) {
		if (program.child_counts[node] != 0) {
			throw InternalException("GQL fixed path contains a nested expression");
		}
		auto binding_index = NumericCast<idx_t>(program.binding_indices[node]);
		if (binding_index >= binding_types.size() || binding_index >= identities.size()) {
			throw InternalException("GQL fixed path binding is missing");
		}
		const auto &table = binding_types[binding_index] == GqlPatternElementType::EDGE ? graph.edge : graph.vertex;
		missing_elements.push_back(make_uniq<OperatorExpression>(
		    ExpressionType::OPERATOR_IS_NULL, Column(identities[binding_index].table_alias, table.key_column)));
		auto value = GraphElementValueAt(program, node, graph, binding_types, identities);
		if (binding_types[binding_index] == GqlPatternElementType::EDGE) {
			edges.push_back(std::move(value));
		} else {
			nodes.push_back(std::move(value));
		}
	}
	if (nodes.empty()) {
		throw InternalException("GQL fixed path has no nodes");
	}

	vector<unique_ptr<ParsedExpression>> fields;
	AppendStructField(fields, Function("list_value", std::move(nodes)), "nodes");
	auto edge_list =
	    edges.empty() ? Constant(Value::LIST(LogicalType::SQLNULL, {})) : Function("list_value", std::move(edges));
	AppendStructField(fields, std::move(edge_list), "edges");
	auto packed = Function("struct_pack", std::move(fields));

	unique_ptr<ParsedExpression> missing;
	if (missing_elements.size() == 1) {
		missing = std::move(missing_elements[0]);
	} else {
		missing = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_OR, std::move(missing_elements));
	}
	auto result = make_uniq<CaseExpression>();
	CaseCheck missing_path;
	missing_path.when_expr = std::move(missing);
	missing_path.then_expr = Constant(Value());
	result->case_checks.push_back(std::move(missing_path));
	result->else_expr = std::move(packed);
	return std::move(result);
}

static void AppendProjections(SelectNode &select, const vector<GqlExpressionProgram> &projections,
                              const vector<string> &projection_names, const RelationalPropertyMap &property_aliases,
                              const vector<RelationalIdentityAccess> &identities, const GqlTableGraphBinding &graph,
                              const vector<GqlPatternElementType> &binding_types) {
	bool has_aggregate = false;
	for (const auto &projection : projections) {
		has_aggregate = has_aggregate || ContainsAggregate(projection);
	}
	GroupingSet grouping_set;
	for (idx_t index = 0; index < projections.size(); index++) {
		auto mutation_value = StringUtil::StartsWith(projection_names[index], "gql_mutation_value_");
		auto desired_type = mutation_value ? GqlTypeId::PROPERTY_VALUE : GqlTypeId::UNKNOWN;
		auto projection_type = static_cast<GqlTypeId>(projections[index].result_types[0]);
		unique_ptr<ParsedExpression> expression;
		if (projection_type == GqlTypeId::NODE || projection_type == GqlTypeId::EDGE) {
			expression = GraphElementValue(projections[index], graph, binding_types, identities);
		} else if (projection_type == GqlTypeId::PATH) {
			expression = GraphPathValue(projections[index], graph, binding_types, identities);
		} else {
			expression = LowerExpression(projections[index], property_aliases, identities, desired_type);
		}
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
                                  const vector<string> &projection_names, const RelationalResultModifiers &modifiers,
                                  bool normalized_property_values = true) {
	if (modifiers.distinct) {
		select.modifiers.push_back(make_uniq<DistinctModifier>());
	}
	if (!modifiers.order_indices.empty()) {
		auto order = make_uniq<OrderModifier>();
		for (idx_t index = 0; index < modifiers.order_indices.size(); index++) {
			auto projection_index = modifiers.order_indices[index];
			unique_ptr<ParsedExpression> column = make_uniq<ColumnRefExpression>(projection_names[projection_index]);
			auto result_type = static_cast<GqlTypeId>(projections[projection_index].result_types[0]);
			if (normalized_property_values && result_type == GqlTypeId::PROPERTY_VALUE) {
				// DuckDB VARIANT is intentionally not directly orderable. ANY graphs do
				// not provide a static property type, so use its scalar rendering as a
				// stable native sort key instead of allowing Top-N to raise an internal
				// error.
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
                                                const RelationalResultModifiers &modifiers,
                                                bool normalized_property_values = true) {
	if (!modifiers.optional) {
		AppendResultModifiers(*select, projections, projection_names, modifiers, normalized_property_values);
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
	AppendResultModifiers(*optional, projections, projection_names, modifiers, normalized_property_values);
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(optional);
	return make_uniq<SubqueryRef>(std::move(statement));
}

static bool TryFindPropertyColumn(const GqlElementTableBinding &table, const string &property, string &column) {
	for (const auto &entry : table.property_columns) {
		if (StringUtil::CIEquals(entry.first, property)) {
			column = entry.second;
			return true;
		}
	}
	column.clear();
	return false;
}

static bool ReferencesOnlyBinding(const GqlExpressionProgram &program, idx_t binding_index) {
	for (const auto index : program.binding_indices) {
		if (index != NumericLimits<uint64_t>::Maximum() && index != binding_index) {
			return false;
		}
	}
	return true;
}

static bool ReferencesBinding(const GqlExpressionProgram &program, idx_t binding_index) {
	for (const auto index : program.binding_indices) {
		if (index == binding_index) {
			return true;
		}
	}
	return false;
}

static unique_ptr<TableRef> TableBackedNativeRecursiveMatch(const GqlTableGraphBinding &graph,
                                                            const RecursiveMatchInput &match) {
	if (match.edge_labels.size() > 1) {
		throw NotImplementedException("Table-backed native paths currently support "
		                              "at most one scalar edge label");
	}
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

	vector<RelationalIdentityAccess> identities(match.binding_count);
	identities[match.source_binding] = {"gql_object_" + to_string(match.source_binding), graph.vertex.key_column};
	identities[match.target_binding] = {"gql_object_" + to_string(match.target_binding), graph.vertex.key_column};

	RelationalPropertyMap property_aliases;
	for (const auto &program : match.projections) {
		CollectProperties(program, property_aliases);
	}
	for (const auto &program : match.predicates) {
		CollectProperties(program, property_aliases);
	}
	for (auto &entry : property_aliases) {
		auto separator = entry.first.find('\x1f');
		if (separator == string::npos) {
			throw InternalException("Invalid GQL recursive property binding key");
		}
		auto binding_index = NumericCast<idx_t>(std::stoull(entry.first.substr(0, separator)));
		if (binding_index != match.source_binding && binding_index != match.target_binding) {
			throw NotImplementedException("Quantified GQL edge group variables are not supported");
		}
		auto property = entry.first.substr(separator + 1);
		entry.second.table_alias = identities[binding_index].table_alias;
		if (property == GQL_LABEL_ACCESS) {
			entry.second.column_name = graph.vertex.label_column;
			entry.second.is_list = graph.vertex.label_is_list;
		} else {
			TryFindPropertyColumn(graph.vertex, property, entry.second.column_name);
		}
	}

	// Seed native recursion from the mapped vertex table. Source-only predicates
	// are repeated here as well as above the CTE so a selective source property
	// can prevent database-wide expansion.
	const string anchor_alias = "gql_anchor_vertex";
	auto anchor = make_uniq<SelectNode>();
	anchor->from_table = ElementTable(graph.vertex, anchor_alias);
	vector<unique_ptr<ParsedExpression>> anchor_filters;
	for (const auto &label : match.source_labels) {
		if (graph.vertex.label_column.empty()) {
			anchor_filters.push_back(Constant(Value(false)));
		} else {
			anchor_filters.push_back(
			    ElementHasLabel(anchor_alias, graph.vertex.label_column, graph.vertex.label_is_list, label));
		}
	}
	vector<RelationalIdentityAccess> anchor_identities(match.binding_count);
	anchor_identities[match.source_binding] = {anchor_alias, graph.vertex.key_column};
	RelationalPropertyMap anchor_properties;
	for (const auto &entry : property_aliases) {
		auto separator = entry.first.find('\x1f');
		auto binding_index = NumericCast<idx_t>(std::stoull(entry.first.substr(0, separator)));
		if (binding_index != match.source_binding) {
			continue;
		}
		auto access = entry.second;
		access.table_alias = anchor_alias;
		anchor_properties.emplace(entry.first, std::move(access));
	}
	for (const auto &predicate : match.predicates) {
		if (ReferencesOnlyBinding(predicate, match.source_binding)) {
			anchor_filters.push_back(
			    LowerExpression(predicate, anchor_properties, anchor_identities, GqlTypeId::BOOLEAN));
		}
	}
	anchor->where_clause = And(std::move(anchor_filters));
	anchor->select_list.push_back(Aliased(Column(anchor_alias, graph.vertex.key_column), "start_id"));
	anchor->select_list.push_back(Aliased(Column(anchor_alias, graph.vertex.key_column), "end_id"));
	const bool managed_edge_identity = StringUtil::CIEquals(graph.edge.ownership, "MANAGED");
	const auto trail_type = managed_edge_identity ? LogicalType::UBIGINT : LogicalType::VARCHAR;
	vector<Value> empty_edges;
	anchor->select_list.push_back(Aliased(Constant(Value::LIST(trail_type, std::move(empty_edges))), "edge_ids"));
	anchor->select_list.push_back(Aliased(Constant(Value::UBIGINT(0)), "depth"));

	// Managed graph edges have canonical UBIGINT identities. Preserve that
	// compact native type in recursive trail state, while retaining VARCHAR as
	// the generic fallback for attached non-managed element tables.
	unique_ptr<TableRef> step_from = NamedTable("gql_recursive_path", "gql_path_previous");
	const string edge_alias = "gql_path_edge";
	vector<unique_ptr<ParsedExpression>> edge_conditions;
	edge_conditions.push_back(
	    Equal(Column(edge_alias, match.reverse ? graph.edge_target_column : graph.edge_source_column),
	          Column("gql_path_previous", "end_id")));
	AppendJoin(step_from, ElementTable(graph.edge, edge_alias), JoinType::INNER, std::move(edge_conditions));

	unique_ptr<ParsedExpression> edge_identity;
	if (managed_edge_identity) {
		edge_identity = Column(edge_alias, graph.edge.key_column);
	} else {
		edge_identity = make_uniq<CastExpression>(LogicalType::VARCHAR, Column(edge_alias, graph.edge.key_column));
	}
	vector<unique_ptr<ParsedExpression>> contains_arguments;
	contains_arguments.push_back(Column("gql_path_previous", "edge_ids"));
	contains_arguments.push_back(edge_identity->Copy());
	vector<unique_ptr<ParsedExpression>> step_filters;
	step_filters.push_back(make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT,
	                                                     Function("list_contains", std::move(contains_arguments))));
	for (const auto &label : match.edge_labels) {
		if (graph.edge.label_column.empty()) {
			step_filters.push_back(Constant(Value(false)));
		} else {
			step_filters.push_back(
			    ElementHasLabel(edge_alias, graph.edge.label_column, graph.edge.label_is_list, label));
		}
	}

	auto step = make_uniq<SelectNode>();
	step->from_table = std::move(step_from);
	step->where_clause = And(std::move(step_filters));
	step->select_list.push_back(Aliased(Column("gql_path_previous", "start_id"), "start_id"));
	step->select_list.push_back(
	    Aliased(Column(edge_alias, match.reverse ? graph.edge_source_column : graph.edge_target_column), "end_id"));
	vector<unique_ptr<ParsedExpression>> append_arguments;
	append_arguments.push_back(Column("gql_path_previous", "edge_ids"));
	append_arguments.push_back(std::move(edge_identity));
	step->select_list.push_back(Aliased(Function("list_append", std::move(append_arguments)), "edge_ids"));
	vector<unique_ptr<ParsedExpression>> depth_arguments;
	depth_arguments.push_back(Column("gql_path_previous", "depth"));
	depth_arguments.push_back(Constant(Value::UBIGINT(1)));
	auto next_depth = Function("+", std::move(depth_arguments));
	next_depth->Cast<FunctionExpression>().is_operator = true;
	step->select_list.push_back(Aliased(std::move(next_depth), "depth"));

	auto recursive = make_uniq<RecursiveCTENode>();
	recursive->ctename = "gql_recursive_path";
	recursive->union_all = true;
	recursive->aliases = {"start_id", "end_id", "edge_ids", "depth"};
	recursive->left = std::move(anchor);
	recursive->right = std::move(step);
	auto recursive_statement = make_uniq<SelectStatement>();
	recursive_statement->node = std::move(recursive);
	unique_ptr<TableRef> from = make_uniq<SubqueryRef>(std::move(recursive_statement), "gql_path");

	bool source_needed_above = match.source_binding == match.target_binding;
	for (const auto &projection : match.projections) {
		source_needed_above |= ReferencesBinding(projection, match.source_binding);
	}
	for (const auto &predicate : match.predicates) {
		source_needed_above |= ReferencesBinding(predicate, match.source_binding) &&
		                       !ReferencesOnlyBinding(predicate, match.source_binding);
	}
	if (source_needed_above) {
		vector<unique_ptr<ParsedExpression>> source_conditions;
		source_conditions.push_back(Equal(Column(identities[match.source_binding].table_alias, graph.vertex.key_column),
		                                  Column("gql_path", "start_id")));
		AppendJoin(from, ElementTable(graph.vertex, identities[match.source_binding].table_alias), JoinType::INNER,
		           std::move(source_conditions));
	}
	if (match.target_binding != match.source_binding) {
		vector<unique_ptr<ParsedExpression>> target_conditions;
		target_conditions.push_back(Equal(Column(identities[match.target_binding].table_alias, graph.vertex.key_column),
		                                  Column("gql_path", "end_id")));
		AppendJoin(from, ElementTable(graph.vertex, identities[match.target_binding].table_alias), JoinType::INNER,
		           std::move(target_conditions));
	}

	vector<unique_ptr<ParsedExpression>> filters;
	if (match.minimum_repetitions > 0) {
		filters.push_back(Compare(ExpressionType::COMPARE_GREATERTHANOREQUALTO, Column("gql_path", "depth"),
		                          Constant(Value::UBIGINT(match.minimum_repetitions))));
	}
	auto append_labels = [&](const vector<string> &labels, idx_t binding_index) {
		for (const auto &label : labels) {
			if (graph.vertex.label_column.empty()) {
				filters.push_back(Constant(Value(false)));
			} else {
				filters.push_back(ElementHasLabel(identities[binding_index].table_alias, graph.vertex.label_column,
				                                  graph.vertex.label_is_list, label));
			}
		}
	};
	append_labels(match.target_labels, match.target_binding);
	if (match.source_binding == match.target_binding) {
		filters.push_back(Equal(Column("gql_path", "start_id"), Column("gql_path", "end_id")));
	}
	for (const auto &predicate : match.predicates) {
		if (!ReferencesOnlyBinding(predicate, match.source_binding)) {
			filters.push_back(LowerExpression(predicate, property_aliases, identities, GqlTypeId::BOOLEAN));
		}
	}

	auto select = make_uniq<SelectNode>();
	select->from_table = std::move(from);
	select->where_clause = And(std::move(filters));
	vector<GqlPatternElementType> binding_types(match.binding_count, GqlPatternElementType::VERTEX);
	binding_types[match.edge_binding] = GqlPatternElementType::EDGE;
	AppendProjections(*select, match.projections, match.projection_names, property_aliases, identities, graph,
	                  binding_types);
	return FinalizeMatchSelect(std::move(select), match.projections, match.projection_names, match.modifiers, false);
}

static unique_ptr<TableRef> RecursiveMatchBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto graph_name = GqlGetSelectedGraph(context);
	if (graph_name.empty()) {
		throw InvalidInputException("No graph selected; use SESSION SET GRAPH before MATCH");
	}
	auto match = ReadRecursiveMatchInput(input);
	GqlTableGraphBinding table_graph;
	if (!GqlTryLoadTableGraph(context, graph_name, table_graph)) {
		throw InvalidInputException("Graph '%s' has no native tables; load it with COPY GRAPH before MATCH",
		                            graph_name);
	}
	return TableBackedNativeRecursiveMatch(table_graph, match);
}

static unique_ptr<TableRef> TableBackedMatch(ClientContext &context, const string &graph_name,
                                             const GqlTableGraphBinding &graph, const RelationalMatchInput &match) {
	vector<RelationalIdentityAccess> identities(match.binding_types.size());
	for (idx_t index = 0; index < match.binding_types.size(); index++) {
		const auto &table = match.binding_types[index] == GqlPatternElementType::EDGE ? graph.edge : graph.vertex;
		auto alias = "gql_object_" + to_string(index);
		identities[index] = {alias, table.key_column};
	}

	RelationalPropertyMap property_aliases;
	for (const auto &program : match.projections) {
		CollectProperties(program, property_aliases);
	}
	for (const auto &program : match.predicates) {
		CollectProperties(program, property_aliases);
	}
	for (auto &entry : property_aliases) {
		auto separator = entry.first.find('\x1f');
		if (separator == string::npos) {
			throw InternalException("Invalid GQL property binding key");
		}
		auto binding_index = NumericCast<idx_t>(std::stoull(entry.first.substr(0, separator)));
		if (binding_index >= match.binding_types.size()) {
			throw InternalException("Invalid GQL property binding index");
		}
		auto property = entry.first.substr(separator + 1);
		const auto &table =
		    match.binding_types[binding_index] == GqlPatternElementType::EDGE ? graph.edge : graph.vertex;
		entry.second.table_alias = identities[binding_index].table_alias;
		if (property == GQL_LABEL_ACCESS) {
			entry.second.column_name = table.label_column;
			entry.second.is_list = table.label_is_list;
		} else {
			TryFindPropertyColumn(table, property, entry.second.column_name);
		}
	}

	GqlAccessPathInput access_input;
	access_input.root = match.root;
	access_input.binding_types = match.binding_types;
	access_input.projections = match.projections;
	access_input.predicates = match.predicates;
	for (const auto &stage : match.match_stages) {
		GqlAccessMatchStage access_stage;
		for (const auto &pattern : stage.patterns) {
			GqlAccessPattern access_pattern;
			for (const auto &element : pattern.elements) {
				access_pattern.elements.push_back({element.type, element.binding_index, element.label, element.reverse,
				                                   element.quantified, element.unbounded, element.minimum_repetitions,
				                                   element.maximum_repetitions});
			}
			access_stage.patterns.push_back(std::move(access_pattern));
		}
		access_input.match_stages.push_back(std::move(access_stage));
	}
	for (const auto &node : match.nodes) {
		access_input.nodes.push_back({node.type, node.child, node.right, node.payload});
	}
	auto access_plan = GqlOptimizeAccessPaths(context, graph_name, graph, access_input);

	struct StageState {
		unique_ptr<TableRef> source;
		vector<bool> introduced;
		vector<unique_ptr<ParsedExpression>> conditions;
	};
	auto build_stage = [&](idx_t stage_index, const vector<bool> &available) {
		if (stage_index >= match.match_stages.size()) {
			throw InternalException("Invalid GQL MATCH stage index");
		}
		StageState result;
		const auto &stage = match.match_stages[stage_index];
		if (stage_index >= access_plan.stages.size()) {
			throw InternalException("Missing GQL optimizer access-path stage");
		}
		const auto &stage_plan = access_plan.stages[stage_index];
		result.introduced = stage_plan.introduced;
		for (const auto binding_index : stage_plan.source_order) {
			unique_ptr<TableRef> source;
			const auto &path = stage_plan.bindings[binding_index];
			if (path.type == GqlBindingAccessPathType::PROPERTY_INDEX_LOOKUP) {
				source = PropertyIndexTable(graph.vertex, identities[binding_index].table_alias, path, stage_index,
				                            binding_index);
			} else if (path.type == GqlBindingAccessPathType::CSR_EXPANSION) {
				source = CsrExpansionTable(graph_name, identities[path.expansion_vertex_binding].table_alias,
				                           graph.vertex.key_column, path.expansion_direction, path.edge_label,
				                           identities[binding_index].table_alias);
			} else if (path.type == GqlBindingAccessPathType::CSR_EDGE_PROPERTY_EXPANSION) {
				source = CsrEdgePropertyExpansionTable(
				    graph, graph_name, identities[path.expansion_vertex_binding].table_alias, graph.vertex.key_column,
				    path, identities[binding_index].table_alias);
			} else if (path.type == GqlBindingAccessPathType::CSR_PATH_EXPANSION) {
				source = CsrPathExpansionTable(graph_name, identities[path.expansion_vertex_binding].table_alias,
				                               graph.vertex.key_column, path, identities[binding_index].table_alias);
			} else if (path.type == GqlBindingAccessPathType::RELATIONAL_PATH_EXPANSION) {
				source =
				    RelationalPathExpansionTable(graph, identities[path.expansion_vertex_binding].table_alias,
				                                 graph.vertex.key_column, path, identities[binding_index].table_alias);
			} else if (path.type == GqlBindingAccessPathType::BATCHED_ELEMENT_FETCH) {
				if (path.fetch_id_binding >= identities.size() || path.fetch_id_column.empty()) {
					throw InternalException("Invalid GQL batched element fetch access path");
				}
				source = ElementFetchTable(graph_name, "vertex",
				                           Column(identities[path.fetch_id_binding].table_alias, path.fetch_id_column),
				                           identities[binding_index].table_alias);
			} else {
				const auto &table =
				    match.binding_types[binding_index] == GqlPatternElementType::EDGE ? graph.edge : graph.vertex;
				source = ElementTable(table, identities[binding_index].table_alias);
			}
			if (!result.source) {
				result.source = std::move(source);
			} else {
				vector<unique_ptr<ParsedExpression>> cross_conditions;
				cross_conditions.push_back(Constant(Value(true)));
				AppendJoin(result.source, std::move(source), JoinType::INNER, std::move(cross_conditions));
			}
		}
		idx_t posting_index = 0;
		for (idx_t binding_index = 0; binding_index < stage_plan.bindings.size(); binding_index++) {
			for (const auto &label : stage_plan.bindings[binding_index].label_postings) {
				auto alias = "gql_vertex_label_" + to_string(stage_index) + "_" + to_string(posting_index++);
				vector<unique_ptr<ParsedExpression>> posting_conditions;
				posting_conditions.push_back(Equal(
				    Column(identities[binding_index].table_alias, graph.vertex.key_column), Column(alias, "__gql_id")));
				AppendJoin(result.source, CsrVerticesTable(graph_name, label, alias), JoinType::INNER,
				           std::move(posting_conditions));
			}
		}
		for (const auto &pattern : stage.patterns) {
			for (const auto &element : pattern.elements) {
				if (element.label.empty()) {
					continue;
				}
				const auto &table = element.type == GqlPatternElementType::EDGE ? graph.edge : graph.vertex;
				if (table.label_column.empty()) {
					result.conditions.push_back(Constant(Value(false)));
				} else {
					for (const auto &label : StringUtil::Split(element.label, ';')) {
						const auto &postings = stage_plan.bindings[element.binding_index].label_postings;
						bool posting_selected = false;
						for (const auto &posting : postings) {
							if (StringUtil::CIEquals(posting, label)) {
								posting_selected = true;
								break;
							}
						}
						if (element.type == GqlPatternElementType::VERTEX && posting_selected) {
							continue;
						}
						result.conditions.push_back(ElementHasLabel(identities[element.binding_index].table_alias,
						                                            table.label_column, table.label_is_list, label));
					}
				}
			}
			for (idx_t element_index = 1; element_index < pattern.elements.size(); element_index += 2) {
				const auto &left = pattern.elements[element_index - 1];
				const auto &edge = pattern.elements[element_index];
				const auto &right = pattern.elements[element_index + 1];
				auto edge_alias = identities[edge.binding_index].table_alias;
				result.conditions.push_back(
				    Equal(Column(edge_alias, edge.reverse ? graph.edge_target_column : graph.edge_source_column),
				          Column(identities[left.binding_index].table_alias, graph.vertex.key_column)));
				result.conditions.push_back(
				    Equal(Column(edge_alias, edge.reverse ? graph.edge_source_column : graph.edge_target_column),
				          Column(identities[right.binding_index].table_alias, graph.vertex.key_column)));
			}
			// TRAIL uniqueness belongs to one path pattern. Independent
			// comma-separated patterns and later MATCH stages may legally reuse an
			// edge; correlating them globally breaks subquery-like stages such as an
			// OPTIONAL MATCH anti-join.
			vector<idx_t> path_edges;
			for (idx_t element_index = 1; element_index < pattern.elements.size(); element_index += 2) {
				auto edge_binding = pattern.elements[element_index].binding_index;
				if (std::find(path_edges.begin(), path_edges.end(), edge_binding) == path_edges.end()) {
					path_edges.push_back(edge_binding);
				}
			}
			for (idx_t edge_index = 0; edge_index < path_edges.size(); edge_index++) {
				for (idx_t prior = 0; prior < edge_index; prior++) {
					result.conditions.push_back(
					    NotEqual(Column(identities[path_edges[edge_index]].table_alias, graph.edge.key_column),
					             Column(identities[path_edges[prior]].table_alias, graph.edge.key_column)));
				}
			}
		}
		return result;
	};

	auto unit_relation = [&]() -> unique_ptr<TableRef> {
		auto seed = make_uniq<SelectNode>();
		seed->from_table = make_uniq<EmptyTableRef>();
		seed->select_list.push_back(Aliased(Constant(Value(true)), "present"));
		auto statement = make_uniq<SelectStatement>();
		statement->node = std::move(seed);
		return make_uniq<SubqueryRef>(std::move(statement), "gql_unit");
	};

	struct PipelineState {
		unique_ptr<TableRef> from;
		vector<bool> available;
		vector<unique_ptr<ParsedExpression>> filters;
	};
	auto build_right_stage = [&](auto &self, idx_t node_index, const vector<bool> &available) -> StageState {
		const auto &node = match.nodes[node_index];
		if (node.type == GqlLogicalOperatorType::MATCH) {
			return build_stage(node.payload, available);
		}
		if (node.type != GqlLogicalOperatorType::FILTER) {
			throw BinderException("GQL APPLY right side is not one MATCH stage");
		}
		auto result = self(self, node.child, available);
		result.conditions.push_back(
		    LowerExpression(match.predicates[node.payload], property_aliases, identities, GqlTypeId::BOOLEAN));
		return result;
	};
	auto build_pipeline = [&](auto &self, idx_t node_index) -> PipelineState {
		const auto &node = match.nodes[node_index];
		switch (node.type) {
		case GqlLogicalOperatorType::UNIT: {
			PipelineState result;
			result.from = unit_relation();
			result.available.resize(match.binding_types.size(), false);
			return result;
		}
		case GqlLogicalOperatorType::MATCH: {
			PipelineState result;
			result.available.resize(match.binding_types.size(), false);
			auto stage = build_stage(node.payload, result.available);
			if (!stage.source) {
				throw InternalException("Initial GQL MATCH introduces no bindings");
			}
			result.from = std::move(stage.source);
			result.available = std::move(stage.introduced);
			result.filters = std::move(stage.conditions);
			return result;
		}
		case GqlLogicalOperatorType::FILTER: {
			auto result = self(self, node.child);
			result.filters.push_back(
			    LowerExpression(match.predicates[node.payload], property_aliases, identities, GqlTypeId::BOOLEAN));
			return result;
		}
		case GqlLogicalOperatorType::INNER_APPLY:
		case GqlLogicalOperatorType::LEFT_APPLY: {
			auto result = self(self, node.child);
			auto stage = build_right_stage(build_right_stage, node.right, result.available);
			if (!stage.source) {
				if (node.type == GqlLogicalOperatorType::LEFT_APPLY) {
					// Every binding on the right is fixed by the left row. Whether the
					// optional condition succeeds or fails, the visible result is the
					// same single left row because there is nothing to null-extend.
					return result;
				}
				for (auto &condition : stage.conditions) {
					result.filters.push_back(std::move(condition));
				}
			} else {
				if (stage.conditions.empty()) {
					stage.conditions.push_back(Constant(Value(true)));
				}
				AppendJoin(result.from, std::move(stage.source),
				           node.type == GqlLogicalOperatorType::LEFT_APPLY ? JoinType::LEFT : JoinType::INNER,
				           std::move(stage.conditions));
			}
			for (idx_t index = 0; index < result.available.size(); index++) {
				result.available[index] = result.available[index] || stage.introduced[index];
			}
			return result;
		}
		case GqlLogicalOperatorType::PROJECT:
		case GqlLogicalOperatorType::CALL:
			throw InternalException("Nested GQL projection in MATCH pipeline");
		}
		throw InternalException("Unknown GQL logical operator");
	};
	auto pipeline = build_pipeline(build_pipeline, match.root);

	auto select = make_uniq<SelectNode>();
	select->from_table = std::move(pipeline.from);
	select->where_clause = And(std::move(pipeline.filters));
	AppendProjections(*select, match.projections, match.projection_names, property_aliases, identities, graph,
	                  match.binding_types);
	return FinalizeMatchSelect(std::move(select), match.projections, match.projection_names, match.modifiers, false);
}

static unique_ptr<TableRef> RelationalMatchBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto graph_name = GqlGetSelectedGraph(context);
	if (graph_name.empty()) {
		throw InvalidInputException("No graph selected; use SESSION SET GRAPH before MATCH");
	}
	auto match = ReadMatchInput(input);
	GqlTableGraphBinding table_graph;
	if (!GqlTryLoadTableGraph(context, graph_name, table_graph)) {
		throw InvalidInputException("Graph '%s' has no native tables; load it with COPY GRAPH before MATCH",
		                            graph_name);
	}
	return TableBackedMatch(context, graph_name, table_graph, match);
}

TableFunction GqlRelationalMatchFunction() {
	TableFunction function("gql_match_relational",
	                       {LogicalType::UTINYINT,
	                        LogicalType::LIST(LogicalType::UBIGINT),
	                        LogicalType::LIST(LogicalType::UTINYINT),
	                        LogicalType::LIST(LogicalType::UBIGINT),
	                        LogicalType::LIST(LogicalType::VARCHAR),
	                        LogicalType::LIST(LogicalType::BOOLEAN),
	                        LogicalType::LIST(LogicalType::BOOLEAN),
	                        LogicalType::LIST(LogicalType::BOOLEAN),
	                        LogicalType::LIST(LogicalType::UBIGINT),
	                        LogicalType::LIST(LogicalType::UBIGINT),
	                        LogicalType::LIST(LogicalType::UBIGINT),
	                        LogicalType::LIST(LogicalType::UTINYINT),
	                        LogicalType::LIST(LogicalType::UBIGINT),
	                        LogicalType::LIST(LogicalType::UBIGINT),
	                        LogicalType::LIST(LogicalType::UBIGINT),
	                        LogicalType::UBIGINT,
	                        LogicalType::LIST(GqlExpressionProgramType()),
	                        LogicalType::LIST(LogicalType::VARCHAR),
	                        LogicalType::LIST(GqlExpressionProgramType()),
	                        LogicalType::BOOLEAN,
	                        LogicalType::LIST(LogicalType::UBIGINT),
	                        LogicalType::LIST(LogicalType::BOOLEAN),
	                        LogicalType::LIST(LogicalType::UTINYINT),
	                        LogicalType::BOOLEAN,
	                        LogicalType::UBIGINT,
	                        LogicalType::BOOLEAN,
	                        LogicalType::UBIGINT},
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
