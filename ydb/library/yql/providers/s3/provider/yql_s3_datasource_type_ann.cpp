#include "yql_s3_provider_impl.h"

#include <ydb/library/yql/core/expr_nodes/yql_expr_nodes.h>
#include <ydb/library/yql/providers/s3/expr_nodes/yql_s3_expr_nodes.h>

#include <ydb/library/yql/providers/common/provider/yql_provider.h>
#include <ydb/library/yql/providers/common/provider/yql_provider_names.h>
#include <ydb/library/yql/providers/common/provider/yql_data_provider_impl.h>

#include <ydb/library/yql/utils/log/log.h>

namespace NYql {

using namespace NNodes;

namespace {

TStringBuf GetCompression(const TExprNode& settings) {
    for (auto i = 0U; i < settings.ChildrenSize(); ++i) {
        const auto& child = *settings.Child(i);
        if (child.Head().IsAtom("compression") && child.Tail().IsCallable({"String", "Utf8"}))
            if (const auto& comp = child.Tail().Head().Content(); !comp.empty())
                return comp;
    }
    return ""sv;
}

class TS3DataSourceTypeAnnotationTransformer : public TVisitorTransformerBase {
public:
    TS3DataSourceTypeAnnotationTransformer(TS3State::TPtr state)
        : TVisitorTransformerBase(true)
        , State_(state)
    {
        using TSelf = TS3DataSourceTypeAnnotationTransformer;
        AddHandler({TS3ReadObject::CallableName()}, Hndl(&TSelf::HandleRead));
        AddHandler({TS3Object::CallableName()}, Hndl(&TSelf::HandleObject));
        AddHandler({TS3SourceSettings::CallableName()}, Hndl(&TSelf::HandleS3SourceSettings));
        AddHandler({TS3ParseSettings::CallableName()}, Hndl(&TSelf::HandleS3ParseSettings));
        AddHandler({TCoConfigure::CallableName()}, Hndl(&TSelf::HandleConfig));
    }

    TStatus HandleS3SourceSettings(const TExprNode::TPtr& input, TExprContext& ctx) {
        if (!EnsureArgsCount(*input, 2U, ctx)) {
            return TStatus::Error;
        }

        if (!EnsureTuple(*input->Child(TS3SourceSettings::idx_Paths), ctx)) {
            return TStatus::Error;
        }

        if (!TCoSecureParam::Match(input->Child(TS3SourceSettings::idx_Token))) {
            ctx.AddError(TIssue(ctx.GetPosition(input->Child(TS3SourceSettings::idx_Token)->Pos()), TStringBuilder() << "Expected " << TCoSecureParam::CallableName()));
            return TStatus::Error;
        }

        input->SetTypeAnn(ctx.MakeType<TStreamExprType>(ctx.MakeType<TDataExprType>(EDataSlot::String)));
        return TStatus::Ok;
    }

    TStatus HandleS3ParseSettings(const TExprNode::TPtr& input, TExprContext& ctx) {
        if (!EnsureMinMaxArgsCount(*input, 4U, 5U, ctx)) {
            return TStatus::Error;
        }

        if (!EnsureTuple(*input->Child(TS3ParseSettings::idx_Paths), ctx)) {
            return TStatus::Error;
        }

        if (!TCoSecureParam::Match(input->Child(TS3ParseSettings::idx_Token))) {
            ctx.AddError(TIssue(ctx.GetPosition(input->Child(TS3ParseSettings::idx_Token)->Pos()), TStringBuilder() << "Expected " << TCoSecureParam::CallableName()));
            return TStatus::Error;
        }

        if (!EnsureAtom(*input->Child(TS3ParseSettings::idx_Format), ctx)) {
            return TStatus::Error;
        }

        const auto type = input->Child(TS3ParseSettings::idx_RowType)->GetTypeAnn()->Cast<TTypeExprType>()->GetType();
        if (!EnsureStructType(input->Child(TS3ParseSettings::idx_RowType)->Pos(), *type, ctx)) {
            return IGraphTransformer::TStatus::Error;
        }

        if (input->ChildrenSize() > TS3ParseSettings::idx_Settings && !EnsureTuple(*input->Child(TS3ParseSettings::idx_Settings), ctx)) {
            return TStatus::Error;
        }

        input->SetTypeAnn(ctx.MakeType<TStreamExprType>(ctx.MakeType<TResourceExprType>("ClickHouseClient.Block")));
        return TStatus::Ok;
    }

    TStatus HandleRead(const TExprNode::TPtr& input, TExprContext& ctx) {
        if (!EnsureMinMaxArgsCount(*input, 4U, 5U, ctx)) {
            return TStatus::Error;
        }

        if (!EnsureWorldType(*input->Child(TS3ReadObject::idx_World), ctx)) {
            return TStatus::Error;
        }

        if (!EnsureSpecificDataSource(*input->Child(TS3ReadObject::idx_DataSource), S3ProviderName, ctx)) {
            return TStatus::Error;
        }

        if (TS3ReadObject::Match(input->Child(TS3ReadObject::idx_Object))) {
            ctx.AddError(TIssue(ctx.GetPosition(input->Child(TS3ReadObject::idx_Object)->Pos()), "Expected S3 object."));
            return TStatus::Error;
        }

        if (!EnsureType(*input->Child(TS3ReadObject::idx_RowType), ctx)) {
            return TStatus::Error;
        }

        const auto itemType = input->Child(TS3ReadObject::idx_RowType)->GetTypeAnn()->Cast<TTypeExprType>()->GetType();
        input->SetTypeAnn(ctx.MakeType<TTupleExprType>(TTypeAnnotationNode::TListType{
            input->Child(TS3ReadObject::idx_World)->GetTypeAnn(),
            ctx.MakeType<TListExprType>(itemType)
        }));

        if (input->ChildrenSize() > TS3ReadObject::idx_ColumnOrder) {
            const auto& order = *input->Child(TS3ReadObject::idx_ColumnOrder);
            if (!EnsureTupleOfAtoms(order, ctx)) {
                return TStatus::Error;
            }
            TVector<TString> columnOrder;
            columnOrder.reserve(order.ChildrenSize());
            order.ForEachChild([&columnOrder](const TExprNode& child) { columnOrder.emplace_back(child.Content()); });
            return State_->Types->SetColumnOrder(*input, columnOrder, ctx);
        }

        return TStatus::Ok;
    }

    TStatus HandleConfig(const TExprNode::TPtr& input, TExprContext& ctx) {
        if (!EnsureMinArgsCount(*input, 2, ctx)) {
            return TStatus::Error;
        }

        if (!EnsureWorldType(*input->Child(TCoConfigure::idx_World), ctx)) {
            return TStatus::Error;
        }

        if (!EnsureSpecificDataSource(*input->Child(TCoConfigure::idx_DataSource), S3ProviderName, ctx)) {
            return TStatus::Error;
        }

        input->SetTypeAnn(input->Child(TCoConfigure::idx_World)->GetTypeAnn());
        return TStatus::Ok;
    }

    TStatus HandleObject(const TExprNode::TPtr& input, TExprContext& ctx) {
        if (!EnsureMinMaxArgsCount(*input, 2U, 3U, ctx)) {
            return TStatus::Error;
        }

        if (!EnsureTuple(*input->Child(TS3Object::idx_Paths), ctx)) {
            return TStatus::Error;
        }

        if (!EnsureAtom(*input->Child(TS3Object::idx_Format), ctx) || !NCommon::ValidateFormat(input->Child(TS3Object::idx_Format)->Content(), ctx)) {
            return TStatus::Error;
        }

        if (input->ChildrenSize() > TS3Object::idx_Settings && !EnsureTuple(*input->Child(TS3Object::idx_Settings), ctx)) {
            return TStatus::Error;
        }

        const auto compression = GetCompression(*input->Child(TS3Object::idx_Settings));
        if (!NCommon::ValidateCompression(compression, ctx)) {
            return TStatus::Error;
        }

        input->SetTypeAnn(ctx.MakeType<TUnitExprType>());
        return TStatus::Ok;
    }
private:
    const TS3State::TPtr State_;
};

}

THolder<TVisitorTransformerBase> CreateS3DataSourceTypeAnnotationTransformer(TS3State::TPtr state) {
    return MakeHolder<TS3DataSourceTypeAnnotationTransformer>(state);
}

} // namespace NYql
