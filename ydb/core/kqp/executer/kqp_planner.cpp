#include "kqp_executer_stats.h"
#include "kqp_planner.h"
#include "kqp_planner_strategy.h"
#include "kqp_shards_resolver.h"

#include <ydb/core/base/appdata.h>
#include <ydb/core/kqp/rm/kqp_rm.h>
#include <ydb/core/kqp/rm/kqp_resource_estimation.h>

#include <util/generic/set.h>

namespace NKikimr::NKqp {

#define LOG_D(stream) LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::KQP_EXECUTER, "TxId: " << TxId << ". " << stream)
#define LOG_I(stream) LOG_INFO_S(*TlsActivationContext, NKikimrServices::KQP_EXECUTER, "TxId: " << TxId << ". " << stream)
#define LOG_C(stream) LOG_CRIT_S(*TlsActivationContext, NKikimrServices::KQP_EXECUTER, "TxId: " << TxId << ". " << stream)
#define LOG_E(stream) LOG_ERROR_S(*TlsActivationContext, NKikimrServices::KQP_EXECUTER, "TxId: " << TxId << ". " << stream)

using namespace NYql;

// Task can allocate extra memory during execution.
// So, we estimate total memory amount required for task as apriori task size multiplied by this constant.
constexpr ui32 MEMORY_ESTIMATION_OVERFLOW = 2;

TKqpPlanner::TKqpPlanner(ui64 txId, const TActorId& executer, TVector<NDqProto::TDqTask>&& tasks,
    THashMap<ui64, TVector<NDqProto::TDqTask>>&& scanTasks, const IKqpGateway::TKqpSnapshot& snapshot,
    const TString& database, const TMaybe<TString>& userToken, TInstant deadline,
    const Ydb::Table::QueryStatsCollection::Mode& statsMode, bool disableLlvmForUdfStages, bool enableLlvm,
    bool withSpilling, const TMaybe<NKikimrKqp::TRlPath>& rlPath)
    : TxId(txId)
    , ExecuterId(executer)
    , Tasks(std::move(tasks))
    , ScanTasks(std::move(scanTasks))
    , Snapshot(snapshot)
    , Database(database)
    , UserToken(userToken)
    , Deadline(deadline)
    , StatsMode(statsMode)
    , DisableLlvmForUdfStages(disableLlvmForUdfStages)
    , EnableLlvm(enableLlvm)
    , WithSpilling(withSpilling)
    , RlPath(rlPath)
{
    if (!Database) {
        // a piece of magic for tests
        for (auto& x : AppData()->DomainsInfo->DomainByName) {
            Database = TStringBuilder() << '/' << x.first;
            LOG_E("Database not set, use " << Database);
        }
    }
}

void TKqpPlanner::Bootstrap(const TActorContext&) {
    GetKqpResourceManager()->RequestClusterResourcesInfo(
        [as = TlsActivationContext->ActorSystem(), self = SelfId()](TVector<NKikimrKqp::TKqpNodeResources>&& resources) {
            TAutoPtr<IEventHandle> eh = new IEventHandle(self, self, new TEvPrivate::TEvResourcesSnapshot(std::move(resources)));
            as->Send(eh);
        });

    Become(&TKqpPlanner::WaitState);
}

void TKqpPlanner::WaitState(TAutoPtr<IEventHandle>& ev, const TActorContext&) {
    switch (ev->GetTypeRewrite()) {
        hFunc(TEvPrivate::TEvResourcesSnapshot, HandleWait);
        hFunc(TEvKqp::TEvAbortExecution, HandleWait);
        default:
            LOG_C("Unexpected event type: " << ev->GetTypeRewrite() << " at Wait state"
                << ", event: " << (ev->HasEvent() ? ev->GetBase()->ToString().data() : "<serialized>"));
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// Wait State
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void TKqpPlanner::HandleWait(TEvPrivate::TEvResourcesSnapshot::TPtr& ev) {
    if (ev->Get()->Snapshot.empty()) {
        LOG_E("Can not find default state storage group for database " << Database);
        RunLocal(ev->Get()->Snapshot);
        return;
    }

    Process(ev->Get()->Snapshot);
}

void TKqpPlanner::HandleWait(TEvKqp::TEvAbortExecution::TPtr& ev) {
    LOG_E("Terminate KqpPlanner, reason: " << ev->Get()->GetIssues().ToOneLineString());
    PassAway();
}

void TKqpPlanner::Process(const TVector<NKikimrKqp::TKqpNodeResources>& snapshot) {
    auto rmConfig = GetKqpResourceManager()->GetConfig();

    ui32 tasksCount = Tasks.size();
    for (auto& [shardId, tasks] : ScanTasks) {
        tasksCount += tasks.size();
    }

    TVector<TTaskResourceEstimation> est;
    est.resize(tasksCount);

    ui64 localRunMemoryEst = 0;

    ui64 i = 0;
    for (auto& task : Tasks) {
        EstimateTaskResources(task, 0, 0, rmConfig, est[i]);
        localRunMemoryEst += est[i].TotalMemoryLimit;
        i++;
    }
    if (auto it = ScanTasks.find(SelfId().NodeId()); it != ScanTasks.end()) {
        for (auto& task : it->second) {
            EstimateTaskResources(task, 0, 0, rmConfig, est[i]);
            localRunMemoryEst += est[i].TotalMemoryLimit;
            i++;
        }
    }

    auto localResources = GetKqpResourceManager()->GetLocalResources();
    if (localRunMemoryEst * MEMORY_ESTIMATION_OVERFLOW <= localResources.Memory[NRm::EKqpMemoryPool::ScanQuery] &&
        tasksCount <= localResources.ExecutionUnits)
    {
        RunLocal(snapshot);
        return;
    }

    if (snapshot.empty() || (snapshot.size() == 1 && snapshot[0].GetNodeId() == SelfId().NodeId())) {
        // try to run without memory overflow settings
        if (localRunMemoryEst <= localResources.Memory[NRm::EKqpMemoryPool::ScanQuery] &&
            tasksCount <= localResources.ExecutionUnits)
        {
            RunLocal(snapshot);
            return;
        }

        LOG_E("Not enough resources to execute query locally and no information about other nodes");
        auto ev = MakeHolder<TEvKqp::TEvAbortExecution>(NYql::NDqProto::StatusIds::PRECONDITION_FAILED,
            "Not enough resources to execute query locally and no information about other nodes");
        Send(ExecuterId, ev.Release());
        PassAway();
        return;
    }

    auto planner = CreateKqpGreedyPlanner();

    auto ctx = TlsActivationContext->AsActorContext();
    if (ctx.LoggerSettings() && ctx.LoggerSettings()->Satisfies(NActors::NLog::PRI_DEBUG, NKikimrServices::KQP_EXECUTER)) {
        planner->SetLogFunc([TxId = TxId](TStringBuf msg) { LOG_D(msg); });
    }

    THashMap<ui64, size_t> nodeIdtoIdx;
    for (size_t idx = 0; idx < snapshot.size(); ++idx) {
        nodeIdtoIdx[snapshot[idx].nodeid()] = idx;
    }

    auto plan = planner->Plan(snapshot, std::move(est));

    if (!plan.empty()) {
        for (auto& group : plan) {
            auto ev = PrepareKqpNodeRequest(group.TaskIds);
            AddScansToKqpNodeRequest(ev, group.NodeId);

            auto target = MakeKqpNodeServiceID(group.NodeId);
            TlsActivationContext->Send(new IEventHandle(target, ExecuterId, ev.Release(),
                CalcSendMessageFlagsForNode(target.NodeId())));
        }

        TVector<ui64> nodes;
        nodes.reserve(ScanTasks.size());
        for (auto& [nodeId, _]: ScanTasks) {
            nodes.push_back(nodeId);
        }

        for (ui64 nodeId: nodes) {
            auto ev = PrepareKqpNodeRequest({});
            AddScansToKqpNodeRequest(ev, nodeId);

            auto target = MakeKqpNodeServiceID(nodeId);
            TlsActivationContext->Send(new IEventHandle(target, ExecuterId, ev.Release(),
                CalcSendMessageFlagsForNode(target.NodeId())));
        }
        Y_VERIFY(ScanTasks.empty());
    } else {
        auto ev = MakeHolder<TEvKqp::TEvAbortExecution>(NYql::NDqProto::StatusIds::PRECONDITION_FAILED,
            "Not enough resources to execute query");
        Send(ExecuterId, ev.Release());
    }

    PassAway();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// Local Execution
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void TKqpPlanner::RunLocal(const TVector<NKikimrKqp::TKqpNodeResources>& snapshot) {
    LOG_D("Execute query locally");

    auto ev = PrepareKqpNodeRequest({});
    AddScansToKqpNodeRequest(ev, SelfId().NodeId());

    auto target = MakeKqpNodeServiceID(SelfId().NodeId());
    TlsActivationContext->Send(new IEventHandle(target, ExecuterId, ev.Release(), IEventHandle::FlagTrackDelivery));


    TVector<ui64> nodes;
    for (const auto& pair: ScanTasks) {
        nodes.push_back(pair.first);
        YQL_ENSURE(pair.first != SelfId().NodeId());
    }

    THashMap<ui64, size_t> nodeIdToIdx;
    for (size_t idx = 0; idx < snapshot.size(); ++idx) {
        nodeIdToIdx[snapshot[idx].nodeid()] = idx;
        LOG_D("snapshot #" << idx << ": " << snapshot[idx].ShortDebugString());
    }

    for (auto nodeId: nodes) {
        auto ev = PrepareKqpNodeRequest({});
        AddScansToKqpNodeRequest(ev, nodeId);
        auto target = MakeKqpNodeServiceID(nodeId);
        TlsActivationContext->Send(new IEventHandle(target, ExecuterId, ev.Release(),
            CalcSendMessageFlagsForNode(target.NodeId())));
    }
    Y_VERIFY(ScanTasks.size() == 0);

    PassAway();
}

void TKqpPlanner::PassAway() {
    TBase::PassAway();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
THolder<TEvKqpNode::TEvStartKqpTasksRequest> TKqpPlanner::PrepareKqpNodeRequest(const TVector<ui64>& taskIds) {
    auto ev = MakeHolder<TEvKqpNode::TEvStartKqpTasksRequest>();

    ev->Record.SetTxId(TxId);
    ActorIdToProto(ExecuterId, ev->Record.MutableExecuterActorId());

    bool withLLVM = EnableLlvm;

    if (taskIds.empty()) {
        for (auto& taskDesc : Tasks) {
            if (taskDesc.GetId()) {
                if (DisableLlvmForUdfStages && taskDesc.GetProgram().GetSettings().GetHasUdf()) {
                    withLLVM = false;
                }
                ev->Record.AddTasks()->Swap(&taskDesc);
            }
        }
    } else {
        for (auto& taskDesc : Tasks) {
            if (taskDesc.GetId() && Find(taskIds, taskDesc.GetId()) != taskIds.end()) {
                if (DisableLlvmForUdfStages && taskDesc.GetProgram().GetSettings().GetHasUdf()) {
                    withLLVM = false;
                }
                ev->Record.AddTasks()->Swap(&taskDesc);
            }
        }
    }

    if (Deadline) {
        TDuration timeout = Deadline - TAppData::TimeProvider->Now();
        ev->Record.MutableRuntimeSettings()->SetTimeoutMs(timeout.MilliSeconds());
    }

    ev->Record.MutableRuntimeSettings()->SetExecType(NDqProto::TComputeRuntimeSettings::SCAN);
    ev->Record.MutableRuntimeSettings()->SetStatsMode(GetDqStatsMode(StatsMode));
    ev->Record.MutableRuntimeSettings()->SetUseLLVM(withLLVM);
    ev->Record.MutableRuntimeSettings()->SetUseSpilling(WithSpilling);

    if (RlPath) {
        auto rlPath = ev->Record.MutableRuntimeSettings()->MutableRlPath();
        rlPath->SetCoordinationNode(RlPath->GetCoordinationNode());
        rlPath->SetResourcePath(RlPath->GetResourcePath());
        rlPath->SetDatabase(Database);
        if (UserToken)
            rlPath->SetToken(UserToken.GetRef());
    }

    ev->Record.SetStartAllOrFail(true);

    return ev;
}

void TKqpPlanner::AddScansToKqpNodeRequest(THolder<TEvKqpNode::TEvStartKqpTasksRequest>& ev, ui64 nodeId) {
    if (!Snapshot.IsValid()) {
        Y_ASSERT(ScanTasks.size() == 0);
        return;
    }

    bool withLLVM = true;
    if (auto nodeTasks = ScanTasks.FindPtr(nodeId)) {
        LOG_D("Adding " << nodeTasks->size() << " scans to KqpNode request");

        ev->Record.MutableSnapshot()->SetTxId(Snapshot.TxId);
        ev->Record.MutableSnapshot()->SetStep(Snapshot.Step);

        for (auto& task: *nodeTasks) {
            if (DisableLlvmForUdfStages && task.GetProgram().GetSettings().GetHasUdf()) {
                withLLVM = false;
            }
            ev->Record.AddTasks()->Swap(&task);
        }
        ScanTasks.erase(nodeId);
    }

    if (ev->Record.GetRuntimeSettings().GetUseLLVM()) {
        ev->Record.MutableRuntimeSettings()->SetUseLLVM(withLLVM);
    }
}

ui32 TKqpPlanner::CalcSendMessageFlagsForNode(ui32 nodeId) {
    ui32 flags = IEventHandle::FlagTrackDelivery;
    if (TrackingNodes.insert(nodeId).second) {
        flags |= IEventHandle::FlagSubscribeOnSession;
    }
    return flags;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
IActor* CreateKqpPlanner(ui64 txId, const TActorId& executer, TVector<NDqProto::TDqTask>&& tasks,
    THashMap<ui64, TVector<NDqProto::TDqTask>>&& scanTasks, const IKqpGateway::TKqpSnapshot& snapshot,
    const TString& database, const TMaybe<TString>& token, TInstant deadline,
    const Ydb::Table::QueryStatsCollection::Mode& statsMode, bool disableLlvmForUdfStages, bool enableLlvm,
    bool withSpilling, const TMaybe<NKikimrKqp::TRlPath>& rlPath)
{
    return new TKqpPlanner(txId, executer, std::move(tasks), std::move(scanTasks), snapshot,
        database, token, deadline, statsMode, disableLlvmForUdfStages, enableLlvm, withSpilling, rlPath);
}

} // namespace NKikimr::NKqp
