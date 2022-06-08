#pragma once
#include "viewer.h"
#include <unordered_map>
#include <library/cpp/actors/core/actor_bootstrapped.h>
#include <library/cpp/actors/core/interconnect.h>
#include <library/cpp/actors/core/mon.h>
#include <library/cpp/json/json_value.h>
#include <library/cpp/json/json_reader.h>
#include <library/cpp/protobuf/json/proto2json.h>
#include <ydb/core/node_whiteboard/node_whiteboard.h>
#include <ydb/core/kqp/kqp.h>
#include <ydb/core/kqp/executer/kqp_executer.h>
#include <ydb/core/viewer/json/json.h>
#include <ydb/public/lib/deprecated/kicli/kicli.h>
#include <ydb/public/sdk/cpp/client/ydb_result/result.h>

namespace NKikimr {
namespace NViewer {

using namespace NActors;
using ::google::protobuf::FieldDescriptor;

class TJsonQuery : public TActorBootstrapped<TJsonQuery> {
    using TThis = TJsonQuery;
    using TBase = TActorBootstrapped<TJsonQuery>;
    IViewer* Viewer;
    TJsonSettings JsonSettings;
    TActorId Initiator;
    NMon::TEvHttpInfo::TPtr Event;
    ui32 Timeout = 0;
    TVector<Ydb::ResultSet> ResultSets;
    TString Action;
    TString Stats;

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::VIEWER_HANDLER;
    }

    TJsonQuery(IViewer* viewer, NMon::TEvHttpInfo::TPtr& ev)
        : Viewer(viewer)
        , Initiator(ev->Sender)
        , Event(ev)
    {}

    STFUNC(StateWork) {
        switch (ev->GetTypeRewrite()) {
            HFunc(NKqp::TEvKqp::TEvQueryResponse, HandleReply);
            HFunc(NKqp::TEvKqp::TEvProcessResponse, HandleReply);
            HFunc(NKqp::TEvKqp::TEvAbortExecution, HandleReply);
            HFunc(NKqp::TEvKqpExecuter::TEvStreamData, HandleReply);
            HFunc(NKqp::TEvKqpExecuter::TEvStreamProfile, HandleReply);
            HFunc(NKqp::TEvKqpExecuter::TEvExecuterProgress, HandleReply);

            CFunc(TEvents::TSystem::Wakeup, HandleTimeout);

            default: {
                Cerr << "Unexpected event received in TJsonQuery::StateWork: " << ev->GetTypeRewrite() << Endl;
            }
        }
    }

    void Bootstrap(const TActorContext& ctx) {
        const auto& params(Event->Get()->Request.GetParams());
        auto event = MakeHolder<NKqp::TEvKqp::TEvQueryRequest>();
        JsonSettings.EnumAsNumbers = !FromStringWithDefault<bool>(params.Get("enums"), false);
        JsonSettings.UI64AsString = !FromStringWithDefault<bool>(params.Get("ui64"), false);
        Timeout = FromStringWithDefault<ui32>(params.Get("timeout"), 60000);
        TString query = params.Get("query");
        TString database = params.Get("database");
        Stats = params.Get("stats");
        Action = params.Get("action");
        if (query.empty() && Event->Get()->Request.GetMethod() == HTTP_METHOD_POST) {
            TStringBuf content = Event->Get()->Request.GetPostContent();
            const THttpHeaders& headers = Event->Get()->Request.GetHeaders();
            auto itContentType = FindIf(headers, [](const auto& header) { return header.Name() == "Content-Type"; });
            if (itContentType != headers.end()) {
                TStringBuf contentTypeHeader = itContentType->Value();
                TStringBuf contentType = contentTypeHeader.NextTok(';');
                if (contentType == "application/json") {
                    static NJson::TJsonReaderConfig JsonConfig;
                    NJson::TJsonValue requestData;
                    bool success = NJson::ReadJsonTree(content, &JsonConfig, &requestData);
                    if (success) {
                        query = requestData["query"].GetStringSafe({});
                        database = requestData["database"].GetStringSafe({});
                        Stats = requestData["stats"].GetStringSafe({});
                        Action = requestData["action"].GetStringSafe({});
                    }
                }
            }
        }
        if (query.empty()) {
            ReplyAndDie(HTTPBADREQUEST, ctx);
            return;
        }
        NKikimrKqp::TQueryRequest& request = *event->Record.MutableRequest();
        request.SetQuery(query);
        if (Action.empty() || Action == "execute-script" || Action == "execute") {
            request.SetAction(NKikimrKqp::QUERY_ACTION_EXECUTE);
            request.SetType(NKikimrKqp::QUERY_TYPE_SQL_SCRIPT);
            request.SetKeepSession(false);
        } else if (Action == "execute-scan") {
            request.SetAction(NKikimrKqp::QUERY_ACTION_EXECUTE);
            request.SetType(NKikimrKqp::QUERY_TYPE_SQL_SCAN);
            request.SetKeepSession(false);
        } else if (Action == "explain" || Action == "explain-ast") {
            request.SetAction(NKikimrKqp::QUERY_ACTION_EXPLAIN);
            request.SetType(NKikimrKqp::QUERY_TYPE_SQL_DML);
        }
        if (Stats == "profile") {
            request.SetStatsMode(NYql::NDqProto::DQ_STATS_MODE_PROFILE);
        }
        if (database) {
            request.SetDatabase(database);
        }
        if (!Event->Get()->UserToken.empty()) {
            event->Record.SetUserToken(Event->Get()->UserToken);
        }
        ActorIdToProto(SelfId(), event->Record.MutableRequestActorId());
        ctx.Send(NKqp::MakeKqpProxyID(ctx.SelfID.NodeId()), event.Release());

        Become(&TThis::StateWork, ctx, TDuration::MilliSeconds(Timeout), new TEvents::TEvWakeup());
    }

private:
    static NJson::TJsonValue ColumnPrimitiveValueToJsonValue(NYdb::TValueParser& valueParser) {
        switch (valueParser.GetPrimitiveType()) {
            case NYdb::EPrimitiveType::Bool:
                return valueParser.GetBool();
            case NYdb::EPrimitiveType::Int8:
                return valueParser.GetInt8();
            case NYdb::EPrimitiveType::Uint8:
                return valueParser.GetUint8();
            case NYdb::EPrimitiveType::Int16:
                return valueParser.GetInt16();
            case NYdb::EPrimitiveType::Uint16:
                return valueParser.GetUint16();
            case NYdb::EPrimitiveType::Int32:
                return valueParser.GetInt32();
            case NYdb::EPrimitiveType::Uint32:
                return valueParser.GetUint32();
            case NYdb::EPrimitiveType::Int64:
                return TStringBuilder() << valueParser.GetInt64();
            case NYdb::EPrimitiveType::Uint64:
                return TStringBuilder() << valueParser.GetUint64();
            case NYdb::EPrimitiveType::Float:
                return valueParser.GetFloat();
            case NYdb::EPrimitiveType::Double:
                return valueParser.GetDouble();
            case NYdb::EPrimitiveType::Utf8:
                return valueParser.GetUtf8();
            case NYdb::EPrimitiveType::Date:
                return valueParser.GetDate().ToString();
            case NYdb::EPrimitiveType::Datetime:
                return valueParser.GetDatetime().ToString();
            case NYdb::EPrimitiveType::Timestamp:
                return valueParser.GetTimestamp().ToString();
            case NYdb::EPrimitiveType::Interval:
                return TStringBuilder() << valueParser.GetInterval();
            case NYdb::EPrimitiveType::TzDate:
                return valueParser.GetTzDate();
            case NYdb::EPrimitiveType::TzDatetime:
                return valueParser.GetTzDatetime();
            case NYdb::EPrimitiveType::TzTimestamp:
                return valueParser.GetTzTimestamp();
            case NYdb::EPrimitiveType::String:
                return Base64Encode(valueParser.GetString());
            case NYdb::EPrimitiveType::Yson:
                return valueParser.GetYson();
            case NYdb::EPrimitiveType::Json:
                return valueParser.GetJson();
            case NYdb::EPrimitiveType::JsonDocument:
                return valueParser.GetJsonDocument();
            case NYdb::EPrimitiveType::DyNumber:
                return valueParser.GetDyNumber();
            case NYdb::EPrimitiveType::Uuid:
                return "<uuid not implemented>";
        }
    }

    static NJson::TJsonValue ColumnValueToJsonValue(NYdb::TValueParser& valueParser) {
        switch (valueParser.GetKind()) {
            case NYdb::TTypeParser::ETypeKind::Primitive:
                return ColumnPrimitiveValueToJsonValue(valueParser);

            case NYdb::TTypeParser::ETypeKind::Optional:
                valueParser.OpenOptional();
                if (valueParser.IsNull()) {
                    return NJson::JSON_NULL;
                }
                switch(valueParser.GetKind()) {
                    case NYdb::TTypeParser::ETypeKind::Primitive:
                        return ColumnPrimitiveValueToJsonValue(valueParser);
                    case NYdb::TTypeParser::ETypeKind::Decimal:
                        return valueParser.GetDecimal().ToString();
                    default:
                        return NJson::JSON_UNDEFINED;
                }

            default:
                return NJson::JSON_UNDEFINED;
        }
    }

    void HandleReply(NKqp::TEvKqp::TEvQueryResponse::TPtr& ev, const TActorContext& ctx) {
        TStringBuilder out;
        NKikimrKqp::TEvQueryResponse& record = ev->Get()->Record.GetRef();
        if (record.GetYdbStatus() == Ydb::StatusIds::SUCCESS) {
            const auto& response = record.GetResponse();
            out << Viewer->GetHTTPOKJSON(Event->Get());
            if (!Stats.empty()) {
                out << "{\"result\":";
            }
            if (response.ResultsSize() > 0) {
                const auto &result = response.GetResults();
                if (result.empty()) {
                    out << "[]";
                } else {
                    const auto &first = *result.begin();

                    if (first.HasType() && first.HasValue()) {
                        auto value = NClient::TValue::Create(first.GetValue(), first.GetType());
                        auto data = value["Data"];
                        if (data.HaveValue()) {
                            out << data.GetValueText<NClient::TFormatJSON>({JsonSettings.UI64AsString});
                        } else {
                            out << "[]";
                        }
                    } else {
                        out << "[]";
                    }
                }
            } else if (ResultSets.size() > 0) {
                out << "[";
                bool comma = false;
                for (auto it = ResultSets.begin(); it != ResultSets.end(); ++it) {
                    NYdb::TResultSet resultSet(*it);
                    const auto& columnsMeta = resultSet.GetColumnsMeta();
                    NYdb::TResultSetParser rsParser(resultSet);
                    while (rsParser.TryNextRow()) {
                        if (comma) {
                            out << ",";
                        }
                        out << "{";
                        for (size_t columnNum = 0; columnNum < columnsMeta.size(); ++columnNum) {
                            const NYdb::TColumn& columnMeta = columnsMeta[columnNum];
                            out << "\"" << TProtoToJson::EscapeJsonString(columnMeta.Name) << "\":";
                            out << NJson::WriteJson(ColumnValueToJsonValue(rsParser.ColumnParser(columnNum)), false);
                            if (columnNum + 1 < columnsMeta.size()) {
                                out << ",";
                            }
                        }
                        out << "}";
                        comma = true;
                    }
                }
                out << ']';
            } else if (response.HasQueryPlan()) {
                if (Action == "explain-ast") {
                    out << "{\"ast\":\"" << TProtoToJson::EscapeJsonString(response.GetQueryAst()) << "\"}";
                } else {
                    out << response.GetQueryPlan();
                }
            }
            if (!Stats.empty()) {
                out << ",\"stats\":";
                TStringStream json;
                TProtoToJson::ProtoToJson(json, response.GetQueryStats(), JsonSettings);
                out << json.Str() << "}";
            }
        } else {
            out << "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nConnection: Close\r\n\r\n";
            NJson::TJsonValue response;
            NJson::TJsonValue& jsonIssues = response["issues"];

            for (const auto& queryIssue : record.GetResponse().GetQueryIssues()) {
                NJson::TJsonValue& issue = jsonIssues.AppendValue({});
                NProtobufJson::Proto2Json(queryIssue, issue);
            }
            // find first deepest error
            const google::protobuf::RepeatedPtrField<Ydb::Issue::IssueMessage>* protoIssues = &(record.GetResponse().GetQueryIssues());
            while (protoIssues->size() > 0 && (*protoIssues)[0].issuesSize() > 0) {
                protoIssues = &((*protoIssues)[0].issues());
            }
            if (protoIssues->size() > 0) {
                const Ydb::Issue::IssueMessage& issue = (*protoIssues)[0];
                NProtobufJson::Proto2Json(issue, response["error"]);
            }
            out << NJson::WriteJson(response, false);
        }

        ReplyAndDie(out, ctx);
    }

    void HandleReply(NKqp::TEvKqp::TEvProcessResponse::TPtr& ev, const TActorContext& ctx) {
        Y_UNUSED(ev);
        Y_UNUSED(ctx);
    }

    void HandleReply(NKqp::TEvKqp::TEvAbortExecution::TPtr& ev, const TActorContext& ctx) {
        Y_UNUSED(ev);
        Y_UNUSED(ctx);
    }

    void HandleReply(NKqp::TEvKqpExecuter::TEvStreamData::TPtr& ev, const TActorContext& ctx) {
        const NKikimrKqp::TEvExecuterStreamData& data(ev->Get()->Record);

        ResultSets.emplace_back();
        ResultSets.back() = std::move(data.GetResultSet());

        THolder<NKqp::TEvKqpExecuter::TEvStreamDataAck> ack = MakeHolder<NKqp::TEvKqpExecuter::TEvStreamDataAck>();
        ack->Record.SetSeqNo(ev->Get()->Record.GetSeqNo());
        ctx.Send(ev->Sender, ack.Release());
    }

    void HandleReply(NKqp::TEvKqpExecuter::TEvStreamProfile::TPtr& ev, const TActorContext& ctx) {
        Y_UNUSED(ev);
        Y_UNUSED(ctx);
    }

    void HandleReply(NKqp::TEvKqpExecuter::TEvExecuterProgress::TPtr& ev, const TActorContext& ctx) {
        Y_UNUSED(ev);
        Y_UNUSED(ctx);
    }

    void HandleTimeout(const TActorContext& ctx) {
        ReplyAndDie(Viewer->GetHTTPGATEWAYTIMEOUT(), ctx);
    }

    void ReplyAndDie(TString data, const TActorContext& ctx) {
        ctx.Send(Initiator, new NMon::TEvHttpInfoRes(std::move(data), 0, NMon::IEvHttpInfoRes::EContentType::Custom));
        Die(ctx);
    }
};

template <>
struct TJsonRequestParameters<TJsonQuery> {
    static TString GetParameters() {
        return R"___([{"name":"ui64","in":"query","description":"return ui64 as number","required":false,"type":"boolean"},
                      {"name":"query","in":"query","description":"query text","required":true,"type":"string"},
                      {"name":"database","in":"query","description":"database name","required":false,"type":"string"},
                      {"name":"stats","in":"query","description":"return stats (profile)","required":false,"type":"string"},
                      {"name":"action","in":"query","description":"execute method (execute-scan, execute-script, explain, explain-ast)","required":false,"type":"string"},
                      {"name":"timeout","in":"query","description":"timeout in ms","required":false,"type":"integer"}])___";
    }
};

template <>
struct TJsonRequestSummary<TJsonQuery> {
    static TString GetSummary() {
        return "\"Execute query\"";
    }
};

template <>
struct TJsonRequestDescription<TJsonQuery> {
    static TString GetDescription() {
        return "\"Executes database query\"";
    }
};


}
}
