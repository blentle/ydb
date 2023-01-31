#include "kqp_opt_phy_rules.h"
#include "kqp_opt_phy_impl.h"

#include <ydb/core/kqp/common/kqp_yql.h>
#include <ydb/core/kqp/opt/kqp_opt_impl.h>

#include <ydb/library/yql/core/yql_opt_utils.h>

namespace NKikimr::NKqp::NOpt {

using namespace NYql;
using namespace NYql::NNodes;

// Temporary solution, should be replaced with constraints
// copy-past from old engine algo: https://a.yandex-team.ru/arc_vcs/yql/providers/kikimr/yql_kikimr_opt.cpp?rev=e592a5a9509952f1c29f1ec02343dd4c05fe426d#L122

using TTableData = std::pair<const NYql::TKikimrTableDescription*, NYql::TKqpReadTableSettings>;

TExprBase KqpRemoveRedundantSortByPkBase(
    TExprBase node,
    TExprContext& ctx,
    const TKqpOptimizeContext& kqpCtx,
    std::function<TMaybe<TTableData>(TExprBase)> tableAccessor,
    std::function<TExprBase(TExprBase, NYql::TKqpReadTableSettings)> rebuildInput)
{
    auto maybeSort = node.Maybe<TCoSort>();
    auto maybeTopSort = node.Maybe<TCoTopSort>();

    if (!maybeSort && !maybeTopSort) {
        return node;
    }

    auto input = maybeSort ? maybeSort.Cast().Input() : maybeTopSort.Cast().Input();
    auto sortDirections = maybeSort ? maybeSort.Cast().SortDirections() : maybeTopSort.Cast().SortDirections();
    auto keySelector = maybeSort ? maybeSort.Cast().KeySelectorLambda() : maybeTopSort.Cast().KeySelectorLambda();

    auto maybeFlatmap = input.Maybe<TCoFlatMap>();

    TMaybe<THashSet<TStringBuf>> passthroughFields;
    if (maybeFlatmap) {
        auto flatmap = input.Cast<TCoFlatMap>();

        if (!IsPassthroughFlatMap(flatmap, &passthroughFields)) {
            return node;
        }

        input = flatmap.Input();
    }

    enum : ui32 {
        SortDirectionNone = 0,
        SortDirectionForward = 1,
        SortDirectionReverse = 2,
        SortDirectionUnknown = 4,
    };

    auto getDirection = [] (TExprBase expr) -> ui32 {
        if (!expr.Maybe<TCoBool>()) {
            return SortDirectionUnknown;
        }

        if (!FromString<bool>(expr.Cast<TCoBool>().Literal().Value())) {
            return SortDirectionReverse;
        }

        return SortDirectionForward;
    };

    ui32 direction = SortDirectionNone;

    if (auto maybeList = sortDirections.Maybe<TExprList>()) {
        for (const auto& expr : maybeList.Cast()) {
            direction |= getDirection(expr);
            if (direction != SortDirectionForward && direction != SortDirectionReverse) {
                return node;
            }
        }
    } else {
        direction |= getDirection(sortDirections);
        if (direction != SortDirectionForward && direction != SortDirectionReverse) {
            return node;
        }
    }

    auto tableData = tableAccessor(input);
    if (!tableData) {
        return node;
    }
    auto& tableDesc = *tableData->first;
    auto settings = tableData->second;

    auto checkKey = [keySelector, &tableDesc, &passthroughFields] (TExprBase key, ui32 index) {
        if (!key.Maybe<TCoMember>()) {
            return false;
        }

        auto member = key.Cast<TCoMember>();
        if (member.Struct().Raw() != keySelector.Args().Arg(0).Raw()) {
            return false;
        }

        auto column = TString(member.Name().Value());
        auto columnIndex = tableDesc.GetKeyColumnIndex(column);
        if (!columnIndex || *columnIndex != index) {
            return false;
        }

        if (passthroughFields && !passthroughFields->contains(column)) {
            return false;
        }

        return true;
    };

    auto lambdaBody = keySelector.Body();
    if (auto maybeTuple = lambdaBody.Maybe<TExprList>()) {
        auto tuple = maybeTuple.Cast();
        for (size_t i = 0; i < tuple.Size(); ++i) {
            if (!checkKey(tuple.Item(i), i)) {
                return node;
            }
        }
    } else {
        if (!checkKey(lambdaBody, 0)) {
            return node;
        }
    }

    bool olapTable = tableDesc.Metadata->Kind == EKikimrTableKind::Olap;
    if (direction == SortDirectionReverse) {
        if (!olapTable && kqpCtx.IsScanQuery()) {
            return node;
        }

        if (settings.Reverse) {
            return node;
        }

        settings.SetReverse();
        settings.SetSorted();

        input = rebuildInput(input, settings);
    } else if (direction == SortDirectionForward) {
        if (olapTable) {
            settings.SetSorted();
            input = rebuildInput(input, settings);
        }
    }

    if (maybeFlatmap) {
        input = Build<TCoFlatMap>(ctx, node.Pos())
            .Input(input)
            .Lambda(maybeFlatmap.Cast().Lambda())
            .Done();
    }

    if (maybeTopSort) {
        return Build<TCoTake>(ctx, node.Pos())
            .Input(input)
            .Count(maybeTopSort.Cast().Count())
            .Done();
    } else {
        return input;
    }
}

TExprBase KqpRemoveRedundantSortByPk(TExprBase node, TExprContext& ctx, const TKqpOptimizeContext& kqpCtx) {
    return KqpRemoveRedundantSortByPkBase(node, ctx, kqpCtx,
        [&](TExprBase input) -> TMaybe<TTableData> {
            bool isReadTable = input.Maybe<TKqpReadTable>().IsValid();
            bool isReadTableRanges = input.Maybe<TKqlReadTableRangesBase>().IsValid();
            if (!isReadTable && !isReadTableRanges) {
                return Nothing();
            }
            auto& tableDesc = kqpCtx.Tables->ExistingTable(kqpCtx.Cluster, GetReadTablePath(input, isReadTableRanges));
            auto settings = GetReadTableSettings(input, isReadTableRanges);
            return TTableData{&tableDesc, settings};
        },
        [&](TExprBase input, NYql::TKqpReadTableSettings settings) {
            return BuildReadNode(input.Pos(), ctx, input, settings);
        });
}

//FIXME: simplify KIKIMR-16987
NYql::NNodes::TExprBase KqpRemoveRedundantSortByPkOverSource(
    NYql::NNodes::TExprBase node, NYql::TExprContext& exprCtx, const TKqpOptimizeContext& kqpCtx)
{
    auto stage = node.Cast<TDqStage>();
    TMaybe<size_t> tableSourceIndex;
    for (size_t i = 0; i < stage.Inputs().Size(); ++i) {
        auto input = stage.Inputs().Item(i);
        if (input.Maybe<TDqSource>() && input.Cast<TDqSource>().Settings().Maybe<TKqpReadRangesSourceSettings>()) {
            tableSourceIndex = i;
        }
    }
    if (!tableSourceIndex) {
        return node;
    }

    auto source = stage.Inputs().Item(*tableSourceIndex).Cast<TDqSource>();
    auto readRangesSource = source.Settings().Cast<TKqpReadRangesSourceSettings>();
    auto settings = TKqpReadTableSettings::Parse(readRangesSource.Settings());

    auto& tableDesc = kqpCtx.Tables->ExistingTable(kqpCtx.Cluster, readRangesSource.Table().Path());

    TVector<NYql::TKqpReadTableSettings> newSettings;
    NYql::TNodeOnNodeOwnedMap replaces;
    auto sourceArg = stage.Program().Args().Arg(*tableSourceIndex);
    VisitExpr(stage.Program().Body().Ptr(),
        [&](const TExprNode::TPtr& exprPtr) -> bool {
            TExprBase expr(exprPtr);
            if (expr.Maybe<TDqConnection>() || expr.Maybe<TDqPrecompute>() || expr.Maybe<TDqPhyPrecompute>()) {
                return false;
            }
            if (TCoSort::Match(expr.Raw()) || TCoTopSort::Match(expr.Raw())) {
                auto newExpr = KqpRemoveRedundantSortByPkBase(expr, exprCtx, kqpCtx,
                    [&](TExprBase node) -> TMaybe<TTableData> {
                        if (node.Ptr() != node.Ptr()) {
                            return Nothing();
                        }
                        return TTableData{&tableDesc, settings};
                    },
                    [&](TExprBase input, NYql::TKqpReadTableSettings settings) {
                        newSettings.push_back(settings);
                        return input;
                    });
                if (newExpr.Ptr() != expr.Ptr()) {
                    replaces[expr.Raw()] = newExpr.Ptr();
                }
            }
            return true;
        });

    if (newSettings) {
        for (size_t i = 1; i < newSettings.size(); ++i) {
            if (newSettings[0] != newSettings[i]) {
                return node;
            }
        }

        auto newSource = Build<TKqpReadRangesSourceSettings>(exprCtx, source.Pos())
            .Table(readRangesSource.Table())
            .Columns(readRangesSource.Columns())
            .Settings(newSettings[0].BuildNode(exprCtx, source.Settings().Pos()))
            .RangesExpr(readRangesSource.RangesExpr())
            .ExplainPrompt(readRangesSource.ExplainPrompt())
            .Done();
        replaces[readRangesSource.Raw()] = newSource.Ptr();
    }

    return TExprBase(exprCtx.ReplaceNodes(node.Ptr(), replaces));
}

} // namespace NKikimr::NKqp::NOpt

