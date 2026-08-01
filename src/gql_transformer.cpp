#include "gql_transformer.hpp"

#include "duckdb/common/string_util.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace duckdb {

static constexpr idx_t GQL_MAX_UNROLLED_PATH_REPETITIONS = 64;

GqlIdentifier GqlTransformer::TransformCallArgumentName(GQLParser::ProcedureArgumentContext &context) const {
	GqlIdentifier result;
	auto source = SourceRange(context);
	auto value_start = source.start_offset;
	if (value_start == 0 || value_start > query.size()) {
		return result;
	}
	auto offset = value_start;
	while (offset > 0 && std::isspace(static_cast<unsigned char>(query[offset - 1]))) {
		offset--;
	}
	if (offset < 2 || query[offset - 1] != '=' || query[offset - 2] != ':') {
		return result;
	}
	auto name_end = offset - 2;
	while (name_end > 0 && std::isspace(static_cast<unsigned char>(query[name_end - 1]))) {
		name_end--;
	}
	auto name_start = name_end;
	while (name_start > 0 &&
	       (std::isalnum(static_cast<unsigned char>(query[name_start - 1])) || query[name_start - 1] == '_')) {
		name_start--;
	}
	if (name_start == name_end ||
	    !(std::isalpha(static_cast<unsigned char>(query[name_start])) || query[name_start] == '_')) {
		return result;
	}
	result.value = StringUtil::Lower(query.substr(name_start, name_end - name_start));
	result.source = SourceRange(name_start, name_end);
	return result;
}

static bool TransformPathBound(GQLParser::UnsignedIntegerContext *integer, idx_t &result) {
	if (!integer || !integer->UNSIGNED_DECIMAL_INTEGER()) {
		return false;
	}
	try {
		auto parsed = std::stoull(integer->getText());
		if (parsed > GQL_MAX_UNROLLED_PATH_REPETITIONS) {
			return false;
		}
		result = static_cast<idx_t>(parsed);
		return true;
	} catch (...) {
		return false;
	}
}

static bool TransformNonNegativeInteger(GQLParser::NonNegativeIntegerSpecificationContext *specification,
                                        idx_t &result) {
	if (!specification || !specification->unsignedInteger() ||
	    !specification->unsignedInteger()->UNSIGNED_DECIMAL_INTEGER()) {
		return false;
	}
	try {
		auto parsed = std::stoull(specification->unsignedInteger()->getText());
		result = NumericCast<idx_t>(parsed);
		return true;
	} catch (...) {
		return false;
	}
}

template <class TARGET>
static void CollectContexts(antlr4::tree::ParseTree *tree, vector<TARGET *> &result) {
	if (auto context = dynamic_cast<TARGET *>(tree)) {
		result.push_back(context);
	}
	for (auto child : tree->children) {
		CollectContexts(child, result);
	}
}

shared_ptr<GqlStatement> GqlTransformer::Transform(GQLParser::GqlProgramContext &root) {
	statement.reset();
	vector<GQLParser::NamedProcedureCallContext *> procedure_calls;
	vector<GQLParser::SimpleMatchStatementContext *> match_statements;
	vector<GQLParser::PrimitiveDataModifyingStatementContext *> modifying_statements;
	vector<GQLParser::ReturnStatementContext *> return_statements;
	vector<GQLParser::OrderByAndPageStatementContext *> order_statements;
	CollectContexts(&root, procedure_calls);
	CollectContexts(&root, match_statements);
	CollectContexts(&root, modifying_statements);
	CollectContexts(&root, return_statements);
	CollectContexts(&root, order_statements);
	if (!procedure_calls.empty() && match_statements.empty()) {
		TransformCall(root);
		if (!statement) {
			Unsupported(root, "CALL procedure pipeline");
		}
		return statement;
	}
	if (!match_statements.empty() && !modifying_statements.empty()) {
		TransformMatch(root);
		if (!statement) {
			Unsupported(root, "MATCH with data modification pipeline");
		}
		return statement;
	}
	visit(&root);
	if (statement && statement->type == GqlStatementType::INSERT && !return_statements.empty()) {
		auto insert = dynamic_cast<GqlInsertStatement *>(statement.get());
		if (!insert) {
			throw InternalException("Invalid GQL INSERT statement");
		}
		auto unsupported = [&](const string &feature) {
			Unsupported(root, feature);
			return statement;
		};
		if (return_statements.size() != 1 || !order_statements.empty()) {
			return unsupported("INSERT RETURN with multiple result clauses or ordering");
		}
		auto body = return_statements[0]->returnStatementBody();
		if (!body || body->ASTERISK() || body->setQuantifier() || body->groupByClause() || !body->returnItemList() ||
		    body->returnItemList()->returnItem().size() != 1) {
			return unsupported("INSERT RETURN form other than one node variable");
		}
		GqlProjection projection;
		if (!TransformProjection(body->returnItemList()->returnItem()[0], projection) || !projection.expression ||
		    projection.expression->type != GqlExpressionType::VARIABLE_REFERENCE) {
			return unsupported("INSERT RETURN expression other than a node variable");
		}
		idx_t vertex_index = DConstants::INVALID_INDEX;
		for (idx_t index = 0; index < insert->vertices.size(); index++) {
			if (insert->vertices[index].variable.value != projection.expression->variable.value) {
				continue;
			}
			if (vertex_index != DConstants::INVALID_INDEX) {
				return unsupported("ambiguous INSERT RETURN node variable");
			}
			vertex_index = index;
		}
		if (vertex_index == DConstants::INVALID_INDEX) {
			for (const auto &edge : insert->edges) {
				if (edge.variable.value == projection.expression->variable.value) {
					return unsupported("INSERT RETURN edge values");
				}
			}
			return unsupported("INSERT RETURN unknown node variable");
		}
		insert->return_vertex_index = vertex_index;
		insert->return_name =
		    projection.alias.IsEmpty() ? projection.expression->variable.value : projection.alias.value;
	}
	if (!statement) {
		TransformMatch(root);
	}
	if (!statement) {
		Unsupported(root, "statement form");
	}
	return statement;
}

std::any GqlTransformer::visitCreateGraphStatement(GQLParser::CreateGraphStatementContext *context) {
	if (context->PROPERTY()) {
		Unsupported(*context, "PROPERTY GRAPH spelling");
		return {};
	}
	if (context->OR() || context->REPLACE()) {
		Unsupported(*context, "CREATE OR REPLACE GRAPH");
		return {};
	}
	if (context->graphSource()) {
		Unsupported(*context, "copied graph creation");
		return {};
	}
	auto parent_and_name = context->catalogGraphParentAndName();
	if (!parent_and_name || parent_and_name->catalogObjectParentReference()) {
		Unsupported(*context, "qualified graph names");
		return {};
	}
	auto graph_name = parent_and_name->graphName();
	if (!graph_name || graph_name->delimitedGraphName()) {
		Unsupported(*context, "delimited graph names");
		return {};
	}
	GqlGraphSchemaDefinition schema;
	if (auto open = context->openGraphType()) {
		if (!open->ANY() || open->typed()) {
			Unsupported(*context, "typed open graph creation");
			return {};
		}
	} else if (auto of_type = context->ofGraphType()) {
		if (!of_type->typed() || !of_type->nestedGraphTypeSpecification() || of_type->graphTypeReference() ||
		    of_type->graphTypeLikeGraph()) {
			Unsupported(*context, "non-inline typed graph creation");
			return {};
		}
		schema.kind = GqlGraphSchemaKind::INLINE;
		schema.typed = true;
		if (!TransformInlineGraphSchema(*of_type->nestedGraphTypeSpecification(), schema)) {
			Unsupported(*context, "typed inline graph schema form");
			return {};
		}
	} else {
		Unsupported(*context, "graph schema");
		return {};
	}
	statement = make_shared_ptr<GqlCreateGraphStatement>(SourceRange(*context), TransformIdentifier(*graph_name),
	                                                     context->IF() != nullptr, std::move(schema));
	return {};
}

bool GqlTransformer::TransformInlineGraphSchema(GQLParser::NestedGraphTypeSpecificationContext &context,
                                                GqlGraphSchemaDefinition &result) {
	auto body = context.graphTypeSpecificationBody();
	if (!body || !body->elementTypeList()) {
		return false;
	}
	for (auto specification : body->elementTypeList()->elementTypeSpecification()) {
		GqlGraphElementDefinition element;
		element.source = SourceRange(*specification);
		if (auto node = specification->nodeTypeSpecification()) {
			if (!TransformNodeType(*node, element)) {
				return false;
			}
		} else if (auto edge = specification->edgeTypeSpecification()) {
			if (!TransformEdgeType(*edge, element)) {
				return false;
			}
		} else {
			return false;
		}
		result.elements.push_back(std::move(element));
	}
	return !result.elements.empty();
}

bool GqlTransformer::TransformNodeType(GQLParser::NodeTypeSpecificationContext &context,
                                       GqlGraphElementDefinition &result) {
	auto pattern = context.nodeTypePattern();
	if (!pattern || context.nodeTypePhrase()) {
		return false;
	}
	result.kind = GqlPatternElementType::VERTEX;
	if (auto name = pattern->nodeTypeName()) {
		if (!IsRegularIdentifier(name->getText())) {
			return false;
		}
		result.type_name = TransformIdentifier(*name);
	}
	if (auto alias = pattern->localNodeTypeAlias()) {
		if (!IsRegularIdentifier(alias->getText())) {
			return false;
		}
		result.local_alias = TransformIdentifier(*alias);
	}
	if (!TransformNodeTypeFiller(pattern->nodeTypeFiller(), result)) {
		return false;
	}
	if (result.type_name.IsEmpty()) {
		if (!result.local_alias.IsEmpty()) {
			result.type_name = result.local_alias;
		} else if (!result.labels.empty()) {
			result.type_name = result.labels[0];
		}
	}
	if (result.local_alias.IsEmpty()) {
		result.local_alias = result.type_name;
	}
	return !result.type_name.IsEmpty();
}

bool GqlTransformer::TransformEdgeType(GQLParser::EdgeTypeSpecificationContext &context,
                                       GqlGraphElementDefinition &result) {
	auto pattern = context.edgeTypePattern();
	if (!pattern || context.edgeTypePhrase()) {
		return false;
	}
	result.kind = GqlPatternElementType::EDGE;
	if (auto name = pattern->edgeTypeName()) {
		if (!IsRegularIdentifier(name->getText())) {
			return false;
		}
		result.type_name = TransformIdentifier(*name);
	}

	GQLParser::SourceNodeTypeReferenceContext *source = nullptr;
	GQLParser::DestinationNodeTypeReferenceContext *target = nullptr;
	GQLParser::EdgeTypeFillerContext *filler = nullptr;
	if (auto directed = pattern->edgeTypePatternDirected()) {
		if (auto right = directed->edgeTypePatternPointingRight()) {
			source = right->sourceNodeTypeReference();
			target = right->destinationNodeTypeReference();
			filler = right->arcTypePointingRight()->edgeTypeFiller();
			result.direction = GqlEdgeDirection::RIGHT;
		} else if (auto left = directed->edgeTypePatternPointingLeft()) {
			source = left->sourceNodeTypeReference();
			target = left->destinationNodeTypeReference();
			filler = left->arcTypePointingLeft()->edgeTypeFiller();
			result.direction = GqlEdgeDirection::LEFT;
		} else {
			return false;
		}
	} else if (auto undirected = pattern->edgeTypePatternUndirected()) {
		source = undirected->sourceNodeTypeReference();
		target = undirected->destinationNodeTypeReference();
		filler = undirected->arcTypeUndirected()->edgeTypeFiller();
		result.direction = GqlEdgeDirection::ANY;
	} else {
		return false;
	}
	if (!source || !target || !source->sourceNodeTypeAlias() || !target->destinationNodeTypeAlias() ||
	    source->nodeTypeFiller() || target->nodeTypeFiller() ||
	    !IsRegularIdentifier(source->sourceNodeTypeAlias()->getText()) ||
	    !IsRegularIdentifier(target->destinationNodeTypeAlias()->getText())) {
		return false;
	}
	result.source_alias = TransformIdentifier(*source->sourceNodeTypeAlias());
	result.target_alias = TransformIdentifier(*target->destinationNodeTypeAlias());
	if (!TransformEdgeTypeFiller(filler, result)) {
		return false;
	}
	if (result.type_name.IsEmpty() && !result.labels.empty()) {
		result.type_name = result.labels[0];
	}
	return !result.type_name.IsEmpty();
}

bool GqlTransformer::TransformNodeTypeFiller(GQLParser::NodeTypeFillerContext *filler,
                                             GqlGraphElementDefinition &result) {
	if (!filler) {
		return true;
	}
	if (filler->nodeTypeKeyLabelSet()) {
		return false;
	}
	auto content = filler->nodeTypeImpliedContent();
	if (!content) {
		return true;
	}
	if (auto labels = content->nodeTypeLabelSet()) {
		if (!TransformGraphTypeLabels(labels->labelSetPhrase(), result.labels)) {
			return false;
		}
	}
	if (auto properties = content->nodeTypePropertyTypes()) {
		if (!TransformGraphTypeProperties(properties->propertyTypesSpecification(), result.properties)) {
			return false;
		}
	}
	return true;
}

bool GqlTransformer::TransformEdgeTypeFiller(GQLParser::EdgeTypeFillerContext *filler,
                                             GqlGraphElementDefinition &result) {
	if (!filler) {
		return true;
	}
	if (filler->edgeTypeKeyLabelSet()) {
		return false;
	}
	auto content = filler->edgeTypeImpliedContent();
	if (!content) {
		return true;
	}
	if (auto labels = content->edgeTypeLabelSet()) {
		if (!TransformGraphTypeLabels(labels->labelSetPhrase(), result.labels)) {
			return false;
		}
	}
	if (auto properties = content->edgeTypePropertyTypes()) {
		if (!TransformGraphTypeProperties(properties->propertyTypesSpecification(), result.properties)) {
			return false;
		}
	}
	return true;
}

bool GqlTransformer::TransformGraphTypeLabels(GQLParser::LabelSetPhraseContext *phrase, vector<GqlIdentifier> &result) {
	if (!phrase) {
		return true;
	}
	if (auto label = phrase->labelName()) {
		if (!IsRegularIdentifier(label->getText())) {
			return false;
		}
		result.push_back(TransformIdentifier(*label));
		return true;
	}
	auto specification = phrase->labelSetSpecification();
	if (!specification) {
		return false;
	}
	for (auto label : specification->labelName()) {
		if (!IsRegularIdentifier(label->getText())) {
			return false;
		}
		result.push_back(TransformIdentifier(*label));
	}
	return !result.empty();
}

bool GqlTransformer::TransformGraphTypeProperties(GQLParser::PropertyTypesSpecificationContext *specification,
                                                  vector<GqlGraphPropertyDefinition> &result) {
	if (!specification || !specification->propertyTypeList()) {
		return true;
	}
	for (auto property : specification->propertyTypeList()->propertyType()) {
		if (!property->propertyName() || !property->propertyValueType() ||
		    !property->propertyValueType()->valueType() || !IsRegularIdentifier(property->propertyName()->getText())) {
			return false;
		}
		GqlGraphPropertyDefinition definition;
		definition.name = TransformIdentifier(*property->propertyName());
		definition.source = SourceRange(*property);
		definition.gql_type = StringUtil::Upper(property->propertyValueType()->valueType()->getText());
		definition.nullable = !StringUtil::EndsWith(definition.gql_type, "NOTNULL") ||
		                      dynamic_cast<GQLParser::ClosedDynamicUnionTypeAtl2Context *>(
		                          property->propertyValueType()->valueType()) != nullptr;
		if (!definition.nullable && StringUtil::EndsWith(definition.gql_type, "NOTNULL")) {
			definition.gql_type.resize(definition.gql_type.size() - 7);
		}
		result.push_back(std::move(definition));
	}
	return true;
}

std::any GqlTransformer::visitDropGraphStatement(GQLParser::DropGraphStatementContext *context) {
	if (context->PROPERTY()) {
		Unsupported(*context, "PROPERTY GRAPH spelling");
		return {};
	}
	auto parent_and_name = context->catalogGraphParentAndName();
	if (!parent_and_name || parent_and_name->catalogObjectParentReference()) {
		Unsupported(*context, "qualified graph names");
		return {};
	}
	auto graph_name = parent_and_name->graphName();
	if (!graph_name || graph_name->delimitedGraphName()) {
		Unsupported(*context, "delimited graph names");
		return {};
	}
	statement = make_shared_ptr<GqlDropGraphStatement>(SourceRange(*context), TransformIdentifier(*graph_name),
	                                                   context->IF() != nullptr);
	return {};
}

std::any GqlTransformer::visitSessionSetGraphClause(GQLParser::SessionSetGraphClauseContext *context) {
	if (context->PROPERTY()) {
		Unsupported(*context, "PROPERTY GRAPH spelling");
		return {};
	}
	auto expression = context->graphExpression();
	if (!expression || !IsRegularIdentifier(expression->getText())) {
		Unsupported(*context, "non-simple graph expressions");
		return {};
	}
	statement = make_shared_ptr<GqlSessionSetGraphStatement>(SourceRange(*context), TransformIdentifier(*expression));
	return {};
}

std::any GqlTransformer::visitInsertStatement(GQLParser::InsertStatementContext *context) {
	auto insert = TransformInsert(*context, false);
	if (insert) {
		statement = std::move(insert);
	}
	return {};
}

shared_ptr<GqlInsertStatement> GqlTransformer::TransformInsert(GQLParser::InsertStatementContext &context,
                                                               bool allow_expressions) {
	auto pattern = context.insertGraphPattern();
	if (!pattern || !pattern->insertPathPatternList()) {
		Unsupported(context, "empty INSERT pattern");
		return nullptr;
	}
	auto paths = pattern->insertPathPatternList()->insertPathPattern();
	if (paths.size() != 1) {
		Unsupported(context, "multiple INSERT paths");
		return nullptr;
	}
	auto nodes = paths[0]->insertNodePattern();
	auto edge_patterns = paths[0]->insertEdgePattern();
	if (nodes.empty() || edge_patterns.size() + 1 != nodes.size()) {
		Unsupported(context, "invalid INSERT path topology");
		return nullptr;
	}

	auto insert = make_shared_ptr<GqlInsertStatement>(SourceRange(context));
	for (auto node : nodes) {
		GqlInsertElement element;
		element.source = SourceRange(*node);
		if (!TransformInsertElement(node->insertElementPatternFiller(), element, allow_expressions)) {
			return nullptr;
		}
		insert->vertices.push_back(std::move(element));
	}
	for (idx_t index = 0; index < edge_patterns.size(); index++) {
		GqlInsertEdge edge;
		edge.source = SourceRange(*edge_patterns[index]);
		GQLParser::InsertElementPatternFillerContext *filler = nullptr;
		if (auto pointing_right = edge_patterns[index]->insertEdgePointingRight()) {
			edge.source_vertex = index;
			edge.target_vertex = index + 1;
			filler = pointing_right->insertElementPatternFiller();
		} else if (auto pointing_left = edge_patterns[index]->insertEdgePointingLeft()) {
			edge.source_vertex = index + 1;
			edge.target_vertex = index;
			filler = pointing_left->insertElementPatternFiller();
		} else {
			Unsupported(context, "undirected edge INSERT patterns");
			return nullptr;
		}
		if (!TransformInsertElement(filler, edge, allow_expressions)) {
			return nullptr;
		}
		insert->edges.push_back(std::move(edge));
	}
	return insert;
}

bool GqlTransformer::TransformInsertElement(GQLParser::InsertElementPatternFillerContext *filler,
                                            GqlInsertElement &element, bool allow_expressions) {
	if (!filler) {
		return true;
	}
	if (auto declaration = filler->elementVariableDeclaration()) {
		auto text = declaration->getText();
		if (!IsRegularIdentifier(text)) {
			Unsupported(*filler, "delimited variables in INSERT");
			return false;
		}
		element.variable = TransformIdentifier(*declaration);
	}
	auto specification = filler->labelAndPropertySetSpecification();
	if (!specification) {
		return true;
	}
	if (auto labels = specification->labelSetSpecification()) {
		for (auto label : labels->labelName()) {
			if (!IsRegularIdentifier(label->getText())) {
				Unsupported(*label, "delimited labels in INSERT");
				return false;
			}
			element.labels.push_back(TransformIdentifier(*label));
		}
	}
	auto property_specification = specification->elementPropertySpecification();
	if (!property_specification) {
		return true;
	}
	for (auto property : property_specification->propertyKeyValuePairList()->propertyKeyValuePair()) {
		if (!IsRegularIdentifier(property->propertyName()->getText())) {
			Unsupported(*property, "delimited property names in INSERT");
			return false;
		}
		GqlPropertyAssignment assignment;
		assignment.source = SourceRange(*property);
		assignment.name = TransformIdentifier(*property->propertyName());
		if (allow_expressions) {
			if (!TransformExpression(*property->valueExpression(), assignment.expression)) {
				Unsupported(*property, "INSERT property expression");
				return false;
			}
		} else if (!TransformLiteral(*property->valueExpression(), assignment.value)) {
			Unsupported(*property, "non-scalar literal property values in INSERT");
			return false;
		}
		element.properties.push_back(std::move(assignment));
	}
	return true;
}

bool GqlTransformer::TransformCall(GQLParser::GqlProgramContext &root) {
	vector<GQLParser::NamedProcedureCallContext *> procedure_calls;
	vector<GQLParser::ReturnStatementContext *> return_statements;
	vector<GQLParser::OrderByAndPageStatementContext *> order_statements;
	CollectContexts(&root, procedure_calls);
	CollectContexts(&root, return_statements);
	CollectContexts(&root, order_statements);
	auto fail = [&]() {
		if (!statement) {
			Unsupported(root, "CALL/YIELD/RETURN procedure pipeline");
		}
		return false;
	};
	if (procedure_calls.size() != 1 || return_statements.size() != 1 || order_statements.size() > 1) {
		return fail();
	}

	auto procedure = procedure_calls[0];
	auto reference = procedure->procedureReference();
	if (!reference) {
		return fail();
	}
	auto reference_text = reference->getText();
	auto separator = reference_text.find('.');
	auto namespace_text = reference_text.substr(0, separator);
	auto procedure_name_text = separator == string::npos ? string() : reference_text.substr(separator + 1);
	if (separator == string::npos || separator != reference_text.rfind('.') || namespace_text.empty() ||
	    procedure_name_text.empty() || namespace_text.front() == '"' || namespace_text.front() == '`' ||
	    procedure_name_text.front() == '"' || procedure_name_text.front() == '`' ||
	    !IsRegularIdentifier(namespace_text) || !IsRegularIdentifier(procedure_name_text) ||
	    !StringUtil::CIEquals(namespace_text, "algo")) {
		return fail();
	}

	auto call = make_shared_ptr<GqlCallStatement>(SourceRange(root));
	call->procedure_namespace.value = StringUtil::Lower(namespace_text);
	call->procedure_namespace.source = SourceRange(*reference);
	call->procedure_name.value = StringUtil::Lower(procedure_name_text);
	call->procedure_name.source = SourceRange(*reference);
	if (auto arguments = procedure->procedureArgumentList()) {
		for (auto argument : arguments->procedureArgument()) {
			GqlLiteral literal;
			if (!argument->valueExpression() || !TransformLiteral(*argument->valueExpression(), literal)) {
				return fail();
			}
			call->arguments.push_back(std::move(literal));
			call->argument_names.push_back(TransformCallArgumentName(*argument));
		}
	}

	auto yield = procedure->yieldClause();
	if (!yield || !yield->yieldItemList()) {
		return fail();
	}
	std::unordered_set<string> yielded_names;
	for (auto item : yield->yieldItemList()->yieldItem()) {
		if (!item->yieldItemName() || !item->yieldItemName()->fieldName() ||
		    !IsRegularIdentifier(item->yieldItemName()->fieldName()->getText())) {
			return fail();
		}
		GqlYieldItem yield_item;
		yield_item.field = TransformIdentifier(*item->yieldItemName()->fieldName());
		yield_item.source = SourceRange(*item);
		if (auto alias = item->yieldItemAlias()) {
			if (!alias->bindingVariable() || !IsRegularIdentifier(alias->bindingVariable()->getText())) {
				return fail();
			}
			yield_item.alias = TransformIdentifier(*alias->bindingVariable());
		}
		auto output_name = yield_item.alias.IsEmpty() ? yield_item.field.value : yield_item.alias.value;
		if (!yielded_names.insert(output_name).second) {
			return fail();
		}
		call->yield_items.push_back(std::move(yield_item));
	}
	if (call->yield_items.empty()) {
		return fail();
	}

	auto body = return_statements[0]->returnStatementBody();
	if (!body || body->groupByClause() || (!body->ASTERISK() && !body->returnItemList())) {
		return fail();
	}
	if (body->setQuantifier()) {
		call->distinct = body->setQuantifier()->DISTINCT() != nullptr;
	}
	std::unordered_set<string> projection_names;
	if (body->ASTERISK()) {
		vector<const GqlYieldItem *> sorted_yields;
		for (const auto &yield_item : call->yield_items) {
			sorted_yields.push_back(&yield_item);
		}
		std::sort(sorted_yields.begin(), sorted_yields.end(), [](const GqlYieldItem *left, const GqlYieldItem *right) {
			auto left_name = left->alias.IsEmpty() ? left->field.value : left->alias.value;
			auto right_name = right->alias.IsEmpty() ? right->field.value : right->alias.value;
			return left_name < right_name;
		});
		for (const auto yield_item : sorted_yields) {
			auto output_name = yield_item->alias.IsEmpty() ? yield_item->field.value : yield_item->alias.value;
			GqlProjection projection;
			projection.expression = make_shared_ptr<GqlExpression>();
			projection.expression->type = GqlExpressionType::VARIABLE_REFERENCE;
			projection.expression->variable.value = output_name;
			projection.expression->variable.source = yield_item->source;
			projection.expression->source = yield_item->source;
			projection.source = yield_item->source;
			call->projections.push_back(std::move(projection));
		}
	} else {
		for (auto item : body->returnItemList()->returnItem()) {
			GqlProjection projection;
			if (!TransformProjection(item, projection) ||
			    projection.expression->type != GqlExpressionType::VARIABLE_REFERENCE ||
			    yielded_names.find(projection.expression->variable.value) == yielded_names.end()) {
				return fail();
			}
			auto output_name =
			    projection.alias.IsEmpty() ? projection.expression->variable.value : projection.alias.value;
			if (!projection_names.insert(output_name).second) {
				return fail();
			}
			call->projections.push_back(std::move(projection));
		}
	}
	if (call->projections.empty()) {
		return fail();
	}

	if (!order_statements.empty()) {
		auto order_page = order_statements[0];
		if (auto order_clause = order_page->orderByClause()) {
			for (auto specification : order_clause->sortSpecificationList()->sortSpecification()) {
				GqlOrderBy order;
				order.source = SourceRange(*specification);
				auto expression = specification->sortKey()->aggregatingValueExpression()->valueExpression();
				if (!TransformExpression(*expression, order.expression) ||
				    order.expression->type != GqlExpressionType::VARIABLE_REFERENCE) {
					return fail();
				}
				auto name = order.expression->variable.value;
				if (yielded_names.find(name) == yielded_names.end() &&
				    projection_names.find(name) == projection_names.end()) {
					return fail();
				}
				if (auto ordering = specification->orderingSpecification()) {
					order.descending = ordering->DESC() || ordering->DESCENDING();
				}
				if (auto null_order = specification->nullOrdering()) {
					order.null_order_specified = true;
					order.nulls_first = null_order->FIRST() != nullptr;
				}
				call->order_by.push_back(std::move(order));
			}
		}
		if (auto offset = order_page->offsetClause()) {
			if (!TransformNonNegativeInteger(offset->nonNegativeIntegerSpecification(), call->offset)) {
				return fail();
			}
			call->has_offset = true;
		}
		if (auto limit = order_page->limitClause()) {
			if (!TransformNonNegativeInteger(limit->nonNegativeIntegerSpecification(), call->limit)) {
				return fail();
			}
			call->has_limit = true;
		}
	}
	statement = std::move(call);
	return true;
}

bool GqlTransformer::TransformMatch(GQLParser::GqlProgramContext &root) {
	vector<GQLParser::MatchStatementContext *> match_statements;
	vector<GQLParser::LetStatementContext *> let_statements;
	vector<GQLParser::FilterStatementContext *> filter_statements;
	vector<GQLParser::OrderByAndPageStatementContext *> order_statements;
	vector<GQLParser::PrimitiveQueryStatementContext *> primitive_statements;
	vector<GQLParser::PrimitiveDataModifyingStatementContext *> modifying_statements;
	vector<GQLParser::ReturnStatementContext *> return_statements;
	vector<GQLParser::NamedProcedureCallContext *> procedure_calls;
	CollectContexts(&root, match_statements);
	CollectContexts(&root, let_statements);
	CollectContexts(&root, filter_statements);
	CollectContexts(&root, order_statements);
	CollectContexts(&root, primitive_statements);
	CollectContexts(&root, modifying_statements);
	CollectContexts(&root, return_statements);
	CollectContexts(&root, procedure_calls);
	auto has_mutation = !modifying_statements.empty();
	if (match_statements.empty() && return_statements.empty() && !has_mutation) {
		return false;
	}
	auto fail = [&]() {
		if (!statement) {
			Unsupported(root, "MATCH pattern or projection");
		}
		return false;
	};
	auto base_primitives = match_statements.size() + let_statements.size() + filter_statements.size();
	auto expected_primitives = base_primitives + procedure_calls.size();
	if (modifying_statements.size() > 1 || procedure_calls.size() > 1 || (has_mutation && !procedure_calls.empty()) ||
	    (has_mutation ? !return_statements.empty() || !order_statements.empty() : return_statements.size() != 1) ||
	    order_statements.size() > 1 ||
	    (primitive_statements.size() != base_primitives &&
	     primitive_statements.size() != base_primitives + order_statements.size() &&
	     primitive_statements.size() != expected_primitives &&
	     primitive_statements.size() != expected_primitives + order_statements.size())) {
		return fail();
	}

	auto match = make_shared_ptr<GqlMatchStatement>(SourceRange(root));
	auto append_match = [&](GQLParser::SimpleMatchStatementContext &simple_match, bool optional, idx_t optional_stage,
	                        GqlQueryClause &clause) {
		auto binding_table = simple_match.graphPatternBindingTable();
		auto graph_pattern = binding_table->graphPattern();
		if (binding_table->graphPatternYieldClause() || graph_pattern->matchMode() || graph_pattern->keepClause()) {
			return false;
		}
		auto paths = graph_pattern->pathPatternList()->pathPattern();
		if (paths.empty()) {
			return false;
		}
		clause.type = GqlQueryClauseType::MATCH;
		clause.pattern_begin = match->patterns.size();
		clause.optional = optional;
		clause.optional_stage = optional_stage;
		for (auto path : paths) {
			if (path->pathPatternPrefix()) {
				return false;
			}
			auto path_expression = dynamic_cast<GQLParser::PpePathTermContext *>(path->pathPatternExpression());
			if (!path_expression) {
				return false;
			}
			auto factors = path_expression->pathTerm()->pathFactor();
			if (factors.empty() || factors.size() % 2 == 0) {
				return false;
			}

			GqlPattern ast_pattern;
			ast_pattern.optional = optional;
			ast_pattern.optional_stage = optional_stage;
			ast_pattern.source = SourceRange(*path);
			if (auto declaration = path->pathVariableDeclaration()) {
				ast_pattern.variable = TransformIdentifier(*declaration->pathVariable());
			}
			for (idx_t index = 0; index < factors.size(); index++) {
				GQLParser::PathPrimaryContext *path_primary = nullptr;
				GQLParser::GraphPatternQuantifierContext *quantifier = nullptr;
				if (auto factor = dynamic_cast<GQLParser::PfPathPrimaryContext *>(factors[index])) {
					path_primary = factor->pathPrimary();
				} else if (auto factor = dynamic_cast<GQLParser::PfQuantifiedPathPrimaryContext *>(factors[index])) {
					path_primary = factor->pathPrimary();
					quantifier = factor->graphPatternQuantifier();
				} else {
					return false;
				}
				auto primary = dynamic_cast<GQLParser::PpElementPatternContext *>(path_primary);
				if (!primary) {
					return false;
				}
				auto element = primary->elementPattern();
				GqlPatternElement ast_element;
				ast_element.source = SourceRange(*element);
				if (index % 2 == 0) {
					if (!element->nodePattern() ||
					    !TransformMatchElement(element->nodePattern()->elementPatternFiller(),
					                           GqlPatternElementType::VERTEX, ast_element)) {
						return false;
					}
				} else {
					auto edge = element->edgePattern();
					if (!edge) {
						return false;
					}
					GQLParser::ElementPatternFillerContext *filler = nullptr;
					if (auto full = edge->fullEdgePattern()) {
						if (auto pointing_right = full->fullEdgePointingRight()) {
							filler = pointing_right->elementPatternFiller();
							ast_element.edge_direction = GqlEdgeDirection::RIGHT;
						} else if (auto pointing_left = full->fullEdgePointingLeft()) {
							filler = pointing_left->elementPatternFiller();
							ast_element.edge_direction = GqlEdgeDirection::LEFT;
						} else if (auto any = full->fullEdgeAnyDirection()) {
							filler = any->elementPatternFiller();
							ast_element.edge_direction = GqlEdgeDirection::ANY;
						} else {
							return false;
						}
					} else if (auto abbreviated = edge->abbreviatedEdgePattern()) {
						if (abbreviated->RIGHT_ARROW()) {
							ast_element.edge_direction = GqlEdgeDirection::RIGHT;
						} else if (abbreviated->LEFT_ARROW()) {
							ast_element.edge_direction = GqlEdgeDirection::LEFT;
						} else if (abbreviated->MINUS_SIGN() || abbreviated->LEFT_MINUS_RIGHT()) {
							ast_element.edge_direction = GqlEdgeDirection::ANY;
						} else {
							return false;
						}
					} else {
						return false;
					}
					if (!TransformMatchElement(filler, GqlPatternElementType::EDGE, ast_element)) {
						return false;
					}
				}
				if (quantifier) {
					if (index % 2 == 0) {
						Unsupported(*quantifier, "non-fixed or non-edge path quantifiers");
						return false;
					}
					if (!ast_element.variable.IsEmpty()) {
						Unsupported(*quantifier, "group variables in quantified paths");
						return false;
					}
					idx_t minimum_repetitions;
					idx_t maximum_repetitions;
					bool unbounded = false;
					if (quantifier->ASTERISK()) {
						minimum_repetitions = 0;
						maximum_repetitions = 0;
						unbounded = true;
					} else if (quantifier->PLUS_SIGN()) {
						minimum_repetitions = 1;
						maximum_repetitions = 1;
						unbounded = true;
					} else if (auto fixed = quantifier->fixedQuantifier()) {
						if (!TransformPathBound(fixed->unsignedInteger(), minimum_repetitions) ||
						    minimum_repetitions == 0) {
							Unsupported(*quantifier, "fixed path quantifier outside the supported range 1..64");
							return false;
						}
						maximum_repetitions = minimum_repetitions;
					} else if (auto general = quantifier->generalQuantifier()) {
						minimum_repetitions = 0;
						if (general->lowerBound() &&
						    !TransformPathBound(general->lowerBound()->unsignedInteger(), minimum_repetitions)) {
							Unsupported(*quantifier, "path quantifier bounds outside the supported range 1..64");
							return false;
						}
						if (!general->upperBound()) {
							maximum_repetitions = minimum_repetitions;
							unbounded = true;
						} else if (!TransformPathBound(general->upperBound()->unsignedInteger(), maximum_repetitions) ||
						           minimum_repetitions == 0 || maximum_repetitions == 0) {
							Unsupported(*quantifier, "path quantifier bounds outside the supported range 1..64");
							return false;
						}
					} else {
						Unsupported(*quantifier, "path quantifier form");
						return false;
					}
					ast_element.quantified = true;
					ast_element.unbounded = unbounded;
					ast_element.minimum_repetitions = minimum_repetitions;
					ast_element.maximum_repetitions = maximum_repetitions;
					ast_element.quantifier_source = SourceRange(*quantifier);
				}
				ast_pattern.elements.push_back(std::move(ast_element));
			}
			match->patterns.push_back(std::move(ast_pattern));
		}
		clause.pattern_count = match->patterns.size() - clause.pattern_begin;

		if (auto where = graph_pattern->graphPatternWhereClause()) {
			shared_ptr<GqlExpression> predicate;
			if (!TransformSearchCondition(*where->searchCondition(), predicate)) {
				return false;
			}
			clause.predicate_indices.push_back(match->predicates.size());
			match->predicates.push_back(std::move(predicate));
		}
		return true;
	};

	bool saw_mandatory = false;
	bool saw_optional = false;
	idx_t optional_stage = 0;
	for (auto match_statement : match_statements) {
		GQLParser::SimpleMatchStatementContext *simple_match = nullptr;
		bool optional = false;
		if (match_statement->simpleMatchStatement()) {
			simple_match = match_statement->simpleMatchStatement();
			saw_mandatory = true;
		} else if (auto optional_match = match_statement->optionalMatchStatement()) {
			auto operand = optional_match->optionalOperand();
			if (!operand || !operand->simpleMatchStatement()) {
				return fail();
			}
			simple_match = operand->simpleMatchStatement();
			optional = true;
			if (saw_mandatory || saw_optional) {
				optional_stage++;
			}
			saw_optional = true;
		}
		if (!simple_match || (has_mutation && optional)) {
			return fail();
		}
		GqlQueryClause clause;
		clause.source = SourceRange(*match_statement);
		if (!append_match(*simple_match, optional, optional ? optional_stage : 0, clause)) {
			return fail();
		}
		match->clauses.push_back(std::move(clause));
	}
	for (auto filter : filter_statements) {
		auto condition = filter->searchCondition();
		if (!condition && filter->whereClause()) {
			condition = filter->whereClause()->searchCondition();
		}
		shared_ptr<GqlExpression> predicate;
		if (!condition || !TransformSearchCondition(*condition, predicate)) {
			return fail();
		}
		GqlQueryClause clause;
		clause.type = GqlQueryClauseType::FILTER;
		clause.source = SourceRange(*filter);
		clause.predicate_index = match->predicates.size();
		match->predicates.push_back(std::move(predicate));
		match->clauses.push_back(std::move(clause));
	}
	for (auto let : let_statements) {
		GqlQueryClause clause;
		clause.type = GqlQueryClauseType::LET;
		clause.source = SourceRange(*let);
		clause.let_begin = match->let_bindings.size();
		for (auto definition : let->letVariableDefinitionList()->letVariableDefinition()) {
			if (definition->valueVariableDefinition() || !definition->bindingVariable() ||
			    !definition->valueExpression() || !IsRegularIdentifier(definition->bindingVariable()->getText())) {
				return fail();
			}
			GqlLetBinding binding;
			binding.variable = TransformIdentifier(*definition->bindingVariable());
			binding.source = SourceRange(*definition);
			if (!TransformExpression(*definition->valueExpression(), binding.expression)) {
				return fail();
			}
			match->let_bindings.push_back(std::move(binding));
		}
		clause.let_count = match->let_bindings.size() - clause.let_begin;
		if (clause.let_count == 0) {
			return fail();
		}
		match->clauses.push_back(std::move(clause));
	}
	for (auto procedure : procedure_calls) {
		auto reference = procedure->procedureReference();
		if (!reference) {
			return fail();
		}
		auto reference_text = reference->getText();
		auto separator = reference_text.find('.');
		auto namespace_text = reference_text.substr(0, separator);
		auto procedure_name_text = separator == string::npos ? string() : reference_text.substr(separator + 1);
		if (separator == string::npos || separator != reference_text.rfind('.') || namespace_text.empty() ||
		    procedure_name_text.empty() || !IsRegularIdentifier(namespace_text) ||
		    !IsRegularIdentifier(procedure_name_text) || !StringUtil::CIEquals(namespace_text, "algo")) {
			return fail();
		}

		GqlProcedureCall call;
		call.source = SourceRange(*procedure);
		call.procedure_namespace.value = StringUtil::Lower(namespace_text);
		call.procedure_namespace.source = SourceRange(*reference);
		call.procedure_name.value = StringUtil::Lower(procedure_name_text);
		call.procedure_name.source = SourceRange(*reference);
		if (auto arguments = procedure->procedureArgumentList()) {
			for (auto argument : arguments->procedureArgument()) {
				shared_ptr<GqlExpression> expression;
				if (!argument->valueExpression() || !TransformExpression(*argument->valueExpression(), expression)) {
					return fail();
				}
				call.arguments.push_back(std::move(expression));
			}
		}
		auto yield = procedure->yieldClause();
		if (!yield || !yield->yieldItemList()) {
			return fail();
		}
		std::unordered_set<string> output_names;
		for (auto item : yield->yieldItemList()->yieldItem()) {
			if (!item->yieldItemName() || !item->yieldItemName()->fieldName() ||
			    !IsRegularIdentifier(item->yieldItemName()->fieldName()->getText())) {
				return fail();
			}
			GqlYieldItem yield_item;
			yield_item.field = TransformIdentifier(*item->yieldItemName()->fieldName());
			yield_item.source = SourceRange(*item);
			if (auto alias = item->yieldItemAlias()) {
				if (!alias->bindingVariable() || !IsRegularIdentifier(alias->bindingVariable()->getText())) {
					return fail();
				}
				yield_item.alias = TransformIdentifier(*alias->bindingVariable());
			}
			auto output_name = yield_item.alias.IsEmpty() ? yield_item.field.value : yield_item.alias.value;
			if (!output_names.insert(output_name).second) {
				return fail();
			}
			call.yield_items.push_back(std::move(yield_item));
		}
		if (call.yield_items.empty()) {
			return fail();
		}
		GqlQueryClause clause;
		clause.type = GqlQueryClauseType::CALL;
		clause.source = call.source;
		clause.procedure_call_index = match->procedure_calls.size();
		match->procedure_calls.push_back(std::move(call));
		match->clauses.push_back(std::move(clause));
	}
	std::stable_sort(match->clauses.begin(), match->clauses.end(),
	                 [](const GqlQueryClause &left, const GqlQueryClause &right) {
		                 return left.source.start_offset < right.source.start_offset;
	                 });
	bool saw_match_clause = false;
	bool saw_call_clause = false;
	idx_t active_optional_stage = 0;
	for (auto &clause : match->clauses) {
		if (clause.type == GqlQueryClauseType::MATCH) {
			if (saw_call_clause) {
				return fail();
			}
			saw_match_clause = true;
			active_optional_stage = clause.optional ? clause.optional_stage : 0;
			continue;
		}
		if (clause.type == GqlQueryClauseType::LET) {
			if (!saw_match_clause || saw_call_clause || clause.let_count == 0 ||
			    clause.let_begin > match->let_bindings.size() ||
			    clause.let_count > match->let_bindings.size() - clause.let_begin) {
				return fail();
			}
			// LET is a new linear-query boundary. A following FILTER applies to the
			// complete row produced by the optional stage, not to its right-hand
			// match condition.
			active_optional_stage = 0;
			continue;
		}
		if (clause.type == GqlQueryClauseType::CALL) {
			if (!saw_match_clause || saw_call_clause || clause.procedure_call_index >= match->procedure_calls.size()) {
				return fail();
			}
			saw_call_clause = true;
			active_optional_stage = 0;
			continue;
		}
		if (clause.type != GqlQueryClauseType::FILTER || !saw_match_clause || saw_call_clause ||
		    clause.predicate_index >= match->predicates.size()) {
			return fail();
		}
		clause.optional_stage = active_optional_stage;
	}
	if (has_mutation) {
		if (!TransformMutation(*modifying_statements[0], *match)) {
			return false;
		}
		statement = std::move(match);
		return true;
	}
	if (!procedure_calls.empty() && !saw_call_clause) {
		return fail();
	}

	auto body = return_statements[0]->returnStatementBody();
	if (!body->ASTERISK() && !body->returnItemList()) {
		return fail();
	}
	if (body->ASTERISK() && body->groupByClause()) {
		return fail();
	}
	if (body->setQuantifier()) {
		match->distinct = body->setQuantifier()->DISTINCT() != nullptr;
	}
	if (body->ASTERISK()) {
		match->return_all = true;
	} else {
		for (auto item : body->returnItemList()->returnItem()) {
			GqlProjection projection;
			if (!TransformProjection(item, projection)) {
				return fail();
			}
			if (!procedure_calls.empty() &&
			    (!projection.expression || projection.expression->type != GqlExpressionType::VARIABLE_REFERENCE)) {
				return fail();
			}
			match->projections.push_back(std::move(projection));
		}
	}
	if (!match->return_all && match->projections.empty()) {
		return fail();
	}
	if (auto group_by = body->groupByClause()) {
		auto elements = group_by->groupingElementList();
		if (!elements) {
			return fail();
		}
		for (auto grouping : elements->groupingElement()) {
			auto reference = grouping->bindingVariableReference();
			if (!reference || !IsRegularIdentifier(reference->getText())) {
				return fail();
			}
			match->group_by_variables.push_back(TransformIdentifier(*reference));
		}
	}
	if (!order_statements.empty()) {
		auto order_page = order_statements[0];
		if (auto order_clause = order_page->orderByClause()) {
			for (auto specification : order_clause->sortSpecificationList()->sortSpecification()) {
				GqlOrderBy order;
				order.source = SourceRange(*specification);
				auto expression = specification->sortKey()->aggregatingValueExpression()->valueExpression();
				if (!TransformExpression(*expression, order.expression)) {
					return fail();
				}
				if (!procedure_calls.empty() && order.expression->type != GqlExpressionType::VARIABLE_REFERENCE) {
					return fail();
				}
				if (order.expression->type == GqlExpressionType::VARIABLE_REFERENCE) {
					for (const auto &projection : match->projections) {
						if (!projection.alias.IsEmpty() && projection.alias.value == order.expression->variable.value) {
							order.expression = projection.expression;
							break;
						}
					}
				}
				if (auto ordering = specification->orderingSpecification()) {
					order.descending = ordering->DESC() || ordering->DESCENDING();
				}
				if (auto null_order = specification->nullOrdering()) {
					order.null_order_specified = true;
					order.nulls_first = null_order->FIRST() != nullptr;
				}
				match->order_by.push_back(std::move(order));
			}
		}
		if (auto offset = order_page->offsetClause()) {
			if (!TransformNonNegativeInteger(offset->nonNegativeIntegerSpecification(), match->offset)) {
				Unsupported(*offset, "dynamic or invalid OFFSET");
				return false;
			}
			match->has_offset = true;
		}
		if (auto limit = order_page->limitClause()) {
			if (!TransformNonNegativeInteger(limit->nonNegativeIntegerSpecification(), match->limit)) {
				Unsupported(*limit, "dynamic or invalid LIMIT");
				return false;
			}
			match->has_limit = true;
		}
	}
	GqlQueryClause return_clause;
	return_clause.type = GqlQueryClauseType::RETURN;
	return_clause.source = SourceRange(*return_statements[0]);
	match->clauses.push_back(std::move(return_clause));
	statement = std::move(match);
	return true;
}

bool GqlTransformer::TransformMutation(GQLParser::PrimitiveDataModifyingStatementContext &context,
                                       GqlMatchStatement &match) {
	auto fail = [&](antlr4::ParserRuleContext &location, const string &feature) {
		Unsupported(location, feature);
		return false;
	};
	if (auto set = context.setStatement()) {
		auto items = set->setItemList()->setItem();
		for (auto item : items) {
			GqlMutation mutation;
			mutation.source = SourceRange(*item);
			if (auto property = item->setPropertyItem()) {
				if (!IsRegularIdentifier(property->bindingVariableReference()->getText()) ||
				    !IsRegularIdentifier(property->propertyName()->getText())) {
					return fail(*property, "delimited SET property targets");
				}
				mutation.type = GqlMutationType::SET_PROPERTY;
				mutation.variable = TransformIdentifier(*property->bindingVariableReference());
				mutation.name = TransformIdentifier(*property->propertyName());
				if (!TransformExpression(*property->valueExpression(), mutation.value)) {
					return fail(*property, "SET property expression");
				}
				match.mutations.push_back(std::move(mutation));
			} else if (auto all = item->setAllPropertiesItem()) {
				if (!IsRegularIdentifier(all->bindingVariableReference()->getText())) {
					return fail(*all, "delimited SET all-properties target");
				}
				auto variable = TransformIdentifier(*all->bindingVariableReference());
				auto equals_offset = all->EQUALS_OPERATOR()->getSymbol()->getStartIndex();
				auto merge = equals_offset > 0 && equals_offset <= query.size() && query[equals_offset - 1] == '+';
				auto rhs_offset = NumericCast<idx_t>(equals_offset + 1);
				auto skip_trivia = [&]() {
					while (rhs_offset < query.size()) {
						if (std::isspace(static_cast<unsigned char>(query[rhs_offset]))) {
							rhs_offset++;
							continue;
						}
						if (rhs_offset + 1 < query.size() && query[rhs_offset] == '-' && query[rhs_offset + 1] == '-') {
							rhs_offset += 2;
							while (rhs_offset < query.size() && query[rhs_offset] != '\n') {
								rhs_offset++;
							}
							continue;
						}
						if (rhs_offset + 1 < query.size() && query[rhs_offset] == '/' && query[rhs_offset + 1] == '*') {
							auto end = query.find("*/", rhs_offset + 2);
							if (end == string::npos) {
								return false;
							}
							rhs_offset = end + 2;
							continue;
						}
						break;
					}
					return true;
				};
				if (!skip_trivia() || rhs_offset >= query.size()) {
					return fail(*all, "SET all-properties expression");
				}
				if (query[rhs_offset] != '{') {
					auto expression = make_shared_ptr<GqlExpression>();
					expression->type = GqlExpressionType::VARIABLE_REFERENCE;
					auto identifier_start = rhs_offset;
					string identifier;
					bool delimited = query[rhs_offset] == '`' || query[rhs_offset] == '"';
					if (delimited) {
						auto quote = query[rhs_offset++];
						while (rhs_offset < query.size()) {
							if (query[rhs_offset] != quote) {
								rhs_offset++;
								continue;
							}
							if (rhs_offset + 1 < query.size() && query[rhs_offset + 1] == quote) {
								rhs_offset += 2;
								continue;
							}
							rhs_offset++;
							break;
						}
						identifier = UnquoteString(query.substr(identifier_start, rhs_offset - identifier_start));
					} else {
						if (!(std::isalpha(static_cast<unsigned char>(query[rhs_offset])) ||
						      query[rhs_offset] == '_')) {
							return fail(*all, "SET all-properties expression");
						}
						rhs_offset++;
						while (
						    rhs_offset < query.size() &&
						    (std::isalnum(static_cast<unsigned char>(query[rhs_offset])) || query[rhs_offset] == '_')) {
							rhs_offset++;
						}
						identifier = query.substr(identifier_start, rhs_offset - identifier_start);
					}
					expression->variable.value = StringUtil::Lower(identifier);
					expression->variable.delimited = delimited;
					expression->variable.source = SourceRange(identifier_start, rhs_offset);
					expression->source = expression->variable.source;
					mutation.type = merge ? GqlMutationType::MERGE_PROPERTIES : GqlMutationType::SET_PROPERTIES;
					mutation.variable = std::move(variable);
					mutation.value = std::move(expression);
					mutation.source = SourceRange(mutation.source.start_offset, rhs_offset);
					match.mutations.push_back(std::move(mutation));
					continue;
				}
				if (!merge) {
					mutation.type = GqlMutationType::CLEAR_PROPERTIES;
					mutation.variable = variable;
					match.mutations.push_back(std::move(mutation));
				}
				if (auto properties = all->propertyKeyValuePairList()) {
					for (auto property : properties->propertyKeyValuePair()) {
						if (!IsRegularIdentifier(property->propertyName()->getText())) {
							return fail(*property, "delimited SET all-properties name");
						}
						GqlMutation assignment;
						assignment.type = GqlMutationType::SET_PROPERTY;
						assignment.variable = variable;
						assignment.name = TransformIdentifier(*property->propertyName());
						assignment.source = SourceRange(*property);
						if (!TransformExpression(*property->valueExpression(), assignment.value)) {
							return fail(*property, "SET all-properties expression");
						}
						match.mutations.push_back(std::move(assignment));
					}
				} else if (merge) {
					mutation.type = GqlMutationType::MERGE_PROPERTIES;
					mutation.variable = variable;
					match.mutations.push_back(std::move(mutation));
				}
			} else if (auto label = item->setLabelItem()) {
				if (!IsRegularIdentifier(label->bindingVariableReference()->getText()) ||
				    !IsRegularIdentifier(label->labelName()->getText())) {
					return fail(*label, "delimited SET label targets");
				}
				auto variable = TransformIdentifier(*label->bindingVariableReference());
				vector<GqlIdentifier> labels {TransformIdentifier(*label->labelName())};
				if (!TransformChainedLabels(*label->labelName(), labels)) {
					return fail(*label, "chained SET label targets");
				}
				for (auto &name : labels) {
					GqlMutation label_mutation;
					label_mutation.type = GqlMutationType::SET_LABEL;
					label_mutation.variable = variable;
					label_mutation.name = std::move(name);
					label_mutation.source = label_mutation.name.source;
					match.mutations.push_back(std::move(label_mutation));
				}
			} else {
				return fail(*item, "SET item");
			}
		}
	} else if (auto remove = context.removeStatement()) {
		auto items = remove->removeItemList()->removeItem();
		for (auto item : items) {
			GqlMutation mutation;
			mutation.source = SourceRange(*item);
			if (auto property = item->removePropertyItem()) {
				if (!IsRegularIdentifier(property->bindingVariableReference()->getText()) ||
				    !IsRegularIdentifier(property->propertyName()->getText())) {
					return fail(*property, "delimited REMOVE property targets");
				}
				mutation.type = GqlMutationType::REMOVE_PROPERTY;
				mutation.variable = TransformIdentifier(*property->bindingVariableReference());
				mutation.name = TransformIdentifier(*property->propertyName());
			} else if (auto label = item->removeLabelItem()) {
				if (!IsRegularIdentifier(label->bindingVariableReference()->getText()) ||
				    !IsRegularIdentifier(label->labelName()->getText())) {
					return fail(*label, "delimited REMOVE label targets");
				}
				auto variable = TransformIdentifier(*label->bindingVariableReference());
				vector<GqlIdentifier> labels {TransformIdentifier(*label->labelName())};
				if (!TransformChainedLabels(*label->labelName(), labels)) {
					return fail(*label, "chained REMOVE label targets");
				}
				for (auto &name : labels) {
					GqlMutation label_mutation;
					label_mutation.type = GqlMutationType::REMOVE_LABEL;
					label_mutation.variable = variable;
					label_mutation.name = std::move(name);
					label_mutation.source = label_mutation.name.source;
					match.mutations.push_back(std::move(label_mutation));
				}
				continue;
			} else {
				return fail(*item, "REMOVE item");
			}
			match.mutations.push_back(std::move(mutation));
		}
	} else if (auto deletion = context.deleteStatement()) {
		auto items = deletion->deleteItemList()->deleteItem();
		for (auto item : items) {
			shared_ptr<GqlExpression> target;
			if (!TransformExpression(*item->valueExpression(), target)) {
				return fail(*item, "DELETE target expression");
			}
			GqlMutation mutation;
			mutation.source = SourceRange(*item);
			mutation.type = GqlMutationType::DELETE_ELEMENT;
			mutation.target = std::move(target);
			mutation.detach = deletion->DETACH() != nullptr;
			match.mutations.push_back(std::move(mutation));
		}
	} else if (auto insertion = context.insertStatement()) {
		match.insertion = TransformInsert(*insertion, true);
		if (!match.insertion) {
			return false;
		}
	} else {
		return fail(context, "data modification statement");
	}
	match.has_mutation = true;
	return true;
}

bool GqlTransformer::TransformMatchElement(GQLParser::ElementPatternFillerContext *filler, GqlPatternElementType type,
                                           GqlPatternElement &result) {
	result.type = type;
	if (!filler) {
		return true;
	}
	if (filler->elementPatternPredicate()) {
		return false;
	}
	if (auto declaration = filler->elementVariableDeclaration()) {
		if (!IsRegularIdentifier(declaration->getText())) {
			return false;
		}
		result.variable = TransformIdentifier(*declaration);
	}
	if (auto is_label = filler->isLabelExpression()) {
		if (!TransformLabelExpression(*is_label->labelExpression(), result.labels)) {
			return false;
		}
	}
	return true;
}

bool GqlTransformer::TransformLabelExpression(GQLParser::LabelExpressionContext &context,
                                              vector<GqlIdentifier> &labels) {
	if (auto name = dynamic_cast<GQLParser::LabelExpressionNameContext *>(&context)) {
		if (!IsRegularIdentifier(name->labelName()->getText())) {
			Unsupported(*name, "delimited labels in MATCH");
			return false;
		}
		labels.push_back(TransformIdentifier(*name->labelName()));
		return true;
	}
	if (auto conjunction = dynamic_cast<GQLParser::LabelExpressionConjunctionContext *>(&context)) {
		for (auto expression : conjunction->labelExpression()) {
			if (!TransformLabelExpression(*expression, labels)) {
				return false;
			}
		}
		return true;
	}
	if (auto parenthesized = dynamic_cast<GQLParser::LabelExpressionParenthesizedContext *>(&context)) {
		return TransformLabelExpression(*parenthesized->labelExpression(), labels);
	}
	return false;
}

bool GqlTransformer::TransformProjection(GQLParser::ReturnItemContext *item, GqlProjection &result) {
	auto expression = item->aggregatingValueExpression()->valueExpression();
	if (!TransformExpression(*expression, result.expression)) {
		return false;
	}
	result.source = SourceRange(*item);
	if (auto alias = item->returnItemAlias()) {
		if (!IsRegularIdentifier(alias->identifier()->getText())) {
			Unsupported(*alias, "delimited RETURN aliases");
			return false;
		}
		result.alias = TransformIdentifier(*alias->identifier());
	}
	return true;
}

bool GqlTransformer::TransformSearchCondition(GQLParser::SearchConditionContext &context,
                                              shared_ptr<GqlExpression> &result) {
	if (!context.booleanValueExpression()) {
		return false;
	}
	return TransformExpression(*context.booleanValueExpression()->valueExpression(), result);
}

bool GqlTransformer::TransformExpression(GQLParser::ValueExpressionContext &context,
                                         shared_ptr<GqlExpression> &result) {
	auto expression = make_shared_ptr<GqlExpression>();
	expression->source = SourceRange(context);
	if (auto primary = dynamic_cast<GQLParser::PrimaryExprAltContext *>(&context)) {
		return TransformExpressionPrimary(*primary->valueExpressionPrimary(), result);
	}
	if (auto function = dynamic_cast<GQLParser::ValueFunctionExprAltContext *>(&context)) {
		return TransformValueFunction(*function->valueFunction(), result);
	}
	if (auto signed_expression = dynamic_cast<GQLParser::SignedExprAltContext *>(&context)) {
		expression->type = GqlExpressionType::UNARY;
		expression->unary_operator = signed_expression->MINUS_SIGN() ? GqlUnaryOperator::MINUS : GqlUnaryOperator::PLUS;
		if (!TransformExpression(*signed_expression->valueExpression(), expression->left)) {
			return false;
		}
		result = std::move(expression);
		return true;
	}
	if (auto not_expression = dynamic_cast<GQLParser::NotExprAltContext *>(&context)) {
		expression->type = GqlExpressionType::UNARY;
		expression->unary_operator = GqlUnaryOperator::NOT;
		if (!TransformExpression(*not_expression->valueExpression(), expression->left)) {
			return false;
		}
		result = std::move(expression);
		return true;
	}
	auto transform_binary = [&](GQLParser::ValueExpressionContext &left, GQLParser::ValueExpressionContext &right,
	                            GqlBinaryOperator operation) {
		expression->type = GqlExpressionType::BINARY;
		expression->binary_operator = operation;
		if (!TransformExpression(left, expression->left) || !TransformExpression(right, expression->right)) {
			return false;
		}
		result = std::move(expression);
		return true;
	};
	if (auto mult = dynamic_cast<GQLParser::MultDivExprAltContext *>(&context)) {
		auto children = mult->valueExpression();
		return transform_binary(*children[0], *children[1],
		                        mult->ASTERISK() ? GqlBinaryOperator::MULTIPLY : GqlBinaryOperator::DIVIDE);
	}
	if (auto add = dynamic_cast<GQLParser::AddSubtractExprAltContext *>(&context)) {
		auto children = add->valueExpression();
		return transform_binary(*children[0], *children[1],
		                        add->PLUS_SIGN() ? GqlBinaryOperator::ADD : GqlBinaryOperator::SUBTRACT);
	}
	if (auto concatenate = dynamic_cast<GQLParser::ConcatenationExprAltContext *>(&context)) {
		auto children = concatenate->valueExpression();
		return transform_binary(*children[0], *children[1], GqlBinaryOperator::CONCATENATE);
	}
	if (auto comparison = dynamic_cast<GQLParser::ComparisonExprAltContext *>(&context)) {
		auto children = comparison->valueExpression();
		auto text = comparison->compOp()->getText();
		GqlBinaryOperator operation;
		if (text == "=") {
			operation = GqlBinaryOperator::EQUAL;
		} else if (text == "<>" || text == "!=") {
			operation = GqlBinaryOperator::NOT_EQUAL;
		} else if (text == "<") {
			operation = GqlBinaryOperator::LESS_THAN;
		} else if (text == ">") {
			operation = GqlBinaryOperator::GREATER_THAN;
		} else if (text == "<=") {
			operation = GqlBinaryOperator::LESS_THAN_OR_EQUAL;
		} else if (text == ">=") {
			operation = GqlBinaryOperator::GREATER_THAN_OR_EQUAL;
		} else {
			return false;
		}
		return transform_binary(*children[0], *children[1], operation);
	}
	if (auto conjunction = dynamic_cast<GQLParser::ConjunctiveExprAltContext *>(&context)) {
		auto children = conjunction->valueExpression();
		return transform_binary(*children[0], *children[1], GqlBinaryOperator::AND);
	}
	if (auto disjunction = dynamic_cast<GQLParser::DisjunctiveExprAltContext *>(&context)) {
		auto children = disjunction->valueExpression();
		return transform_binary(*children[0], *children[1],
		                        disjunction->OR() ? GqlBinaryOperator::OR : GqlBinaryOperator::XOR);
	}
	if (auto predicate_expression = dynamic_cast<GQLParser::PredicateExprAltContext *>(&context)) {
		auto predicate = predicate_expression->predicate();
		if (!predicate) {
			return false;
		}
		if (auto null_predicate = predicate->nullPredicate()) {
			expression->type = GqlExpressionType::IS_NULL;
			expression->negated = null_predicate->nullPredicatePart2()->NOT() != nullptr;
			if (!TransformExpressionPrimary(*null_predicate->valueExpressionPrimary(), expression->left)) {
				return false;
			}
			result = std::move(expression);
			return true;
		}
		if (auto labeled = predicate->labeledPredicate()) {
			auto variable = labeled->elementVariableReference();
			auto part = labeled->labeledPredicatePart2();
			if (!variable || !part || !part->labelExpression() || !IsRegularIdentifier(variable->getText())) {
				return false;
			}
			vector<GqlIdentifier> labels;
			if (!TransformLabelExpression(*part->labelExpression(), labels) || labels.empty()) {
				return false;
			}
			expression->type = GqlExpressionType::LABELED;
			expression->negated = part->isLabeledOrColon()->NOT() != nullptr;
			for (idx_t index = 0; index < labels.size(); index++) {
				if (index > 0) {
					expression->property.value += ";";
				}
				expression->property.value += labels[index].value;
			}
			expression->property.source = SourceRange(*part->labelExpression());
			expression->left = make_shared_ptr<GqlExpression>();
			expression->left->type = GqlExpressionType::VARIABLE_REFERENCE;
			expression->left->variable = TransformIdentifier(*variable);
			expression->left->source = SourceRange(*variable);
			result = std::move(expression);
			return true;
		}
		return false;
	}
	return false;
}

bool GqlTransformer::TransformExpressionPrimary(GQLParser::ValueExpressionPrimaryContext &context,
                                                shared_ptr<GqlExpression> &result) {
	if (auto parenthesized = context.parenthesizedValueExpression()) {
		return TransformExpression(*parenthesized->valueExpression(), result);
	}
	auto expression = make_shared_ptr<GqlExpression>();
	expression->source = SourceRange(context);
	if (auto aggregate = context.aggregateFunction()) {
		return TransformAggregate(*aggregate, result);
	}
	if (auto case_expression = context.caseExpression()) {
		auto abbreviation = case_expression->caseAbbreviation();
		if (!abbreviation || !abbreviation->COALESCE()) {
			return false;
		}
		expression->type = GqlExpressionType::FUNCTION;
		expression->function_name = "coalesce";
		for (auto argument : abbreviation->valueExpression()) {
			shared_ptr<GqlExpression> child;
			if (!TransformExpression(*argument, child)) {
				return false;
			}
			expression->arguments.push_back(std::move(child));
		}
		result = std::move(expression);
		return true;
	}
	if (auto specification = context.unsignedValueSpecification()) {
		auto literal = specification->unsignedLiteral();
		if (!literal) {
			return false;
		}
		if (auto general = literal->generalLiteral()) {
			if (auto list = general->listLiteral()) {
				return TransformListConstructor(*list->listValueConstructorByEnumeration(), result);
			}
			if (auto record = general->recordLiteral()) {
				return TransformRecordConstructor(*record->recordConstructor(), result);
			}
		}
		if (!TransformUnsignedLiteral(*literal, expression->literal)) {
			return false;
		}
		expression->type = GqlExpressionType::LITERAL;
		result = std::move(expression);
		return true;
	}
	if (auto element_id = context.element_idFunction()) {
		auto variable = element_id->elementVariableReference();
		if (!variable || !IsRegularIdentifier(variable->getText())) {
			return false;
		}
		expression->type = GqlExpressionType::ELEMENT_ID;
		expression->left = make_shared_ptr<GqlExpression>();
		expression->left->type = GqlExpressionType::VARIABLE_REFERENCE;
		expression->left->variable = TransformIdentifier(*variable);
		expression->left->source = SourceRange(*variable);
		result = std::move(expression);
		return true;
	}
	if (context.propertyName() && context.valueExpressionPrimary()) {
		if (!IsRegularIdentifier(context.propertyName()->getText())) {
			Unsupported(context, "delimited property references");
			return false;
		}
		if (!TransformExpressionPrimary(*context.valueExpressionPrimary(), expression->left)) {
			return false;
		}
		expression->type = GqlExpressionType::PROPERTY_REFERENCE;
		expression->property = TransformIdentifier(*context.propertyName());
		result = std::move(expression);
		return true;
	}
	if (auto variable = context.bindingVariableReference()) {
		if (!IsRegularIdentifier(variable->getText())) {
			return false;
		}
		expression->type = GqlExpressionType::VARIABLE_REFERENCE;
		expression->variable = TransformIdentifier(*variable);
		result = std::move(expression);
		return true;
	}
	return false;
}

bool GqlTransformer::TransformListConstructor(GQLParser::ListValueConstructorByEnumerationContext &context,
                                              shared_ptr<GqlExpression> &result) {
	auto expression = make_shared_ptr<GqlExpression>();
	expression->type = GqlExpressionType::LIST_CONSTRUCTOR;
	expression->source = SourceRange(context);
	if (context.listValueTypeName()) {
		return false;
	}
	if (auto elements = context.listElementList()) {
		for (auto element : elements->listElement()) {
			shared_ptr<GqlExpression> child;
			if (!TransformExpression(*element->valueExpression(), child)) {
				return false;
			}
			expression->arguments.push_back(std::move(child));
		}
	}
	result = std::move(expression);
	return true;
}

bool GqlTransformer::TransformRecordConstructor(GQLParser::RecordConstructorContext &context,
                                                shared_ptr<GqlExpression> &result) {
	auto expression = make_shared_ptr<GqlExpression>();
	expression->type = GqlExpressionType::RECORD_CONSTRUCTOR;
	expression->source = SourceRange(context);
	auto fields = context.fieldsSpecification();
	if (!fields) {
		return false;
	}
	std::unordered_set<string> names;
	if (auto field_list = fields->fieldList()) {
		for (auto field : field_list->field()) {
			if (!field->fieldName() || !IsRegularIdentifier(field->fieldName()->getText())) {
				return false;
			}
			auto name = TransformIdentifier(*field->fieldName());
			if (!names.insert(name.value).second) {
				return false;
			}
			shared_ptr<GqlExpression> child;
			if (!TransformExpression(*field->valueExpression(), child)) {
				return false;
			}
			expression->field_names.push_back(std::move(name));
			expression->arguments.push_back(std::move(child));
		}
	}
	result = std::move(expression);
	return true;
}

bool GqlTransformer::TransformAggregate(GQLParser::AggregateFunctionContext &context,
                                        shared_ptr<GqlExpression> &result) {
	auto expression = make_shared_ptr<GqlExpression>();
	expression->type = GqlExpressionType::FUNCTION;
	expression->aggregate = true;
	expression->source = SourceRange(context);
	if (context.COUNT() && context.ASTERISK()) {
		expression->function_name = "count";
		result = std::move(expression);
		return true;
	}
	auto general = context.generalSetFunction();
	if (!general || !general->generalSetFunctionType() || !general->valueExpression()) {
		return false;
	}
	expression->function_name = StringUtil::Lower(general->generalSetFunctionType()->getText());
	expression->distinct = general->setQuantifier() && general->setQuantifier()->DISTINCT();
	shared_ptr<GqlExpression> argument;
	if (!TransformExpression(*general->valueExpression(), argument)) {
		return false;
	}
	expression->arguments.push_back(std::move(argument));
	result = std::move(expression);
	return true;
}

bool GqlTransformer::TransformNumericExpression(GQLParser::NumericValueExpressionContext &context,
                                                shared_ptr<GqlExpression> &result) {
	if (context.valueExpressionPrimary()) {
		return TransformExpressionPrimary(*context.valueExpressionPrimary(), result);
	}
	if (context.numericValueFunction()) {
		return TransformNumericFunction(*context.numericValueFunction(), result);
	}
	auto children = context.numericValueExpression();
	if (children.size() == 1 && (context.PLUS_SIGN() || context.MINUS_SIGN())) {
		auto expression = make_shared_ptr<GqlExpression>();
		expression->type = GqlExpressionType::UNARY;
		expression->unary_operator = context.MINUS_SIGN() ? GqlUnaryOperator::MINUS : GqlUnaryOperator::PLUS;
		expression->source = SourceRange(context);
		if (!TransformNumericExpression(*children[0], expression->left)) {
			return false;
		}
		result = std::move(expression);
		return true;
	}
	if (children.size() == 2) {
		auto expression = make_shared_ptr<GqlExpression>();
		expression->type = GqlExpressionType::BINARY;
		expression->source = SourceRange(context);
		if (context.ASTERISK()) {
			expression->binary_operator = GqlBinaryOperator::MULTIPLY;
		} else if (context.SOLIDUS()) {
			expression->binary_operator = GqlBinaryOperator::DIVIDE;
		} else if (context.PLUS_SIGN()) {
			expression->binary_operator = GqlBinaryOperator::ADD;
		} else if (context.MINUS_SIGN()) {
			expression->binary_operator = GqlBinaryOperator::SUBTRACT;
		} else {
			return false;
		}
		if (!TransformNumericExpression(*children[0], expression->left) ||
		    !TransformNumericExpression(*children[1], expression->right)) {
			return false;
		}
		result = std::move(expression);
		return true;
	}
	return false;
}

bool GqlTransformer::TransformNumericFunction(GQLParser::NumericValueFunctionContext &context,
                                              shared_ptr<GqlExpression> &result) {
	auto make_function = [&](const string &name, GQLParser::NumericValueExpressionContext &argument) {
		auto expression = make_shared_ptr<GqlExpression>();
		expression->type = GqlExpressionType::FUNCTION;
		expression->function_name = name;
		expression->source = SourceRange(context);
		shared_ptr<GqlExpression> child;
		if (!TransformNumericExpression(argument, child)) {
			return false;
		}
		expression->arguments.push_back(std::move(child));
		result = std::move(expression);
		return true;
	};
	if (auto absolute = context.absoluteValueExpression()) {
		auto expression = make_shared_ptr<GqlExpression>();
		expression->type = GqlExpressionType::FUNCTION;
		expression->function_name = "abs";
		expression->source = SourceRange(context);
		shared_ptr<GqlExpression> child;
		if (!TransformExpression(*absolute->valueExpression(), child)) {
			return false;
		}
		expression->arguments.push_back(std::move(child));
		result = std::move(expression);
		return true;
	}
	if (auto modulus = context.modulusExpression()) {
		auto expression = make_shared_ptr<GqlExpression>();
		expression->type = GqlExpressionType::FUNCTION;
		expression->function_name = "mod";
		expression->source = SourceRange(context);
		shared_ptr<GqlExpression> left;
		shared_ptr<GqlExpression> right;
		if (!TransformNumericExpression(*modulus->numericValueExpressionDividend()->numericValueExpression(), left) ||
		    !TransformNumericExpression(*modulus->numericValueExpressionDivisor()->numericValueExpression(), right)) {
			return false;
		}
		expression->arguments.push_back(std::move(left));
		expression->arguments.push_back(std::move(right));
		result = std::move(expression);
		return true;
	}
	if (auto square_root = context.squareRoot()) {
		return make_function("sqrt", *square_root->numericValueExpression());
	}
	if (auto floor = context.floorFunction()) {
		return make_function("floor", *floor->numericValueExpression());
	}
	if (auto ceiling = context.ceilingFunction()) {
		return make_function("ceil", *ceiling->numericValueExpression());
	}
	if (auto length = context.lengthExpression()) {
		auto character = length->charLengthExpression();
		if (!character || !character->characterStringValueExpression()) {
			return false;
		}
		auto expression = make_shared_ptr<GqlExpression>();
		expression->type = GqlExpressionType::FUNCTION;
		expression->function_name = "char_length";
		expression->source = SourceRange(context);
		shared_ptr<GqlExpression> child;
		if (!TransformExpression(*character->characterStringValueExpression()->valueExpression(), child)) {
			return false;
		}
		expression->arguments.push_back(std::move(child));
		result = std::move(expression);
		return true;
	}
	return false;
}

bool GqlTransformer::TransformValueFunction(GQLParser::ValueFunctionContext &context,
                                            shared_ptr<GqlExpression> &result) {
	if (context.numericValueFunction()) {
		return TransformNumericFunction(*context.numericValueFunction(), result);
	}
	auto string_function = context.characterOrByteStringFunction();
	if (!string_function) {
		return false;
	}
	auto expression = make_shared_ptr<GqlExpression>();
	expression->type = GqlExpressionType::FUNCTION;
	expression->source = SourceRange(context);
	if (auto fold = string_function->foldCharacterString()) {
		expression->function_name = fold->UPPER() ? "upper" : "lower";
		shared_ptr<GqlExpression> child;
		if (!TransformExpression(*fold->valueExpression(), child)) {
			return false;
		}
		expression->arguments.push_back(std::move(child));
	} else if (auto substring = string_function->subCharacterOrByteString()) {
		expression->function_name = substring->LEFT() ? "left" : "right";
		shared_ptr<GqlExpression> value;
		shared_ptr<GqlExpression> length;
		if (!TransformExpression(*substring->valueExpression(), value) ||
		    !TransformNumericExpression(*substring->stringLength()->numericValueExpression(), length)) {
			return false;
		}
		expression->arguments.push_back(std::move(value));
		expression->arguments.push_back(std::move(length));
	} else if (auto trim = string_function->trimMultiCharacterCharacterString()) {
		expression->function_name = trim->LTRIM() ? "ltrim" : trim->RTRIM() ? "rtrim" : "trim";
		for (auto argument : trim->valueExpression()) {
			shared_ptr<GqlExpression> child;
			if (!TransformExpression(*argument, child)) {
				return false;
			}
			expression->arguments.push_back(std::move(child));
		}
	} else if (auto trim = string_function->trimSingleCharacterOrByteString()) {
		auto operands = trim->trimOperands();
		if (!operands || !operands->trimCharacterOrByteStringSource()) {
			return false;
		}
		expression->function_name = operands->trimSpecification() && operands->trimSpecification()->LEADING() ? "ltrim"
		                            : operands->trimSpecification() && operands->trimSpecification()->TRAILING()
		                                ? "rtrim"
		                                : "trim";
		shared_ptr<GqlExpression> source;
		if (!TransformExpression(*operands->trimCharacterOrByteStringSource()->valueExpression(), source)) {
			return false;
		}
		expression->arguments.push_back(std::move(source));
		if (operands->trimCharacterOrByteString()) {
			shared_ptr<GqlExpression> characters;
			if (!TransformExpression(*operands->trimCharacterOrByteString()->valueExpression(), characters)) {
				return false;
			}
			expression->arguments.push_back(std::move(characters));
		}
	} else if (auto normalize = string_function->normalizeCharacterString()) {
		if (normalize->normalForm() && !normalize->normalForm()->NFC()) {
			return false;
		}
		expression->function_name = "nfc_normalize";
		shared_ptr<GqlExpression> child;
		if (!TransformExpression(*normalize->valueExpression(), child)) {
			return false;
		}
		expression->arguments.push_back(std::move(child));
	} else {
		return false;
	}
	result = std::move(expression);
	return true;
}

bool GqlTransformer::TransformUnsignedLiteral(GQLParser::UnsignedLiteralContext &context, GqlLiteral &result) {
	result.source = SourceRange(context);
	if (auto numeric = context.unsignedNumericLiteral()) {
		if (auto exact = numeric->exactNumericLiteral()) {
			if (exact->unsignedInteger() && exact->unsignedInteger()->UNSIGNED_DECIMAL_INTEGER()) {
				result.type = GqlLiteralType::INTEGER;
			} else if (exact->UNSIGNED_DECIMAL_IN_COMMON_NOTATION_WITHOUT_SUFFIX()) {
				result.type = GqlLiteralType::DECIMAL;
			} else {
				return false;
			}
		} else if (auto approximate = numeric->approximateNumericLiteral()) {
			if (!approximate->UNSIGNED_DECIMAL_IN_SCIENTIFIC_NOTATION_WITHOUT_SUFFIX()) {
				return false;
			}
			result.type = GqlLiteralType::DOUBLE;
		} else {
			return false;
		}
		result.value = numeric->getText();
		return true;
	}
	auto general = context.generalLiteral();
	if (!general) {
		return false;
	}
	if (general->BOOLEAN_LITERAL()) {
		result.type = GqlLiteralType::BOOLEAN;
		result.value = StringUtil::Lower(general->BOOLEAN_LITERAL()->getText());
		return true;
	}
	if (auto string_literal = general->characterStringLiteral()) {
		result.type = GqlLiteralType::STRING;
		result.value = UnquoteString(string_literal->getText());
		return true;
	}
	if (general->nullLiteral()) {
		result.type = GqlLiteralType::NULL_VALUE;
		result.value.clear();
		return true;
	}
	return false;
}

bool GqlTransformer::TransformLiteral(GQLParser::ValueExpressionContext &context, GqlLiteral &result) {
	result.source = SourceRange(context);
	if (auto signed_expression = dynamic_cast<GQLParser::SignedExprAltContext *>(&context)) {
		GqlLiteral unsigned_literal;
		if (!TransformLiteral(*signed_expression->valueExpression(), unsigned_literal) ||
		    (unsigned_literal.type != GqlLiteralType::INTEGER && unsigned_literal.type != GqlLiteralType::DECIMAL &&
		     unsigned_literal.type != GqlLiteralType::DOUBLE)) {
			return false;
		}
		result = std::move(unsigned_literal);
		result.source = SourceRange(context);
		result.value = (signed_expression->MINUS_SIGN() ? "-" : "+") + result.value;
		return true;
	}

	auto primary_expression = dynamic_cast<GQLParser::PrimaryExprAltContext *>(&context);
	if (!primary_expression) {
		return false;
	}
	auto specification = primary_expression->valueExpressionPrimary()->unsignedValueSpecification();
	if (!specification || !specification->unsignedLiteral()) {
		return false;
	}
	if (!TransformUnsignedLiteral(*specification->unsignedLiteral(), result)) {
		return false;
	}
	return result.type != GqlLiteralType::NULL_VALUE;
}

bool GqlTransformer::TransformChainedLabels(antlr4::ParserRuleContext &first_label,
                                            vector<GqlIdentifier> &labels) const {
	auto stop = first_label.getStop();
	if (!stop) {
		return false;
	}
	auto offset = NumericCast<idx_t>(stop->getStopIndex() + 1);
	auto skip_trivia = [&]() {
		while (offset < query.size()) {
			if (std::isspace(static_cast<unsigned char>(query[offset]))) {
				offset++;
				continue;
			}
			if (offset + 1 < query.size() && query[offset] == '-' && query[offset + 1] == '-') {
				offset += 2;
				while (offset < query.size() && query[offset] != '\n') {
					offset++;
				}
				continue;
			}
			if (offset + 1 < query.size() && query[offset] == '/' && query[offset + 1] == '*') {
				auto end = query.find("*/", offset + 2);
				if (end == string::npos) {
					return false;
				}
				offset = end + 2;
				continue;
			}
			break;
		}
		return true;
	};
	if (!skip_trivia()) {
		return false;
	}
	while (offset < query.size() && query[offset] == ':') {
		offset++;
		if (!skip_trivia()) {
			return false;
		}
		auto start = offset;
		if (offset < query.size() && (query[offset] == '`' || query[offset] == '"')) {
			auto quote = query[offset++];
			bool closed = false;
			while (offset < query.size()) {
				if (query[offset] != quote) {
					offset++;
					continue;
				}
				if (offset + 1 < query.size() && query[offset + 1] == quote) {
					offset += 2;
					continue;
				}
				offset++;
				closed = true;
				break;
			}
			if (!closed) {
				return false;
			}
		} else {
			if (offset >= query.size() ||
			    !(std::isalpha(static_cast<unsigned char>(query[offset])) || query[offset] == '_')) {
				return false;
			}
			offset++;
			while (offset < query.size() &&
			       (std::isalnum(static_cast<unsigned char>(query[offset])) || query[offset] == '_')) {
				offset++;
			}
		}
		auto text = query.substr(start, offset - start);
		if (!IsRegularIdentifier(text)) {
			return false;
		}
		GqlIdentifier label;
		label.delimited = text.front() == '`' || text.front() == '"';
		label.value = StringUtil::Lower(label.delimited ? UnquoteString(text) : text);
		label.source = SourceRange(start, offset);
		labels.push_back(std::move(label));
		if (!skip_trivia()) {
			return false;
		}
	}
	return true;
}

void GqlTransformer::Unsupported(antlr4::ParserRuleContext &context, const string &feature) {
	statement = make_shared_ptr<GqlUnsupportedStatement>(SourceRange(context), feature);
}

bool GqlTransformer::IsRegularIdentifier(const string &value) {
	if (value.empty() || !(std::isalpha(static_cast<unsigned char>(value[0])) || value[0] == '_')) {
		return false;
	}
	for (idx_t index = 1; index < value.size(); index++) {
		auto character = static_cast<unsigned char>(value[index]);
		if (!(std::isalnum(character) || character == '_')) {
			return false;
		}
	}
	return true;
}

string GqlTransformer::UnquoteString(const string &text) {
	if (text.size() < 2) {
		return text;
	}
	auto quote = text.front();
	string result;
	for (idx_t index = 1; index + 1 < text.size(); index++) {
		if (text[index] == quote && index + 2 < text.size() && text[index + 1] == quote) {
			result += quote;
			index++;
		} else {
			result += text[index];
		}
	}
	return result;
}

GqlIdentifier GqlTransformer::TransformIdentifier(antlr4::ParserRuleContext &context) {
	GqlIdentifier result;
	auto text = context.getText();
	result.delimited = !text.empty() && (text[0] == '`' || text[0] == '"');
	result.value = StringUtil::Lower(result.delimited ? UnquoteString(text) : text);
	result.source = SourceRange(context);
	return result;
}

GqlSourceRange GqlTransformer::SourceRange(antlr4::ParserRuleContext &context) {
	GqlSourceRange result;
	auto start = context.getStart();
	auto stop = context.getStop();
	if (!start || !stop) {
		return result;
	}
	result.start_offset = start->getStartIndex();
	result.end_offset = stop->getStopIndex() + 1;
	result.start_line = start->getLine();
	result.start_column = start->getCharPositionInLine();
	result.end_line = stop->getLine();
	result.end_column = stop->getCharPositionInLine() + stop->getText().size();
	return result;
}

GqlSourceRange GqlTransformer::SourceRange(idx_t start, idx_t end) const {
	GqlSourceRange result;
	result.start_offset = start;
	result.end_offset = end;
	result.start_line = 1;
	result.start_column = 0;
	for (idx_t index = 0; index < start; index++) {
		if (query[index] == '\n') {
			result.start_line++;
			result.start_column = 0;
		} else {
			result.start_column++;
		}
	}
	result.end_line = result.start_line;
	result.end_column = result.start_column;
	for (idx_t index = start; index < end; index++) {
		if (query[index] == '\n') {
			result.end_line++;
			result.end_column = 0;
		} else {
			result.end_column++;
		}
	}
	return result;
}

} // namespace duckdb
