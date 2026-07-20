
// Generated from third_party/opengql/GQL.g4 by ANTLR 4.13.2

#pragma once


#include "antlr4-runtime.h"
#include "GQLVisitor.h"


/**
 * This class provides an empty implementation of GQLVisitor, which can be
 * extended to create a visitor which only needs to handle a subset of the available methods.
 */
class  GQLBaseVisitor : public GQLVisitor {
public:

  virtual std::any visitGqlProgram(GQLParser::GqlProgramContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitProgramActivity(GQLParser::ProgramActivityContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSessionActivity(GQLParser::SessionActivityContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTransactionActivity(GQLParser::TransactionActivityContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEndTransactionCommand(GQLParser::EndTransactionCommandContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSessionSetCommand(GQLParser::SessionSetCommandContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSessionSetSchemaClause(GQLParser::SessionSetSchemaClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSessionSetGraphClause(GQLParser::SessionSetGraphClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSessionSetTimeZoneClause(GQLParser::SessionSetTimeZoneClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSetTimeZoneValue(GQLParser::SetTimeZoneValueContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSessionSetParameterClause(GQLParser::SessionSetParameterClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSessionSetGraphParameterClause(GQLParser::SessionSetGraphParameterClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSessionSetBindingTableParameterClause(GQLParser::SessionSetBindingTableParameterClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSessionSetValueParameterClause(GQLParser::SessionSetValueParameterClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSessionSetParameterName(GQLParser::SessionSetParameterNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSessionResetCommand(GQLParser::SessionResetCommandContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSessionResetArguments(GQLParser::SessionResetArgumentsContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSessionCloseCommand(GQLParser::SessionCloseCommandContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSessionParameterSpecification(GQLParser::SessionParameterSpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitStartTransactionCommand(GQLParser::StartTransactionCommandContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTransactionCharacteristics(GQLParser::TransactionCharacteristicsContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTransactionMode(GQLParser::TransactionModeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTransactionAccessMode(GQLParser::TransactionAccessModeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRollbackCommand(GQLParser::RollbackCommandContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCommitCommand(GQLParser::CommitCommandContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNestedProcedureSpecification(GQLParser::NestedProcedureSpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitProcedureSpecification(GQLParser::ProcedureSpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNestedDataModifyingProcedureSpecification(GQLParser::NestedDataModifyingProcedureSpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNestedQuerySpecification(GQLParser::NestedQuerySpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitProcedureBody(GQLParser::ProcedureBodyContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitBindingVariableDefinitionBlock(GQLParser::BindingVariableDefinitionBlockContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitBindingVariableDefinition(GQLParser::BindingVariableDefinitionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitStatementBlock(GQLParser::StatementBlockContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitStatement(GQLParser::StatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNextStatement(GQLParser::NextStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGraphVariableDefinition(GQLParser::GraphVariableDefinitionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitOptTypedGraphInitializer(GQLParser::OptTypedGraphInitializerContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGraphInitializer(GQLParser::GraphInitializerContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitBindingTableVariableDefinition(GQLParser::BindingTableVariableDefinitionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitOptTypedBindingTableInitializer(GQLParser::OptTypedBindingTableInitializerContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitBindingTableInitializer(GQLParser::BindingTableInitializerContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitValueVariableDefinition(GQLParser::ValueVariableDefinitionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitOptTypedValueInitializer(GQLParser::OptTypedValueInitializerContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitValueInitializer(GQLParser::ValueInitializerContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGraphExpression(GQLParser::GraphExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCurrentGraph(GQLParser::CurrentGraphContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitBindingTableExpression(GQLParser::BindingTableExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNestedBindingTableQuerySpecification(GQLParser::NestedBindingTableQuerySpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitObjectExpressionPrimary(GQLParser::ObjectExpressionPrimaryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLinearCatalogModifyingStatement(GQLParser::LinearCatalogModifyingStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimpleCatalogModifyingStatement(GQLParser::SimpleCatalogModifyingStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPrimitiveCatalogModifyingStatement(GQLParser::PrimitiveCatalogModifyingStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCreateSchemaStatement(GQLParser::CreateSchemaStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDropSchemaStatement(GQLParser::DropSchemaStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCreateGraphStatement(GQLParser::CreateGraphStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitOpenGraphType(GQLParser::OpenGraphTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitOfGraphType(GQLParser::OfGraphTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGraphTypeLikeGraph(GQLParser::GraphTypeLikeGraphContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGraphSource(GQLParser::GraphSourceContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDropGraphStatement(GQLParser::DropGraphStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCreateGraphTypeStatement(GQLParser::CreateGraphTypeStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGraphTypeSource(GQLParser::GraphTypeSourceContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCopyOfGraphType(GQLParser::CopyOfGraphTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDropGraphTypeStatement(GQLParser::DropGraphTypeStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCallCatalogModifyingProcedureStatement(GQLParser::CallCatalogModifyingProcedureStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLinearDataModifyingStatement(GQLParser::LinearDataModifyingStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFocusedLinearDataModifyingStatement(GQLParser::FocusedLinearDataModifyingStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFocusedLinearDataModifyingStatementBody(GQLParser::FocusedLinearDataModifyingStatementBodyContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFocusedNestedDataModifyingProcedureSpecification(GQLParser::FocusedNestedDataModifyingProcedureSpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAmbientLinearDataModifyingStatement(GQLParser::AmbientLinearDataModifyingStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAmbientLinearDataModifyingStatementBody(GQLParser::AmbientLinearDataModifyingStatementBodyContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimpleLinearDataAccessingStatement(GQLParser::SimpleLinearDataAccessingStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimpleDataAccessingStatement(GQLParser::SimpleDataAccessingStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimpleDataModifyingStatement(GQLParser::SimpleDataModifyingStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPrimitiveDataModifyingStatement(GQLParser::PrimitiveDataModifyingStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitInsertStatement(GQLParser::InsertStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSetStatement(GQLParser::SetStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSetItemList(GQLParser::SetItemListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSetItem(GQLParser::SetItemContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSetPropertyItem(GQLParser::SetPropertyItemContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSetAllPropertiesItem(GQLParser::SetAllPropertiesItemContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSetLabelItem(GQLParser::SetLabelItemContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRemoveStatement(GQLParser::RemoveStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRemoveItemList(GQLParser::RemoveItemListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRemoveItem(GQLParser::RemoveItemContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRemovePropertyItem(GQLParser::RemovePropertyItemContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRemoveLabelItem(GQLParser::RemoveLabelItemContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDeleteStatement(GQLParser::DeleteStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDeleteItemList(GQLParser::DeleteItemListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDeleteItem(GQLParser::DeleteItemContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCallDataModifyingProcedureStatement(GQLParser::CallDataModifyingProcedureStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCompositeQueryStatement(GQLParser::CompositeQueryStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCompositeQueryExpression(GQLParser::CompositeQueryExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitQueryConjunction(GQLParser::QueryConjunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSetOperator(GQLParser::SetOperatorContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCompositeQueryPrimary(GQLParser::CompositeQueryPrimaryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLinearQueryStatement(GQLParser::LinearQueryStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFocusedLinearQueryStatement(GQLParser::FocusedLinearQueryStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFocusedLinearQueryStatementPart(GQLParser::FocusedLinearQueryStatementPartContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFocusedLinearQueryAndPrimitiveResultStatementPart(GQLParser::FocusedLinearQueryAndPrimitiveResultStatementPartContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFocusedPrimitiveResultStatement(GQLParser::FocusedPrimitiveResultStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFocusedNestedQuerySpecification(GQLParser::FocusedNestedQuerySpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAmbientLinearQueryStatement(GQLParser::AmbientLinearQueryStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimpleLinearQueryStatement(GQLParser::SimpleLinearQueryStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimpleQueryStatement(GQLParser::SimpleQueryStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPrimitiveQueryStatement(GQLParser::PrimitiveQueryStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitMatchStatement(GQLParser::MatchStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimpleMatchStatement(GQLParser::SimpleMatchStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitOptionalMatchStatement(GQLParser::OptionalMatchStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitOptionalOperand(GQLParser::OptionalOperandContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitMatchStatementBlock(GQLParser::MatchStatementBlockContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCallQueryStatement(GQLParser::CallQueryStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFilterStatement(GQLParser::FilterStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLetStatement(GQLParser::LetStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLetVariableDefinitionList(GQLParser::LetVariableDefinitionListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLetVariableDefinition(GQLParser::LetVariableDefinitionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitForStatement(GQLParser::ForStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitForItem(GQLParser::ForItemContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitForItemAlias(GQLParser::ForItemAliasContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitForItemSource(GQLParser::ForItemSourceContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitForOrdinalityOrOffset(GQLParser::ForOrdinalityOrOffsetContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitOrderByAndPageStatement(GQLParser::OrderByAndPageStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPrimitiveResultStatement(GQLParser::PrimitiveResultStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitReturnStatement(GQLParser::ReturnStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitReturnStatementBody(GQLParser::ReturnStatementBodyContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitReturnItemList(GQLParser::ReturnItemListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitReturnItem(GQLParser::ReturnItemContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitReturnItemAlias(GQLParser::ReturnItemAliasContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSelectStatement(GQLParser::SelectStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSelectItemList(GQLParser::SelectItemListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSelectItem(GQLParser::SelectItemContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSelectItemAlias(GQLParser::SelectItemAliasContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitHavingClause(GQLParser::HavingClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSelectStatementBody(GQLParser::SelectStatementBodyContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSelectGraphMatchList(GQLParser::SelectGraphMatchListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSelectGraphMatch(GQLParser::SelectGraphMatchContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSelectQuerySpecification(GQLParser::SelectQuerySpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCallProcedureStatement(GQLParser::CallProcedureStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitProcedureCall(GQLParser::ProcedureCallContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitInlineProcedureCall(GQLParser::InlineProcedureCallContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitVariableScopeClause(GQLParser::VariableScopeClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitBindingVariableReferenceList(GQLParser::BindingVariableReferenceListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNamedProcedureCall(GQLParser::NamedProcedureCallContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitProcedureArgumentList(GQLParser::ProcedureArgumentListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitProcedureArgument(GQLParser::ProcedureArgumentContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAtSchemaClause(GQLParser::AtSchemaClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitUseGraphClause(GQLParser::UseGraphClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGraphPatternBindingTable(GQLParser::GraphPatternBindingTableContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGraphPatternYieldClause(GQLParser::GraphPatternYieldClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGraphPatternYieldItemList(GQLParser::GraphPatternYieldItemListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGraphPatternYieldItem(GQLParser::GraphPatternYieldItemContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGraphPattern(GQLParser::GraphPatternContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitMatchMode(GQLParser::MatchModeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRepeatableElementsMatchMode(GQLParser::RepeatableElementsMatchModeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDifferentEdgesMatchMode(GQLParser::DifferentEdgesMatchModeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitElementBindingsOrElements(GQLParser::ElementBindingsOrElementsContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEdgeBindingsOrEdges(GQLParser::EdgeBindingsOrEdgesContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPathPatternList(GQLParser::PathPatternListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPathPattern(GQLParser::PathPatternContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPathVariableDeclaration(GQLParser::PathVariableDeclarationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitKeepClause(GQLParser::KeepClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGraphPatternWhereClause(GQLParser::GraphPatternWhereClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitInsertGraphPattern(GQLParser::InsertGraphPatternContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitInsertPathPatternList(GQLParser::InsertPathPatternListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitInsertPathPattern(GQLParser::InsertPathPatternContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitInsertNodePattern(GQLParser::InsertNodePatternContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitInsertEdgePattern(GQLParser::InsertEdgePatternContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitInsertEdgePointingLeft(GQLParser::InsertEdgePointingLeftContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitInsertEdgePointingRight(GQLParser::InsertEdgePointingRightContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitInsertEdgeUndirected(GQLParser::InsertEdgeUndirectedContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitInsertElementPatternFiller(GQLParser::InsertElementPatternFillerContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLabelAndPropertySetSpecification(GQLParser::LabelAndPropertySetSpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPathPatternPrefix(GQLParser::PathPatternPrefixContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPathModePrefix(GQLParser::PathModePrefixContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPathMode(GQLParser::PathModeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPathSearchPrefix(GQLParser::PathSearchPrefixContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAllPathSearch(GQLParser::AllPathSearchContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPathOrPaths(GQLParser::PathOrPathsContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAnyPathSearch(GQLParser::AnyPathSearchContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNumberOfPaths(GQLParser::NumberOfPathsContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitShortestPathSearch(GQLParser::ShortestPathSearchContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAllShortestPathSearch(GQLParser::AllShortestPathSearchContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAnyShortestPathSearch(GQLParser::AnyShortestPathSearchContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCountedShortestPathSearch(GQLParser::CountedShortestPathSearchContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCountedShortestGroupSearch(GQLParser::CountedShortestGroupSearchContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNumberOfGroups(GQLParser::NumberOfGroupsContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPpePathTerm(GQLParser::PpePathTermContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPpeMultisetAlternation(GQLParser::PpeMultisetAlternationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPpePatternUnion(GQLParser::PpePatternUnionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPathTerm(GQLParser::PathTermContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPfPathPrimary(GQLParser::PfPathPrimaryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPfQuantifiedPathPrimary(GQLParser::PfQuantifiedPathPrimaryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPfQuestionedPathPrimary(GQLParser::PfQuestionedPathPrimaryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPpElementPattern(GQLParser::PpElementPatternContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPpParenthesizedPathPatternExpression(GQLParser::PpParenthesizedPathPatternExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPpSimplifiedPathPatternExpression(GQLParser::PpSimplifiedPathPatternExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitElementPattern(GQLParser::ElementPatternContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNodePattern(GQLParser::NodePatternContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitElementPatternFiller(GQLParser::ElementPatternFillerContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitElementVariableDeclaration(GQLParser::ElementVariableDeclarationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitIsLabelExpression(GQLParser::IsLabelExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitIsOrColon(GQLParser::IsOrColonContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitElementPatternPredicate(GQLParser::ElementPatternPredicateContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitElementPatternWhereClause(GQLParser::ElementPatternWhereClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitElementPropertySpecification(GQLParser::ElementPropertySpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPropertyKeyValuePairList(GQLParser::PropertyKeyValuePairListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPropertyKeyValuePair(GQLParser::PropertyKeyValuePairContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEdgePattern(GQLParser::EdgePatternContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFullEdgePattern(GQLParser::FullEdgePatternContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFullEdgePointingLeft(GQLParser::FullEdgePointingLeftContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFullEdgeUndirected(GQLParser::FullEdgeUndirectedContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFullEdgePointingRight(GQLParser::FullEdgePointingRightContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFullEdgeLeftOrUndirected(GQLParser::FullEdgeLeftOrUndirectedContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFullEdgeUndirectedOrRight(GQLParser::FullEdgeUndirectedOrRightContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFullEdgeLeftOrRight(GQLParser::FullEdgeLeftOrRightContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFullEdgeAnyDirection(GQLParser::FullEdgeAnyDirectionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAbbreviatedEdgePattern(GQLParser::AbbreviatedEdgePatternContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitParenthesizedPathPatternExpression(GQLParser::ParenthesizedPathPatternExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSubpathVariableDeclaration(GQLParser::SubpathVariableDeclarationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitParenthesizedPathPatternWhereClause(GQLParser::ParenthesizedPathPatternWhereClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLabelExpressionNegation(GQLParser::LabelExpressionNegationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLabelExpressionDisjunction(GQLParser::LabelExpressionDisjunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLabelExpressionParenthesized(GQLParser::LabelExpressionParenthesizedContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLabelExpressionWildcard(GQLParser::LabelExpressionWildcardContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLabelExpressionConjunction(GQLParser::LabelExpressionConjunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLabelExpressionName(GQLParser::LabelExpressionNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPathVariableReference(GQLParser::PathVariableReferenceContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitElementVariableReference(GQLParser::ElementVariableReferenceContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGraphPatternQuantifier(GQLParser::GraphPatternQuantifierContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFixedQuantifier(GQLParser::FixedQuantifierContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGeneralQuantifier(GQLParser::GeneralQuantifierContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLowerBound(GQLParser::LowerBoundContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitUpperBound(GQLParser::UpperBoundContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedPathPatternExpression(GQLParser::SimplifiedPathPatternExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedDefaultingLeft(GQLParser::SimplifiedDefaultingLeftContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedDefaultingUndirected(GQLParser::SimplifiedDefaultingUndirectedContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedDefaultingRight(GQLParser::SimplifiedDefaultingRightContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedDefaultingLeftOrUndirected(GQLParser::SimplifiedDefaultingLeftOrUndirectedContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedDefaultingUndirectedOrRight(GQLParser::SimplifiedDefaultingUndirectedOrRightContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedDefaultingLeftOrRight(GQLParser::SimplifiedDefaultingLeftOrRightContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedDefaultingAnyDirection(GQLParser::SimplifiedDefaultingAnyDirectionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedContents(GQLParser::SimplifiedContentsContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedPathUnion(GQLParser::SimplifiedPathUnionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedMultisetAlternation(GQLParser::SimplifiedMultisetAlternationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedFactorLowLabel(GQLParser::SimplifiedFactorLowLabelContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedConcatenationLabel(GQLParser::SimplifiedConcatenationLabelContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedConjunctionLabel(GQLParser::SimplifiedConjunctionLabelContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedFactorHighLabel(GQLParser::SimplifiedFactorHighLabelContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedFactorHigh(GQLParser::SimplifiedFactorHighContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedQuantified(GQLParser::SimplifiedQuantifiedContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedQuestioned(GQLParser::SimplifiedQuestionedContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedTertiary(GQLParser::SimplifiedTertiaryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedDirectionOverride(GQLParser::SimplifiedDirectionOverrideContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedOverrideLeft(GQLParser::SimplifiedOverrideLeftContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedOverrideUndirected(GQLParser::SimplifiedOverrideUndirectedContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedOverrideRight(GQLParser::SimplifiedOverrideRightContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedOverrideLeftOrUndirected(GQLParser::SimplifiedOverrideLeftOrUndirectedContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedOverrideUndirectedOrRight(GQLParser::SimplifiedOverrideUndirectedOrRightContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedOverrideLeftOrRight(GQLParser::SimplifiedOverrideLeftOrRightContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedOverrideAnyDirection(GQLParser::SimplifiedOverrideAnyDirectionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedSecondary(GQLParser::SimplifiedSecondaryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedNegation(GQLParser::SimplifiedNegationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimplifiedPrimary(GQLParser::SimplifiedPrimaryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitWhereClause(GQLParser::WhereClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitYieldClause(GQLParser::YieldClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitYieldItemList(GQLParser::YieldItemListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitYieldItem(GQLParser::YieldItemContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitYieldItemName(GQLParser::YieldItemNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitYieldItemAlias(GQLParser::YieldItemAliasContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGroupByClause(GQLParser::GroupByClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGroupingElementList(GQLParser::GroupingElementListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGroupingElement(GQLParser::GroupingElementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEmptyGroupingSet(GQLParser::EmptyGroupingSetContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitOrderByClause(GQLParser::OrderByClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSortSpecificationList(GQLParser::SortSpecificationListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSortSpecification(GQLParser::SortSpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSortKey(GQLParser::SortKeyContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitOrderingSpecification(GQLParser::OrderingSpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNullOrdering(GQLParser::NullOrderingContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLimitClause(GQLParser::LimitClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitOffsetClause(GQLParser::OffsetClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitOffsetSynonym(GQLParser::OffsetSynonymContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSchemaReference(GQLParser::SchemaReferenceContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAbsoluteCatalogSchemaReference(GQLParser::AbsoluteCatalogSchemaReferenceContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCatalogSchemaParentAndName(GQLParser::CatalogSchemaParentAndNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRelativeCatalogSchemaReference(GQLParser::RelativeCatalogSchemaReferenceContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPredefinedSchemaReference(GQLParser::PredefinedSchemaReferenceContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAbsoluteDirectoryPath(GQLParser::AbsoluteDirectoryPathContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRelativeDirectoryPath(GQLParser::RelativeDirectoryPathContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimpleDirectoryPath(GQLParser::SimpleDirectoryPathContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGraphReference(GQLParser::GraphReferenceContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCatalogGraphParentAndName(GQLParser::CatalogGraphParentAndNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitHomeGraph(GQLParser::HomeGraphContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGraphTypeReference(GQLParser::GraphTypeReferenceContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCatalogGraphTypeParentAndName(GQLParser::CatalogGraphTypeParentAndNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitBindingTableReference(GQLParser::BindingTableReferenceContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitProcedureReference(GQLParser::ProcedureReferenceContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCatalogProcedureParentAndName(GQLParser::CatalogProcedureParentAndNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCatalogObjectParentReference(GQLParser::CatalogObjectParentReferenceContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitReferenceParameterSpecification(GQLParser::ReferenceParameterSpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNestedGraphTypeSpecification(GQLParser::NestedGraphTypeSpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGraphTypeSpecificationBody(GQLParser::GraphTypeSpecificationBodyContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitElementTypeList(GQLParser::ElementTypeListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitElementTypeSpecification(GQLParser::ElementTypeSpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNodeTypeSpecification(GQLParser::NodeTypeSpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNodeTypePattern(GQLParser::NodeTypePatternContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNodeTypePhrase(GQLParser::NodeTypePhraseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNodeTypePhraseFiller(GQLParser::NodeTypePhraseFillerContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNodeTypeFiller(GQLParser::NodeTypeFillerContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLocalNodeTypeAlias(GQLParser::LocalNodeTypeAliasContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNodeTypeImpliedContent(GQLParser::NodeTypeImpliedContentContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNodeTypeKeyLabelSet(GQLParser::NodeTypeKeyLabelSetContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNodeTypeLabelSet(GQLParser::NodeTypeLabelSetContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNodeTypePropertyTypes(GQLParser::NodeTypePropertyTypesContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEdgeTypeSpecification(GQLParser::EdgeTypeSpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEdgeTypePattern(GQLParser::EdgeTypePatternContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEdgeTypePhrase(GQLParser::EdgeTypePhraseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEdgeTypePhraseFiller(GQLParser::EdgeTypePhraseFillerContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEdgeTypeFiller(GQLParser::EdgeTypeFillerContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEdgeTypeImpliedContent(GQLParser::EdgeTypeImpliedContentContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEdgeTypeKeyLabelSet(GQLParser::EdgeTypeKeyLabelSetContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEdgeTypeLabelSet(GQLParser::EdgeTypeLabelSetContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEdgeTypePropertyTypes(GQLParser::EdgeTypePropertyTypesContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEdgeTypePatternDirected(GQLParser::EdgeTypePatternDirectedContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEdgeTypePatternPointingRight(GQLParser::EdgeTypePatternPointingRightContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEdgeTypePatternPointingLeft(GQLParser::EdgeTypePatternPointingLeftContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEdgeTypePatternUndirected(GQLParser::EdgeTypePatternUndirectedContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitArcTypePointingRight(GQLParser::ArcTypePointingRightContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitArcTypePointingLeft(GQLParser::ArcTypePointingLeftContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitArcTypeUndirected(GQLParser::ArcTypeUndirectedContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSourceNodeTypeReference(GQLParser::SourceNodeTypeReferenceContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDestinationNodeTypeReference(GQLParser::DestinationNodeTypeReferenceContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEdgeKind(GQLParser::EdgeKindContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEndpointPairPhrase(GQLParser::EndpointPairPhraseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEndpointPair(GQLParser::EndpointPairContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEndpointPairDirected(GQLParser::EndpointPairDirectedContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEndpointPairPointingRight(GQLParser::EndpointPairPointingRightContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEndpointPairPointingLeft(GQLParser::EndpointPairPointingLeftContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEndpointPairUndirected(GQLParser::EndpointPairUndirectedContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitConnectorPointingRight(GQLParser::ConnectorPointingRightContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitConnectorUndirected(GQLParser::ConnectorUndirectedContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSourceNodeTypeAlias(GQLParser::SourceNodeTypeAliasContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDestinationNodeTypeAlias(GQLParser::DestinationNodeTypeAliasContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLabelSetPhrase(GQLParser::LabelSetPhraseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLabelSetSpecification(GQLParser::LabelSetSpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPropertyTypesSpecification(GQLParser::PropertyTypesSpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPropertyTypeList(GQLParser::PropertyTypeListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPropertyType(GQLParser::PropertyTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPropertyValueType(GQLParser::PropertyValueTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitBindingTableType(GQLParser::BindingTableTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDynamicPropertyValueTypeLabel(GQLParser::DynamicPropertyValueTypeLabelContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitClosedDynamicUnionTypeAtl1(GQLParser::ClosedDynamicUnionTypeAtl1Context *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitClosedDynamicUnionTypeAtl2(GQLParser::ClosedDynamicUnionTypeAtl2Context *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPathValueTypeLabel(GQLParser::PathValueTypeLabelContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitListValueTypeAlt3(GQLParser::ListValueTypeAlt3Context *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitListValueTypeAlt2(GQLParser::ListValueTypeAlt2Context *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitListValueTypeAlt1(GQLParser::ListValueTypeAlt1Context *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPredefinedTypeLabel(GQLParser::PredefinedTypeLabelContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRecordTypeLabel(GQLParser::RecordTypeLabelContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitOpenDynamicUnionTypeLabel(GQLParser::OpenDynamicUnionTypeLabelContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTyped(GQLParser::TypedContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPredefinedType(GQLParser::PredefinedTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitBooleanType(GQLParser::BooleanTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCharacterStringType(GQLParser::CharacterStringTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitByteStringType(GQLParser::ByteStringTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitMinLength(GQLParser::MinLengthContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitMaxLength(GQLParser::MaxLengthContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFixedLength(GQLParser::FixedLengthContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNumericType(GQLParser::NumericTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitExactNumericType(GQLParser::ExactNumericTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitBinaryExactNumericType(GQLParser::BinaryExactNumericTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSignedBinaryExactNumericType(GQLParser::SignedBinaryExactNumericTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitUnsignedBinaryExactNumericType(GQLParser::UnsignedBinaryExactNumericTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitVerboseBinaryExactNumericType(GQLParser::VerboseBinaryExactNumericTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDecimalExactNumericType(GQLParser::DecimalExactNumericTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPrecision(GQLParser::PrecisionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitScale(GQLParser::ScaleContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitApproximateNumericType(GQLParser::ApproximateNumericTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTemporalType(GQLParser::TemporalTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTemporalInstantType(GQLParser::TemporalInstantTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDatetimeType(GQLParser::DatetimeTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLocaldatetimeType(GQLParser::LocaldatetimeTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDateType(GQLParser::DateTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTimeType(GQLParser::TimeTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLocaltimeType(GQLParser::LocaltimeTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTemporalDurationType(GQLParser::TemporalDurationTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTemporalDurationQualifier(GQLParser::TemporalDurationQualifierContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitReferenceValueType(GQLParser::ReferenceValueTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitImmaterialValueType(GQLParser::ImmaterialValueTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNullType(GQLParser::NullTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEmptyType(GQLParser::EmptyTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGraphReferenceValueType(GQLParser::GraphReferenceValueTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitClosedGraphReferenceValueType(GQLParser::ClosedGraphReferenceValueTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitOpenGraphReferenceValueType(GQLParser::OpenGraphReferenceValueTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitBindingTableReferenceValueType(GQLParser::BindingTableReferenceValueTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNodeReferenceValueType(GQLParser::NodeReferenceValueTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitClosedNodeReferenceValueType(GQLParser::ClosedNodeReferenceValueTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitOpenNodeReferenceValueType(GQLParser::OpenNodeReferenceValueTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEdgeReferenceValueType(GQLParser::EdgeReferenceValueTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitClosedEdgeReferenceValueType(GQLParser::ClosedEdgeReferenceValueTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitOpenEdgeReferenceValueType(GQLParser::OpenEdgeReferenceValueTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPathValueType(GQLParser::PathValueTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitListValueTypeName(GQLParser::ListValueTypeNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitListValueTypeNameSynonym(GQLParser::ListValueTypeNameSynonymContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRecordType(GQLParser::RecordTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFieldTypesSpecification(GQLParser::FieldTypesSpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFieldTypeList(GQLParser::FieldTypeListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNotNull(GQLParser::NotNullContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFieldType(GQLParser::FieldTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSearchCondition(GQLParser::SearchConditionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPredicate(GQLParser::PredicateContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCompOp(GQLParser::CompOpContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitExistsPredicate(GQLParser::ExistsPredicateContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNullPredicate(GQLParser::NullPredicateContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNullPredicatePart2(GQLParser::NullPredicatePart2Context *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitValueTypePredicate(GQLParser::ValueTypePredicateContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitValueTypePredicatePart2(GQLParser::ValueTypePredicatePart2Context *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNormalizedPredicatePart2(GQLParser::NormalizedPredicatePart2Context *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDirectedPredicate(GQLParser::DirectedPredicateContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDirectedPredicatePart2(GQLParser::DirectedPredicatePart2Context *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLabeledPredicate(GQLParser::LabeledPredicateContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLabeledPredicatePart2(GQLParser::LabeledPredicatePart2Context *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitIsLabeledOrColon(GQLParser::IsLabeledOrColonContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSourceDestinationPredicate(GQLParser::SourceDestinationPredicateContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNodeReference(GQLParser::NodeReferenceContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSourcePredicatePart2(GQLParser::SourcePredicatePart2Context *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDestinationPredicatePart2(GQLParser::DestinationPredicatePart2Context *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEdgeReference(GQLParser::EdgeReferenceContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAll_differentPredicate(GQLParser::All_differentPredicateContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSamePredicate(GQLParser::SamePredicateContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitProperty_existsPredicate(GQLParser::Property_existsPredicateContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitConjunctiveExprAlt(GQLParser::ConjunctiveExprAltContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPropertyGraphExprAlt(GQLParser::PropertyGraphExprAltContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitMultDivExprAlt(GQLParser::MultDivExprAltContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitBindingTableExprAlt(GQLParser::BindingTableExprAltContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSignedExprAlt(GQLParser::SignedExprAltContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitIsNotExprAlt(GQLParser::IsNotExprAltContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNormalizedPredicateExprAlt(GQLParser::NormalizedPredicateExprAltContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNotExprAlt(GQLParser::NotExprAltContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitValueFunctionExprAlt(GQLParser::ValueFunctionExprAltContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitConcatenationExprAlt(GQLParser::ConcatenationExprAltContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDisjunctiveExprAlt(GQLParser::DisjunctiveExprAltContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitComparisonExprAlt(GQLParser::ComparisonExprAltContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPrimaryExprAlt(GQLParser::PrimaryExprAltContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAddSubtractExprAlt(GQLParser::AddSubtractExprAltContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPredicateExprAlt(GQLParser::PredicateExprAltContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitValueFunction(GQLParser::ValueFunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitBooleanValueExpression(GQLParser::BooleanValueExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCharacterOrByteStringFunction(GQLParser::CharacterOrByteStringFunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSubCharacterOrByteString(GQLParser::SubCharacterOrByteStringContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTrimSingleCharacterOrByteString(GQLParser::TrimSingleCharacterOrByteStringContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFoldCharacterString(GQLParser::FoldCharacterStringContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTrimMultiCharacterCharacterString(GQLParser::TrimMultiCharacterCharacterStringContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNormalizeCharacterString(GQLParser::NormalizeCharacterStringContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNodeReferenceValueExpression(GQLParser::NodeReferenceValueExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEdgeReferenceValueExpression(GQLParser::EdgeReferenceValueExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAggregatingValueExpression(GQLParser::AggregatingValueExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitValueExpressionPrimary(GQLParser::ValueExpressionPrimaryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitParenthesizedValueExpression(GQLParser::ParenthesizedValueExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNonParenthesizedValueExpressionPrimary(GQLParser::NonParenthesizedValueExpressionPrimaryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNonParenthesizedValueExpressionPrimarySpecialCase(GQLParser::NonParenthesizedValueExpressionPrimarySpecialCaseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitUnsignedValueSpecification(GQLParser::UnsignedValueSpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNonNegativeIntegerSpecification(GQLParser::NonNegativeIntegerSpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGeneralValueSpecification(GQLParser::GeneralValueSpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDynamicParameterSpecification(GQLParser::DynamicParameterSpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLetValueExpression(GQLParser::LetValueExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitValueQueryExpression(GQLParser::ValueQueryExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCaseExpression(GQLParser::CaseExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCaseAbbreviation(GQLParser::CaseAbbreviationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCaseSpecification(GQLParser::CaseSpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimpleCase(GQLParser::SimpleCaseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSearchedCase(GQLParser::SearchedCaseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSimpleWhenClause(GQLParser::SimpleWhenClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSearchedWhenClause(GQLParser::SearchedWhenClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitElseClause(GQLParser::ElseClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCaseOperand(GQLParser::CaseOperandContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitWhenOperandList(GQLParser::WhenOperandListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitWhenOperand(GQLParser::WhenOperandContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitResult(GQLParser::ResultContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitResultExpression(GQLParser::ResultExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCastSpecification(GQLParser::CastSpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCastOperand(GQLParser::CastOperandContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCastTarget(GQLParser::CastTargetContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAggregateFunction(GQLParser::AggregateFunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGeneralSetFunction(GQLParser::GeneralSetFunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitBinarySetFunction(GQLParser::BinarySetFunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGeneralSetFunctionType(GQLParser::GeneralSetFunctionTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSetQuantifier(GQLParser::SetQuantifierContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitBinarySetFunctionType(GQLParser::BinarySetFunctionTypeContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDependentValueExpression(GQLParser::DependentValueExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitIndependentValueExpression(GQLParser::IndependentValueExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitElement_idFunction(GQLParser::Element_idFunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitBindingVariableReference(GQLParser::BindingVariableReferenceContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPathValueExpression(GQLParser::PathValueExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPathValueConstructor(GQLParser::PathValueConstructorContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPathValueConstructorByEnumeration(GQLParser::PathValueConstructorByEnumerationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPathElementList(GQLParser::PathElementListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPathElementListStart(GQLParser::PathElementListStartContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPathElementListStep(GQLParser::PathElementListStepContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitListValueExpression(GQLParser::ListValueExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitListValueFunction(GQLParser::ListValueFunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTrimListFunction(GQLParser::TrimListFunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitElementsFunction(GQLParser::ElementsFunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitListValueConstructor(GQLParser::ListValueConstructorContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitListValueConstructorByEnumeration(GQLParser::ListValueConstructorByEnumerationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitListElementList(GQLParser::ListElementListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitListElement(GQLParser::ListElementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRecordConstructor(GQLParser::RecordConstructorContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFieldsSpecification(GQLParser::FieldsSpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFieldList(GQLParser::FieldListContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitField(GQLParser::FieldContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTruthValue(GQLParser::TruthValueContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNumericValueExpression(GQLParser::NumericValueExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNumericValueFunction(GQLParser::NumericValueFunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLengthExpression(GQLParser::LengthExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCardinalityExpression(GQLParser::CardinalityExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCardinalityExpressionArgument(GQLParser::CardinalityExpressionArgumentContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCharLengthExpression(GQLParser::CharLengthExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitByteLengthExpression(GQLParser::ByteLengthExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPathLengthExpression(GQLParser::PathLengthExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAbsoluteValueExpression(GQLParser::AbsoluteValueExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitModulusExpression(GQLParser::ModulusExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNumericValueExpressionDividend(GQLParser::NumericValueExpressionDividendContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNumericValueExpressionDivisor(GQLParser::NumericValueExpressionDivisorContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTrigonometricFunction(GQLParser::TrigonometricFunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTrigonometricFunctionName(GQLParser::TrigonometricFunctionNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGeneralLogarithmFunction(GQLParser::GeneralLogarithmFunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGeneralLogarithmBase(GQLParser::GeneralLogarithmBaseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGeneralLogarithmArgument(GQLParser::GeneralLogarithmArgumentContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCommonLogarithm(GQLParser::CommonLogarithmContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNaturalLogarithm(GQLParser::NaturalLogarithmContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitExponentialFunction(GQLParser::ExponentialFunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPowerFunction(GQLParser::PowerFunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNumericValueExpressionBase(GQLParser::NumericValueExpressionBaseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNumericValueExpressionExponent(GQLParser::NumericValueExpressionExponentContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSquareRoot(GQLParser::SquareRootContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFloorFunction(GQLParser::FloorFunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCeilingFunction(GQLParser::CeilingFunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCharacterStringValueExpression(GQLParser::CharacterStringValueExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitByteStringValueExpression(GQLParser::ByteStringValueExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTrimOperands(GQLParser::TrimOperandsContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTrimCharacterOrByteStringSource(GQLParser::TrimCharacterOrByteStringSourceContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTrimSpecification(GQLParser::TrimSpecificationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTrimCharacterOrByteString(GQLParser::TrimCharacterOrByteStringContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNormalForm(GQLParser::NormalFormContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitStringLength(GQLParser::StringLengthContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDatetimeValueExpression(GQLParser::DatetimeValueExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDatetimeValueFunction(GQLParser::DatetimeValueFunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDateFunction(GQLParser::DateFunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTimeFunction(GQLParser::TimeFunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLocaltimeFunction(GQLParser::LocaltimeFunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDatetimeFunction(GQLParser::DatetimeFunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLocaldatetimeFunction(GQLParser::LocaldatetimeFunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDateFunctionParameters(GQLParser::DateFunctionParametersContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTimeFunctionParameters(GQLParser::TimeFunctionParametersContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDatetimeFunctionParameters(GQLParser::DatetimeFunctionParametersContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDurationValueExpression(GQLParser::DurationValueExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDatetimeSubtraction(GQLParser::DatetimeSubtractionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDatetimeSubtractionParameters(GQLParser::DatetimeSubtractionParametersContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDatetimeValueExpression1(GQLParser::DatetimeValueExpression1Context *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDatetimeValueExpression2(GQLParser::DatetimeValueExpression2Context *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDurationValueFunction(GQLParser::DurationValueFunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDurationFunction(GQLParser::DurationFunctionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDurationFunctionParameters(GQLParser::DurationFunctionParametersContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitObjectName(GQLParser::ObjectNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitObjectNameOrBindingVariable(GQLParser::ObjectNameOrBindingVariableContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDirectoryName(GQLParser::DirectoryNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSchemaName(GQLParser::SchemaNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGraphName(GQLParser::GraphNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDelimitedGraphName(GQLParser::DelimitedGraphNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGraphTypeName(GQLParser::GraphTypeNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNodeTypeName(GQLParser::NodeTypeNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEdgeTypeName(GQLParser::EdgeTypeNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitBindingTableName(GQLParser::BindingTableNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDelimitedBindingTableName(GQLParser::DelimitedBindingTableNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitProcedureName(GQLParser::ProcedureNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLabelName(GQLParser::LabelNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPropertyName(GQLParser::PropertyNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFieldName(GQLParser::FieldNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitElementVariable(GQLParser::ElementVariableContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPathVariable(GQLParser::PathVariableContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSubpathVariable(GQLParser::SubpathVariableContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitBindingVariable(GQLParser::BindingVariableContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitUnsignedLiteral(GQLParser::UnsignedLiteralContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitGeneralLiteral(GQLParser::GeneralLiteralContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTemporalLiteral(GQLParser::TemporalLiteralContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDateLiteral(GQLParser::DateLiteralContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTimeLiteral(GQLParser::TimeLiteralContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDatetimeLiteral(GQLParser::DatetimeLiteralContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitListLiteral(GQLParser::ListLiteralContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRecordLiteral(GQLParser::RecordLiteralContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitIdentifier(GQLParser::IdentifierContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRegularIdentifier(GQLParser::RegularIdentifierContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTimeZoneString(GQLParser::TimeZoneStringContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCharacterStringLiteral(GQLParser::CharacterStringLiteralContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitUnsignedNumericLiteral(GQLParser::UnsignedNumericLiteralContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitExactNumericLiteral(GQLParser::ExactNumericLiteralContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitApproximateNumericLiteral(GQLParser::ApproximateNumericLiteralContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitUnsignedInteger(GQLParser::UnsignedIntegerContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitUnsignedDecimalInteger(GQLParser::UnsignedDecimalIntegerContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNullLiteral(GQLParser::NullLiteralContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDateString(GQLParser::DateStringContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTimeString(GQLParser::TimeStringContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDatetimeString(GQLParser::DatetimeStringContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDurationLiteral(GQLParser::DurationLiteralContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDurationString(GQLParser::DurationStringContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNodeSynonym(GQLParser::NodeSynonymContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEdgesSynonym(GQLParser::EdgesSynonymContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitEdgeSynonym(GQLParser::EdgeSynonymContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNonReservedWords(GQLParser::NonReservedWordsContext *ctx) override {
    return visitChildren(ctx);
  }


};

