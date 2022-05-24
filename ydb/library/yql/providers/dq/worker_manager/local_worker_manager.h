#pragma once

#include <ydb/library/yql/dq/actors/compute/dq_compute_actor_sources.h>
#include <ydb/library/yql/dq/actors/compute/dq_compute_actor_async_output.h>
#include <ydb/library/yql/minikql/mkql_function_registry.h>
#include <ydb/library/yql/providers/dq/worker_manager/interface/events.h>
#include <ydb/library/yql/providers/dq/worker_manager/interface/counters.h>

#include <ydb/library/yql/providers/dq/task_runner/tasks_runner_proxy.h>
#include <ydb/library/yql/providers/dq/task_runner/task_runner_invoker.h>

#include <ydb/library/yql/providers/dq/task_runner_actor/task_runner_actor.h>

namespace NYql {

struct TWorkerRuntimeData;

}

namespace NYql::NDqs {

    struct TLocalWorkerManagerOptions {
        TWorkerManagerCounters Counters;
        NTaskRunnerProxy::IProxyFactory::TPtr Factory;
        NDq::IDqSourceFactory::TPtr SourceFactory;
        NDq::IDqSinkFactory::TPtr SinkFactory;
        NDq::IDqOutputTransformFactory::TPtr TransformFactory;
        const NKikimr::NMiniKQL::IFunctionRegistry* FunctionRegistry = nullptr;
        TWorkerRuntimeData* RuntimeData = nullptr;
        TTaskRunnerInvokerFactory::TPtr TaskRunnerInvokerFactory;
        NDq::NTaskRunnerActor::ITaskRunnerActorFactory::TPtr TaskRunnerActorFactory;
        THashMap<TString, TString> ClusterNamesMapping;

        ui64 MkqlInitialMemoryLimit = 8_GB;
        ui64 MkqlTotalMemoryLimit = 0;
        ui64 MkqlMinAllocSize = 30_MB;

        bool CanUseComputeActor = true;
    };

    NActors::IActor* CreateLocalWorkerManager(const TLocalWorkerManagerOptions& options);

} // namespace NYql
