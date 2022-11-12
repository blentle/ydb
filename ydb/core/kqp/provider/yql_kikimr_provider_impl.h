#pragma once

#include "yql_kikimr_provider.h"

#include <ydb/core/kqp/provider/yql_kikimr_expr_nodes.h>
#include <ydb/core/kqp/provider/yql_kikimr_results.h>

#include <ydb/library/yql/providers/common/provider/yql_provider.h>
#include <ydb/library/yql/providers/common/provider/yql_table_lookup.h>


namespace NYql {

class TKiSourceVisitorTransformer: public TSyncTransformerBase {
public:
    TStatus DoTransform(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) final;

private:
    virtual TStatus HandleKiRead(NNodes::TKiReadBase node, TExprContext& ctx) = 0;
    virtual TStatus HandleRead(NNodes::TExprBase node, TExprContext& ctx) = 0;
    virtual TStatus HandleLength(NNodes::TExprBase node, TExprContext& ctx) = 0;
    virtual TStatus HandleConfigure(NNodes::TExprBase node, TExprContext& ctx) = 0;
};

class TKiSinkVisitorTransformer : public TSyncTransformerBase {
public:
    TStatus DoTransform(TExprNode::TPtr input, TExprNode::TPtr& output, TExprContext& ctx) final;

private:
    virtual TStatus HandleWriteTable(NNodes::TKiWriteTable node, TExprContext& ctx) = 0;
    virtual TStatus HandleUpdateTable(NNodes::TKiUpdateTable node, TExprContext& ctx) = 0;
    virtual TStatus HandleDeleteTable(NNodes::TKiDeleteTable node, TExprContext& ctx) = 0;
    virtual TStatus HandleCreateTable(NNodes::TKiCreateTable node, TExprContext& ctx) = 0;
    virtual TStatus HandleAlterTable(NNodes::TKiAlterTable node, TExprContext& ctx) = 0;
    virtual TStatus HandleDropTable(NNodes::TKiDropTable node, TExprContext& ctx) = 0;
    virtual TStatus HandleCreateUser(NNodes::TKiCreateUser node, TExprContext& ctx) = 0;
    virtual TStatus HandleAlterUser(NNodes::TKiAlterUser node, TExprContext& ctx) = 0;
    virtual TStatus HandleDropUser(NNodes::TKiDropUser node, TExprContext& ctx) = 0;
    virtual TStatus HandleCreateGroup(NNodes::TKiCreateGroup node, TExprContext& ctx) = 0;
    virtual TStatus HandleAlterGroup(NNodes::TKiAlterGroup node, TExprContext& ctx) = 0;
    virtual TStatus HandleDropGroup(NNodes::TKiDropGroup node, TExprContext& ctx) = 0;
    virtual TStatus HandleWrite(NNodes::TExprBase node, TExprContext& ctx) = 0;
    virtual TStatus HandleCommit(NNodes::TCoCommit node, TExprContext& ctx) = 0;
    virtual TStatus HandleKql(NNodes::TCallable node, TExprContext& ctx) = 0;
    virtual TStatus HandleExecDataQuery(NNodes::TKiExecDataQuery node, TExprContext& ctx) = 0;
    virtual TStatus HandleDataQuery(NNodes::TKiDataQuery node, TExprContext& ctx) = 0;
    virtual TStatus HandleEffects(NNodes::TKiEffects node, TExprContext& ctx) = 0;
};

class TKikimrKey {
public:
    enum class Type {
        Table,
        TableList,
        TableScheme,
        Role
    };

public:
    TKikimrKey(TExprContext& ctx)
        : Ctx(ctx) {}

    Type GetKeyType() const {
        Y_VERIFY_DEBUG(KeyType.Defined());
        return *KeyType;
    }

    TString GetTablePath() const {
        Y_VERIFY_DEBUG(KeyType.Defined());
        Y_VERIFY_DEBUG(KeyType == Type::Table || KeyType == Type::TableScheme);
        return Target;
    }

    TString GetFolderPath() const {
        Y_VERIFY_DEBUG(KeyType.Defined());
        Y_VERIFY_DEBUG(KeyType == Type::TableList);
        return Target;
    }

    TString GetRoleName() const {
        Y_VERIFY_DEBUG(KeyType.Defined());
        Y_VERIFY_DEBUG(KeyType == Type::Role);
        return Target;
    }

    const TMaybe<TString>& GetView() const {
        return View;
    }

    bool Extract(const TExprNode& key);

private:
    TExprContext& Ctx;
    TMaybe<Type> KeyType;
    TString Target;
    TMaybe<TString> View;
};

struct TKiExecDataQuerySettings {
    TMaybe<TString> Mode;
    TVector<NNodes::TCoNameValueTuple> Other;

    NNodes::TCoNameValueTupleList BuildNode(TExprContext& ctx, TPositionHandle pos) const;

    static TKiExecDataQuerySettings Parse(NNodes::TKiExecDataQuery exec);
};

template<typename TResult>
class TKikimrFutureResult : public IKikimrAsyncResult<TResult> {
public:
    TKikimrFutureResult(const NThreading::TFuture<TResult>& future, TExprContext& ctx)
        : Future(future)
        , ExprCtx(ctx)
        , Completed(false) {}

    bool HasResult() const override {
        if (Completed) {
            YQL_ENSURE(ExtractedResult.has_value());
        }
        return Completed;
    }

    TResult GetResult() override {
        YQL_ENSURE(Completed);
        if (ExtractedResult) {
            return std::move(*ExtractedResult);
        }
        return std::move(Future.ExtractValue());
    }

    NThreading::TFuture<bool> Continue() override {
        if (Completed) {
            return NThreading::MakeFuture(true);
        }

        if (Future.HasValue()) {
            ExtractedResult.emplace(std::move(Future.ExtractValue()));
            ExtractedResult->ReportIssues(ExprCtx.IssueManager);

            Completed = true;
            return NThreading::MakeFuture(true);
        }

        return Future.Apply([](const NThreading::TFuture<TResult>& future) {
            YQL_ENSURE(future.HasValue());
            return false;
        });
    }

private:
    NThreading::TFuture<TResult> Future;
    std::optional<TResult> ExtractedResult;
    TExprContext& ExprCtx;
    bool Completed;
};

TAutoPtr<IGraphTransformer> CreateKiSourceTypeAnnotationTransformer(TIntrusivePtr<TKikimrSessionContext> sessionCtx,
    TTypeAnnotationContext& types);
TAutoPtr<IGraphTransformer> CreateKiSinkTypeAnnotationTransformer(TIntrusivePtr<IKikimrGateway> gateway,
    TIntrusivePtr<TKikimrSessionContext> sessionCtx);
TAutoPtr<IGraphTransformer> CreateKiLogicalOptProposalTransformer(TIntrusivePtr<TKikimrSessionContext> sessionCtx,
    TTypeAnnotationContext& types);
TAutoPtr<IGraphTransformer> CreateKiPhysicalOptProposalTransformer(TIntrusivePtr<TKikimrSessionContext> sessionCtx);
TAutoPtr<IGraphTransformer> CreateKiSourceLoadTableMetadataTransformer(TIntrusivePtr<IKikimrGateway> gateway,
    TIntrusivePtr<TKikimrSessionContext> sessionCtx);
TAutoPtr<IGraphTransformer> CreateKiSinkIntentDeterminationTransformer(TIntrusivePtr<TKikimrSessionContext> sessionCtx);

TAutoPtr<IGraphTransformer> CreateKiSourceCallableExecutionTransformer(
    TIntrusivePtr<IKikimrGateway> gateway,
    TIntrusivePtr<TKikimrSessionContext> sessionCtx);

TAutoPtr<IGraphTransformer> CreateKiSinkCallableExecutionTransformer(
    TIntrusivePtr<IKikimrGateway> gateway,
    TIntrusivePtr<TKikimrSessionContext> sessionCtx,
    TIntrusivePtr<IKikimrQueryExecutor> queryExecutor);

TAutoPtr<IGraphTransformer> CreateKiSinkPlanInfoTransformer(TIntrusivePtr<IKikimrQueryExecutor> queryExecutor);

NNodes::TCoAtomList BuildColumnsList(const TKikimrTableDescription& table, TPositionHandle pos,
    TExprContext& ctx, bool withSystemColumns);

const TTypeAnnotationNode* GetReadTableRowType(TExprContext& ctx, const TKikimrTablesData& tablesData,
    const TString& cluster, const TString& table, NNodes::TCoAtomList select, bool withSystemColumns = false);

NKikimrKqp::EIsolationLevel GetIsolationLevel(const TMaybe<TString>& isolationLevel);
TMaybe<TString> GetIsolationLevel(const NKikimrKqp::EIsolationLevel& isolationLevel);

TYdbOperation GetTableOp(const NNodes::TKiWriteTable& write);
TVector<NKqpProto::TKqpTableOp> TableOperationsToProto(const NNodes::TCoNameValueTupleList& operations,
    TExprContext& ctx);
TVector<NKqpProto::TKqpTableOp> TableOperationsToProto(const NNodes::TKiOperationList& operations, TExprContext& ctx);

void TableDescriptionToTableInfo(const TKikimrTableDescription& desc, TYdbOperation op,
    NProtoBuf::RepeatedPtrField<NKqpProto::TKqpTableInfo>& infos);
void TableDescriptionToTableInfo(const TKikimrTableDescription& desc, TYdbOperation op,
    TVector<NKqpProto::TKqpTableInfo>& infos);

// Optimizer rules
TExprNode::TPtr KiBuildQuery(NNodes::TExprBase node, TExprContext& ctx);
TExprNode::TPtr KiBuildResult(NNodes::TExprBase node,  const TString& cluster, TExprContext& ctx);

const THashSet<TStringBuf>& KikimrDataSourceFunctions();
const THashSet<TStringBuf>& KikimrDataSinkFunctions();
const THashSet<TStringBuf>& KikimrKqlFunctions();
const THashSet<TStringBuf>& KikimrSupportedEffects();

const THashSet<TStringBuf>& KikimrCommitModes();
const TStringBuf& KikimrCommitModeFlush();
const TStringBuf& KikimrCommitModeRollback();
const TStringBuf& KikimrCommitModeScheme();

const TMap<TString, NKikimr::NUdf::EDataSlot>& KikimrSystemColumns();
bool IsKikimrSystemColumn(const TStringBuf columnName);

bool ValidateTableHasIndex(TKikimrTableMetadataPtr metadata, TExprContext& ctx, const TPositionHandle& pos);

} // namespace NYql
