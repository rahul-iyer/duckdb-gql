#include "gql_binder.hpp"

#include "gql_algorithms.hpp"
#include "gql_optimizer.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

static constexpr idx_t GQL_MAX_FINITE_PATH_ALTERNATIVES = 64;

static bool IsNumeric(GqlTypeId type) {
	return type == GqlTypeId::INTEGER || type == GqlTypeId::ELEMENT_ID || type == GqlTypeId::DECIMAL ||
	       type == GqlTypeId::DOUBLE || type == GqlTypeId::PROPERTY_VALUE;
}

static bool IsBoolean(GqlTypeId type) {
	return type == GqlTypeId::BOOLEAN || type == GqlTypeId::PROPERTY_VALUE;
}

static bool IsScalar(GqlTypeId type) {
	return type != GqlTypeId::UNKNOWN && type != GqlTypeId::NODE && type != GqlTypeId::EDGE && type != GqlTypeId::PATH;
}

static bool ContainsAggregate(const GqlBoundExpression &expression) {
	if (expression.aggregate) {
		return true;
	}
	if (expression.left && ContainsAggregate(*expression.left)) {
		return true;
	}
	if (expression.right && ContainsAggregate(*expression.right)) {
		return true;
	}
	for (const auto &argument : expression.arguments) {
		if (ContainsAggregate(*argument)) {
			return true;
		}
	}
	return false;
}

static bool ExpressionsEqual(const GqlBoundExpression &left, const GqlBoundExpression &right) {
	if (left.expression_type != right.expression_type || left.binding_index != right.binding_index ||
	    left.literal.type != right.literal.type || left.literal.value != right.literal.value ||
	    left.property != right.property || left.unary_operator != right.unary_operator ||
	    left.binary_operator != right.binary_operator || left.function_name != right.function_name ||
	    left.aggregate != right.aggregate || left.distinct != right.distinct || left.negated != right.negated ||
	    static_cast<bool>(left.left) != static_cast<bool>(right.left) ||
	    static_cast<bool>(left.right) != static_cast<bool>(right.right) ||
	    left.arguments.size() != right.arguments.size()) {
		return false;
	}
	if (left.left && !ExpressionsEqual(*left.left, *right.left)) {
		return false;
	}
	if (left.right && !ExpressionsEqual(*left.right, *right.right)) {
		return false;
	}
	for (idx_t index = 0; index < left.arguments.size(); index++) {
		if (!ExpressionsEqual(*left.arguments[index], *right.arguments[index])) {
			return false;
		}
	}
	return true;
}

static GqlType LiteralType(const GqlLiteral &literal) {
	switch (literal.type) {
	case GqlLiteralType::NULL_VALUE:
		return {GqlTypeId::NULL_VALUE, true};
	case GqlLiteralType::BOOLEAN:
		return {GqlTypeId::BOOLEAN, false};
	case GqlLiteralType::INTEGER:
		return {GqlTypeId::INTEGER, false};
	case GqlLiteralType::DECIMAL:
		return {GqlTypeId::DECIMAL, false};
	case GqlLiteralType::DOUBLE:
		return {GqlTypeId::DOUBLE, false};
	case GqlLiteralType::STRING:
		return {GqlTypeId::STRING, false};
	}
	throw InternalException("Unknown GQL literal type");
}

static GqlType NumericResult(const GqlType &left, const GqlType &right) {
	GqlType result;
	if (left.id == GqlTypeId::PROPERTY_VALUE || right.id == GqlTypeId::PROPERTY_VALUE) {
		result.id = GqlTypeId::PROPERTY_VALUE;
	} else if (left.id == GqlTypeId::DOUBLE || right.id == GqlTypeId::DOUBLE) {
		result.id = GqlTypeId::DOUBLE;
	} else if (left.id == GqlTypeId::DECIMAL || right.id == GqlTypeId::DECIMAL) {
		result.id = GqlTypeId::DECIMAL;
	} else {
		result.id = GqlTypeId::INTEGER;
	}
	result.nullable = left.nullable || right.nullable;
	return result;
}

void GqlBinder::ResetScopes() {
	scopes.clear();
	scopes.emplace_back();
	current_scope = 0;
}

void GqlBinder::PushScope() {
	if (current_scope == DConstants::INVALID_INDEX || current_scope >= scopes.size()) {
		throw InternalException("GQL binder has no active scope");
	}
	BindingScope scope;
	scope.parent = current_scope;
	scopes.push_back(std::move(scope));
	current_scope = scopes.size() - 1;
}

idx_t GqlBinder::FindBindingIndex(const string &name) const {
	auto scope_index = current_scope;
	while (scope_index != DConstants::INVALID_INDEX) {
		if (scope_index >= scopes.size()) {
			throw InternalException("GQL binder scope parent is invalid");
		}
		auto entry = scopes[scope_index].bindings.find(name);
		if (entry != scopes[scope_index].bindings.end()) {
			return entry->second;
		}
		scope_index = scopes[scope_index].parent;
	}
	return DConstants::INVALID_INDEX;
}

idx_t GqlBinder::DefineBinding(GqlBinding binding) {
	if (current_scope == DConstants::INVALID_INDEX || current_scope >= scopes.size()) {
		throw InternalException("GQL binder has no active scope");
	}
	auto binding_index = bindings.size();
	if (!scopes[current_scope].bindings.emplace(binding.name, binding_index).second) {
		throw BinderException("GQL binding variable '%s' is already defined", binding.name);
	}
	bindings.push_back(std::move(binding));
	return binding_index;
}

idx_t GqlBinder::ResolveIndex(const GqlIdentifier &identifier) const {
	auto binding_index = FindBindingIndex(identifier.value);
	if (binding_index == DConstants::INVALID_INDEX) {
		throw BinderException("GQL binding variable '%s' is not defined (line %llu, column "
		                      "%llu)",
		                      identifier.value, static_cast<unsigned long long>(identifier.source.start_line),
		                      static_cast<unsigned long long>(identifier.source.start_column));
	}
	return binding_index;
}

GqlBinding &GqlBinder::Resolve(const GqlIdentifier &identifier) {
	return bindings[ResolveIndex(identifier)];
}

const GqlExpression &GqlBinder::ResolveValueExpression(const GqlExpression &expression) const {
	if (expression.type == GqlExpressionType::VARIABLE_REFERENCE) {
		auto entry = let_bindings.find(expression.variable.value);
		if (entry != let_bindings.end()) {
			return ResolveValueExpression(*entry->second);
		}
		return expression;
	}
	if (expression.type != GqlExpressionType::PROPERTY_REFERENCE || !expression.left) {
		return expression;
	}
	auto &receiver = ResolveValueExpression(*expression.left);
	if (receiver.type != GqlExpressionType::RECORD_CONSTRUCTOR ||
	    receiver.field_names.size() != receiver.arguments.size()) {
		return expression;
	}
	for (idx_t field_index = 0; field_index < receiver.field_names.size(); field_index++) {
		if (receiver.field_names[field_index].value == expression.property.value) {
			return ResolveValueExpression(*receiver.arguments[field_index]);
		}
	}
	throw BinderException("GQL LET record field '%s' does not exist", expression.property.value);
}

void GqlBinder::ValidateLetExpression(const GqlExpression &expression) {
	auto &resolved = ResolveValueExpression(expression);
	if (resolved.type == GqlExpressionType::LIST_CONSTRUCTOR ||
	    resolved.type == GqlExpressionType::RECORD_CONSTRUCTOR) {
		for (const auto &child : resolved.arguments) {
			ValidateLetExpression(*child);
		}
		return;
	}
	BindExpression(resolved);
}

shared_ptr<GqlBoundExpression> GqlBinder::BindExpression(const GqlExpression &expression) {
	auto &resolved = ResolveValueExpression(expression);
	if (&resolved != &expression) {
		return BindExpression(resolved);
	}
	auto result = make_shared_ptr<GqlBoundExpression>();
	result->expression_type = expression.type;
	result->literal = expression.literal;
	result->property = expression.property.value;
	result->unary_operator = expression.unary_operator;
	result->binary_operator = expression.binary_operator;
	result->function_name = expression.function_name;
	result->aggregate = expression.aggregate;
	result->distinct = expression.distinct;
	result->negated = expression.negated;
	result->source = expression.source;

	switch (expression.type) {
	case GqlExpressionType::LITERAL:
		result->result_type = LiteralType(expression.literal);
		return result;
	case GqlExpressionType::LIST_CONSTRUCTOR:
	case GqlExpressionType::RECORD_CONSTRUCTOR:
		throw BinderException("GQL collection constructors are currently supported "
		                      "only as DELETE targets");
	case GqlExpressionType::VARIABLE_REFERENCE: {
		auto binding_index = ResolveIndex(expression.variable);
		auto &binding = bindings[binding_index];
		if (binding.type.id == GqlTypeId::PATH) {
			auto path = path_bindings.find(binding_index);
			if (path == path_bindings.end()) {
				throw InternalException("GQL path binding is missing its elements");
			}
			result->expression_type = GqlExpressionType::FUNCTION;
			result->function_name = "__gql_path";
			for (const auto &element : path->second) {
				auto argument = make_shared_ptr<GqlBoundExpression>();
				argument->expression_type = GqlExpressionType::VARIABLE_REFERENCE;
				argument->binding_index = element.binding_index;
				argument->result_type = {element.type == GqlPatternElementType::VERTEX ? GqlTypeId::NODE
				                                                                       : GqlTypeId::EDGE,
				                         binding.type.nullable};
				argument->source = element.source;
				result->arguments.push_back(std::move(argument));
			}
			result->result_type = binding.type;
			return result;
		}
		result->binding_index = binding.index;
		result->binding_source = binding.binding_source;
		result->result_type = binding.type;
		return result;
	}
	case GqlExpressionType::PROPERTY_REFERENCE:
		if (!expression.left) {
			throw InternalException("GQL property expression has no receiver");
		}
		result->left = BindExpression(*expression.left);
		if (result->left->result_type.id != GqlTypeId::NODE && result->left->result_type.id != GqlTypeId::EDGE) {
			throw BinderException("GQL property receiver must be a node or edge");
		}
		result->binding_index = result->left->binding_index;
		result->result_type = {GqlTypeId::PROPERTY_VALUE, true};
		return result;
	case GqlExpressionType::ELEMENT_ID:
		if (!expression.left) {
			throw InternalException("GQL element_id expression has no argument");
		}
		result->left = BindExpression(*expression.left);
		if (result->left->result_type.id != GqlTypeId::NODE && result->left->result_type.id != GqlTypeId::EDGE) {
			throw BinderException("GQL element_id argument must be a node or edge");
		}
		result->binding_index = result->left->binding_index;
		result->result_type = {GqlTypeId::ELEMENT_ID, false};
		return result;
	case GqlExpressionType::FUNCTION: {
		for (const auto &argument : expression.arguments) {
			auto bound = BindExpression(*argument);
			if (expression.aggregate && ContainsAggregate(*bound)) {
				throw BinderException("Nested GQL aggregate functions are not supported");
			}
			result->arguments.push_back(std::move(bound));
		}
		auto name = StringUtil::Lower(expression.function_name);
		if (name == "count") {
			result->result_type = {GqlTypeId::INTEGER, false};
			return result;
		}
		if (result->arguments.empty()) {
			throw BinderException("GQL function '%s' requires an argument", expression.function_name);
		}
		if (name == "coalesce") {
			result->result_type = result->arguments[0]->result_type;
			result->result_type.nullable = true;
			for (const auto &argument : result->arguments) {
				if (result->result_type.id == GqlTypeId::PROPERTY_VALUE &&
				    argument->result_type.id != GqlTypeId::PROPERTY_VALUE) {
					result->result_type.id = argument->result_type.id;
				} else if (argument->result_type.id != GqlTypeId::PROPERTY_VALUE &&
				           result->result_type.id != argument->result_type.id) {
					throw BinderException("GQL COALESCE arguments must have compatible types");
				}
				if (!argument->result_type.nullable) {
					result->result_type.nullable = false;
				}
			}
			return result;
		}
		if (name == "lower" || name == "upper" || name == "trim" || name == "ltrim" || name == "rtrim" ||
		    name == "left" || name == "right" || name == "nfc_normalize") {
			if (result->arguments[0]->result_type.id != GqlTypeId::STRING &&
			    result->arguments[0]->result_type.id != GqlTypeId::PROPERTY_VALUE) {
				throw BinderException("GQL function '%s' requires a string argument", expression.function_name);
			}
			result->result_type = {GqlTypeId::STRING, result->arguments[0]->result_type.nullable};
			return result;
		}
		if (name == "char_length" || name == "length") {
			if (result->arguments[0]->result_type.id != GqlTypeId::STRING &&
			    result->arguments[0]->result_type.id != GqlTypeId::PROPERTY_VALUE) {
				throw BinderException("GQL function '%s' requires a string argument", expression.function_name);
			}
			result->result_type = {GqlTypeId::INTEGER, result->arguments[0]->result_type.nullable};
			return result;
		}
		if (name == "abs" || name == "ceil" || name == "floor" || name == "sqrt" || name == "mod") {
			if (!IsNumeric(result->arguments[0]->result_type.id)) {
				throw BinderException("GQL function '%s' requires numeric arguments", expression.function_name);
			}
			if (name == "mod" && (result->arguments.size() != 2 || !IsNumeric(result->arguments[1]->result_type.id))) {
				throw BinderException("GQL function MOD requires two numeric arguments");
			}
			result->result_type = result->arguments[0]->result_type;
			return result;
		}
		if (name == "sum" || name == "avg") {
			if (!IsNumeric(result->arguments[0]->result_type.id)) {
				throw BinderException("GQL aggregate '%s' requires a numeric argument", expression.function_name);
			}
			result->result_type = result->arguments[0]->result_type;
			result->result_type.nullable = true;
			return result;
		}
		if (name == "min" || name == "max") {
			if (!IsScalar(result->arguments[0]->result_type.id)) {
				throw BinderException("GQL aggregate '%s' requires a scalar argument", expression.function_name);
			}
			result->result_type = result->arguments[0]->result_type;
			result->result_type.nullable = true;
			return result;
		}
		throw NotImplementedException("GQL function '%s'", expression.function_name);
	}
	case GqlExpressionType::UNARY:
		if (!expression.left) {
			throw InternalException("GQL unary expression has no operand");
		}
		result->left = BindExpression(*expression.left);
		if (expression.unary_operator == GqlUnaryOperator::NOT) {
			if (!IsBoolean(result->left->result_type.id)) {
				throw BinderException("GQL NOT operand must be BOOLEAN");
			}
			result->result_type = {GqlTypeId::BOOLEAN, result->left->result_type.nullable};
		} else {
			if (!IsNumeric(result->left->result_type.id)) {
				throw BinderException("GQL signed operand must be numeric");
			}
			result->result_type = result->left->result_type;
		}
		return result;
	case GqlExpressionType::BINARY:
		if (!expression.left || !expression.right) {
			throw InternalException("GQL binary expression has a missing operand");
		}
		result->left = BindExpression(*expression.left);
		result->right = BindExpression(*expression.right);
		switch (expression.binary_operator) {
		case GqlBinaryOperator::MULTIPLY:
		case GqlBinaryOperator::DIVIDE:
		case GqlBinaryOperator::ADD:
		case GqlBinaryOperator::SUBTRACT:
			if (!IsNumeric(result->left->result_type.id) || !IsNumeric(result->right->result_type.id)) {
				throw BinderException("GQL arithmetic operands must be numeric");
			}
			result->result_type = NumericResult(result->left->result_type, result->right->result_type);
			break;
		case GqlBinaryOperator::CONCATENATE:
			if ((result->left->result_type.id != GqlTypeId::STRING &&
			     result->left->result_type.id != GqlTypeId::PROPERTY_VALUE) ||
			    (result->right->result_type.id != GqlTypeId::STRING &&
			     result->right->result_type.id != GqlTypeId::PROPERTY_VALUE)) {
				throw BinderException("GQL concatenation operands must be strings");
			}
			result->result_type = {GqlTypeId::STRING,
			                       result->left->result_type.nullable || result->right->result_type.nullable};
			break;
		case GqlBinaryOperator::AND:
		case GqlBinaryOperator::OR:
		case GqlBinaryOperator::XOR:
			if (!IsBoolean(result->left->result_type.id) || !IsBoolean(result->right->result_type.id)) {
				throw BinderException("GQL Boolean operands must be BOOLEAN");
			}
			result->result_type = {GqlTypeId::BOOLEAN,
			                       result->left->result_type.nullable || result->right->result_type.nullable};
			break;
		case GqlBinaryOperator::EQUAL:
		case GqlBinaryOperator::NOT_EQUAL:
		case GqlBinaryOperator::LESS_THAN:
		case GqlBinaryOperator::GREATER_THAN:
		case GqlBinaryOperator::LESS_THAN_OR_EQUAL:
		case GqlBinaryOperator::GREATER_THAN_OR_EQUAL:
			if (!IsScalar(result->left->result_type.id) || !IsScalar(result->right->result_type.id)) {
				throw BinderException("GQL comparison operands must be scalar values");
			}
			result->result_type = {GqlTypeId::BOOLEAN,
			                       result->left->result_type.nullable || result->right->result_type.nullable};
			break;
		}
		return result;
	case GqlExpressionType::IS_NULL:
		if (!expression.left) {
			throw InternalException("GQL null predicate has no operand");
		}
		result->left = BindExpression(*expression.left);
		result->result_type = {GqlTypeId::BOOLEAN, false};
		return result;
	case GqlExpressionType::LABELED:
		if (!expression.left) {
			throw InternalException("GQL labeled predicate has no element");
		}
		result->left = BindExpression(*expression.left);
		if (result->left->result_type.id != GqlTypeId::NODE && result->left->result_type.id != GqlTypeId::EDGE) {
			throw BinderException("GQL labeled predicate requires a node or edge");
		}
		result->binding_index = result->left->binding_index;
		result->result_type = {GqlTypeId::BOOLEAN, false};
		return result;
	}
	throw InternalException("Unknown GQL expression type");
}

string GqlBinder::ProjectionName(const GqlProjection &projection, const GqlBoundExpression &expression) const {
	if (!projection.alias.IsEmpty()) {
		return projection.alias.value;
	}
	const GqlBinding *binding = nullptr;
	for (const auto &candidate : bindings) {
		if (candidate.index == expression.binding_index) {
			binding = &candidate;
			break;
		}
	}
	if (expression.expression_type == GqlExpressionType::PROPERTY_REFERENCE && binding) {
		return binding->name + "." + expression.property;
	}
	if (expression.expression_type == GqlExpressionType::ELEMENT_ID && binding) {
		return "element_id(" + binding->name + ")";
	}
	return "expression";
}

vector<GqlLogicalPlan> GqlBinder::BindAlternatives(const GqlMatchStatement &statement) {
	vector<pair<idx_t, idx_t>> any_direction_elements;
	idx_t alternative_count = 1;
	for (idx_t pattern_index = 0; pattern_index < statement.patterns.size(); pattern_index++) {
		const auto &pattern = statement.patterns[pattern_index];
		for (idx_t element_index = 0; element_index < pattern.elements.size(); element_index++) {
			const auto &element = pattern.elements[element_index];
			if (element.type == GqlPatternElementType::EDGE && element.edge_direction == GqlEdgeDirection::ANY) {
				if (alternative_count > GQL_MAX_FINITE_PATH_ALTERNATIVES / 2) {
					throw BinderException("GQL any-direction patterns produce more than "
					                      "64 native alternatives");
				}
				alternative_count *= 2;
				any_direction_elements.emplace_back(pattern_index, element_index);
			}
		}
	}

	vector<GqlLogicalPlan> result;
	result.reserve(alternative_count);
	for (idx_t alternative = 0; alternative < alternative_count; alternative++) {
		GqlMatchStatement branch(statement);
		auto selection = alternative;
		for (const auto &location : any_direction_elements) {
			auto &element = branch.patterns[location.first].elements[location.second];
			element.edge_direction = selection % 2 == 0 ? GqlEdgeDirection::RIGHT : GqlEdgeDirection::LEFT;
			selection /= 2;
		}
		result.push_back(Bind(branch));
	}
	return result;
}

GqlLogicalPlan GqlBinder::Bind(const GqlMatchStatement &statement) {
	bindings.clear();
	path_bindings.clear();
	let_bindings.clear();
	ResetScopes();
	idx_t next_binding_index = 0;
	auto bind_pattern = [&](const GqlPattern &pattern) {
		GqlBoundPattern bound_pattern;
		// Optionality belongs to LEFT_APPLY in the logical plan. The compatibility
		// lowerer derives the legacy per-pattern flags from that operator boundary.
		bound_pattern.optional = false;
		bound_pattern.optional_stage = 0;
		bound_pattern.source = pattern.source;
		auto bind_element = [&](const GqlPatternElement &element, bool force_anonymous) {
			GqlBoundPatternElement bound_element;
			bound_element.type = element.type;
			bound_element.edge_direction = element.edge_direction;
			bound_element.quantified = element.quantified;
			bound_element.unbounded = element.unbounded;
			bound_element.minimum_repetitions = element.minimum_repetitions;
			bound_element.maximum_repetitions = element.maximum_repetitions;
			bound_element.source = element.source;
			for (const auto &label : element.labels) {
				bound_element.labels.push_back(label.value);
			}

			if (force_anonymous || element.variable.IsEmpty()) {
				bound_element.binding_index = next_binding_index++;
			} else {
				auto existing = FindBindingIndex(element.variable.value);
				if (existing != DConstants::INVALID_INDEX) {
					auto &binding = bindings[existing];
					auto expected_type =
					    element.type == GqlPatternElementType::VERTEX ? GqlTypeId::NODE : GqlTypeId::EDGE;
					if (binding.type.id != expected_type) {
						throw BinderException("GQL binding variable '%s' is used as "
						                      "incompatible element types",
						                      element.variable.value);
					}
					bound_element.binding_index = binding.index;
				} else {
					bound_element.binding_index = next_binding_index++;
					GqlBinding binding;
					binding.name = element.variable.value;
					binding.index = bound_element.binding_index;
					binding.type = {element.type == GqlPatternElementType::VERTEX ? GqlTypeId::NODE : GqlTypeId::EDGE,
					                false};
					binding.source = element.variable.source;
					DefineBinding(std::move(binding));
				}
			}
			bound_pattern.elements.push_back(std::move(bound_element));
		};

		for (const auto &element : pattern.elements) {
			if (!element.quantified) {
				bind_element(element, false);
				continue;
			}
			if (element.unbounded) {
				if (element.type != GqlPatternElementType::EDGE || !element.variable.IsEmpty()) {
					throw InternalException("Invalid unbounded quantified GQL path element");
				}
				bind_element(element, true);
				continue;
			}
			if (element.type != GqlPatternElementType::EDGE || element.minimum_repetitions == 0 ||
			    (!element.unbounded && element.minimum_repetitions > element.maximum_repetitions) ||
			    !element.variable.IsEmpty()) {
				throw InternalException("Invalid fixed quantified GQL path element");
			}
			if (element.minimum_repetitions == element.maximum_repetitions) {
				auto fixed_element = element;
				fixed_element.quantified = false;
				fixed_element.unbounded = false;
				fixed_element.minimum_repetitions = 1;
				fixed_element.maximum_repetitions = 1;
				for (idx_t repetition = 0; repetition < element.minimum_repetitions; repetition++) {
					bind_element(fixed_element, true);
					if (repetition + 1 < element.minimum_repetitions) {
						GqlPatternElement intermediate;
						intermediate.type = GqlPatternElementType::VERTEX;
						intermediate.source = element.quantifier_source;
						bind_element(intermediate, true);
					}
				}
				continue;
			}
			// Quantified edges are one logical factor. The relational access-path
			// optimizer can lower them to a composable CSR path expansion without
			// cloning the rest of the query once per hop count.
			bind_element(element, true);
		}
		if (!pattern.variable.IsEmpty()) {
			if (FindBindingIndex(pattern.variable.value) != DConstants::INVALID_INDEX) {
				throw BinderException("GQL binding variable '%s' is already defined", pattern.variable.value);
			}
			for (const auto &element : bound_pattern.elements) {
				if (element.quantified) {
					throw NotImplementedException("GQL quantified path value projection");
				}
			}
			GqlBinding binding;
			binding.name = pattern.variable.value;
			binding.index = DConstants::INVALID_INDEX;
			binding.type = {GqlTypeId::PATH, pattern.optional};
			binding.source = pattern.variable.source;
			auto symbol_index = DefineBinding(std::move(binding));
			path_bindings.emplace(symbol_index, bound_pattern.elements);
		}
		return bound_pattern;
	};

	auto bind_predicate = [&](idx_t predicate_index) {
		if (predicate_index >= statement.predicates.size()) {
			throw InternalException("GQL clause predicate index is outside the AST predicate pool");
		}
		const auto &predicate = statement.predicates[predicate_index];
		if (!predicate) {
			throw InternalException("GQL MATCH contains an empty predicate");
		}
		auto bound_predicate = BindExpression(*predicate);
		if (ContainsAggregate(*bound_predicate)) {
			throw BinderException("GQL aggregate functions are not allowed in WHERE or FILTER");
		}
		if (!IsBoolean(bound_predicate->result_type.id)) {
			throw BinderException("GQL WHERE/FILTER expression must be BOOLEAN");
		}
		return bound_predicate;
	};

	if (statement.clauses.empty()) {
		throw InternalException("GQL binder requires an ordered clause AST");
	}
	vector<bool> bound_patterns(statement.patterns.size(), false);
	vector<bool> bound_predicate_indices(statement.predicates.size(), false);
	shared_ptr<GqlLogicalOperator> current;
	shared_ptr<GqlLogicalLeftApply> active_optional_apply;
	bool saw_match_clause = false;
	bool saw_call_clause = false;
	bool saw_return_clause = false;
	idx_t previous_offset = 0;
	bool first_clause = true;
	for (const auto &clause : statement.clauses) {
		if (!first_clause && clause.source.start_offset < previous_offset) {
			throw InternalException("GQL query clauses are not in source order");
		}
		first_clause = false;
		previous_offset = clause.source.start_offset;
		switch (clause.type) {
		case GqlQueryClauseType::MATCH: {
			if (saw_return_clause || saw_call_clause || clause.pattern_count == 0 ||
			    clause.pattern_begin > statement.patterns.size() ||
			    clause.pattern_count > statement.patterns.size() - clause.pattern_begin) {
				throw InternalException("Invalid GQL MATCH clause payload");
			}
			if ((clause.optional_stage == 0 && clause.optional && current) ||
			    (clause.optional_stage > 0 && (!clause.optional || !current))) {
				throw InternalException("Invalid GQL OPTIONAL MATCH stage placement");
			}
			if (saw_match_clause) {
				PushScope();
			}
			saw_match_clause = true;
			auto match_stage = make_shared_ptr<GqlLogicalMatch>();
			for (idx_t pattern_index = clause.pattern_begin;
			     pattern_index < clause.pattern_begin + clause.pattern_count; pattern_index++) {
				if (bound_patterns[pattern_index]) {
					throw InternalException("GQL pattern belongs to more than one MATCH clause");
				}
				const auto &pattern = statement.patterns[pattern_index];
				if (pattern.optional != clause.optional || pattern.optional_stage != clause.optional_stage) {
					throw InternalException("GQL MATCH clause optionality disagrees with its pattern");
				}
				match_stage->patterns.push_back(bind_pattern(pattern));
				bound_patterns[pattern_index] = true;
			}
			shared_ptr<GqlLogicalOperator> stage = std::move(match_stage);
			for (auto predicate_index : clause.predicate_indices) {
				if (predicate_index >= bound_predicate_indices.size() || bound_predicate_indices[predicate_index]) {
					throw InternalException("Invalid GQL MATCH predicate ownership");
				}
				auto filter = make_shared_ptr<GqlLogicalFilter>(bind_predicate(predicate_index));
				filter->child = std::move(stage);
				stage = std::move(filter);
				bound_predicate_indices[predicate_index] = true;
			}
			if (clause.optional) {
				auto apply = make_shared_ptr<GqlLogicalLeftApply>(clause.optional_stage);
				apply->child = current ? std::move(current) : make_shared_ptr<GqlLogicalUnit>();
				apply->right = std::move(stage);
				current = apply;
				active_optional_apply = std::move(apply);
			} else if (!current) {
				current = std::move(stage);
				active_optional_apply.reset();
			} else {
				auto apply = make_shared_ptr<GqlLogicalInnerApply>();
				apply->child = std::move(current);
				apply->right = std::move(stage);
				current = std::move(apply);
				active_optional_apply.reset();
			}
			break;
		}
		case GqlQueryClauseType::FILTER: {
			if (!saw_match_clause || saw_return_clause || saw_call_clause ||
			    clause.predicate_index >= bound_predicate_indices.size() ||
			    bound_predicate_indices[clause.predicate_index]) {
				throw InternalException("Invalid GQL FILTER clause payload");
			}
			auto filter = make_shared_ptr<GqlLogicalFilter>(bind_predicate(clause.predicate_index));
			if (active_optional_apply && active_optional_apply->optional_stage == clause.optional_stage) {
				filter->child = std::move(active_optional_apply->right);
				active_optional_apply->right = std::move(filter);
			} else if (clause.optional_stage > 0) {
				throw InternalException("GQL FILTER has no active OPTIONAL MATCH stage");
			} else {
				filter->child = std::move(current);
				current = std::move(filter);
			}
			bound_predicate_indices[clause.predicate_index] = true;
			break;
		}
		case GqlQueryClauseType::LET: {
			if (!saw_match_clause || saw_return_clause || saw_call_clause || clause.let_count == 0 ||
			    clause.let_begin > statement.let_bindings.size() ||
			    clause.let_count > statement.let_bindings.size() - clause.let_begin) {
				throw InternalException("Invalid GQL LET clause payload");
			}
			for (idx_t let_index = clause.let_begin; let_index < clause.let_begin + clause.let_count; let_index++) {
				auto &binding = statement.let_bindings[let_index];
				if (!binding.expression) {
					throw InternalException("GQL LET binding has no expression");
				}
				if (FindBindingIndex(binding.variable.value) != DConstants::INVALID_INDEX ||
				    let_bindings.find(binding.variable.value) != let_bindings.end()) {
					throw BinderException("GQL LET variable '%s' is already defined", binding.variable.value);
				}
				ValidateLetExpression(*binding.expression);
				let_bindings.emplace(binding.variable.value, binding.expression.get());
			}
			// LET expressions are currently inlined, but their clause boundary is
			// still semantically material: later FILTER clauses operate after the
			// LEFT_APPLY rather than becoming optional-match predicates.
			active_optional_apply.reset();
			break;
		}
		case GqlQueryClauseType::CALL: {
			if (!saw_match_clause || saw_return_clause || saw_call_clause ||
			    clause.procedure_call_index >= statement.procedure_calls.size()) {
				throw InternalException("Invalid GQL CALL clause placement");
			}
			const auto &call = statement.procedure_calls[clause.procedure_call_index];
			auto definition = GqlFindProcedure(call.procedure_namespace.value, call.procedure_name.value);
			if (!definition) {
				throw BinderException("Unknown GQL procedure '%s.%s'", call.procedure_namespace.value,
				                      call.procedure_name.value);
			}
			idx_t required_arguments = 0;
			for (const auto &argument : definition->arguments) {
				required_arguments += !argument.optional;
			}
			if (call.arguments.size() < required_arguments || call.arguments.size() > definition->arguments.size()) {
				throw BinderException("GQL procedure '%s.%s' expects between %llu and %llu arguments, "
				                      "got %llu",
				                      definition->procedure_namespace, definition->name,
				                      static_cast<unsigned long long>(required_arguments),
				                      static_cast<unsigned long long>(definition->arguments.size()),
				                      static_cast<unsigned long long>(call.arguments.size()));
			}

			auto input_project = make_shared_ptr<GqlLogicalProject>();
			auto logical_call = make_shared_ptr<GqlLogicalCall>();
			logical_call->procedure_namespace = definition->procedure_namespace;
			logical_call->procedure_name = definition->name;
			logical_call->input_mode = definition->input_mode;
			for (idx_t argument_index = 0; argument_index < call.arguments.size(); argument_index++) {
				const auto &argument = definition->arguments[argument_index];
				const auto &expression = call.arguments[argument_index];
				if (!expression) {
					throw InternalException("GQL procedure argument is empty");
				}
				if (argument.mode == GqlProcedureArgumentMode::CONFIGURATION) {
					if (expression->type != GqlExpressionType::LITERAL) {
						throw BinderException("GQL procedure configuration argument '%s' must be constant",
						                      argument.name);
					}
					auto type = LiteralType(expression->literal);
					if (type.id != argument.type.id) {
						throw BinderException("GQL procedure configuration argument '%s' has an invalid type",
						                      argument.name);
					}
					logical_call->configuration_arguments.push_back(expression->literal);
					continue;
				}
				auto bound = BindExpression(*expression);
				if (ContainsAggregate(*bound)) {
					throw BinderException("GQL procedure input argument '%s' cannot contain an aggregate",
					                      argument.name);
				}
				if (bound->result_type.id != argument.type.id &&
				    !(argument.type.id == GqlTypeId::ELEMENT_ID && bound->result_type.id == GqlTypeId::INTEGER)) {
					throw BinderException("GQL procedure input argument '%s' has an invalid type", argument.name);
				}
				input_project->projections.push_back(
				    {std::move(bound), "gql_call_input_" + to_string(argument_index), expression->source});
			}
			if (definition->input_mode == GqlProcedureInputMode::ROW) {
				throw NotImplementedException("Row-correlated GQL procedure execution is not implemented");
			}
			if (definition->input_mode == GqlProcedureInputMode::NONE) {
				if (!input_project->projections.empty()) {
					throw InternalException("No-input GQL procedure has row input arguments");
				}
				auto marker = make_shared_ptr<GqlBoundExpression>();
				marker->expression_type = GqlExpressionType::LITERAL;
				marker->literal.type = GqlLiteralType::INTEGER;
				marker->literal.value = "1";
				marker->result_type = {GqlTypeId::INTEGER, false};
				input_project->projections.push_back({std::move(marker), "gql_call_input_marker", call.source});
			} else if (input_project->projections.empty()) {
				throw InternalException("Batch GQL procedure has no input arguments");
			}
			input_project->child = std::move(current);
			logical_call->child = std::move(input_project);
			for (const auto &output : definition->outputs) {
				logical_call->output_names.push_back(output.name);
				logical_call->output_types.push_back(output.type);
			}
			current = std::move(logical_call);

			// A blocking batch procedure replaces its input relation. Only YIELD
			// names are visible after the CALL; arbitrary upstream rows cannot be
			// meaningfully correlated with one result row from a whole-frontier run.
			scopes.emplace_back();
			current_scope = scopes.size() - 1;
			let_bindings.clear();
			case_insensitive_set_t yielded_names;
			for (const auto &yield : call.yield_items) {
				idx_t output_index = DConstants::INVALID_INDEX;
				for (idx_t index = 0; index < definition->outputs.size(); index++) {
					if (StringUtil::CIEquals(definition->outputs[index].name, yield.field.value)) {
						output_index = index;
						break;
					}
				}
				if (output_index == DConstants::INVALID_INDEX) {
					throw BinderException("GQL procedure '%s.%s' has no output '%s'", definition->procedure_namespace,
					                      definition->name, yield.field.value);
				}
				auto visible_name = yield.alias.IsEmpty() ? yield.field.value : yield.alias.value;
				if (!yielded_names.insert(visible_name).second) {
					throw BinderException("Duplicate GQL YIELD name '%s'", visible_name);
				}
				GqlBinding binding;
				binding.name = visible_name;
				binding.index = output_index;
				binding.type = definition->outputs[output_index].type;
				binding.source = yield.source;
				binding.binding_source = GqlBinding::Source::PROCEDURE;
				DefineBinding(std::move(binding));
			}
			saw_call_clause = true;
			active_optional_apply.reset();
			break;
		}
		case GqlQueryClauseType::RETURN:
			if (!saw_match_clause || saw_return_clause) {
				throw InternalException("Invalid GQL RETURN clause placement");
			}
			saw_return_clause = true;
			break;
		}
	}
	for (auto is_bound : bound_patterns) {
		if (!is_bound) {
			throw InternalException("GQL pattern is not owned by an ordered MATCH clause");
		}
	}
	for (auto is_bound : bound_predicate_indices) {
		if (!is_bound) {
			throw InternalException("GQL predicate is not owned by an ordered query clause");
		}
	}
	if (!statement.has_mutation && !saw_return_clause) {
		throw InternalException("GQL query has no RETURN clause");
	}
	if (!current) {
		throw InternalException("GQL query has no MATCH pipeline");
	}
	if (!statement.procedure_calls.empty() && !saw_call_clause) {
		throw InternalException("GQL procedure is not owned by an ordered CALL clause");
	}

	auto project = make_shared_ptr<GqlLogicalProject>();
	vector<GqlBoundMutation> bound_mutations;
	shared_ptr<GqlBoundInsert> bound_insertion;
	if (statement.has_mutation) {
		if (statement.insertion) {
			bound_insertion = make_shared_ptr<GqlBoundInsert>();
			unordered_map<string, pair<bool, idx_t>> inserted_variables;
			auto bind_properties = [&](const GqlInsertElement &element, const string &column_prefix,
			                           vector<GqlBoundInsertProperty> &properties) {
				case_insensitive_set_t names;
				for (idx_t property_index = 0; property_index < element.properties.size(); property_index++) {
					const auto &property = element.properties[property_index];
					if (!names.insert(property.name.value).second) {
						throw BinderException("Duplicate GQL INSERT property '%s'", property.name.value);
					}
					if (!property.expression) {
						throw InternalException("MATCH INSERT property has no expression");
					}
					auto value = BindExpression(*property.expression);
					if (ContainsAggregate(*value)) {
						throw BinderException("GQL aggregate functions are not allowed in INSERT");
					}
					if (!IsScalar(value->result_type.id)) {
						throw BinderException("GQL INSERT property requires a scalar value");
					}
					auto value_column = column_prefix + std::to_string(property_index);
					project->projections.push_back({std::move(value), value_column, property.source});
					properties.push_back({property.name.value, std::move(value_column), property.source});
				}
			};

			for (idx_t vertex_index = 0; vertex_index < statement.insertion->vertices.size(); vertex_index++) {
				const auto &vertex = statement.insertion->vertices[vertex_index];
				GqlBoundInsertVertex bound_vertex;
				bound_vertex.source = vertex.source;
				for (const auto &label : vertex.labels) {
					bound_vertex.labels.push_back(label.value);
				}
				auto existing =
				    vertex.variable.IsEmpty() ? DConstants::INVALID_INDEX : FindBindingIndex(vertex.variable.value);
				if (existing != DConstants::INVALID_INDEX) {
					auto &binding = bindings[existing];
					if (binding.type.id != GqlTypeId::NODE) {
						throw BinderException("GQL INSERT cannot reuse bound non-node "
						                      "variable '%s' as a node",
						                      vertex.variable.value);
					}
					if (statement.insertion->edges.empty() || !vertex.labels.empty() || !vertex.properties.empty()) {
						throw BinderException("GQL INSERT node variable '%s' is already bound", vertex.variable.value);
					}
					bound_vertex.existing = true;
					bound_vertex.binding_index = binding.index;
					bound_vertex.existing_id_column = "gql_insert_existing_vertex_" + std::to_string(vertex_index);
					auto id = make_shared_ptr<GqlBoundExpression>();
					id->expression_type = GqlExpressionType::VARIABLE_REFERENCE;
					id->binding_index = binding.index;
					id->result_type = {GqlTypeId::ELEMENT_ID, false};
					project->projections.push_back({std::move(id), bound_vertex.existing_id_column, vertex.source});
				} else {
					bool first_declaration = true;
					if (!vertex.variable.IsEmpty()) {
						auto inserted = inserted_variables.find(vertex.variable.value);
						if (inserted != inserted_variables.end()) {
							if (!inserted->second.first) {
								throw BinderException("GQL INSERT variable '%s' is already defined as an edge",
								                      vertex.variable.value);
							}
							if (!vertex.labels.empty() || !vertex.properties.empty()) {
								throw BinderException("GQL INSERT node variable '%s' is already defined",
								                      vertex.variable.value);
							}
							bound_vertex.allocation_index = inserted->second.second;
							first_declaration = false;
						} else {
							bound_vertex.allocation_index = bound_insertion->new_vertex_count++;
							inserted_variables.emplace(vertex.variable.value,
							                           make_pair(true, bound_vertex.allocation_index));
						}
					} else {
						bound_vertex.allocation_index = bound_insertion->new_vertex_count++;
					}
					bound_vertex.create = first_declaration;
					if (bound_vertex.create) {
						bind_properties(vertex, "gql_insert_vertex_property_" + std::to_string(vertex_index) + "_",
						                bound_vertex.properties);
					}
				}
				bound_insertion->vertices.push_back(std::move(bound_vertex));
			}

			for (idx_t edge_index = 0; edge_index < statement.insertion->edges.size(); edge_index++) {
				const auto &edge = statement.insertion->edges[edge_index];
				if (edge.source_vertex >= bound_insertion->vertices.size() ||
				    edge.target_vertex >= bound_insertion->vertices.size()) {
					throw InternalException("GQL INSERT edge endpoint is outside the vertex path");
				}
				if (!edge.variable.IsEmpty()) {
					if (FindBindingIndex(edge.variable.value) != DConstants::INVALID_INDEX ||
					    inserted_variables.find(edge.variable.value) != inserted_variables.end()) {
						throw BinderException("GQL INSERT edge variable '%s' is already bound", edge.variable.value);
					}
					inserted_variables.emplace(edge.variable.value, make_pair(false, edge_index));
				}
				GqlBoundInsertEdge bound_edge;
				bound_edge.allocation_index = edge_index;
				bound_edge.source_vertex = edge.source_vertex;
				bound_edge.target_vertex = edge.target_vertex;
				bound_edge.source = edge.source;
				for (const auto &label : edge.labels) {
					bound_edge.labels.push_back(label.value);
				}
				bind_properties(edge, "gql_insert_edge_property_" + std::to_string(edge_index) + "_",
				                bound_edge.properties);
				bound_insertion->edges.push_back(std::move(bound_edge));
			}
			auto row_marker = make_shared_ptr<GqlBoundExpression>();
			row_marker->expression_type = GqlExpressionType::LITERAL;
			row_marker->literal.type = GqlLiteralType::INTEGER;
			row_marker->literal.value = "1";
			row_marker->result_type = {GqlTypeId::INTEGER, false};
			project->projections.push_back({std::move(row_marker), "gql_insert_row_marker", statement.source});
		} else {
			bool saw_null_delete_target = false;
			auto append_mutation = [&](const GqlMutation &mutation, idx_t binding_index, GqlType binding_type) {
				auto mutation_index = bound_mutations.size();
				GqlBoundMutation bound_mutation;
				bound_mutation.type = mutation.type;
				bound_mutation.binding_index = binding_index;
				bound_mutation.binding_type = binding_type;
				bound_mutation.name = mutation.name.value;
				bound_mutation.detach = mutation.detach;
				bound_mutation.source = mutation.source;

				auto target_expression = make_shared_ptr<GqlBoundExpression>();
				target_expression->expression_type = GqlExpressionType::VARIABLE_REFERENCE;
				target_expression->binding_index = binding_index;
				target_expression->result_type = {GqlTypeId::ELEMENT_ID, false};
				project->projections.push_back(
				    {std::move(target_expression), "gql_target_id_" + std::to_string(mutation_index), mutation.source});
				if (mutation.type == GqlMutationType::SET_PROPERTY) {
					if (!mutation.value) {
						throw InternalException("GQL SET property has no value expression");
					}
					bound_mutation.value = BindExpression(*mutation.value);
					if (ContainsAggregate(*bound_mutation.value)) {
						throw BinderException("GQL aggregate functions are not allowed in SET");
					}
					auto value_type = bound_mutation.value->result_type.id;
					if (value_type == GqlTypeId::UNKNOWN || value_type == GqlTypeId::NODE ||
					    value_type == GqlTypeId::EDGE || value_type == GqlTypeId::PATH) {
						throw NotImplementedException("GQL SET property requires a scalar value");
					}
					project->projections.push_back({bound_mutation.value,
					                                "gql_mutation_value_" + std::to_string(mutation_index),
					                                mutation.source});
				}
				bound_mutations.push_back(std::move(bound_mutation));
			};
			auto append_delete_binding = [&](const GqlMutation &mutation, const GqlIdentifier &variable) {
				auto symbol_index = ResolveIndex(variable);
				auto &target = bindings[symbol_index];
				if (target.type.id == GqlTypeId::PATH) {
					auto path = path_bindings.find(symbol_index);
					if (path == path_bindings.end()) {
						throw InternalException("GQL path mutation target has no bound elements");
					}
					for (const auto &element : path->second) {
						append_mutation(
						    mutation, element.binding_index,
						    {element.type == GqlPatternElementType::VERTEX ? GqlTypeId::NODE : GqlTypeId::EDGE,
						     target.type.nullable});
					}
					return;
				}
				if (target.type.id != GqlTypeId::NODE && target.type.id != GqlTypeId::EDGE) {
					throw BinderException("GQL DELETE target must resolve to a node, edge, or path");
				}
				append_mutation(mutation, target.index, target.type);
			};
			auto resolve_delete_selection = [&](auto &self, const GqlExpression &expression) -> const GqlExpression & {
				if (expression.type != GqlExpressionType::PROPERTY_REFERENCE) {
					return expression;
				}
				if (!expression.left) {
					throw InternalException("GQL DELETE field selection has no receiver");
				}
				auto &receiver = self(self, *expression.left);
				if (receiver.type != GqlExpressionType::RECORD_CONSTRUCTOR ||
				    receiver.field_names.size() != receiver.arguments.size()) {
					throw BinderException("GQL DELETE field selection requires a record value");
				}
				for (idx_t field_index = 0; field_index < receiver.field_names.size(); field_index++) {
					if (receiver.field_names[field_index].value == expression.property.value) {
						return self(self, *receiver.arguments[field_index]);
					}
				}
				throw BinderException("GQL DELETE record field '%s' does not exist", expression.property.value);
			};
			auto append_delete_target = [&](auto &self, const GqlMutation &mutation,
			                                const GqlExpression &expression) -> void {
				auto &target = resolve_delete_selection(resolve_delete_selection, expression);
				switch (target.type) {
				case GqlExpressionType::VARIABLE_REFERENCE:
					append_delete_binding(mutation, target.variable);
					return;
				case GqlExpressionType::LIST_CONSTRUCTOR:
				case GqlExpressionType::RECORD_CONSTRUCTOR:
					for (const auto &child : target.arguments) {
						self(self, mutation, *child);
					}
					return;
				case GqlExpressionType::LITERAL:
					if (target.literal.type == GqlLiteralType::NULL_VALUE) {
						saw_null_delete_target = true;
						return;
					}
					break;
				default:
					break;
				}
				auto bound_target = BindExpression(target);
				throw BinderException("GQL DELETE target has invalid type %d",
				                      static_cast<int>(bound_target->result_type.id));
			};
			for (const auto &mutation : statement.mutations) {
				if (mutation.type == GqlMutationType::DELETE_ELEMENT) {
					if (!mutation.target) {
						throw InternalException("GQL DELETE mutation has no target expression");
					}
					append_delete_target(append_delete_target, mutation, *mutation.target);
					continue;
				}
				auto symbol_index = ResolveIndex(mutation.variable);
				auto &target = bindings[symbol_index];
				if (target.type.id == GqlTypeId::PATH) {
					throw BinderException("GQL path mutation targets are only valid in DELETE");
				}
				if (target.type.id != GqlTypeId::NODE && target.type.id != GqlTypeId::EDGE) {
					throw BinderException("GQL mutation target must be a node or edge");
				}
				if (mutation.type == GqlMutationType::SET_PROPERTIES ||
				    (mutation.type == GqlMutationType::MERGE_PROPERTIES && mutation.value)) {
					if (!mutation.value) {
						throw InternalException("GQL property-map mutation has no value expression");
					}
					auto &map = ResolveValueExpression(*mutation.value);
					if (map.type != GqlExpressionType::RECORD_CONSTRUCTOR ||
					    map.field_names.size() != map.arguments.size()) {
						throw BinderException("GQL property-map mutation requires a record value");
					}
					if (mutation.type == GqlMutationType::SET_PROPERTIES) {
						GqlMutation clear;
						clear.type = GqlMutationType::CLEAR_PROPERTIES;
						clear.variable = mutation.variable;
						clear.source = mutation.source;
						append_mutation(clear, target.index, target.type);
					}
					for (idx_t field_index = 0; field_index < map.field_names.size(); field_index++) {
						GqlMutation assignment;
						assignment.type = GqlMutationType::SET_PROPERTY;
						assignment.variable = mutation.variable;
						assignment.name = map.field_names[field_index];
						assignment.value = map.arguments[field_index];
						assignment.source = map.arguments[field_index]->source;
						append_mutation(assignment, target.index, target.type);
					}
					if (mutation.type == GqlMutationType::MERGE_PROPERTIES && map.field_names.empty()) {
						GqlMutation no_op = mutation;
						no_op.value.reset();
						append_mutation(no_op, target.index, target.type);
					}
					continue;
				}
				append_mutation(mutation, target.index, target.type);
			}
			if (bound_mutations.empty() && saw_null_delete_target) {
				GqlBoundMutation null_mutation;
				null_mutation.type = GqlMutationType::DELETE_ELEMENT;
				null_mutation.binding_type = {GqlTypeId::NODE, true};
				null_mutation.detach = statement.mutations[0].detach;
				null_mutation.source = statement.mutations[0].source;
				auto null_target = make_shared_ptr<GqlBoundExpression>();
				null_target->expression_type = GqlExpressionType::LITERAL;
				null_target->literal.type = GqlLiteralType::NULL_VALUE;
				null_target->result_type = {GqlTypeId::ELEMENT_ID, true};
				project->projections.push_back({std::move(null_target), "gql_target_id_0", null_mutation.source});
				bound_mutations.push_back(std::move(null_mutation));
			} else if (bound_mutations.empty()) {
				throw BinderException("GQL DELETE collection target contains no elements");
			}
		}
		if (!statement.insertion) {
			project->distinct = true;
		}
	} else {
		for (const auto &projection : statement.projections) {
			if (!projection.expression) {
				throw InternalException("GQL MATCH contains an empty projection");
			}
			auto expression = BindExpression(*projection.expression);
			GqlBoundProjection bound_projection;
			bound_projection.name = ProjectionName(projection, *expression);
			bound_projection.expression = std::move(expression);
			bound_projection.source = projection.source;
			project->projections.push_back(std::move(bound_projection));
		}
	}
	project->visible_projection_count = project->projections.size();
	for (const auto &group : statement.group_by_variables) {
		(void)Resolve(group);
	}
	for (const auto &order : statement.order_by) {
		GqlBoundOrderBy bound_order;
		bound_order.expression = BindExpression(*order.expression);
		for (idx_t index = 0; index < project->projections.size(); index++) {
			if (ExpressionsEqual(*bound_order.expression, *project->projections[index].expression)) {
				bound_order.projection_index = index;
				break;
			}
		}
		if (bound_order.projection_index == DConstants::INVALID_INDEX) {
			if (statement.distinct || saw_call_clause) {
				throw BinderException(
				    "GQL ORDER BY expressions must appear in RETURN when DISTINCT or CALL is used");
			}
			bound_order.projection_index = project->projections.size();
			GqlBoundProjection hidden_order;
			hidden_order.expression = bound_order.expression;
			hidden_order.name = "gql_hidden_order_" + to_string(project->order_by.size());
			hidden_order.source = order.source;
			project->projections.push_back(std::move(hidden_order));
		}
		bound_order.descending = order.descending;
		bound_order.nulls_first = order.nulls_first;
		bound_order.null_order_specified = order.null_order_specified;
		bound_order.source = order.source;
		project->order_by.push_back(std::move(bound_order));
	}
	if (!statement.has_mutation) {
		project->distinct = statement.distinct;
		project->has_offset = statement.has_offset;
		project->has_limit = statement.has_limit;
		project->offset = statement.offset;
		project->limit = statement.limit;
	}
	if (project->projections.empty()) {
		throw BinderException("GQL MATCH requires at least one projection");
	}
	project->child = std::move(current);

	GqlLogicalPlan result;
	result.bindings = bindings;
	result.root = std::move(project);
	result.binding_count = next_binding_index;
	result.mutations = std::move(bound_mutations);
	result.insertion = std::move(bound_insertion);
	GqlOptimize(result);
	return result;
}

} // namespace duckdb
