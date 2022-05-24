#pragma once

#include "private_client.h"

#include <library/cpp/actors/core/actor.h>
#include <library/cpp/actors/core/event_local.h>

#include <library/cpp/monlib/dynamic_counters/counters.h>
#include <ydb/core/yq/libs/events/event_subspace.h>
#include <ydb/public/sdk/cpp/client/ydb_table/table.h>

#include <ydb/core/yq/libs/shared_resources/shared_resources.h>

#include <ydb/core/yq/libs/control_plane_storage/proto/yq_internal.pb.h>

namespace NYq {

struct TEvInternalService {
    // Event ids.
    enum EEv : ui32 {
        EvHealthCheckRequest = YqEventSubspaceBegin(NYq::TYqEventSubspace::InternalService),
        EvHealthCheckResponse,
        EvGetTaskRequest,
        EvGetTaskResponse,
        EvPingTaskRequest,
        EvPingTaskResponse,
        EvWriteResultRequest,
        EvWriteResultResponse,
        EvEnd,
    };

    static_assert(EvEnd <= YqEventSubspaceEnd(NYq::TYqEventSubspace::InternalService), "All events must be in their subspace");

    template <class TProtoRequest, ui32 TEventType>
    struct TInternalServiceRequestEvent : public NActors::TEventLocal<TInternalServiceRequestEvent<TProtoRequest, TEventType>, TEventType> {
        TProtoRequest Request;
        TInstant SentAt;
        explicit TInternalServiceRequestEvent(const TProtoRequest& request) 
            : Request(request), SentAt(Now())
        { }
    };

    using TEvHealthCheckRequest = TInternalServiceRequestEvent<Yq::Private::NodesHealthCheckRequest, EvHealthCheckRequest>;
    using TEvGetTaskRequest = TInternalServiceRequestEvent<Yq::Private::GetTaskRequest, EvGetTaskRequest>;
    using TEvPingTaskRequest = TInternalServiceRequestEvent<Yq::Private::PingTaskRequest, EvPingTaskRequest>;
    using TEvWriteResultRequest = TInternalServiceRequestEvent<Yq::Private::WriteTaskResultRequest, EvWriteResultRequest>;

    template <class TProtoResult, ui32 TEventType>
    struct TInternalServiceResponseEvent : public NActors::TEventLocal<TInternalServiceResponseEvent<TProtoResult, TEventType>, TEventType> {
        NYdb::TStatus Status;
        TProtoResult Result;
        explicit TInternalServiceResponseEvent(const TProtoResultInternalWrapper<TProtoResult>& wrappedResult) : Status(wrappedResult) { 
            if (wrappedResult.IsResultSet()) {
                Result = wrappedResult.GetResult();
            }
        }
        explicit TInternalServiceResponseEvent(const TString& errorMessage) : Status(NYdb::EStatus::INTERNAL_ERROR, {NYql::TIssue(errorMessage).SetCode(NYql::UNEXPECTED_ERROR, NYql::TSeverityIds::S_ERROR)}) { 
        }
        explicit TInternalServiceResponseEvent(const TProtoResult& result) : Status(NYdb::EStatus::SUCCESS, NYql::TIssues()), Result(result) {
        }
        TInternalServiceResponseEvent(NYdb::EStatus statusCode, NYql::TIssues&& issues) : Status(statusCode, std::move(issues)) {
        }
    };

    using TEvHealthCheckResponse = TInternalServiceResponseEvent<Yq::Private::NodesHealthCheckResult, EvHealthCheckResponse>;
    using TEvGetTaskResponse = TInternalServiceResponseEvent<Yq::Private::GetTaskResult, EvGetTaskResponse>;
    using TEvPingTaskResponse = TInternalServiceResponseEvent<Yq::Private::PingTaskResult, EvPingTaskResponse>;
    using TEvWriteResultResponse = TInternalServiceResponseEvent<Yq::Private::WriteTaskResultResult, EvWriteResultResponse>;
};

NActors::TActorId MakeInternalServiceActorId();

} /* NYq */
