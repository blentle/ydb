#pragma once

#include <library/cpp/monlib/dynamic_counters/counters.h>

#include <ydb/public/sdk/cpp/client/ydb_table/table.h>
#include <ydb/public/api/grpc/draft/yql_db_v1.grpc.pb.h>

namespace NYq {

template<class TProtoResult>
class TProtoResultInternalWrapper : public NYdb::TStatus {
    friend class TPrivateClient;

public:
    TProtoResultInternalWrapper(
            NYdb::TStatus&& status,
            std::unique_ptr<TProtoResult> result)
        : TStatus(std::move(status))
        , Result(std::move(result))
    { }

public:
    const TProtoResult& GetResult() const {
        Y_VERIFY(Result, "Uninitialized result");
        return *Result;
    }

    bool IsResultSet() const {
        return Result ? true : false;
    }

private:
    std::unique_ptr<TProtoResult> Result;
};


using TGetTaskResult = TProtoResultInternalWrapper<Yq::Private::GetTaskResult>;
using TPingTaskResult = TProtoResultInternalWrapper<Yq::Private::PingTaskResult>;
using TWriteTaskResult = TProtoResultInternalWrapper<Yq::Private::WriteTaskResultResult>;
using TNodesHealthCheckResult = TProtoResultInternalWrapper<Yq::Private::NodesHealthCheckResult>;

using TAsyncGetTaskResult = NThreading::TFuture<TGetTaskResult>;
using TAsyncPingTaskResult = NThreading::TFuture<TPingTaskResult>;
using TAsyncWriteTaskResult = NThreading::TFuture<TWriteTaskResult>;
using TAsyncNodesHealthCheckResult = NThreading::TFuture<TNodesHealthCheckResult>;

struct TGetTaskSettings : public NYdb::TOperationRequestSettings<TGetTaskSettings> {};
struct TPingTaskSettings : public NYdb::TOperationRequestSettings<TPingTaskSettings> {};
struct TWriteTaskResultSettings : public NYdb::TOperationRequestSettings<TWriteTaskResultSettings> {};
struct TNodesHealthCheckSettings : public NYdb::TOperationRequestSettings<TNodesHealthCheckSettings> {};

class TPrivateClient {
    class TImpl;

public:
    TPrivateClient(
        const NYdb::TDriver& driver,
        const NYdb::TCommonClientSettings& settings = NYdb::TCommonClientSettings(),
        const ::NMonitoring::TDynamicCounterPtr& counters = MakeIntrusive<::NMonitoring::TDynamicCounters>());

    TAsyncGetTaskResult GetTask(
        Yq::Private::GetTaskRequest&& request,
        const TGetTaskSettings& settings = TGetTaskSettings());

    TAsyncPingTaskResult PingTask(
        Yq::Private::PingTaskRequest&& request,
        const TPingTaskSettings& settings = TPingTaskSettings());

    TAsyncWriteTaskResult WriteTaskResult(
        Yq::Private::WriteTaskResultRequest&& request,
        const TWriteTaskResultSettings& settings = TWriteTaskResultSettings());

    TAsyncNodesHealthCheckResult NodesHealthCheck(
        Yq::Private::NodesHealthCheckRequest&& request,
        const TNodesHealthCheckSettings& settings = TNodesHealthCheckSettings());

private:
    std::shared_ptr<TImpl> Impl;
};

} // namespace NYq
