#include "kqp_opt_phy_rules.h"
#include "kqp_opt_phy_olap_filter_collection.h"

#include <ydb/core/kqp/common/kqp_yql.h>
#include <ydb/library/yql/core/extract_predicate/extract_predicate.h>

namespace NKikimr::NKqp::NOpt {

using namespace NYql;
using namespace NYql::NNodes;

namespace {

static TMaybeNode<TExprBase> NullNode = TMaybeNode<TExprBase>();

bool IsFalseLiteral(TExprBase node) {
    return node.Maybe<TCoBool>() && !FromString<bool>(node.Cast<TCoBool>().Literal().Value());
}

bool ValidateIfArgument(const TCoOptionalIf& optionalIf, const TExprNode* rawLambdaArg) {
    // Check it is SELECT * or SELECT `field1`, `field2`...
    if (optionalIf.Value().Raw() == rawLambdaArg) {
        return true;
    }

    // Ok, maybe it is SELECT `field1`, `field2` ?
    auto maybeAsStruct = optionalIf.Value().Maybe<TCoAsStruct>();
    if (!maybeAsStruct) {
        return false;
    }

    for (auto arg : maybeAsStruct.Cast()) {
        // Check that second tuple element is Member(lambda arg)
        auto tuple = arg.Maybe<TExprList>().Cast();
        if (tuple.Size() != 2) {
            return false;
        }

        auto maybeMember = tuple.Item(1).Maybe<TCoMember>();
        if (!maybeMember) {
            return false;
        }

        auto member = maybeMember.Cast();
        if (member.Struct().Raw() != rawLambdaArg) {
            return false;
        }
    }

    return true;
}

TVector<TExprBase> ConvertComparisonNode(const TExprBase& nodeIn)
{
    TVector<TExprBase> out;
    auto convertNode = [](const TExprBase& node) -> TMaybeNode<TExprBase> {
        if (node.Maybe<TCoNull>()) {
            return node;
        }

        if (auto maybeSafeCast = node.Maybe<TCoSafeCast>()) {
            return node;
        }

        if (auto maybeParameter = node.Maybe<TCoParameter>()) {
            return maybeParameter.Cast();
        }

        if (auto maybeData = node.Maybe<TCoDataCtor>()) {
            return node;
        }

        if (auto maybeMember = node.Maybe<TCoMember>()) {
            return maybeMember.Cast().Name();
        }

        return NullNode;
    };

    // Columns & values may be single element
    TMaybeNode<TExprBase> node = convertNode(nodeIn);

    if (node.IsValid()) {
        out.emplace_back(std::move(node.Cast()));
        return out;
    }

    // Or columns and values can be Tuple
    if (!nodeIn.Maybe<TExprList>()) {
        // something unusual found, return empty vector
        return out;
    }

    auto tuple = nodeIn.Cast<TExprList>();

    out.reserve(tuple.Size());

    for (ui32 i = 0; i < tuple.Size(); ++i) {
        TMaybeNode<TExprBase> node = convertNode(tuple.Item(i));

        if (!node.IsValid()) {
            // Return empty vector
            return TVector<TExprBase>();
        }

        out.emplace_back(node.Cast());
    }

    return out;
}

TVector<std::pair<TExprBase, TExprBase>> ExtractComparisonParameters(const TCoCompare& predicate)
{
    TVector<std::pair<TExprBase, TExprBase>> out;
    auto left = ConvertComparisonNode(predicate.Left());

    if (left.empty()) {
        return out;
    }

    auto right = ConvertComparisonNode(predicate.Right());
    if (left.size() != right.size()) {
        return out;
    }

    for (ui32 i = 0; i < left.size(); ++i) {
        out.emplace_back(std::move(std::make_pair(left[i], right[i])));
    }

    return out;
}

TExprBase BuildOneElementComparison(const std::pair<TExprBase, TExprBase>& parameter, const TCoCompare& predicate,
    TExprContext& ctx, TPositionHandle pos, bool forceStrictComparison)
{
    auto isNull = [](const TExprBase& node) {
        if (node.Maybe<TCoNull>()) {
            return true;
        }

        if (node.Maybe<TCoNothing>()) {
            return true;
        }

        return false;
    };

    // Any comparison with NULL should return false even if NULL is uncomparable
    // See postgres documentation https://www.postgresql.org/docs/13/functions-comparisons.html
    // 9.24.5. Row Constructor Comparison
    if (isNull(parameter.first) || isNull(parameter.second)) {
        return Build<TCoBool>(ctx, pos)
            .Literal().Build("false")
            .Done();
    }

    std::string compareOperator = "";

    if (predicate.Maybe<TCoCmpEqual>()) {
        compareOperator = "eq";
    } else if (predicate.Maybe<TCoCmpNotEqual>()) {
        compareOperator = "neq";
    } else if (predicate.Maybe<TCoCmpLess>() || (predicate.Maybe<TCoCmpLessOrEqual>() && forceStrictComparison)) {
        compareOperator = "lt";
    } else if (predicate.Maybe<TCoCmpLessOrEqual>() && !forceStrictComparison) {
        compareOperator = "lte";
    } else if (predicate.Maybe<TCoCmpGreater>() || (predicate.Maybe<TCoCmpGreaterOrEqual>() && forceStrictComparison)) {
        compareOperator = "gt";
    } else if (predicate.Maybe<TCoCmpGreaterOrEqual>() && !forceStrictComparison) {
        compareOperator = "gte";
    }

    YQL_ENSURE(!compareOperator.empty(), "Unsupported comparison node: " << predicate.Ptr()->Content());

    return Build<TKqpOlapFilterCompare>(ctx, pos)
        .Operator(ctx.NewAtom(pos, compareOperator))
        .Left(parameter.first)
        .Right(parameter.second)
        .Done();
}

TMaybeNode<TExprBase> ComparisonPushdown(const TVector<std::pair<TExprBase, TExprBase>>& parameters, const TCoCompare& predicate,
    TExprContext& ctx, TPositionHandle pos)
{
    ui32 conditionsCount = parameters.size();

    if (conditionsCount == 1) {
        auto condition = BuildOneElementComparison(parameters[0], predicate, ctx, pos, false);
        return IsFalseLiteral(condition) ? NullNode : condition;
    }

    if (predicate.Maybe<TCoCmpEqual>() || predicate.Maybe<TCoCmpNotEqual>()) {
        TVector<TExprBase> conditions;
        conditions.reserve(conditionsCount);
        bool hasFalseCondition = false;

        for (ui32 i = 0; i < conditionsCount; ++i) {
            auto condition = BuildOneElementComparison(parameters[i], predicate, ctx, pos, false);
            if (IsFalseLiteral(condition)) {
                hasFalseCondition = true;
            } else {
                conditions.emplace_back(condition);
            }
        }

        if (predicate.Maybe<TCoCmpEqual>()) {
            if (hasFalseCondition) {
                return NullNode;
            }
            return Build<TKqpOlapAnd>(ctx, pos)
                .Add(conditions)
                .Done();
        }

        return Build<TKqpOlapOr>(ctx, pos)
            .Add(conditions)
            .Done();
    }

    TVector<TExprBase> orConditions;
    orConditions.reserve(conditionsCount);

    // Here we can be only when comparing tuples lexicographically
    for (ui32 i = 0; i < conditionsCount; ++i) {
        TVector<TExprBase> andConditions;
        andConditions.reserve(conditionsCount);

        // We need strict < and > in beginning columns except the last one
        // For example: (c1, c2, c3) >= (1, 2, 3) ==> (c1 > 1) OR (c2 > 2 AND c1 = 1) OR (c3 >= 3 AND c2 = 2 AND c1 = 1)
        auto condition = BuildOneElementComparison(parameters[i], predicate, ctx, pos, i < conditionsCount - 1);
        if (IsFalseLiteral(condition)) {
            continue;
        }
        andConditions.emplace_back(condition);

        for (ui32 j = 0; j < i; ++j) {
            andConditions.emplace_back(Build<TKqpOlapFilterCompare>(ctx, pos)
                .Operator(ctx.NewAtom(pos, "eq"))
                .Left(parameters[j].first)
                .Right(parameters[j].second)
                .Done());
        }

        orConditions.emplace_back(
            Build<TKqpOlapAnd>(ctx, pos)
                .Add(std::move(andConditions))
                .Done()
        );
    }

    return Build<TKqpOlapOr>(ctx, pos)
        .Add(std::move(orConditions))
        .Done();
}

// TODO: Check how to reduce columns if they are not needed. Unfortunately columnshard need columns list
// for every column present in program even if it is not used in result set.
//#define ENABLE_COLUMNS_PRUNING
#ifdef ENABLE_COLUMNS_PRUNING
TMaybeNode<TCoAtomList> BuildColumnsFromLambda(const TCoLambda& lambda, TExprContext& ctx, TPositionHandle pos)
{
    auto exprType = lambda.Ptr()->GetTypeAnn();

    if (exprType->GetKind() == ETypeAnnotationKind::Optional) {
        exprType = exprType->Cast<TOptionalExprType>()->GetItemType();
    }

    if (exprType->GetKind() != ETypeAnnotationKind::Struct) {
        return nullptr;
    }

    auto items = exprType->Cast<TStructExprType>()->GetItems();

    auto columnsList = Build<TCoAtomList>(ctx, pos);

    for (auto& item: items) {
        columnsList.Add(ctx.NewAtom(pos, item->GetName()));
    }

    return columnsList.Done();
}
#endif

TMaybeNode<TExprBase> ExistsPushdown(const TCoExists& exists, TExprContext& ctx, TPositionHandle pos)
{
    auto columnName = exists.Optional().Cast<TCoMember>().Name();

    return Build<TKqpOlapFilterExists>(ctx, pos)
        .Column(columnName)
        .Done();
}

TMaybeNode<TExprBase> SafeCastPredicatePushdown(const TCoFlatMap& inputFlatmap,
    TExprContext& ctx, TPositionHandle pos)
{
    /*
     * There are three ways of comparison in following format:
     *
     * FlatMap (LeftArgument, FlatMap(RightArgument(), Just(Predicate))
     *
     * Examples:
     * FlatMap (SafeCast(), FlatMap(Member(), Just(Comparison))
     * FlatMap (Member(), FlatMap(SafeCast(), Just(Comparison))
     * FlatMap (SafeCast(), FlatMap(SafeCast(), Just(Comparison))
     */
    TVector<std::pair<TExprBase, TExprBase>> out;

    auto left = ConvertComparisonNode(inputFlatmap.Input());
    if (left.empty()) {
        return NullNode;
    }

    auto flatmap = inputFlatmap.Lambda().Body().Cast<TCoFlatMap>();
    auto right = ConvertComparisonNode(flatmap.Input());
    if (right.empty()) {
        return NullNode;
    }

    auto predicate = flatmap.Lambda().Body().Cast<TCoJust>().Input().Cast<TCoCompare>();

    TVector<std::pair<TExprBase, TExprBase>> parameters;
    if (left.size() != right.size()) {
        return NullNode;
    }

    for (ui32 i = 0; i < left.size(); ++i) {
        parameters.emplace_back(std::move(std::make_pair(left[i], right[i])));
    }

    return ComparisonPushdown(parameters, predicate, ctx, pos);
}

TMaybeNode<TExprBase> SimplePredicatePushdown(const TCoCompare& predicate, TExprContext& ctx, TPositionHandle pos)
{
    auto parameters = ExtractComparisonParameters(predicate);
    if (parameters.empty()) {
        return NullNode;
    }

    return ComparisonPushdown(parameters, predicate, ctx, pos);
}

TMaybeNode<TExprBase> CoalescePushdown(const TCoCoalesce& coalesce, TExprContext& ctx, TPositionHandle pos)
{
    if (auto maybeFlatmap = coalesce.Predicate().Maybe<TCoFlatMap>()) {
        return SafeCastPredicatePushdown(maybeFlatmap.Cast(), ctx, pos);
    } else if (auto maybePredicate = coalesce.Predicate().Maybe<TCoCompare>()) {
        return SimplePredicatePushdown(maybePredicate.Cast(), ctx, pos);
    }

    return NullNode;
}

TMaybeNode<TExprBase> PredicatePushdown(const TExprBase& predicate, TExprContext& ctx, TPositionHandle pos)
{
    auto maybeCoalesce = predicate.Maybe<TCoCoalesce>();
    if (maybeCoalesce.IsValid()) {
        return CoalescePushdown(maybeCoalesce.Cast(), ctx, pos);
    }

    auto maybeExists = predicate.Maybe<TCoExists>();
    if (maybeExists.IsValid()) {
        return ExistsPushdown(maybeExists.Cast(), ctx, pos);
    }

    auto maybePredicate = predicate.Maybe<TCoCompare>();
    if (maybePredicate.IsValid()) {
        return SimplePredicatePushdown(maybePredicate.Cast(), ctx, pos);
    }

    if (predicate.Maybe<TCoNot>()) {
        auto notNode = predicate.Cast<TCoNot>();
        auto pushedNot = PredicatePushdown(notNode.Value(), ctx, pos);

        if (!pushedNot.IsValid()) {
            return NullNode;
        }

        return Build<TKqpOlapNot>(ctx, pos)
            .Value(pushedNot.Cast())
            .Done();
    }

    if (!predicate.Maybe<TCoAnd>() && !predicate.Maybe<TCoOr>() && !predicate.Maybe<TCoXor>()) {
        return NullNode;
    }

    TVector<TExprBase> pushedOps;
    pushedOps.reserve(predicate.Ptr()->ChildrenSize());

    for (auto& child: predicate.Ptr()->Children()) {
        auto pushedChild = PredicatePushdown(TExprBase(child), ctx, pos);

        if (!pushedChild.IsValid()) {
            return NullNode;
        }

        pushedOps.emplace_back(pushedChild.Cast());
    }

    if (predicate.Maybe<TCoAnd>()) {
        return Build<TKqpOlapAnd>(ctx, pos)
            .Add(pushedOps)
            .Done();
    }

    if (predicate.Maybe<TCoOr>()) {
        return Build<TKqpOlapOr>(ctx, pos)
            .Add(pushedOps)
            .Done();
    }

    Y_VERIFY_DEBUG(predicate.Maybe<TCoXor>());

    return Build<TKqpOlapXor>(ctx, pos)
        .Add(pushedOps)
        .Done();
}

} // anonymous namespace end

TExprBase KqpPushOlapFilter(TExprBase node, TExprContext& ctx, const TKqpOptimizeContext& kqpCtx,
    TTypeAnnotationContext& typesCtx)
{
    Y_UNUSED(typesCtx);

    if (!kqpCtx.Config->HasOptEnableOlapPushdown()) {
        return node;
    }

    if (!node.Maybe<TCoFlatMap>().Input().Maybe<TKqpReadOlapTableRanges>()) {
        return node;
    }

    auto flatmap = node.Cast<TCoFlatMap>();
    auto read = flatmap.Input().Cast<TKqpReadOlapTableRanges>();

    if (read.Process().Body().Raw() != read.Process().Args().Arg(0).Raw()) {
        return node;
    }

    const auto& lambda = flatmap.Lambda();
    auto lambdaArg = lambda.Args().Arg(0).Raw();

    YQL_CLOG(TRACE, ProviderKqp) << "Initial OLAP lambda: " << KqpExprToPrettyString(lambda, ctx);

    auto maybeOptionalIf = lambda.Body().Maybe<TCoOptionalIf>();
    if (!maybeOptionalIf.IsValid()) {
        return node;
    }

    auto optionalIf = maybeOptionalIf.Cast();
    if (!ValidateIfArgument(optionalIf, lambdaArg)) {
        return node;
    }

    TPredicateNode predicateTree(optionalIf.Predicate());
    CollectPredicates(optionalIf.Predicate(), predicateTree, lambdaArg, read.Process().Body());

    TPredicateNode predicatesToPush;
    TPredicateNode remainingPredicates;
    if (predicateTree.CanBePushed) {
        predicatesToPush = predicateTree;
        remainingPredicates.ExprNode = Build<TCoBool>(ctx, node.Pos()).Literal().Build("true").Done();
    } else {
        return node;
    }

    YQL_ENSURE(predicatesToPush.IsValid(), "Predicates to push is invalid");
    YQL_ENSURE(remainingPredicates.IsValid(), "Remaining predicates is invalid");

    auto pushedPredicate = PredicatePushdown(predicatesToPush.ExprNode.Cast(), ctx, node.Pos());
    YQL_ENSURE(pushedPredicate.IsValid(), "Pushed predicate should be always valid!");

    auto olapFilter = Build<TKqpOlapFilter>(ctx, node.Pos())
        .Input(read.Process().Body())
        .Condition(pushedPredicate.Cast())
        .Done();

    auto newProcessLambda = Build<TCoLambda>(ctx, node.Pos())
        .Args({"olap_filter_row"})
        .Body<TExprApplier>()
            .Apply(olapFilter)
            .With(read.Process().Args().Arg(0), "olap_filter_row")
            .Build()
        .Done();

    YQL_CLOG(TRACE, ProviderKqp) << "Pushed OLAP lambda: " << KqpExprToPrettyString(newProcessLambda, ctx);

#ifdef ENABLE_COLUMNS_PRUNING
    TMaybeNode<TCoAtomList> readColumns = BuildColumnsFromLambda(lambda, ctx, node.Pos());

    if (!readColumns.IsValid()) {
        readColumns = read.Columns();
    }
#endif

    auto newRead = Build<TKqpReadOlapTableRanges>(ctx, node.Pos())
        .Table(read.Table())
        .Ranges(read.Ranges())
#ifdef ENABLE_COLUMNS_PRUNING
        .Columns(readColumns.Cast())
#else
        .Columns(read.Columns())
#endif
        .Settings(read.Settings())
        .ExplainPrompt(read.ExplainPrompt())
        .Process(newProcessLambda)
        .Done();

#ifdef ENABLE_COLUMNS_PRUNING
    return newRead;
#else
    auto newFlatmap = Build<TCoFlatMap>(ctx, node.Pos())
        .Input(newRead)
        .Lambda<TCoLambda>()
            .Args({"new_arg"})
            .Body<TCoOptionalIf>()
                .Predicate(remainingPredicates.ExprNode.Cast())
                .Value<TExprApplier>()
                    .Apply(optionalIf.Value())
                    .With(lambda.Args().Arg(0), "new_arg")
                    .Build()
                .Build()
            .Build()
        .Done();

    return newFlatmap;
#endif
}

} // namespace NKikimr::NKqp::NOpt
