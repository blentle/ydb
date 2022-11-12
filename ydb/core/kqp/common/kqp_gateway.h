#pragma once

#include <ydb/core/protos/kqp_physical.pb.h>
#include <ydb/core/protos/tx_proxy.pb.h>

#include <ydb/library/yql/ast/yql_expr.h>
#include <ydb/library/yql/dq/common/dq_value.h>
#include <ydb/core/kqp/provider/yql_kikimr_gateway.h>
#include <ydb/core/tx/long_tx_service/public/lock_handle.h>

#include <library/cpp/actors/wilson/wilson_trace.h>
#include <library/cpp/actors/core/actorid.h>
#include <library/cpp/lwtrace/shuttle.h>

#include <ydb/core/kqp/common/kqp_topic.h>

namespace NKikimr {
namespace NKqp {

struct TKqpParamsMap {
    TKqpParamsMap() {}

    TKqpParamsMap(std::shared_ptr<void> owner)
        : Owner(owner) {}

    std::shared_ptr<void> Owner;
    TMap<TString, NYql::NDq::TMkqlValueRef> Values;
};

class IKqpGateway : public NYql::IKikimrGateway {
public:
    struct TPhysicalTxData : private TMoveOnly {
        std::shared_ptr<const NKqpProto::TKqpPhyTx> Body;
        TKqpParamsMap Params;

        TPhysicalTxData(std::shared_ptr<const NKqpProto::TKqpPhyTx> body, TKqpParamsMap&& params)
            : Body(std::move(body))
            , Params(std::move(params)) {}
    };

    struct TKqpSnapshot {
        ui64 Step;
        ui64 TxId;

        constexpr TKqpSnapshot()
            : Step(0)
            , TxId(0)
        {}

        TKqpSnapshot(ui64 step, ui64 txId)
            : Step(step)
            , TxId(txId)
        {}

        bool IsValid() const {
            return Step != 0 || TxId != 0;
        }

        bool operator ==(const TKqpSnapshot &snapshot) const {
            return snapshot.Step == Step && snapshot.TxId == TxId;
        }

        size_t GetHash() const noexcept {
            auto tuple = std::make_tuple(Step, TxId);
            return THash<decltype(tuple)>()(tuple);
        }

        static const TKqpSnapshot InvalidSnapshot;
    };

    struct TKqpSnapshotHandle : public IKqpGateway::TGenericResult {
        TKqpSnapshot Snapshot;
        NActors::TActorId ManagingActor;
        NKikimrIssues::TStatusIds::EStatusCode Status =  NKikimrIssues::TStatusIds::UNKNOWN;
    };

    struct TExecPhysicalRequest : private TMoveOnly {
        TVector<TPhysicalTxData> Transactions;
        TVector<NYql::NDq::TMkqlValueRef> Locks;
        bool ValidateLocks = false;
        bool EraseLocks = false;
        TMaybe<ui64> AcquireLocksTxId;
        TDuration Timeout;
        TMaybe<TDuration> CancelAfter;
        ui32 MaxComputeActors = 10'000;
        ui32 MaxAffectedShards = 0;
        ui64 TotalReadSizeLimitBytes = 0;
        ui64 MkqlMemoryLimit = 0; // old engine compatibility
        ui64 PerShardKeysSizeLimitBytes = 0;
        Ydb::Table::QueryStatsCollection::Mode StatsMode = Ydb::Table::QueryStatsCollection::STATS_COLLECTION_NONE;
        bool DisableLlvmForUdfStages = false;
        bool LlvmEnabled = true;
        TKqpSnapshot Snapshot = TKqpSnapshot();
        NKikimrKqp::EIsolationLevel IsolationLevel = NKikimrKqp::ISOLATION_LEVEL_UNDEFINED;
        TMaybe<NKikimrKqp::TRlPath> RlPath;
        bool NeedTxId = true;

        NLWTrace::TOrbit Orbit;
        NWilson::TTraceId TraceId;

        NTopic::TTopicOperations TopicOperations;
    };

    struct TExecPhysicalResult : public TGenericResult {
        NKikimrKqp::TExecuterTxResult ExecuterResult;
        NLongTxService::TLockHandle LockHandle;
    };

    struct TAstQuerySettings {
        Ydb::Table::QueryStatsCollection::Mode CollectStats = Ydb::Table::QueryStatsCollection::STATS_COLLECTION_NONE;
    };

public:
    /* Compute */
    virtual NThreading::TFuture<TExecPhysicalResult> ExecutePure(TExecPhysicalRequest&& request) = 0;

    /* Scripting */
    virtual NThreading::TFuture<TQueryResult> ExplainDataQueryAst(const TString& cluster, const TString& query) = 0;

    virtual NThreading::TFuture<TQueryResult> ExecDataQueryAst(const TString& cluster, const TString& query,
        TKqpParamsMap&& params, const TAstQuerySettings& settings,
        const Ydb::Table::TransactionSettings& txSettings) = 0;

    virtual NThreading::TFuture<TQueryResult> ExplainScanQueryAst(const TString& cluster, const TString& query) = 0;

    virtual NThreading::TFuture<TQueryResult> ExecScanQueryAst(const TString& cluster, const TString& query,
        TKqpParamsMap&& params, const TAstQuerySettings& settings, ui64 rowsLimit) = 0;

    virtual NThreading::TFuture<TQueryResult> StreamExecDataQueryAst(const TString& cluster, const TString& query,
        TKqpParamsMap&& params, const TAstQuerySettings& settings,
        const Ydb::Table::TransactionSettings& txSettings, const NActors::TActorId& target) = 0;

    virtual NThreading::TFuture<TQueryResult> StreamExecScanQueryAst(const TString& cluster, const TString& query,
        TKqpParamsMap&& params, const TAstQuerySettings& settings, const NActors::TActorId& target) = 0;
};

} // namespace NKqp
} // namespace NKikimr

template<>
struct THash<NKikimr::NKqp::IKqpGateway::TKqpSnapshot> {
    inline size_t operator()(const NKikimr::NKqp::IKqpGateway::TKqpSnapshot& snapshot) const {
        return snapshot.GetHash();
    }
};
