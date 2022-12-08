#pragma once
#include "common.h"
#include "config.h"

#include <library/cpp/actors/core/log.h>
#include <ydb/core/base/appdata.h>
#include <ydb/core/grpc_services/base/base.h>
#include <ydb/core/grpc_services/local_rpc/local_rpc.h>
#include <ydb/library/accessor/accessor.h>
#include <ydb/library/aclib/aclib.h>
#include <ydb/library/yql/public/issue/yql_issue_message.h>
#include <ydb/library/yql/public/issue/yql_issue.h>

namespace NKikimr::NInternal::NRequest {

template <class TRequestExt, class TResponseExt, ui32 EvStartExt, ui32 EvResultInternalExt, ui32 EvResultExt>
class TDialogPolicyImpl {
public:
    using TRequest = TRequestExt;
    using TResponse = TResponseExt;
    static constexpr ui32 EvStart = EvStartExt;
    static constexpr ui32 EvResultInternal = EvResultInternalExt;
    static constexpr ui32 EvResult = EvResultExt;
};

using TDialogCreateTable = TDialogPolicyImpl<Ydb::Table::CreateTableRequest, Ydb::Table::CreateTableResponse,
    EEvents::EvCreateTableRequest, EEvents::EvCreateTableInternalResponse, EEvents::EvCreateTableResponse>;
using TDialogModifyPermissions = TDialogPolicyImpl<Ydb::Scheme::ModifyPermissionsRequest, Ydb::Scheme::ModifyPermissionsResponse,
    EEvents::EvModifyPermissionsRequest, EEvents::EvModifyPermissionsInternalResponse, EEvents::EvModifyPermissionsResponse>;
using TDialogSelect = TDialogPolicyImpl<Ydb::Table::ExecuteDataQueryRequest, Ydb::Table::ExecuteDataQueryResponse,
    EEvents::EvSelectRequest, EEvents::EvSelectInternalResponse, EEvents::EvSelectResponse>;
using TDialogCreateSession = TDialogPolicyImpl<Ydb::Table::CreateSessionRequest, Ydb::Table::CreateSessionResponse,
    EEvents::EvCreateSessionRequest, EEvents::EvCreateSessionInternalResponse, EEvents::EvCreateSessionResponse>;

template <ui32 evResult = EEvents::EvGeneralYQLResponse>
using TCustomDialogYQLRequest = TDialogPolicyImpl<Ydb::Table::ExecuteDataQueryRequest, Ydb::Table::ExecuteDataQueryResponse,
    EEvents::EvYQLRequest, EEvents::EvYQLInternalResponse, evResult>;
template <ui32 evResult = EEvents::EvCreateSessionResponse>
using TCustomDialogCreateSpecialSession = TDialogPolicyImpl<Ydb::Table::CreateSessionRequest, Ydb::Table::CreateSessionResponse,
    EEvents::EvCreateSessionRequest, EEvents::EvCreateSessionInternalResponse, evResult>;

using TDialogYQLRequest = TCustomDialogYQLRequest<EEvents::EvGeneralYQLResponse>;
using TDialogCreateSpecialSession = TCustomDialogCreateSpecialSession<EEvents::EvCreateSessionResponse>;

template <class TDialogPolicy>
class TEvRequestResult: public NActors::TEventLocal<TEvRequestResult<TDialogPolicy>, TDialogPolicy::EvResult> {
private:
    YDB_READONLY_DEF(typename TDialogPolicy::TResponse, Result);
public:
    TEvRequestResult(typename TDialogPolicy::TResponse&& result)
        : Result(std::move(result)) {

    }
};

class TEvRequestFinished: public NActors::TEventLocal<TEvRequestFinished, EEvents::EvRequestFinished> {
public:
};

class TEvRequestStart: public NActors::TEventLocal<TEvRequestStart, EEvents::EvRequestStart> {
public:
};

class TEvRequestFailed: public NActors::TEventLocal<TEvRequestFailed, EEvents::EvRequestFailed> {
private:
    YDB_READONLY_DEF(TString, ErrorMessage)
public:
    TEvRequestFailed(const TString& errorMessage)
        : ErrorMessage(errorMessage)
    {

    }
};

template <class TResponse>
class TOperatorChecker {
public:
    static bool IsSuccess(const TResponse& r) {
        return r.operation().status() == Ydb::StatusIds::SUCCESS;
    }
};

template <>
class TOperatorChecker<Ydb::Table::CreateTableResponse> {
public:
    static bool IsSuccess(const Ydb::Table::CreateTableResponse& r) {
        return r.operation().status() == Ydb::StatusIds::SUCCESS ||
            r.operation().status() == Ydb::StatusIds::ALREADY_EXISTS;
    }
};

template <class TDialogPolicy>
class TYDBRequest: public NActors::TActorBootstrapped<TYDBRequest<TDialogPolicy>> {
private:
    using TBase = NActors::TActorBootstrapped<TYDBRequest<TDialogPolicy>>;
    using TRequest = typename TDialogPolicy::TRequest;
    using TResponse = typename TDialogPolicy::TResponse;
    using TSelf = TYDBRequest<TDialogPolicy>;
    TRequest ProtoRequest;
    const NActors::TActorId ActorFinishId;
    const NActors::TActorId ActorRestartId;
    const TConfig Config;
    ui32 Retry = 0;
    const NACLib::TUserToken UserToken;
protected:
    class TEvRequestInternalResult: public NActors::TEventLocal<TEvRequestInternalResult, TDialogPolicy::EvResultInternal> {
    private:
        YDB_READONLY_DEF(NThreading::TFuture<typename TDialogPolicy::TResponse>, Future);
    public:
        TEvRequestInternalResult(const NThreading::TFuture<typename TDialogPolicy::TResponse>& f)
            : Future(f) {

        }
    };
public:
    void Bootstrap(const TActorContext& /*ctx*/) {
        TBase::Become(&TBase::TThis::StateMain);
        TBase::template Sender<TEvRequestStart>().SendTo(TBase::SelfId());
    }

    STATEFN(StateMain) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvRequestInternalResult, Handle);
            hFunc(TEvRequestStart, Handle);
            default:
                break;
        }
    }
    void Handle(typename TEvRequestInternalResult::TPtr& ev) {
        if (!ev->Get()->GetFuture().HasValue() || ev->Get()->GetFuture().HasException()) {
            ALS_ERROR(NKikimrServices::METADATA_PROVIDER) << "cannot receive result on initialization";
            if (ActorRestartId) {
                TBase::template Sender<TEvRequestFailed>("incorrect future result").SendTo(ActorRestartId);
            } else {
                TBase::Schedule(Config.GetRetryPeriod(++Retry), new TEvRequestStart);
            }
            return;
        }
        auto f = ev->Get()->GetFuture();
        TResponse response = f.ExtractValue();
        if (!TOperatorChecker<TResponse>::IsSuccess(response)) {
            ALS_ERROR(NKikimrServices::METADATA_PROVIDER) << "incorrect reply: " << response.DebugString();
            NYql::TIssues issue;
            NYql::IssuesFromMessage(response.operation().issues(), issue);
            if (ActorRestartId) {
                TBase::template Sender<TEvRequestFailed>(issue.ToString()).SendTo(ActorRestartId);
            } else {
                TBase::Schedule(Config.GetRetryPeriod(++Retry), new TEvRequestStart);
            }
            return;
        }
        TBase::template Sender<TEvRequestResult<TDialogPolicy>>(std::move(response)).SendTo(ActorFinishId);
        TBase::template Sender<TEvRequestFinished>().SendTo(ActorFinishId);
        TBase::Die(TActivationContext::AsActorContext());
    }

    void Handle(typename TEvRequestStart::TPtr& /*ev*/) {
        auto aSystem = TActivationContext::ActorSystem();
        using TRpcRequest = NGRpcService::TGrpcRequestOperationCall<TRequest, TResponse>;
        auto request = ProtoRequest;
        NACLib::TUserToken uToken("metadata@system", {});
        auto result = NRpcService::DoLocalRpc<TRpcRequest>(std::move(request), AppData()->TenantName, uToken.SerializeAsString(), aSystem);
        const NActors::TActorId selfId = TBase::SelfId();
        const auto replyCallback = [aSystem, selfId](const NThreading::TFuture<TResponse>& f) {
            aSystem->Send(selfId, new TEvRequestInternalResult(f));
        };
        result.Subscribe(replyCallback);
    }

    TYDBRequest(const TRequest& request, const NACLib::TUserToken& uToken,
        const NActors::TActorId actorFinishId, const TConfig& config, const NActors::TActorId& actorRestartId = {})
        : ProtoRequest(request)
        , ActorFinishId(actorFinishId)
        , ActorRestartId(actorRestartId)
        , Config(config)
        , UserToken(uToken)
    {

    }

    TYDBRequest(const TRequest& request, const NACLib::TUserToken& uToken, const NActors::TActorId actorCallbackId)
        : ProtoRequest(request)
        , ActorFinishId(actorCallbackId)
        , ActorRestartId(actorCallbackId)
        , UserToken(uToken)
    {

    }
};

template <class TDialogPolicy>
class TSessionedActorImpl: public NActors::TActorBootstrapped<TSessionedActorImpl<TDialogPolicy>> {
private:
    ui32 Retry = 0;
    static_assert(!std::is_same<TDialogPolicy, TDialogCreateSession>());
    using TBase = NActors::TActorBootstrapped<TSessionedActorImpl<TDialogPolicy>>;
    void Handle(TEvRequestResult<TDialogCreateSession>::TPtr& ev) {
        Ydb::Table::CreateSessionResponse currentFullReply = ev->Get()->GetResult();
        Ydb::Table::CreateSessionResult session;
        currentFullReply.operation().result().UnpackTo(&session);
        const TString sessionId = session.session_id();
        Y_VERIFY(sessionId);
        std::optional<typename TDialogPolicy::TRequest> nextRequest = OnSessionId(sessionId);
        Y_VERIFY(nextRequest);
        TBase::Register(new TYDBRequest<TDialogPolicy>(*nextRequest, UserToken, TBase::SelfId(), Config, TBase::SelfId()));
    }
protected:
    const NInternal::NRequest::TConfig Config;
    const NACLib::TUserToken UserToken;
    virtual std::optional<typename TDialogPolicy::TRequest> OnSessionId(const TString& sessionId) = 0;
    virtual void OnResult(const typename TDialogPolicy::TResponse& response) = 0;
public:
    STATEFN(StateMain) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvRequestResult<TDialogCreateSession>, Handle);
            hFunc(TEvRequestFailed, Handle);
            hFunc(TEvRequestStart, Handle);
            hFunc(TEvRequestResult<TDialogPolicy>, Handle);
            default:
                break;
        }
    }

    TSessionedActorImpl(const NInternal::NRequest::TConfig& config, const NACLib::TUserToken& uToken)
        : Config(config)
        , UserToken(uToken)
    {

    }

    void Handle(typename TEvRequestResult<TDialogPolicy>::TPtr& ev) {
        OnResult(ev->Get()->GetResult());
        TBase::PassAway();
    }

    void Handle(typename TEvRequestFailed::TPtr& /*ev*/) {
        TBase::Schedule(Config.GetRetryPeriod(++Retry), new TEvRequestStart);
    }

    void Handle(typename TEvRequestFinished::TPtr& /*ev*/) {
        Retry = 0;
    }

    void Handle(typename TEvRequestStart::TPtr& /*ev*/) {
        TBase::Register(new TYDBRequest<TDialogCreateSession>(TDialogCreateSession::TRequest(), UserToken, TBase::SelfId(), Config, TBase::SelfId()));
    }

    void Bootstrap() {
        TBase::Become(&TSessionedActorImpl::StateMain);
        TBase::template Sender<TEvRequestStart>().SendTo(TBase::SelfId());
    }
};

class IQueryOutput {
public:
    using TPtr = std::shared_ptr<IQueryOutput>;
    virtual ~IQueryOutput() = default;

    virtual void OnReply(const TDialogYQLRequest::TResponse& response) = 0;
};

class TYQLQuerySessionedActor: public TSessionedActorImpl<TDialogYQLRequest> {
private:
    using TBase = TSessionedActorImpl<TDialogYQLRequest>;
    const TString Query;
    IQueryOutput::TPtr Output;
protected:
    virtual std::optional<TDialogYQLRequest::TRequest> OnSessionId(const TString& sessionId) override {
        Ydb::Table::ExecuteDataQueryRequest request;
        request.mutable_query()->set_yql_text(Query);
        request.set_session_id(sessionId);
        request.mutable_tx_control()->mutable_begin_tx()->mutable_snapshot_read_only();
        return request;
    }
    virtual void OnResult(const TDialogYQLRequest::TResponse& response) override {
        Output->OnReply(response);
    }
public:
    TYQLQuerySessionedActor(const TString& query, const NACLib::TUserToken& uToken,
        const NInternal::NRequest::TConfig& config, IQueryOutput::TPtr output)
        : TBase(config, uToken)
        , Query(query)
        , Output(output)
    {

    }
};

using TSessionedActor = TSessionedActorImpl<TDialogYQLRequest>;

}
