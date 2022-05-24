#include "yql_co_pgselect.h"

#include <ydb/library/yql/core/yql_expr_optimize.h>
#include <ydb/library/yql/core/yql_opt_utils.h>

namespace NYql {

TExprNode::TPtr BuildFilter(TPositionHandle pos, const TExprNode::TPtr& list, const TExprNode::TPtr& filter, TExprContext& ctx) {
    return ctx.Builder(pos)
        .Callable("Filter")
            .Add(0, list)
            .Lambda(1)
                .Param("row")
                .Callable("Coalesce")
                    .Callable(0, "FromPg")
                        .Apply(0, filter->Tail().TailPtr())
                            .With(0, "row")
                        .Seal()
                    .Seal()
                    .Callable(1, "Bool")
                        .Atom(0, "0")
                    .Seal()
                .Seal()
            .Seal()
        .Seal()
        .Build();
}

TExprNode::TPtr ExpandPositionalUnionAll(const TExprNode& node, const TVector<TColumnOrder>& columnOrders,
    TExprNode::TListType children, TExprContext& ctx, TOptimizeContext& optCtx) {
    auto targetColumnOrder = optCtx.Types->LookupColumnOrder(node);
    YQL_ENSURE(targetColumnOrder);

    for (ui32 childIndex = 0; childIndex < children.size(); ++childIndex) {
        const auto& childColumnOrder = columnOrders[childIndex];
        auto& child = children[childIndex];
        if (childColumnOrder == *targetColumnOrder) {
            continue;
        }

        YQL_ENSURE(childColumnOrder.size() == targetColumnOrder->size());
        child = ctx.Builder(child->Pos())
            .Callable("Map")
                .Add(0, child)
                .Lambda(1)
                    .Param("row")
                    .Callable("AsStruct")
                    .Do([&](TExprNodeBuilder &parent) -> TExprNodeBuilder & {
                        for (size_t i = 0; i < childColumnOrder.size(); ++i) {
                            parent
                                .List(i)
                                    .Atom(0, child->Pos(), (*targetColumnOrder)[i])
                                    .Callable(1, "Member")
                                        .Arg(0, "row")
                                        .Atom(1, childColumnOrder[i])
                                    .Seal()
                                .Seal();
                        }
                        return parent;
                    })
                    .Seal()
                .Seal()
            .Seal()
            .Build();
    }

    auto res = ctx.NewCallable(node.Pos(), "UnionAll", std::move(children));
    return KeepColumnOrder(res, node, ctx, *optCtx.Types);
}

TExprNode::TPtr BuildValues(TPositionHandle pos, const TExprNode::TPtr& values, TExprContext& ctx) {
    return ctx.Builder(pos)
        .Callable("Map")
            .Add(0, values->ChildPtr(2))
            .Lambda(1)
                .Param("row")
                .Callable("AsStruct")
                .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                    for (ui32 index = 0; index < values->Child(1)->ChildrenSize(); ++index) {
                        parent
                            .List(index)
                                .Atom(0, values->Child(1)->Child(index)->Content())
                                .Callable(1, "Nth")
                                    .Arg(0, "row")
                                    .Atom(1, ToString(index))
                                .Seal()
                            .Seal();
                    }

                    return parent;
                })
                .Seal()
            .Seal()
        .Seal()
        .Build();
}

std::tuple<TExprNode::TPtr, TExprNode::TPtr> BuildOneRow(TPositionHandle pos, const TExprNode::TPtr& result, TExprContext& ctx) {
    TExprNode::TListType rowItems;
    for (const auto& x : result->Tail().Children()) {
        rowItems.push_back(ctx.NewList(x->Pos(), { x->HeadPtr(), x->Tail().TailPtr() }));
    }

    auto row = ctx.NewCallable(pos, "AsStruct", std::move(rowItems));
    auto list = ctx.Builder(pos)
        .Callable("AsList")
            .Callable(0, "AsStruct")
            .Seal()
        .Seal()
        .Build();

    auto projectionLambda = ctx.Builder(pos)
        .Lambda()
            .Param("row")
            .Set(row)
            .Seal()
        .Build();

    return { list, projectionLambda };
}

using TUsedColumns = TMap<TString, std::pair<ui32, TString>>;

TUsedColumns GatherUsedColumns(const TExprNode::TPtr& result, const TExprNode::TPtr& joinOps) {
    TUsedColumns usedColumns;
    for (const auto& x : result->Tail().Children()) {
        auto type = x->Child(1)->GetTypeAnn()->Cast<TTypeExprType>()->GetType()->Cast<TStructExprType>();
        for (auto item : type->GetItems()) {
            usedColumns.insert(std::make_pair(TString(item->GetName()), std::make_pair(Max<ui32>(), TString())));
        }
    }

    for (ui32 groupNo = 0; groupNo < joinOps->Tail().ChildrenSize(); ++groupNo) {
        auto groupTuple = joinOps->Tail().Child(groupNo);
        for (ui32 i = 0; i < groupTuple->ChildrenSize(); ++i) {
            auto join = groupTuple->Child(i);
            auto joinType = join->Child(0)->Content();
            if (joinType != "cross") {
                auto type = join->Tail().Child(0)->GetTypeAnn()->Cast<TTypeExprType>()->GetType()->Cast<TStructExprType>();
                for (auto item : type->GetItems()) {
                    usedColumns.insert(std::make_pair(TString(item->GetName()), std::make_pair(Max<ui32>(), TString())));
                }
            }
        }
    }

    return usedColumns;
}

void FillInputIndices(const TExprNode::TPtr& from, TUsedColumns& usedColumns, TOptimizeContext& optCtx) {
    for (auto& x : usedColumns) {
        bool foundColumn = false;
        for (ui32 inputIndex = 0; inputIndex < from->Tail().ChildrenSize(); ++inputIndex) {
            const auto& read = from->Tail().Child(inputIndex)->Head();
            const auto& columns = from->Tail().Child(inputIndex)->Tail();
            if (read.IsCallable("PgResolvedCall")) {
                const auto& alias = from->Tail().Child(inputIndex)->Child(1)->Content();
                Y_ENSURE(!alias.empty());
                Y_ENSURE(columns.ChildrenSize() == 0 || columns.ChildrenSize() == 1);
                auto memberName = (columns.ChildrenSize() == 1) ? columns.Head().Content() : alias;
                foundColumn = (memberName == x.first);
            } else {
                if (columns.ChildrenSize() > 0) {
                    auto readOrder = optCtx.Types->LookupColumnOrder(read);
                    YQL_ENSURE(readOrder);
                    for (ui32 i = 0; i < columns.ChildrenSize(); ++i) {
                        if (columns.Child(i)->Content() == x.first) {
                            foundColumn = true;
                            x.second.second = (*readOrder)[i];
                            break;
                        }
                    }
                } else {
                    auto type = read.GetTypeAnn()->
                        Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();
                    auto pos = type->FindItem(x.first);
                    foundColumn = pos.Defined();
                }
            }

            if (foundColumn) {
                x.second.first = inputIndex;
                break;
            }
        }

        YQL_ENSURE(foundColumn, "Missing column: " << x.first);
    }
}

TExprNode::TListType BuildCleanedColumns(TPositionHandle pos, const TExprNode::TPtr& from, const TUsedColumns& usedColumns, TExprContext& ctx) {
    TExprNode::TListType cleanedInputs;
    for (ui32 i = 0; i < from->Tail().ChildrenSize(); ++i) {
        auto list = from->Tail().Child(i)->HeadPtr();
        if (list->IsCallable("PgResolvedCall")) {
            const auto& alias = from->Tail().Child(i)->Child(1)->Content();
            const auto& columns = from->Tail().Child(i)->Tail();
            Y_ENSURE(!alias.empty());
            Y_ENSURE(columns.ChildrenSize() == 0 || columns.ChildrenSize() == 1);
            auto memberName = (columns.ChildrenSize() == 1) ? columns.Head().Content() : alias;
            if (list->GetTypeAnn()->GetKind() == ETypeAnnotationKind::List) {
                list = ctx.Builder(pos)
                    .Callable("OrderedMap")
                        .Add(0, list)
                        .Lambda(1)
                            .Param("item")
                            .Callable("AsStruct")
                                .List(0)
                                    .Atom(0, memberName)
                                    .Arg(1, "item")
                                .Seal()
                            .Seal()
                        .Seal()
                    .Seal()
                    .Build();
            } else {
                list = ctx.Builder(pos)
                    .Callable("AsList")
                        .Callable(0, "AsStruct")
                            .List(0)
                                .Atom(0, memberName)
                                .Add(1, list)
                            .Seal()
                        .Seal()
                    .Seal()
                    .Build();
            }
        }

        auto cleaned = ctx.Builder(pos)
            .Callable("Map")
                .Add(0, list)
                .Lambda(1)
                    .Param("row")
                    .Callable("AsStruct")
                        .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                            ui32 index = 0;
                            for (const auto& x : usedColumns) {
                                if (x.second.first != i) {
                                    continue;
                                }

                                auto listBuilder = parent.List(index++);
                                listBuilder.Atom(0, x.first);
                                listBuilder.Callable(1, "Member")
                                    .Arg(0, "row")
                                    .Atom(1, x.second.second ? x.second.second : x.first)
                                .Seal();
                                listBuilder.Seal();
                            }

                            return parent;
                        })
                    .Seal()
                .Seal()
            .Seal()
            .Build();

        cleanedInputs.push_back(cleaned);
    }

    return cleanedInputs;
}

std::tuple<TVector<ui32>, TExprNode::TListType> BuildJoinGroups(TPositionHandle pos, const TExprNode::TListType& cleanedInputs,
    const TExprNode::TPtr& joinOps, TExprContext& ctx) {
    TVector<ui32> groupForIndex;
    TExprNode::TListType joinGroups;

    ui32 inputIndex = 0;
    for (ui32 groupNo = 0; groupNo < joinOps->Tail().ChildrenSize(); ++groupNo) {
        groupForIndex.push_back(groupNo);
        auto groupTuple = joinOps->Tail().Child(groupNo);
        if (groupTuple->ChildrenSize() == 0) {
            joinGroups.push_back(cleanedInputs[inputIndex++]);
            continue;
        }

        auto current = cleanedInputs[inputIndex++];
        for (ui32 i = 0; i < groupTuple->ChildrenSize(); ++i) {
            groupForIndex.push_back(groupNo);
            auto with = cleanedInputs[inputIndex++];
            // current = join current & with
            auto join = groupTuple->Child(i);
            auto joinType = join->Child(0)->Content();
            auto cartesian = ctx.Builder(pos)
                    .Callable("FlatMap")
                        .Add(0, current)
                        .Lambda(1)
                            .Param("x")
                            .Callable("Map")
                                .Add(0, with)
                                .Lambda(1)
                                    .Param("y")
                                    .Callable("FlattenMembers")
                                        .List(0)
                                            .Atom(0, "")
                                            .Arg(1,"x")
                                        .Seal()
                                        .List(1)
                                            .Atom(0, "")
                                            .Arg(1, "y")
                                        .Seal()
                                    .Seal()
                                .Seal()
                            .Seal()
                        .Seal()
                    .Seal()
                    .Build();

            auto buildMinus = [&](auto left, auto right) {
                return ctx.Builder(pos)
                    .Callable("Filter")
                        .Add(0, left)
                        .Lambda(1)
                            .Param("x")
                            .Callable(0, "Not")
                                .Callable(0, "HasItems")
                                    .Callable(0, "Filter")
                                        .Add(0, right)
                                        .Lambda(1)
                                            .Param("y")
                                            .Callable("Coalesce")
                                                .Callable(0, "FromPg")
                                                    .Apply(0, join->Tail().TailPtr())
                                                        .With(0)
                                                            .Callable("FlattenMembers")
                                                                .List(0)
                                                                    .Atom(0, "")
                                                                    .Arg(1,"x")
                                                                .Seal()
                                                                .List(1)
                                                                    .Atom(0, "")
                                                                    .Arg(1, "y")
                                                                .Seal()
                                                            .Seal()
                                                        .Done()
                                                    .Seal()
                                                .Seal()
                                                .Callable(1, "Bool")
                                                    .Atom(0, "0")
                                                .Seal()
                                            .Seal()
                                        .Seal()
                                    .Seal()
                                .Seal()
                            .Seal()
                        .Seal()
                    .Seal()
                    .Build();
            };

            TExprNode::TPtr filteredCartesian;
            if (joinType != "cross") {
                filteredCartesian = BuildFilter(pos, cartesian, join, ctx);
            }

            if (joinType == "cross") {
                current = cartesian;
            } else if (joinType == "inner") {
                current = filteredCartesian;
            } else if (joinType == "left") {
                current = ctx.Builder(pos)
                    .Callable("UnionAll")
                        .Add(0, filteredCartesian)
                        .Add(1, buildMinus(current, with))
                    .Seal()
                    .Build();
            } else if (joinType == "right") {
                current = ctx.Builder(pos)
                    .Callable("UnionAll")
                        .Add(0, filteredCartesian)
                        .Add(1, buildMinus(with, current))
                    .Seal()
                    .Build();
            } else {
                YQL_ENSURE(joinType == "full");
                current = ctx.Builder(pos)
                    .Callable("UnionAll")
                        .Add(0, filteredCartesian)
                        .Add(1, buildMinus(current, with))
                        .Add(2, buildMinus(with, current))
                    .Seal()
                    .Build();
            }
        }

        joinGroups.push_back(current);
    }

    return { groupForIndex, joinGroups };
}

TExprNode::TPtr BuildCrossJoinsBetweenGroups(TPositionHandle pos, const TExprNode::TListType& joinGroups,
    const TUsedColumns& usedColumns, const TVector<ui32>& groupForIndex, TExprContext& ctx) {
    TExprNode::TListType args;
    for (ui32 i = 0; i < joinGroups.size(); ++i) {
        args.push_back(ctx.Builder(pos)
            .List()
                .Add(0, joinGroups[i])
                .Atom(1, ToString(i))
            .Seal()
            .Build());
    }

    auto tree = ctx.Builder(pos)
        .List()
            .Atom(0, "Cross")
            .Atom(1, "0")
            .Atom(2, "1")
            .List(3)
            .Seal()
            .List(4)
            .Seal()
            .List(5)
            .Seal()
        .Seal()
        .Build();

    for (ui32 i = 2; i < joinGroups.size(); ++i) {
        tree = ctx.Builder(pos)
            .List()
                .Atom(0, "Cross")
                .Add(1, tree)
                .Atom(2, ToString(i))
                .List(3)
                .Seal()
                .List(4)
                .Seal()
                .List(5)
                .Seal()
            .Seal()
            .Build();
    }

    args.push_back(tree);
    TExprNode::TListType settings;
    for (const auto& x : usedColumns) {
        settings.push_back(ctx.Builder(pos)
            .List()
                .Atom(0, "rename")
                .Atom(1, ToString(groupForIndex[x.second.first]) + "." + x.first)
                .Atom(2, x.first)
            .Seal()
            .Build());
    }

    auto settingsNode = ctx.NewList(pos, std::move(settings));
    args.push_back(settingsNode);
    return ctx.NewCallable(pos, "EquiJoin", std::move(args));
}

TExprNode::TPtr BuildProjectionLambda(TPositionHandle pos, const TExprNode::TPtr& result, TExprContext& ctx) {
    return ctx.Builder(pos)
        .Lambda()
            .Param("row")
            .Callable("AsStruct")
            .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                ui32 index = 0;
                for (const auto& x : result->Tail().Children()) {
                    if (x->HeadPtr()->IsAtom()) {
                        auto listBuilder = parent.List(index++);
                        listBuilder.Add(0, x->HeadPtr());
                        listBuilder.Apply(1, x->TailPtr())
                            .With(0, "row")
                        .Seal();
                        listBuilder.Seal();
                    } else {
                        for (ui32 i = 0; i < x->Head().ChildrenSize(); ++i) {
                            auto listBuilder = parent.List(index++);
                            listBuilder.Add(0, x->Head().ChildPtr(i));
                            listBuilder.Callable(1, "Member")
                                .Arg(0, "row")
                                .Add(1, x->Head().ChildPtr(i));
                            listBuilder.Seal();
                        }
                    }
                }

                return parent;
            })
            .Seal()
        .Seal()
        .Build();
}

using TAggs = TVector<std::pair<TExprNode::TPtr, TExprNode::TPtr>>;

std::tuple<TAggs, TNodeMap<ui32>> GatherAggregations(const TExprNode::TPtr& projectionLambda, const TExprNode::TPtr& having) {
    TAggs aggs;
    TNodeMap<ui32> aggId;

    VisitExpr(projectionLambda->TailPtr(), [&](const TExprNode::TPtr& node) {
        if (node->IsCallable("PgAgg") || node->IsCallable("PgAggAll")) {
            aggId[node.Get()] = aggs.size();
            aggs.push_back({ node, projectionLambda->Head().HeadPtr() });
        }

        return true;
    });

    if (having) {
        auto havingLambda = having->Tail().TailPtr();
        VisitExpr(having->Tail().TailPtr(), [&](const TExprNode::TPtr& node) {
            if (node->IsCallable("PgAgg") || node->IsCallable("PgAggAll")) {
                aggId[node.Get()] = aggs.size();
                aggs.push_back({ node, havingLambda->Head().HeadPtr() });
            }

            return true;
        });
    }

    return { aggs, aggId };
}

TExprNode::TPtr BuildAggregationTraits(TPositionHandle pos, bool onWindow,
    const std::pair<TExprNode::TPtr, TExprNode::TPtr>& agg,
    const TExprNode::TPtr& listTypeNode, TExprContext& ctx) {
    auto arg = ctx.NewArgument(pos, "row");
    auto arguments = ctx.NewArguments(pos, { arg });
    auto func = agg.first->Head().Content();
    TExprNode::TListType aggFuncArgs;
    for (ui32 j = onWindow ? 2 : 1; j < agg.first->ChildrenSize(); ++j) {
        aggFuncArgs.push_back(ctx.ReplaceNode(agg.first->ChildPtr(j), *agg.second, arg));
    }

    auto extractor = ctx.NewLambda(pos, std::move(arguments), std::move(aggFuncArgs));

    return ctx.Builder(pos)
        .Callable(onWindow ? "PgWindowTraits" : "PgAggregationTraits")
            .Atom(0, func)
            .Callable(1, "ListItemType")
                .Add(0, listTypeNode)
            .Seal()
            .Add(2, extractor)
        .Seal()
        .Build();
}

TExprNode::TPtr BuildGroupByAndHaving(TPositionHandle pos, const TExprNode::TPtr& list, const TAggs& aggs, const TNodeMap<ui32>& aggId,
    const TExprNode::TPtr& groupBy, const TExprNode::TPtr& having, TExprNode::TPtr& projectionLambda, TExprContext& ctx, TOptimizeContext& optCtx) {
    auto listTypeNode = ctx.Builder(pos)
        .Callable("TypeOf")
            .Add(0, list)
        .Seal()
        .Build();

    TExprNode::TListType payloadItems;
    for (ui32 i = 0; i < aggs.size(); ++i) {
        auto traits = BuildAggregationTraits(pos, false, aggs[i], listTypeNode, ctx);
        payloadItems.push_back(ctx.Builder(pos)
            .List()
                .Atom(0, "_yql_agg_" + ToString(i))
                .Add(1, traits)
            .Seal()
            .Build());
    }

    auto payloadsNode = ctx.NewList(pos, std::move(payloadItems));
    TExprNode::TListType keysItems;
    if (groupBy) {
        for (const auto& group : groupBy->Tail().Children()) {
            const auto& lambda = group->Tail();
            YQL_ENSURE(lambda.IsLambda());
            YQL_ENSURE(lambda.Tail().IsCallable("Member"));
            keysItems.push_back(lambda.Tail().TailPtr());
        }
    }

    auto keys = ctx.NewList(pos, std::move(keysItems));

    auto ret = ctx.Builder(pos)
        .Callable("Aggregate")
            .Add(0, list)
            .Add(1, keys)
            .Add(2, payloadsNode)
            .List(3) // options
            .Seal()
        .Seal()
        .Build();

    auto rewriteAggs = [&](auto& lambda) {
        auto status = OptimizeExpr(lambda, lambda, [&](const TExprNode::TPtr& node, TExprContext& ctx) -> TExprNode::TPtr {
            auto it = aggId.find(node.Get());
            if (it != aggId.end()) {
                auto ret = ctx.Builder(pos)
                    .Callable("Member")
                        .Add(0, lambda->Head().HeadPtr())
                        .Atom(1, "_yql_agg_" + ToString(it->second))
                    .Seal()
                    .Build();

                return ret;
            }

            return node;
        }, ctx, TOptimizeExprSettings(optCtx.Types));

        return status.Level != IGraphTransformer::TStatus::Error;
    };

    if (!rewriteAggs(projectionLambda)) {
        return {};
    }

    if (having) {
        auto havingLambda = having->Tail().TailPtr();
        if (!rewriteAggs(havingLambda)) {
            return {};
        }

        ret = ctx.Builder(pos)
            .Callable("Filter")
                .Add(0, ret)
                .Lambda(1)
                    .Param("row")
                    .Callable("Coalesce")
                        .Callable(0, "FromPg")
                            .Apply(0, havingLambda)
                                .With(0, "row")
                            .Seal()
                        .Seal()
                        .Callable(1, "Bool")
                            .Atom(0, "0")
                        .Seal()
                    .Seal()
                .Seal()
            .Seal()
            .Build();
    }

    return ret;
}

std::tuple<TExprNode::TPtr, TExprNode::TPtr> BuildFrame(TPositionHandle pos, const TExprNode& frameSettings, TExprContext& ctx) {
    TExprNode::TPtr begin;
    TExprNode::TPtr end;
    const auto& from = GetSetting(frameSettings, "from");
    const auto& fromValue = GetSetting(frameSettings, "from_value");

    auto fromName = from->Tail().Content();
    if (fromName == "up") {
        begin = ctx.NewCallable(pos, "Void", {});
    } else if (fromName == "p") {
        auto val = FromString<i32>(fromValue->Tail().Head().Content());
        begin = ctx.NewCallable(pos, "Int32", { ctx.NewAtom(pos, ToString(-val)) });
    } else if (fromName == "c") {
        begin = ctx.NewCallable(pos, "Int32", { ctx.NewAtom(pos, "0") });
    } else {
        YQL_ENSURE(fromName == "f");
        auto val = FromString<i32>(fromValue->Tail().Head().Content());
        begin = ctx.NewCallable(pos, "Int32", { ctx.NewAtom(pos, ToString(val)) });
    }

    const auto& to = GetSetting(frameSettings, "to");
    const auto& toValue = GetSetting(frameSettings, "to_value");

    auto toName = to->Tail().Content();
    if (toName == "p") {
        auto val = FromString<i32>(toValue->Tail().Head().Content());
        end = ctx.NewCallable(pos, "Int32", { ctx.NewAtom(pos, ToString(-val)) });
    } else if (toName == "c") {
        end = ctx.NewCallable(pos, "Int32", { ctx.NewAtom(pos, "0") });
    } else if (toName == "f") {
        auto val = FromString<i32>(toValue->Tail().Head().Content());
        end = ctx.NewCallable(pos, "Int32", { ctx.NewAtom(pos, ToString(val)) });
    } else {
        YQL_ENSURE(toName == "uf");
        end = ctx.NewCallable(pos, "Void", {});
    }

    return { begin, end };
}

TExprNode::TPtr BuildSortTraits(TPositionHandle pos, const TExprNode& sortColumns, const TExprNode::TPtr& list, TExprContext& ctx) {
    if (sortColumns.ChildrenSize() == 1) {
        return ctx.Builder(pos)
            .Callable("SortTraits")
                .Callable(0, "TypeOf")
                    .Add(0, list)
                .Seal()
                .Callable(1, "Bool")
                    .Atom(0, sortColumns.Head().Tail().Content() == "asc" ? "true" : "false")
                .Seal()
                .Lambda(2)
                    .Param("row")
                    .Apply(sortColumns.Head().ChildPtr(1))
                        .With(0, "row")
                    .Seal()
                .Seal()
            .Seal()
            .Build();
    } else {
        return ctx.Builder(pos)
            .Callable("SortTraits")
                .Callable(0, "TypeOf")
                    .Add(0, list)
                .Seal()
                .List(1)
                    .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                        for (ui32 i = 0; i < sortColumns.ChildrenSize(); ++i) {
                            parent.Callable(i, "Bool")
                                .Atom(0, sortColumns.Child(i)->Tail().Content() == "asc" ? "true" : "false")
                                .Seal();
                        }
                        return parent;
                    })
                .Seal()
                .Lambda(2)
                    .Param("row")
                    .List()
                        .Do([&](TExprNodeBuilder& parent) -> TExprNodeBuilder& {
                            for (ui32 i = 0; i < sortColumns.ChildrenSize(); ++i) {
                                parent.Apply(i, sortColumns.Child(i)->ChildPtr(1))
                                    .With(0, "row")
                                    .Seal();
                            }

                            return parent;
                        })
                    .Seal()
                .Seal()
            .Seal()
            .Build();
    }
}

TExprNode::TPtr BuildWindows(TPositionHandle pos, const TExprNode::TPtr& list, const TExprNode::TPtr& window,
    TExprNode::TPtr& projectionLambda, TExprContext& ctx, TOptimizeContext& optCtx) {
    TVector<std::pair<TExprNode::TPtr, TExprNode::TPtr>> winFuncs;
    TMap<ui32, TVector<ui32>> window2funcs;
    TNodeMap<ui32> winFuncsId;
    auto ret = list;
    VisitExpr(projectionLambda->TailPtr(), [&](const TExprNode::TPtr& node) {
        if (node->IsCallable("PgWindowCall") || node->IsCallable("PgAggWindowCall")) {
            YQL_ENSURE(window);
            ui32 windowIndex;
            if (node->Child(1)->IsCallable("PgAnonWindow")) {
                windowIndex = FromString<ui32>(node->Child(1)->Head().Content());
            } else {
                auto name = node->Child(1)->Content();
                bool found = false;
                for (ui32 index = 0; index < window->Tail().ChildrenSize(); ++index) {
                    if (window->Tail().Child(index)->Head().Content() == name) {
                        windowIndex = index;
                        found = true;
                        break;
                    }
                }

                YQL_ENSURE(found);
            }

            window2funcs[windowIndex].push_back(winFuncs.size());
            winFuncsId[node.Get()] = winFuncs.size();
            winFuncs.push_back({ node, projectionLambda->Head().HeadPtr() });
        }

        return true;
    });

    if (!winFuncs.empty()) {
        auto listTypeNode = ctx.Builder(pos)
            .Callable("TypeOf")
                .Add(0, list)
            .Seal()
            .Build();

        for (const auto& x : window2funcs) {
            auto win = window->Tail().Child(x.first);
            const auto& frameSettings = win->Tail();

            TExprNode::TListType args;
            // default frame
            auto begin = ctx.NewCallable(pos, "Void", {});
            auto end = win->Child(3)->ChildrenSize() > 0 ?
                ctx.NewCallable(pos, "Int32", { ctx.NewAtom(pos, "0") }) :
                ctx.NewCallable(pos, "Void", {});
            if (HasSetting(frameSettings, "type")) {
                std::tie(begin, end) = BuildFrame(pos, frameSettings, ctx);
            }

            args.push_back(ctx.Builder(pos)
                .List()
                    .List(0)
                        .Atom(0, "begin")
                        .Add(1, begin)
                    .Seal()
                    .List(1)
                        .Atom(0, "end")
                        .Add(1, end)
                    .Seal()
                .Seal()
                .Build());

            for (const auto& index : x.second) {
                auto p = winFuncs[index];
                auto name = p.first->Head().Content();
                bool isAgg = p.first->IsCallable("PgAggWindowCall");
                TExprNode::TPtr value;
                if (isAgg) {
                    value = BuildAggregationTraits(pos, true, p, listTypeNode, ctx);
                } else {
                    if (name == "row_number") {
                        value = ctx.Builder(pos)
                            .Callable("RowNumber")
                                .Callable(0, "TypeOf")
                                    .Add(0, list)
                                .Seal()
                            .Seal()
                            .Build();
                    } else if (name == "lead" || name == "lag") {
                        auto arg = ctx.NewArgument(pos, "row");
                        auto arguments = ctx.NewArguments(pos, { arg });
                        auto extractor = ctx.NewLambda(pos, std::move(arguments),
                            ctx.ReplaceNode(p.first->TailPtr(), *p.second, arg));

                        value = ctx.Builder(pos)
                            .Callable(name == "lead" ? "Lead" : "Lag")
                                .Callable(0, "TypeOf")
                                    .Add(0, list)
                                .Seal()
                                .Add(1, extractor)
                            .Seal()
                            .Build();
                    } else {
                        ythrow yexception() << "Not supported function: " << name;
                    }
                }

                args.push_back(ctx.Builder(pos)
                    .List()
                        .Atom(0, "_yql_win_" + ToString(index))
                        .Add(1, value)
                    .Seal()
                    .Build());
            }

            auto winOnRows = ctx.NewCallable(pos, "WinOnRows", std::move(args));

            auto frames = ctx.Builder(pos)
                .List()
                    .Add(0, winOnRows)
                .Seal()
                .Build();

            TExprNode::TListType keys;
            for (auto p : win->Child(2)->Children()) {
                YQL_ENSURE(p->IsCallable("PgGroup"));
                const auto& member = p->Tail().Tail();
                YQL_ENSURE(member.IsCallable("Member"));
                keys.push_back(member.TailPtr());
            }

            auto keysNode = ctx.NewList(pos, std::move(keys));
            auto sortNode = ctx.NewCallable(pos, "Void", {});
            if (win->Child(3)->ChildrenSize() > 0) {
                sortNode = BuildSortTraits(pos, *win->Child(3), ret, ctx);
            }

            ret = ctx.Builder(pos)
                .Callable("CalcOverWindow")
                    .Add(0, ret)
                    .Add(1, keysNode)
                    .Add(2, sortNode)
                    .Add(3, frames)
                .Seal()
                .Build();
        }

        auto status = OptimizeExpr(projectionLambda, projectionLambda, [&](const TExprNode::TPtr& node, TExprContext& ctx) -> TExprNode::TPtr {
            auto it = winFuncsId.find(node.Get());
            if (it != winFuncsId.end()) {
                auto ret = ctx.Builder(pos)
                    .Callable("Member")
                        .Add(0, projectionLambda->Head().HeadPtr())
                        .Atom(1, "_yql_win_" + ToString(it->second))
                    .Seal()
                    .Build();

                if (node->Head().Content() == "row_number") {
                    ret = ctx.Builder(node->Pos())
                        .Callable("ToPg")
                            .Callable(0, "SafeCast")
                                .Add(0, ret)
                                .Atom(1, "Int64")
                            .Seal()
                        .Seal()
                        .Build();
                }

                return ret;
            }

            return node;
        }, ctx, TOptimizeExprSettings(optCtx.Types));

        if (status.Level == IGraphTransformer::TStatus::Error) {
            return nullptr;
        }
    }

    return ret;
}

TExprNode::TPtr BuildSort(TPositionHandle pos, const TExprNode::TPtr& sort, const TExprNode::TPtr& list, TExprContext& ctx) {
    const auto& keys = sort->Tail();
    auto argNode = ctx.NewArgument(pos, "row");
    auto argsNode = ctx.NewArguments(pos, { argNode });

    TExprNode::TListType dirItems;
    TExprNode::TListType rootItems;
    for (const auto& key : keys.Children()) {
        dirItems.push_back(ctx.Builder(pos)
            .Callable("Bool")
                .Atom(0, key->Tail().Content() == "asc" ? "true" : "false")
            .Seal()
            .Build());

        auto keyLambda = key->ChildPtr(1);
        rootItems.push_back(ctx.ReplaceNode(keyLambda->TailPtr(), keyLambda->Head().Head(), argNode));
    }

    auto root = ctx.NewList(pos, std::move(rootItems));
    auto dir = ctx.NewList(pos, std::move(dirItems));
    auto lambda = ctx.NewLambda(pos, std::move(argsNode), std::move(root));

    return ctx.Builder(pos)
        .Callable("Sort")
            .Add(0, list)
            .Add(1, dir)
            .Add(2, lambda)
        .Seal()
        .Build();
}

TExprNode::TPtr BuildOffset(TPositionHandle pos, const TExprNode::TPtr& offset, const TExprNode::TPtr& list, TExprContext& ctx) {
    return ctx.Builder(pos)
        .Callable("Skip")
            .Add(0, list)
            .Callable(1, "Unwrap")
                .Callable(0, "SafeCast")
                    .Callable(0, "Coalesce")
                        .Callable(0,"FromPg")
                            .Add(0, offset->ChildPtr(1))
                        .Seal()
                        .Callable(1, "Int64")
                            .Atom(0, "0")
                        .Seal()
                    .Seal()
                    .Atom(1, "Uint64")
                .Seal()
                .Callable(1, "String")
                    .Atom(0, "Negative offset")
                .Seal()
            .Seal()
        .Seal()
        .Build();
}

TExprNode::TPtr BuildLimit(TPositionHandle pos, const TExprNode::TPtr& limit, const TExprNode::TPtr& list, TExprContext& ctx) {
    return ctx.Builder(pos)
        .Callable("Take")
            .Add(0, list)
            .Callable(1, "Unwrap")
                .Callable(0, "SafeCast")
                    .Callable(0, "Coalesce")
                        .Callable(0,"FromPg")
                            .Add(0, limit->ChildPtr(1))
                        .Seal()
                        .Callable(1, "Int64")
                            .Atom(0, "9223372036854775807") // 2**63-1
                        .Seal()
                    .Seal()
                    .Atom(1, "Uint64")
                .Seal()
                .Callable(1, "String")
                    .Atom(0, "Negative limit")
                .Seal()
            .Seal()
        .Seal()
        .Build();
}

TExprNode::TPtr ExpandPgSelect(const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
   auto setItems = GetSetting(node->Head(), "set_items");
   auto order = optCtx.Types->LookupColumnOrder(*node);
   YQL_ENSURE(order);
   TExprNode::TListType columnsItems;
   for (const auto& x : *order) {
       columnsItems.push_back(ctx.NewAtom(node->Pos(), x));
   }

   auto columns = ctx.NewList(node->Pos(), std::move(columnsItems));
   TExprNode::TListType setItemNodes;
   TVector<TColumnOrder> columnOrders;
   for (auto setItem : setItems->Tail().Children()) {
       auto childOrder = optCtx.Types->LookupColumnOrder(*setItem);
       YQL_ENSURE(*childOrder);
       columnOrders.push_back(*childOrder);
       auto result = GetSetting(setItem->Tail(), "result");
       auto values = GetSetting(setItem->Tail(), "values");
       auto from = GetSetting(setItem->Tail(), "from");
       auto filter = GetSetting(setItem->Tail(), "where");
       auto joinOps = GetSetting(setItem->Tail(), "join_ops");
       auto groupBy = GetSetting(setItem->Tail(), "group_by");
       auto having = GetSetting(setItem->Tail(), "having");
       auto window = GetSetting(setItem->Tail(), "window");
       bool oneRow = !from;
       TExprNode::TPtr list;
       if (values) {
           YQL_ENSURE(!result);
           list = BuildValues(node->Pos(), values, ctx);
       } else {
           YQL_ENSURE(result);
           TExprNode::TPtr projectionLambda;
           if (oneRow) {
               std::tie(list, projectionLambda) = BuildOneRow(node->Pos(), result, ctx);
           } else {
               // extract all used columns
               auto usedColumns = GatherUsedColumns(result, joinOps);

               // fill index of input for each column
               FillInputIndices(from, usedColumns, optCtx);

               auto cleanedInputs = BuildCleanedColumns(node->Pos(), from, usedColumns, ctx);
               if (cleanedInputs.size() == 1) {
                   list = cleanedInputs.front();
               } else {
                   TVector<ui32> groupForIndex;
                   TExprNode::TListType joinGroups;
                   std::tie(groupForIndex, joinGroups) = BuildJoinGroups(node->Pos(), cleanedInputs, joinOps, ctx);
                   if (joinGroups.size() == 1) {
                       list = joinGroups.front();
                   } else {
                       list = BuildCrossJoinsBetweenGroups(node->Pos(), joinGroups, usedColumns, groupForIndex, ctx);
                   }
               }

               projectionLambda = BuildProjectionLambda(node->Pos(), result, ctx);
           }

           if (filter) {
               list = BuildFilter(node->Pos(), list, filter, ctx);
           }

           TAggs aggs;
           TNodeMap<ui32> aggId;
           std::tie(aggs, aggId) = GatherAggregations(projectionLambda, having);
           if (!aggs.empty() || groupBy) {
               list = BuildGroupByAndHaving(node->Pos(), list, aggs, aggId, groupBy, having, projectionLambda, ctx, optCtx);
           }

           list = BuildWindows(node->Pos(), list, window, projectionLambda, ctx, optCtx);
           list = ctx.Builder(node->Pos())
               .Callable("Map")
                   .Add(0, list)
                   .Add(1, projectionLambda)
               .Seal()
               .Build();
       }

       setItemNodes.push_back(list);
   }

   TExprNode::TPtr list;
   if (setItemNodes.size() == 1) {
       list = setItemNodes.front();
   } else {
       list = ExpandPositionalUnionAll(*node, columnOrders, setItemNodes, ctx, optCtx);
   }

   auto sort = GetSetting(node->Head(), "sort");
   if (sort && sort->Tail().ChildrenSize() > 0) {
       list = BuildSort(node->Pos(), sort, list, ctx);
   }

   auto limit = GetSetting(node->Head(), "limit");
   auto offset = GetSetting(node->Head(), "offset");

   if (offset) {
       list = BuildOffset(node->Pos(), offset, list, ctx);
   }

   if (limit) {
       list = BuildLimit(node->Pos(), limit, list, ctx);
   }

   return ctx.Builder(node->Pos())
       .Callable("AssumeColumnOrder")
           .Add(0, list)
           .Add(1, columns)
       .Seal()
       .Build();
}

TExprNode::TPtr ExpandPgLike(const TExprNode::TPtr& node, TExprContext& ctx, TOptimizeContext& optCtx) {
    Y_UNUSED(optCtx);
    const bool insensitive = node->IsCallable("PgILike");
    auto matcher = ctx.Builder(node->Pos())
        .Callable("Udf")
            .Atom(0, "Re2.Match")
            .List(1)
                .Callable(0, "Apply")
                    .Callable(0, "Udf")
                        .Atom(0, "Re2.PatternFromLike")
                    .Seal()
                    .Callable(1, "Coalesce")
                        .Callable(0, "FromPg")
                            .Add(0, node->ChildPtr(1))
                        .Seal()
                        .Callable(1, "Utf8")
                            .Atom(0, "")
                        .Seal()
                    .Seal()
                .Seal()
                .Callable(1, "NamedApply")
                    .Callable(0, "Udf")
                        .Atom(0, "Re2.Options")
                    .Seal()
                    .List(1)
                    .Seal()
                    .Callable(2, "AsStruct")
                        .List(0)
                            .Atom(0, "CaseSensitive")
                            .Callable(1, "Bool")
                                .Atom(0, insensitive ? "false" : "true")
                            .Seal()
                        .Seal()
                    .Seal()
                .Seal()
            .Seal()
        .Seal()
        .Build();

    return ctx.Builder(node->Pos())
        .Callable("ToPg")
            .Callable(0, "If")
                .Callable(0, "And")
                    .Callable(0, "Exists")
                        .Add(0, node->ChildPtr(0))
                    .Seal()
                    .Callable(1, "Exists")
                        .Add(0, node->ChildPtr(1))
                    .Seal()
                .Seal()
                .Callable(1, "Apply")
                    .Add(0, matcher)
                    .Callable(1, "FromPg")
                        .Add(0, node->ChildPtr(0))
                    .Seal()
                .Seal()
                .Callable(2, "Null")
                .Seal()
            .Seal()
        .Seal()
        .Build();
}

} // namespace NYql
