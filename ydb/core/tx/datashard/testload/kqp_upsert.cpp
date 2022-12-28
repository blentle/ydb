#include "actors.h"

#include <ydb/core/base/tablet.h>
#include <ydb/core/kqp/common/kqp.h>
#include <ydb/core/ydb_convert/ydb_convert.h>

#include <ydb/public/lib/operation_id/operation_id.h>
#include <ydb/public/sdk/cpp/client/ydb_params/params.h>
#include <ydb/public/sdk/cpp/client/ydb_params/params.h>
#include <ydb/public/sdk/cpp/client/ydb_proto/accessor.h>

#include <library/cpp/monlib/service/pages/templates.h>

#include <google/protobuf/text_format.h>

// * Scheme is hardcoded and it is like default YCSB setup:
// 1 Text "id" column, 10 Bytes "field0" - "field9" columns
// * row is ~ 1 KB, keys are like user1000385178204227360

namespace NKikimr::NDataShardLoad {

namespace {

struct TQueryInfo {
    TQueryInfo()
        : Query("")
        , Params(NYdb::TParamsBuilder().Build())
    {}

    TQueryInfo(const std::string& query, const NYdb::TParams&& params)
        : Query(query)
        , Params(std::move(params))
    {}

    TString Query;
    NYdb::TParams Params;
};

TQueryInfo GenerateUpsert(size_t n, const TString& table) {
    TStringStream str;

    NYdb::TParamsBuilder paramsBuilder;

    str << Sprintf(R"__(
        --!syntax_v1

        DECLARE $key AS Text;
        DECLARE $field0 AS Bytes;
        DECLARE $field1 AS Bytes;
        DECLARE $field2 AS Bytes;
        DECLARE $field3 AS Bytes;
        DECLARE $field4 AS Bytes;
        DECLARE $field5 AS Bytes;
        DECLARE $field6 AS Bytes;
        DECLARE $field7 AS Bytes;
        DECLARE $field8 AS Bytes;
        DECLARE $field9 AS Bytes;

        UPSERT INTO `%s` ( id, field0, field1, field2, field3, field4, field5, field6, field7, field8, field9 )
            VALUES ( $key, $field0, $field1, $field2, $field3, $field4, $field5, $field6, $field7, $field8, $field9 );
    )__", table.c_str());

    paramsBuilder.AddParam("$key").Utf8(GetKey(n)).Build();

    for (size_t i = 0; i < 10; ++i) {
        TString name = "$field" + ToString(i);
        paramsBuilder.AddParam(name).String(Value).Build();
    }

    auto params = paramsBuilder.Build();

    return TQueryInfo(str.Str(), std::move(params));
}


// it's a partial copy-paste from TUpsertActor: logic slightly differs, so that
// it seems better to have copy-paste rather if/else for different loads
class TKqpUpsertActor : public TActorBootstrapped<TKqpUpsertActor> {
    const NKikimrDataShardLoad::TEvTestLoadRequest::TUpdateStart Config;
    const NKikimrDataShardLoad::TEvTestLoadRequest::TTargetShard Target;
    const TActorId Parent;
    const ui64 Tag;
    const TString Database;

    TString ConfingString;

    TString Session;
    TRequestsVector Requests;
    size_t CurrentRequest = 0;
    size_t Inflight = 0;

    TInstant StartTs;
    TInstant EndTs;

    size_t Errors = 0;

public:
    TKqpUpsertActor(const NKikimrDataShardLoad::TEvTestLoadRequest::TUpdateStart& cmd,
                    const NKikimrDataShardLoad::TEvTestLoadRequest::TTargetShard& target,
                    const TActorId& parent,
                    TIntrusivePtr<::NMonitoring::TDynamicCounters> counters,
                    ui64 tag,
                    TRequestsVector requests)
        : Config(cmd)
        , Target(target)
        , Parent(parent)
        , Tag(tag)
        , Database(Target.GetWorkingDir())
        , Requests(std::move(requests))
    {
        Y_UNUSED(counters);
        google::protobuf::TextFormat::PrintToString(cmd, &ConfingString);
    }

    void Bootstrap(const TActorContext& ctx) {
        LOG_NOTICE_S(ctx, NKikimrServices::DS_LOAD_TEST, "TKqpUpsertActor# " << Tag
            << " Bootstrap called: " << ConfingString);

        Become(&TKqpUpsertActor::StateFunc);
        CreateSession(ctx);
    }

private:
    void CreateSession(const TActorContext& ctx) {
        auto kqpProxy = NKqp::MakeKqpProxyID(ctx.SelfID.NodeId());
        LOG_DEBUG_S(ctx, NKikimrServices::DS_LOAD_TEST, "TKqpUpsertActor# " << Tag
            << " sends event for session creation to proxy: " << kqpProxy.ToString());

        auto ev = MakeHolder<NKqp::TEvKqp::TEvCreateSessionRequest>();
        ev->Record.MutableRequest()->SetDatabase(Database);
        Send(kqpProxy, ev.Release());
    }

    void CloseSession(const TActorContext& ctx) {
        if (!Session)
            return;

        auto kqpProxy = NKqp::MakeKqpProxyID(ctx.SelfID.NodeId());
        LOG_DEBUG_S(ctx, NKikimrServices::DS_LOAD_TEST, "TKqpUpsertActor# " << Tag
            << " sends session close query to proxy: " << kqpProxy);

        auto ev = MakeHolder<NKqp::TEvKqp::TEvCloseSessionRequest>();
        ev->Record.MutableRequest()->SetSessionId(Session);
        ctx.Send(kqpProxy, ev.Release());
    }

    void SendRows(const TActorContext &ctx) {
        while (Inflight < Config.GetInflight() && CurrentRequest < Requests.size()) {
            auto* request = static_cast<NKqp::TEvKqp::TEvQueryRequest*>(Requests[CurrentRequest].get());
            request->Record.MutableRequest()->SetSessionId(Session);

            auto kqpProxy = NKqp::MakeKqpProxyID(ctx.SelfID.NodeId());
            LOG_TRACE_S(ctx, NKikimrServices::DS_LOAD_TEST, "TKqpUpsertActor# " << Tag
                << " send request# " << CurrentRequest
                << " to proxy# " << kqpProxy << ": " << request->ToString());

            ctx.Send(kqpProxy, Requests[CurrentRequest].release());

            ++CurrentRequest;
            ++Inflight;
        }
    }

    void OnRequestDone(const TActorContext& ctx) {
        if (CurrentRequest < Requests.size()) {
            SendRows(ctx);
        } else if (Inflight == 0) {
            EndTs = TInstant::Now();
            auto delta = EndTs - StartTs;

            auto response = std::make_unique<TEvDataShardLoad::TEvTestLoadFinished>(Tag);
            auto& report = *response->Record.MutableReport();
            report.SetTag(Tag);
            report.SetDurationMs(delta.MilliSeconds());
            report.SetOperationsOK(Requests.size() - Errors);
            report.SetOperationsError(Errors);

            ctx.Send(Parent, response.release());

            LOG_NOTICE_S(ctx, NKikimrServices::DS_LOAD_TEST, "TKqpUpsertActor# " << Tag
                << " finished in " << delta << ", errors=" << Errors);
            Die(ctx);
        }
    }

    void HandlePoison(const TActorContext& ctx) {
        LOG_DEBUG_S(ctx, NKikimrServices::DS_LOAD_TEST, "TKqpUpsertActor# " << Tag << " tablet recieved PoisonPill, going to die");
        CloseSession(ctx);
        Die(ctx);
    }

    void Handle(NKqp::TEvKqp::TEvCreateSessionResponse::TPtr& ev, const TActorContext& ctx) {
        auto& response = ev->Get()->Record;

        if (response.GetYdbStatus() == Ydb::StatusIds_StatusCode_SUCCESS) {
            Session = response.GetResponse().GetSessionId();
            LOG_DEBUG_S(ctx, NKikimrServices::DS_LOAD_TEST, "TKqpUpsertActor# " << Tag << " session: " << Session);
            SendRows(ctx);
        } else {
            StopWithError(ctx, "failed to create session: " + ev->Get()->ToString());
        }
    }

    void Handle(NKqp::TEvKqp::TEvQueryResponse::TPtr& ev, const TActorContext& ctx) {
        LOG_TRACE_S(ctx, NKikimrServices::DS_LOAD_TEST, "TKqpUpsertActor# " << Tag
            << " received from " << ev->Sender << ": " << ev->Get()->Record.DebugString());

        --Inflight;

        auto& response = ev->Get()->Record.GetRef();
        if (response.GetYdbStatus() != Ydb::StatusIds_StatusCode_SUCCESS) {
            ++Errors;
        }

        OnRequestDone(ctx);
    }

    void StopWithError(const TActorContext& ctx, const TString& reason) {
        LOG_WARN_S(ctx, NKikimrServices::DS_LOAD_TEST, "Load tablet stopped with error: " << reason);
        ctx.Send(Parent, new TEvDataShardLoad::TEvTestLoadFinished(Tag, reason));
        Die(ctx);
    }

    void Handle(TEvDataShardLoad::TEvTestLoadInfoRequest::TPtr& ev, const TActorContext& ctx) {
        TStringStream str;
        HTML(str) {
            str << "TKqpUpsertActor# " << Tag << " started on " << StartTs
                << " sent " << CurrentRequest << " out of " << Requests.size();
            TInstant ts = EndTs ? EndTs : TInstant::Now();
            auto delta = ts - StartTs;
            auto throughput = Requests.size() / delta.Seconds();
            str << " in " << delta << " (" << throughput << " op/s)"
                << " errors=" << Errors;
        }

        ctx.Send(ev->Sender, new TEvDataShardLoad::TEvTestLoadInfoResponse(Tag, str.Str()));
    }

    STRICT_STFUNC(StateFunc,
        CFunc(TEvents::TSystem::PoisonPill, HandlePoison)
        HFunc(TEvDataShardLoad::TEvTestLoadInfoRequest, Handle)
        HFunc(NKqp::TEvKqp::TEvQueryResponse, Handle)
        HFunc(NKqp::TEvKqp::TEvCreateSessionResponse, Handle)
    )
};

// creates multiple TKqpUpsertActor for inflight > 1 and waits completion
class TKqpUpsertActorMultiSession : public TActorBootstrapped<TKqpUpsertActorMultiSession> {
    const NKikimrDataShardLoad::TEvTestLoadRequest::TUpdateStart Config;
    const NKikimrDataShardLoad::TEvTestLoadRequest::TTargetShard Target;
    const TActorId Parent;
    const ui64 Tag;
    TIntrusivePtr<::NMonitoring::TDynamicCounters> Counters;
    const TString Database;

    TString ConfingString;

    TVector<TActorId> Actors;

    size_t Inflight = 0;

    TInstant StartTs;
    TInstant EndTs;

    size_t Oks = 0;
    size_t Errors = 0;

public:
    TKqpUpsertActorMultiSession(const NKikimrDataShardLoad::TEvTestLoadRequest::TUpdateStart& cmd,
                                const NKikimrDataShardLoad::TEvTestLoadRequest::TTargetShard& target,
                                const TActorId& parent,
                                TIntrusivePtr<::NMonitoring::TDynamicCounters> counters,
                                ui64 tag)
        : Config(cmd)
        , Target(target)
        , Parent(parent)
        , Tag(tag)
        , Counters(counters)
        , Database(target.GetWorkingDir())
    {
        Y_UNUSED(counters);
        google::protobuf::TextFormat::PrintToString(cmd, &ConfingString);
    }

    void Bootstrap(const TActorContext& ctx) {
        LOG_NOTICE_S(ctx, NKikimrServices::DS_LOAD_TEST, "TKqpUpsertActorMultiSession# " << Tag
            << " Bootstrap called: " << ConfingString);

        Become(&TKqpUpsertActorMultiSession::StateFunc);
        StartActors(ctx);
    }

private:
    void StartActors(const TActorContext& ctx) {
        const auto actorsCount = Config.GetInflight();
        const auto requestsPerActor = Config.GetRowCount() / actorsCount;

        TVector<TRequestsVector> perActorRequests;
        perActorRequests.reserve(actorsCount);

        size_t currentKey = Config.GetKeyFrom();
        for (size_t i = 0; i < actorsCount; ++i) {
            TRequestsVector requests;

            requests.reserve(requestsPerActor);
            for (size_t i = 0; i < requestsPerActor; ++i) {
                auto queryInfo = GenerateUpsert(currentKey++, Target.GetTableName());

                auto request = std::make_unique<NKqp::TEvKqp::TEvQueryRequest>();
                request->Record.MutableRequest()->SetKeepSession(true);
                request->Record.MutableRequest()->SetDatabase(Database);

                request->Record.MutableRequest()->SetAction(NKikimrKqp::QUERY_ACTION_EXECUTE);
                request->Record.MutableRequest()->SetType(NKikimrKqp::QUERY_TYPE_SQL_DML);
                request->Record.MutableRequest()->SetQuery(queryInfo.Query);

                request->Record.MutableRequest()->MutableQueryCachePolicy()->set_keep_in_cache(true);
                request->Record.MutableRequest()->MutableTxControl()->mutable_begin_tx()->mutable_serializable_read_write();
                request->Record.MutableRequest()->MutableTxControl()->set_commit_tx(true);

                const auto& params = NYdb::TProtoAccessor::GetProtoMap(queryInfo.Params);
                request->Record.MutableRequest()->MutableYdbParameters()->insert(params.begin(), params.end());

                requests.emplace_back(std::move(request));
            }
            perActorRequests.emplace_back(std::move(requests));
        }

        StartTs = TInstant::Now();

        Actors.reserve(actorsCount);
        Inflight = actorsCount;
        for (size_t i = 0; i < actorsCount; ++i) {
            ui32 pseudoTag = 1000000 + i;
            auto configCopy = Config;
            configCopy.SetInflight(1); // we have only 1 session
            configCopy.SetRowCount(requestsPerActor);

            auto* kqpActor = new TKqpUpsertActor(
                configCopy,
                Target,
                SelfId(),
                Counters,
                pseudoTag,
                std::move(perActorRequests[i]));
            Actors.emplace_back(ctx.Register(kqpActor));
        }

        LOG_NOTICE_S(ctx, NKikimrServices::DS_LOAD_TEST, "TKqpUpsertActorMultiSession# " << Tag
            << " started# " << actorsCount << " actors each with inflight# " << requestsPerActor);
    }

    void Handle(const TEvDataShardLoad::TEvTestLoadFinished::TPtr& ev, const TActorContext& ctx) {
        const auto& record = ev->Get()->Record;
        if (record.HasErrorReason() || !record.HasReport()) {
            TStringStream ss;
            ss << "kqp actor# " << record.GetTag() << " finished with error: " << record.GetErrorReason();
            if (record.HasReport())
                ss << ", report: " << ev->Get()->ToString();

            StopWithError(ctx, ss.Str());
            return;
        }

        LOG_DEBUG_S(ctx, NKikimrServices::DS_LOAD_TEST, "kqp# " << Tag << " finished: " << ev->Get()->ToString());

        Errors += record.GetReport().GetOperationsError();
        Oks += record.GetReport().GetOperationsOK();

        --Inflight;
        if (Inflight == 0) {
            EndTs = TInstant::Now();
            auto delta = EndTs - StartTs;

            auto response = std::make_unique<TEvDataShardLoad::TEvTestLoadFinished>(Tag);
            auto& report = *response->Record.MutableReport();
            report.SetTag(Tag);
            report.SetDurationMs(delta.MilliSeconds());
            report.SetOperationsOK(Oks);
            report.SetOperationsError(Errors);
            ctx.Send(Parent, response.release());

            LOG_NOTICE_S(ctx, NKikimrServices::DS_LOAD_TEST, "TKqpUpsertActorMultiSession# " << Tag
                << " finished in " << delta << ", oks# " << Oks << ", errors# " << Errors);

            Stop(ctx);
        }
    }

    void Handle(TEvDataShardLoad::TEvTestLoadInfoRequest::TPtr& ev, const TActorContext& ctx) {
        TStringStream str;
        HTML(str) {
            str << "TKqpUpsertActorMultiSession# " << Tag << " started on " << StartTs;
        }
        ctx.Send(ev->Sender, new TEvDataShardLoad::TEvTestLoadInfoResponse(Tag, str.Str()));
    }

    void HandlePoison(const TActorContext& ctx) {
        LOG_DEBUG_S(ctx, NKikimrServices::DS_LOAD_TEST, "TKqpUpsertActorMultiSession# " << Tag
            << " tablet recieved PoisonPill, going to die");
        // TODO: stop subactors?
        Die(ctx);
    }

    void StopWithError(const TActorContext& ctx, const TString& reason) {
        LOG_WARN_S(ctx, NKikimrServices::DS_LOAD_TEST, "TKqpUpsertActorMultiSession# " << Tag
            << " stopped with error: " << reason);

        ctx.Send(Parent, new TEvDataShardLoad::TEvTestLoadFinished(Tag, reason));
        Stop(ctx);
    }

    void Stop(const TActorContext& ctx) {
        for (const auto& actorId: Actors) {
            ctx.Send(actorId, new TEvents::TEvPoison());
        }

        Die(ctx);
    }

    STRICT_STFUNC(StateFunc,
        CFunc(TEvents::TSystem::PoisonPill, HandlePoison)
        HFunc(TEvDataShardLoad::TEvTestLoadInfoRequest, Handle)
        HFunc(TEvDataShardLoad::TEvTestLoadFinished, Handle);
    )
};

} // anonymous

IActor *CreateKqpUpsertActor(
    const NKikimrDataShardLoad::TEvTestLoadRequest::TUpdateStart& cmd,
    const NKikimrDataShardLoad::TEvTestLoadRequest::TTargetShard& target,
    const TActorId& parent,
    TIntrusivePtr<::NMonitoring::TDynamicCounters> counters,
    ui64 tag)
{
    return new TKqpUpsertActorMultiSession(cmd, target, parent, std::move(counters), tag);
}

} // NKikimr::NDataShardLoad
