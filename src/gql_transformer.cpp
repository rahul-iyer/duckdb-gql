#include "gql_transformer.hpp"

#include "duckdb/common/string_util.hpp"

#include <cctype>

namespace duckdb {

static constexpr idx_t GQL_MAX_UNROLLED_PATH_REPETITIONS = 64;

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
	vector<GQLParser::SimpleMatchStatementContext *> match_statements;
	vector<GQLParser::PrimitiveDataModifyingStatementContext *> modifying_statements;
	CollectContexts(&root, match_statements);
	CollectContexts(&root, modifying_statements);
	if (!match_statements.empty() && !modifying_statements.empty()) {
		TransformMatch(root);
		if (!statement) {
			Unsupported(root, "MATCH with data modification pipeline");
		}
		return statement;
	}
	visit(&root);
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
	if (!context->openGraphType() || !context->openGraphType()->ANY() || context->ofGraphType() ||
	    context->graphSource()) {
		Unsupported(*context, "typed, copied, or closed graph creation");
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
	statement = make_shared_ptr<GqlCreateGraphStatement>(SourceRange(*context), TransformIdentifier(*graph_name),
	                                                     context->IF() != nullptr);
	return {};
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
	auto pattern = context->insertGraphPattern();
	if (!pattern || !pattern->insertPathPatternList()) {
		Unsupported(*context, "empty INSERT pattern");
		return {};
	}
	auto paths = pattern->insertPathPatternList()->insertPathPattern();
	if (paths.size() != 1) {
		Unsupported(*context, "multiple INSERT paths");
		return {};
	}
	auto nodes = paths[0]->insertNodePattern();
	auto edge_patterns = paths[0]->insertEdgePattern();
	if (nodes.empty() || edge_patterns.size() + 1 != nodes.size()) {
		Unsupported(*context, "invalid INSERT path topology");
		return {};
	}

	auto insert = make_shared_ptr<GqlInsertStatement>(SourceRange(*context));
	for (auto node : nodes) {
		GqlInsertElement element;
		element.source = SourceRange(*node);
		if (!TransformInsertElement(node->insertElementPatternFiller(), element)) {
			return {};
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
			Unsupported(*context, "undirected edge INSERT patterns");
			return {};
		}
		if (!TransformInsertElement(filler, edge)) {
			return {};
		}
		insert->edges.push_back(std::move(edge));
	}
	statement = std::move(insert);
	return {};
}

bool GqlTransformer::TransformInsertElement(GQLParser::InsertElementPatternFillerContext *filler,
                                            GqlInsertElement &element) {
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
		if (!TransformLiteral(*property->valueExpression(), assignment.value)) {
			Unsupported(*property, "non-scalar literal property values in INSERT");
			return false;
		}
		element.properties.push_back(std::move(assignment));
	}
	return true;
}

bool GqlTransformer::TransformMatch(GQLParser::GqlProgramContext &root) {
	vector<GQLParser::MatchStatementContext *> match_statements;
	vector<GQLParser::FilterStatementContext *> filter_statements;
	vector<GQLParser::OrderByAndPageStatementContext *> order_statements;
	vector<GQLParser::PrimitiveQueryStatementContext *> primitive_statements;
	vector<GQLParser::PrimitiveDataModifyingStatementContext *> modifying_statements;
	vector<GQLParser::ReturnStatementContext *> return_statements;
	CollectContexts(&root, match_statements);
	CollectContexts(&root, filter_statements);
	CollectContexts(&root, order_statements);
	CollectContexts(&root, primitive_statements);
	CollectContexts(&root, modifying_statements);
	CollectContexts(&root, return_statements);
	auto has_mutation = !modifying_statements.empty();
	if (match_statements.empty() && return_statements.empty() && !has_mutation) {
		return false;
	}
	auto fail = [&]() {
		Unsupported(root, "MATCH pattern or projection");
		return false;
	};
	auto expected_primitives = match_statements.size() + filter_statements.size();
	if (modifying_statements.size() > 1 ||
	    (has_mutation ? !return_statements.empty() || !order_statements.empty() : return_statements.size() != 1) ||
	    order_statements.size() > 1 ||
	    (primitive_statements.size() != expected_primitives &&
	     primitive_statements.size() != expected_primitives + order_statements.size())) {
		return fail();
	}

	auto match = make_shared_ptr<GqlMatchStatement>(SourceRange(root));
	auto append_match = [&](GQLParser::SimpleMatchStatementContext &simple_match, bool optional,
	                        idx_t optional_stage) {
		auto binding_table = simple_match.graphPatternBindingTable();
		auto graph_pattern = binding_table->graphPattern();
		if (binding_table->graphPatternYieldClause() || graph_pattern->matchMode() || graph_pattern->keepClause()) {
			return false;
		}
		auto paths = graph_pattern->pathPatternList()->pathPattern();
		if (paths.empty()) {
			return false;
		}
		for (auto path : paths) {
		if (path->pathVariableDeclaration() || path->pathPatternPrefix()) {
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
				if (!element->nodePattern() || !TransformMatchElement(element->nodePattern()->elementPatternFiller(),
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

		if (auto where = graph_pattern->graphPatternWhereClause()) {
		shared_ptr<GqlExpression> predicate;
		if (!TransformSearchCondition(*where->searchCondition(), predicate)) {
			return false;
		}
		match->predicates.push_back(std::move(predicate));
		match->predicate_optional_stages.push_back(optional_stage);
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
			if (saw_optional) {
				Unsupported(*match_statement, "mandatory MATCH after OPTIONAL MATCH");
				return false;
			}
			simple_match = match_statement->simpleMatchStatement();
			saw_mandatory = true;
		} else if (auto optional_match = match_statement->optionalMatchStatement()) {
			auto operand = optional_match->optionalOperand();
			if (!operand || !operand->simpleMatchStatement()) {
				return fail();
			}
			simple_match = operand->simpleMatchStatement();
			optional = true;
			saw_optional = true;
			if (match_statements.size() > 1) {
				optional_stage++;
			}
			if (!saw_mandatory && match_statements.size() > 1) {
				Unsupported(*optional_match, "OPTIONAL MATCH before mandatory MATCH");
				return false;
			}
		}
		if (!simple_match || (has_mutation && optional) ||
		    !append_match(*simple_match, optional, optional ? optional_stage : 0)) {
			return fail();
		}
	}
	match->optional = match_statements.size() == 1 && match->patterns[0].optional;
	for (auto filter : filter_statements) {
		auto condition = filter->searchCondition();
		if (!condition && filter->whereClause()) {
			condition = filter->whereClause()->searchCondition();
		}
		shared_ptr<GqlExpression> predicate;
		if (!condition || !TransformSearchCondition(*condition, predicate)) {
			return fail();
		}
		match->predicates.push_back(std::move(predicate));
		idx_t owner_optional_stage = 0;
		if (match_statements.size() > 1) {
			auto filter_offset = filter->getStart()->getStartIndex();
			idx_t candidate_optional_stage = 0;
			for (auto candidate : match_statements) {
				if (candidate->optionalMatchStatement()) {
					candidate_optional_stage++;
				}
				if (candidate->getStart()->getStartIndex() < filter_offset) {
					owner_optional_stage = candidate->optionalMatchStatement() ? candidate_optional_stage : 0;
				}
			}
		}
		match->predicate_optional_stages.push_back(owner_optional_stage);
	}
	if (has_mutation) {
		if (!TransformMutation(*modifying_statements[0], *match)) {
			return false;
		}
		statement = std::move(match);
		return true;
	}

	auto body = return_statements[0]->returnStatementBody();
	if (body->ASTERISK() || !body->returnItemList()) {
		return fail();
	}
	if (body->setQuantifier()) {
		match->distinct = body->setQuantifier()->DISTINCT() != nullptr;
	}
	for (auto item : body->returnItemList()->returnItem()) {
		GqlProjection projection;
		if (!TransformProjection(item, projection)) {
			return fail();
		}
		match->projections.push_back(std::move(projection));
	}
	if (match->projections.empty()) {
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
				mutation.type = GqlMutationType::CLEAR_PROPERTIES;
				mutation.variable = variable;
				match.mutations.push_back(std::move(mutation));
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
				}
			} else if (auto label = item->setLabelItem()) {
				if (!IsRegularIdentifier(label->bindingVariableReference()->getText()) ||
				    !IsRegularIdentifier(label->labelName()->getText())) {
					return fail(*label, "delimited SET label targets");
				}
				mutation.type = GqlMutationType::SET_LABEL;
				mutation.variable = TransformIdentifier(*label->bindingVariableReference());
				mutation.name = TransformIdentifier(*label->labelName());
				match.mutations.push_back(std::move(mutation));
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
				mutation.type = GqlMutationType::REMOVE_LABEL;
				mutation.variable = TransformIdentifier(*label->bindingVariableReference());
				mutation.name = TransformIdentifier(*label->labelName());
			} else {
				return fail(*item, "REMOVE item");
			}
			match.mutations.push_back(std::move(mutation));
		}
	} else if (auto deletion = context.deleteStatement()) {
		auto items = deletion->deleteItemList()->deleteItem();
		for (auto item : items) {
			shared_ptr<GqlExpression> target;
			if (!TransformExpression(*item->valueExpression(), target) ||
			    target->type != GqlExpressionType::VARIABLE_REFERENCE) {
				return fail(*item, "non-element DELETE target");
			}
			GqlMutation mutation;
			mutation.source = SourceRange(*item);
			mutation.type = GqlMutationType::DELETE_ELEMENT;
			mutation.variable = target->variable;
			mutation.detach = deletion->DETACH() != nullptr;
			match.mutations.push_back(std::move(mutation));
		}
	} else if (context.insertStatement()) {
		return fail(context, "MATCH with data modification pipeline");
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
			if (!variable || !part || !part->labelExpression() ||
			    !IsRegularIdentifier(variable->getText())) {
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
	if (auto specification = context.unsignedValueSpecification()) {
		if (!specification->unsignedLiteral() ||
		    !TransformUnsignedLiteral(*specification->unsignedLiteral(), expression->literal)) {
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
		if (!IsRegularIdentifier(context.propertyName()->getText()) ||
		    !TransformExpressionPrimary(*context.valueExpressionPrimary(), expression->left)) {
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

void GqlTransformer::Unsupported(antlr4::ParserRuleContext &context, const string &feature) {
	statement = make_shared_ptr<GqlUnsupportedStatement>(SourceRange(context), feature);
}

bool GqlTransformer::IsRegularIdentifier(const string &value) {
	if (value.size() >= 2 &&
	    ((value.front() == '"' && value.back() == '"') ||
	     (value.front() == '`' && value.back() == '`'))) {
		auto quote = value.front();
		for (idx_t index = 1; index + 1 < value.size(); index++) {
			if (value[index] != quote) {
				continue;
			}
			if (index + 2 >= value.size() || value[index + 1] != quote) {
				return false;
			}
			index++;
		}
		return true;
	}
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

} // namespace duckdb
