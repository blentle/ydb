#include "service_query.h"

#include <ydb/core/base/appdata.h>
#include <ydb/core/base/kikimr_issue.h>
#include <ydb/core/grpc_services/base/base.h>
#include <ydb/core/grpc_services/rpc_request_base.h>
#include <ydb/core/kqp/common/kqp.h>
#include <ydb/core/kqp/common/kqp_script_executions.h>
#include <ydb/core/kqp/common/simple/services.h>
#include <ydb/public/api/protos/draft/ydb_query.pb.h>

#include <library/cpp/actors/core/actor_bootstrapped.h>
#include <library/cpp/actors/core/events.h>
#include <library/cpp/actors/core/hfunc.h>
#include <library/cpp/actors/core/interconnect.h>

namespace NKikimr::NGRpcService {

namespace {

using namespace NActors;

using TEvFetchScriptResultsRequest = TGrpcRequestNoOperationCall<Ydb::Query::FetchScriptResultsRequest,
    Ydb::Query::FetchScriptResultsResponse>;

constexpr i64 MAX_ROWS_LIMIT = 1000;

class TFetchScriptResultsRPC : public TRpcRequestActor<TFetchScriptResultsRPC, TEvFetchScriptResultsRequest, false> {
public:
    using TRpcRequestActorBase = TRpcRequestActor<TFetchScriptResultsRPC, TEvFetchScriptResultsRequest, false>;

    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::GRPC_REQ;
    }

    TFetchScriptResultsRPC(TEvFetchScriptResultsRequest* request)
        : TRpcRequestActorBase(request)
    {}

    void Bootstrap() {
        const auto* req = GetProtoRequest();
        if (!req) {
            Reply(Ydb::StatusIds::INTERNAL_ERROR, "Internal error");
            return;
        }

        if (req->rows_limit() <= 0) {
            Reply(Ydb::StatusIds::BAD_REQUEST, "Invalid rows limit");
            return;
        }

        if (req->rows_offset() < 0) {
            Reply(Ydb::StatusIds::BAD_REQUEST, "Invalid rows offset");
            return;
        }

        if (req->rows_limit() > MAX_ROWS_LIMIT) {
            Reply(Ydb::StatusIds::BAD_REQUEST, TStringBuilder() << "Rows limit is too large. Values <= " << MAX_ROWS_LIMIT << " are allowed");
            return;
        }

        if (!GetExecutionIdFromRequest()) {
            return;
        }

        Send(NKqp::MakeKqpProxyID(SelfId().NodeId()), new NKqp::TEvKqp::TEvGetRunScriptActorRequest(DatabaseName, ExecutionId));

        Become(&TFetchScriptResultsRPC::StateFunc);
    }

private:
    STRICT_STFUNC(StateFunc,
        hFunc(NKqp::TEvKqp::TEvGetRunScriptActorResponse, Handle);
        hFunc(NKqp::TEvKqp::TEvFetchScriptResultsResponse, Handle);
        hFunc(NActors::TEvents::TEvUndelivered, Handle);
        hFunc(NActors::TEvInterconnect::TEvNodeDisconnected, Handle);
    )

    void Handle(NKqp::TEvKqp::TEvGetRunScriptActorResponse::TPtr& ev) {
        if (ev->Get()->Status != Ydb::StatusIds::SUCCESS) {
            Reply(ev->Get()->Status, ev->Get()->Issues);
            return;
        }

        const auto* userReq = GetProtoRequest();

        auto req = MakeHolder<NKqp::TEvKqp::TEvFetchScriptResultsRequest>();
        req->Record.SetRowsOffset(userReq->rows_offset());
        req->Record.SetRowsLimit(userReq->rows_limit());

        const NActors::TActorId runScriptActor = ev->Get()->RunScriptActorId;
        ui64 flags = IEventHandle::FlagTrackDelivery;
        if (runScriptActor.NodeId() != SelfId().NodeId()) {
            flags |= IEventHandle::FlagSubscribeOnSession;
            SubscribedOnSession = runScriptActor.NodeId();
        }
        Send(runScriptActor, std::move(req), flags);
    }

    void Handle(NKqp::TEvKqp::TEvFetchScriptResultsResponse::TPtr& ev) {
        Ydb::Query::FetchScriptResultsResponse resp;
        resp.set_status(ev->Get()->Record.GetStatus());
        resp.mutable_issues()->Swap(ev->Get()->Record.MutableIssues());
        resp.set_result_set_index(static_cast<i64>(ev->Get()->Record.GetResultSetIndex()));
        if (ev->Get()->Record.HasResultSet()) {
            resp.mutable_result_set()->Swap(ev->Get()->Record.MutableResultSet());
        }
        Reply(resp.status(), std::move(resp));
    }

    void Handle(NActors::TEvents::TEvUndelivered::TPtr& ev) {
        if (ev->Get()->Reason == NActors::TEvents::TEvUndelivered::ReasonActorUnknown) {
            Reply(Ydb::StatusIds::NOT_FOUND, "No such execution");
        } else {
            Reply(Ydb::StatusIds::UNAVAILABLE, "Failed to deliver fetch request to destination");
        }
    }

    void Handle(NActors::TEvInterconnect::TEvNodeDisconnected::TPtr&) {
        Reply(Ydb::StatusIds::UNAVAILABLE, "Failed to deliver fetch request to destination");
    }

    void PassAway() override {
        if (SubscribedOnSession) {
            Send(TActivationContext::InterconnectProxy(*SubscribedOnSession), new TEvents::TEvUnsubscribe());
        }
        TActorBootstrapped<TFetchScriptResultsRPC>::PassAway();
    }

    void Reply(Ydb::StatusIds::StatusCode status, Ydb::Query::FetchScriptResultsResponse&& result, const NYql::TIssues& issues = {}) {
        LOG_INFO_S(TActivationContext::AsActorContext(), NKikimrServices::RPC_REQUEST, "Fetch script results, status: "
            << Ydb::StatusIds::StatusCode_Name(status) << (issues ? ". Issues: " : "") << issues.ToOneLineString());

        for (const auto& issue : issues) {
            auto item = result.add_issues();
            NYql::IssueToMessage(issue, item);
        }

        result.set_status(status);

        TString serializedResult;
        Y_PROTOBUF_SUPPRESS_NODISCARD result.SerializeToString(&serializedResult);

        Request->SendSerializedResult(std::move(serializedResult), status);

        PassAway();
    }

    void Reply(Ydb::StatusIds::StatusCode status, const NYql::TIssues& issues) {
        Ydb::Query::FetchScriptResultsResponse result;
        Reply(status, std::move(result), issues);
    }

    void Reply(Ydb::StatusIds::StatusCode status, const TString& errorText) {
        NYql::TIssues issues;
        issues.AddIssue(errorText);
        Reply(status, issues);
    }

    bool GetExecutionIdFromRequest() {
        switch (GetProtoRequest()->execution_case()) {
        case Ydb::Query::FetchScriptResultsRequest::kExecutionId:
            ExecutionId = GetProtoRequest()->execution_id();
            break;
        case Ydb::Query::FetchScriptResultsRequest::kOperationId:
        {
            TMaybe<TString> executionId = NKqp::ScriptExecutionIdFromOperation(GetProtoRequest()->operation_id());
            if (!executionId) {
                Reply(Ydb::StatusIds::BAD_REQUEST, "Invalid operation id");
                return false;
            }
            ExecutionId = *executionId;
            break;
        }
        case Ydb::Query::FetchScriptResultsRequest::EXECUTION_NOT_SET:
            Reply(Ydb::StatusIds::BAD_REQUEST, "No execution id");
            return false;
        }
        return true;
    }

private:
    TMaybe<ui32> SubscribedOnSession;
    TString ExecutionId;
};

} // namespace

void DoFetchScriptResults(std::unique_ptr<IRequestNoOpCtx> p, const IFacilityProvider& f) {
    Y_UNUSED(f);
    auto* req = dynamic_cast<TEvFetchScriptResultsRequest*>(p.release());
    Y_VERIFY(req != nullptr, "Wrong using of TGRpcRequestWrapper");
    TActivationContext::AsActorContext().Register(new TFetchScriptResultsRPC(req));
}

} // namespace NKikimr::NGRpcService
