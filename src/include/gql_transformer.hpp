#pragma once

#include "gql_ast.hpp"

#include "GQLBaseVisitor.h"

// ANTLR exposes this implementation detail as a macro. DuckDB has a typed
// DConstants::INVALID_INDEX member, so do not leak the ANTLR macro across the
// frontend boundary.
#ifdef INVALID_INDEX
#undef INVALID_INDEX
#endif

namespace duckdb {

class GqlTransformer final : private GQLBaseVisitor {
public:
	shared_ptr<GqlStatement> Transform(GQLParser::GqlProgramContext &root);

private:
	std::any visitCreateGraphStatement(GQLParser::CreateGraphStatementContext *context) override;
	std::any visitDropGraphStatement(GQLParser::DropGraphStatementContext *context) override;
	std::any visitSessionSetGraphClause(GQLParser::SessionSetGraphClauseContext *context) override;
	std::any visitInsertStatement(GQLParser::InsertStatementContext *context) override;

	shared_ptr<GqlInsertStatement> TransformInsert(GQLParser::InsertStatementContext &context,
	                                               bool allow_expressions);
	bool TransformInsertElement(GQLParser::InsertElementPatternFillerContext *filler, GqlInsertElement &element,
	                            bool allow_expressions);
	bool TransformMatch(GQLParser::GqlProgramContext &root);
	bool TransformMutation(GQLParser::PrimitiveDataModifyingStatementContext &context, GqlMatchStatement &match);
	bool TransformMatchElement(GQLParser::ElementPatternFillerContext *filler, GqlPatternElementType type,
	                           GqlPatternElement &result);
	bool TransformLabelExpression(GQLParser::LabelExpressionContext &context, vector<GqlIdentifier> &labels);
	bool TransformProjection(GQLParser::ReturnItemContext *item, GqlProjection &result);
	bool TransformSearchCondition(GQLParser::SearchConditionContext &context, shared_ptr<GqlExpression> &result);
	bool TransformExpression(GQLParser::ValueExpressionContext &context, shared_ptr<GqlExpression> &result);
	bool TransformExpressionPrimary(GQLParser::ValueExpressionPrimaryContext &context,
	                                shared_ptr<GqlExpression> &result);
	bool TransformValueFunction(GQLParser::ValueFunctionContext &context, shared_ptr<GqlExpression> &result);
	bool TransformNumericExpression(GQLParser::NumericValueExpressionContext &context,
	                                shared_ptr<GqlExpression> &result);
	bool TransformNumericFunction(GQLParser::NumericValueFunctionContext &context, shared_ptr<GqlExpression> &result);
	bool TransformAggregate(GQLParser::AggregateFunctionContext &context, shared_ptr<GqlExpression> &result);
	bool TransformUnsignedLiteral(GQLParser::UnsignedLiteralContext &context, GqlLiteral &result);
	bool TransformLiteral(GQLParser::ValueExpressionContext &context, GqlLiteral &result);

	void Unsupported(antlr4::ParserRuleContext &context, const string &feature);
	static bool IsRegularIdentifier(const string &value);
	static string UnquoteString(const string &text);
	static GqlIdentifier TransformIdentifier(antlr4::ParserRuleContext &context);
	static GqlSourceRange SourceRange(antlr4::ParserRuleContext &context);

	shared_ptr<GqlStatement> statement;
};

} // namespace duckdb
