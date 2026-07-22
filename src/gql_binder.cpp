#include "gql_binder.hpp"

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

GqlBinding &GqlBinder::Resolve(const GqlIdentifier &identifier) {
	auto entry = binding_map.find(identifier.value);
	if (entry == binding_map.end()) {
		throw BinderException("GQL binding variable '%s' is not defined (line %llu, column %llu)", identifier.value,
		                      static_cast<unsigned long long>(identifier.source.start_line),
		                      static_cast<unsigned long long>(identifier.source.start_column));
	}
	return bindings[entry->second];
}

shared_ptr<GqlBoundExpression> GqlBinder::BindExpression(const GqlExpression &expression) {
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
	case GqlExpressionType::VARIABLE_REFERENCE: {
		auto &binding = Resolve(expression.variable);
		if (binding.type.id == GqlTypeId::PATH) {
			auto path = path_bindings.find(expression.variable.value);
			if (path == path_bindings.end()) {
				throw InternalException("GQL path binding is missing its elements");
			}
			result->expression_type = GqlExpressionType::FUNCTION;
			result->function_name = "__gql_path";
			for (const auto &element : path->second) {
				auto argument = make_shared_ptr<GqlBoundExpression>();
				argument->expression_type = GqlExpressionType::VARIABLE_REFERENCE;
				argument->binding_index = element.binding_index;
				argument->result_type = {
				    element.type == GqlPatternElementType::VERTEX ? GqlTypeId::NODE : GqlTypeId::EDGE,
				    binding.type.nullable};
				argument->source = element.source;
				result->arguments.push_back(std::move(argument));
			}
			result->result_type = binding.type;
			return result;
		}
		result->binding_index = binding.index;
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
		if (result->left->result_type.id != GqlTypeId::NODE &&
		    result->left->result_type.id != GqlTypeId::EDGE) {
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
	vector<pair<idx_t, idx_t>> ranged_elements;
	vector<pair<idx_t, idx_t>> any_direction_elements;
	idx_t alternative_count = 1;
	for (idx_t pattern_index = 0; pattern_index < statement.patterns.size(); pattern_index++) {
		const auto &pattern = statement.patterns[pattern_index];
		for (idx_t element_index = 0; element_index < pattern.elements.size(); element_index++) {
			const auto &element = pattern.elements[element_index];
			if (element.type == GqlPatternElementType::EDGE &&
			    element.edge_direction == GqlEdgeDirection::ANY) {
				if (alternative_count > GQL_MAX_FINITE_PATH_ALTERNATIVES / 2) {
					throw BinderException("GQL any-direction patterns produce more than 64 native alternatives");
				}
				alternative_count *= 2;
				any_direction_elements.emplace_back(pattern_index, element_index);
			}
			if (!element.quantified || element.unbounded ||
			    element.minimum_repetitions == element.maximum_repetitions) {
				continue;
			}
			if (element.minimum_repetitions == 0 || element.minimum_repetitions > element.maximum_repetitions) {
				throw BinderException("GQL path quantifier lower bound must be between "
				                      "1 and its upper bound");
			}
			auto count = element.maximum_repetitions - element.minimum_repetitions + 1;
			if (count > GQL_MAX_FINITE_PATH_ALTERNATIVES ||
			    alternative_count > GQL_MAX_FINITE_PATH_ALTERNATIVES / count) {
				throw BinderException("GQL finite path quantifiers produce more than "
				                      "64 native alternatives");
			}
			alternative_count *= count;
			ranged_elements.emplace_back(pattern_index, element_index);
		}
	}

	vector<GqlLogicalPlan> result;
	result.reserve(alternative_count);
	for (idx_t alternative = 0; alternative < alternative_count; alternative++) {
		GqlMatchStatement branch(statement);
		auto selection = alternative;
		for (const auto &location : ranged_elements) {
			auto &element = branch.patterns[location.first].elements[location.second];
			auto count = element.maximum_repetitions - element.minimum_repetitions + 1;
			auto repetitions = element.minimum_repetitions + selection % count;
			selection /= count;
			element.minimum_repetitions = repetitions;
			element.maximum_repetitions = repetitions;
		}
		for (const auto &location : any_direction_elements) {
			auto &element = branch.patterns[location.first].elements[location.second];
			element.edge_direction = selection % 2 == 0
			                             ? GqlEdgeDirection::RIGHT
			                             : GqlEdgeDirection::LEFT;
			selection /= 2;
		}
		result.push_back(Bind(branch));
	}
	return result;
}

GqlLogicalPlan GqlBinder::Bind(const GqlMatchStatement &statement) {
	bindings.clear();
	binding_map.clear();
	path_bindings.clear();
	auto match = make_shared_ptr<GqlLogicalMatch>();
	idx_t next_binding_index = 0;
	for (const auto &pattern : statement.patterns) {
		GqlBoundPattern bound_pattern;
		bound_pattern.optional = pattern.optional;
		bound_pattern.optional_stage = pattern.optional_stage;
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
				auto existing = binding_map.find(element.variable.value);
				if (existing != binding_map.end()) {
					auto &binding = bindings[existing->second];
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
					binding_map.emplace(binding.name, bindings.size());
					bindings.push_back(std::move(binding));
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
			    element.minimum_repetitions != element.maximum_repetitions || !element.variable.IsEmpty()) {
				throw InternalException("Invalid fixed quantified GQL path element");
			}
			for (idx_t repetition = 0; repetition < element.minimum_repetitions; repetition++) {
				bind_element(element, true);
				if (repetition + 1 < element.minimum_repetitions) {
					GqlPatternElement intermediate;
					intermediate.type = GqlPatternElementType::VERTEX;
					intermediate.source = element.quantifier_source;
					bind_element(intermediate, true);
				}
			}
		}
		if (!pattern.variable.IsEmpty()) {
			if (binding_map.find(pattern.variable.value) != binding_map.end()) {
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
			binding_map.emplace(binding.name, bindings.size());
			bindings.push_back(std::move(binding));
			path_bindings.emplace(pattern.variable.value, bound_pattern.elements);
		}
		match->patterns.push_back(std::move(bound_pattern));
	}
	match->binding_count = next_binding_index;
	match->optional = statement.optional;

	shared_ptr<GqlLogicalOperator> current = match;
	if (!statement.predicate_optional_stages.empty() &&
	    statement.predicate_optional_stages.size() != statement.predicates.size()) {
		throw InternalException("GQL MATCH predicate stage metadata is inconsistent");
	}
	for (idx_t predicate_index = 0; predicate_index < statement.predicates.size(); predicate_index++) {
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
		auto optional_stage = statement.predicate_optional_stages.empty()
		                          ? 0
		                          : statement.predicate_optional_stages[predicate_index];
		if (optional_stage > 0) {
			match->optional_predicates.push_back(std::move(bound_predicate));
			match->optional_predicate_stages.push_back(optional_stage);
		} else {
			auto filter = make_shared_ptr<GqlLogicalFilter>(std::move(bound_predicate));
			filter->child = std::move(current);
			current = std::move(filter);
		}
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
				auto existing = vertex.variable.IsEmpty() ? binding_map.end() : binding_map.find(vertex.variable.value);
				if (existing != binding_map.end()) {
					auto &binding = bindings[existing->second];
					if (binding.type.id != GqlTypeId::NODE) {
						throw BinderException("GQL INSERT cannot reuse bound non-node variable '%s' as a node",
						                      vertex.variable.value);
					}
					if (statement.insertion->edges.empty() || !vertex.labels.empty() || !vertex.properties.empty()) {
						throw BinderException("GQL INSERT node variable '%s' is already bound",
						                      vertex.variable.value);
					}
					bound_vertex.existing = true;
					bound_vertex.binding_index = binding.index;
					bound_vertex.existing_id_column =
					    "gql_insert_existing_vertex_" + std::to_string(vertex_index);
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
						bind_properties(vertex,
						                "gql_insert_vertex_property_" + std::to_string(vertex_index) + "_",
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
					if (binding_map.find(edge.variable.value) != binding_map.end() ||
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
			for (idx_t mutation_index = 0; mutation_index < statement.mutations.size(); mutation_index++) {
				auto &mutation = statement.mutations[mutation_index];
				auto &target = Resolve(mutation.variable);
				if (target.type.id != GqlTypeId::NODE && target.type.id != GqlTypeId::EDGE) {
					throw BinderException("GQL mutation target must be a node or edge");
				}
				GqlBoundMutation bound_mutation;
				bound_mutation.type = mutation.type;
				bound_mutation.binding_index = target.index;
				bound_mutation.binding_type = target.type;
				bound_mutation.name = mutation.name.value;
				bound_mutation.detach = mutation.detach;
				bound_mutation.source = mutation.source;

				auto target_expression = make_shared_ptr<GqlBoundExpression>();
				target_expression->expression_type = GqlExpressionType::VARIABLE_REFERENCE;
				target_expression->binding_index = target.index;
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
					if (value_type == GqlTypeId::UNKNOWN || value_type == GqlTypeId::NULL_VALUE ||
					    value_type == GqlTypeId::NODE || value_type == GqlTypeId::EDGE ||
					    value_type == GqlTypeId::PATH) {
						throw NotImplementedException("GQL SET property requires a non-null scalar value");
					}
					project->projections.push_back(
					    {bound_mutation.value, "gql_mutation_value_" + std::to_string(mutation_index), mutation.source});
				}
				bound_mutations.push_back(std::move(bound_mutation));
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
			throw BinderException("GQL ORDER BY expressions must also appear in RETURN");
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
	result.mutations = std::move(bound_mutations);
	result.insertion = std::move(bound_insertion);
	return result;
}

} // namespace duckdb
