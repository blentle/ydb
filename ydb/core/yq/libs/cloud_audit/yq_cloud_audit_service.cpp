#include "yq_cloud_audit_service.h"

#include <ydb/public/api/client/yc_public/events/yq.pb.h>

#include <ydb/core/yq/libs/actors/logging/log.h>
#include <ydb/core/yq/libs/audit/events/events.h>
#include <ydb/core/yq/libs/config/protos/issue_id.pb.h>

#include <ydb/library/folder_service/events.h>
#include <ydb/library/folder_service/folder_service.h>

#include <library/cpp/unified_agent_client/client.h>

#include <library/cpp/actors/core/actor.h>
#include <library/cpp/actors/core/actor_bootstrapped.h>
#include <library/cpp/actors/core/hfunc.h>
#include <library/cpp/actors/log_backend/actor_log_backend.h>
#include <library/cpp/retry/retry_policy.h>

#include <util/datetime/base.h>
#include <util/generic/guid.h>
#include <util/string/builder.h>

#include <contrib/libs/grpc/include/grpcpp/impl/codegen/status_code_enum.h>

#include <google/protobuf/util/json_util.h>

namespace {

// TODO: move to utils
TString ParsePeer(TString peerName) {
    TString res(peerName);
    if (res.StartsWith("ipv4:[") || res.StartsWith("ipv6:[")) {
        size_t pos = res.find(']');
        Y_VERIFY(pos != TString::npos);
        res = res.substr(6, pos - 6);
    } else if (res.StartsWith("ipv4:")) {
        size_t pos = res.rfind(':');
        if (pos == TString::npos) {//no port
            res = res.substr(5);
        } else {
            res = res.substr(5, pos - 5);
        }
    } else {
        size_t pos = res.rfind(":"); //port
        if (pos != TString::npos) {
            res = res.substr(0, pos);
        }
    }
    return res;
}

std::string MapConnectionType(const YandexQuery::ConnectionSetting::ConnectionCase& connectionCase) {
    switch (connectionCase) {
    case YandexQuery::ConnectionSetting::ConnectionCase::kYdbDatabase:
        return "YdbDatabase";
    case YandexQuery::ConnectionSetting::ConnectionCase::kClickhouseCluster:
        return "ClickhouseCluster";
    case YandexQuery::ConnectionSetting::ConnectionCase::kDataStreams:
        return "DataStreams";
    case YandexQuery::ConnectionSetting::ConnectionCase::kObjectStorage:
        return "ObjectStorage";
    case YandexQuery::ConnectionSetting::ConnectionCase::kMonitoring:
        return "Monitoring";
    default:
        Y_ENSURE(false, "Invalid connection case " << i32(connectionCase));
    }
}

std::string MapBindingType(const YandexQuery::BindingSetting::BindingCase& bindingCase) {
    switch (bindingCase) {
    case YandexQuery::BindingSetting::BindingSetting::kDataStreams:
        return "YdbDataStreams";
    case YandexQuery::BindingSetting::BindingSetting::kObjectStorage:
        return "ObjectStorage";
    default:
        Y_ENSURE(false, "Invalid connection case " << i32(bindingCase));
    }
}

// void FillSubjectType(::yandex::cloud::events::Authentication* authentication, TAuthentication::ESubjectType subjectType) {
//     switch (subjectType) {
//     case TAuthentication::ESubjectType::SERVICE_ACCOUNT:
//         authentication->set_subject_type(::yandex::cloud::events::Authentication::SERVICE_ACCOUNT);
//         return;
//     case TAuthentication::ESubjectType::FEDERATED_USER_ACCOUNT:
//         authentication->set_subject_type(::yandex::cloud::events::Authentication::FEDERATED_USER_ACCOUNT);
//         return;
//     case TAuthentication::ESubjectType::PASSPORT_USER_ACCOUNT:
//         authentication->set_subject_type(::yandex::cloud::events::Authentication::YANDEX_PASSPORT_USER_ACCOUNT);
//         return;
//     }
// }

TString MaybeRemoveSuffix(const TString& token) {
    const TString suffix = "@as";
    return token.EndsWith(suffix)
        ? token.substr(0, token.length() - suffix.length())
        : token;
}

void FillAuthentication(::yandex::cloud::events::Authentication& authentication, const NYq::TEvAuditService::TExtraInfo& info) {
    authentication.set_authenticated(true);
    authentication.set_subject_id(MaybeRemoveSuffix(info.User));
    authentication.set_subject_type(::yandex::cloud::events::Authentication::FEDERATED_USER_ACCOUNT); // TODO:
}

void FillAuthorization(::yandex::cloud::events::Authorization& authorization, const NYql::TIssues& issues) {
    authorization.set_authorized(AllOf(issues, [](const auto& t){ return t.IssueCode != NYq::TIssuesIds::ACCESS_DENIED; }));
    // for (const auto& permission : ctx.Authorization.Permissions) {
    //     auto* permision = authorization->add_permissions();
    //     permision->set_permission(permission.Permission);
    //     permision->set_resource_type(permission.ResourceType);
    //     permision->set_resource_id(permission.ResourceId);
    //     permision->set_authorized(permission.Authorized);
    // }
}

template<typename TRequest>
void FillRequestMetadata(
    ::yandex::cloud::events::RequestMetadata& metadata,
    const NYq::TEvAuditService::TExtraInfo& extraInfo,
    const TRequest& request)
{
    metadata.set_remote_address(ParsePeer(extraInfo.PeerName));
    metadata.set_user_agent(extraInfo.UserAgent);
    metadata.set_request_id(extraInfo.RequestId);
    metadata.set_idempotency_id(request.idempotency_key());
}

template<typename TEvent>
void FillResponse(TEvent& cloudEvent, const NYql::TIssues& issues) {
    cloudEvent.set_event_status(issues.Empty()
        ? yandex::cloud::events::EventStatus::DONE
        : yandex::cloud::events::EventStatus::ERROR);

    if (issues.Empty()) {
        cloudEvent.mutable_response();
        //auto* response = cloudEvent.mutable_response();
        //response->set_operation_id(response->operation_id());
    } else {
        auto* error = cloudEvent.mutable_error();
        error->set_code(grpc::StatusCode::UNKNOWN);
        error->set_message(issues.ToString());
    }
}

struct TAuditServiceSensors {
    NMonitoring::TDynamicCounterPtr UACounters;

    NMonitoring::TDynamicCounters::TCounterPtr Skipped;
    NMonitoring::TDynamicCounters::TCounterPtr CloudIdResolvedSuccess;
    NMonitoring::TDynamicCounters::TCounterPtr CloudIdResolvedError;
    NMonitoring::TDynamicCounters::TCounterPtr CloudIdResolvedRetry;
    NMonitoring::TDynamicCounters::TCounterPtr RecordsSent;

    TAuditServiceSensors(const NMonitoring::TDynamicCounterPtr& counters) {
        UACounters = counters->GetSubgroup("subcomponent", "ua_client");

        Skipped = counters->GetCounter("Skipped", true);
        CloudIdResolvedSuccess = counters->GetCounter("CloudIdResolvedSuccess", true);
        CloudIdResolvedError = counters->GetCounter("CloudIdResolvedError", true);
        CloudIdResolvedRetry = counters->GetCounter("CloudIdResolvedRetry", true);
        RecordsSent = counters->GetCounter("RecordsSent", true);
    }

    void ReportSkipped() {
        Skipped->Inc();
    }

    void ReportCloudIdResolvedSuccess() {
        CloudIdResolvedSuccess->Inc();
    }

    void ReportCloudIdResolvedError() {
        CloudIdResolvedError->Inc();
    }

    void ReportCloudIdResolvedRetry() {
        CloudIdResolvedRetry->Inc();
    }

    void ReportSent() {
        RecordsSent->Inc();
    }
};

}

namespace NYq {

template<class TEvent, class TRequest, class TAuditDetailsObj>
class TAuditEventSenderActor : public NActors::TActorBootstrapped<TAuditEventSenderActor<TEvent, TRequest, TAuditDetailsObj>> {
    using Base = NActors::TActorBootstrapped<TAuditEventSenderActor<TEvent, TRequest, TAuditDetailsObj>>;
    using IRetryPolicy = IRetryPolicy<NKikimr::NFolderService::TEvFolderService::TEvGetFolderResponse::TPtr&>;

public:
    TAuditEventSenderActor(
            NUnifiedAgent::TClientSessionPtr& session,
            std::shared_ptr<TAuditServiceSensors> auditServiceSensors,
            const std::function<typename TEvent::EventDetails(const TAuditDetails<TAuditDetailsObj>&)>& prepareEventDetails,
            const std::function<typename TEvent::RequestParameters(const TRequest&)>& prepareRequestParameters,
            const TString& eventType,
            const TString& messageName,
            TEvAuditService::TExtraInfo&& extraInfo,
            TRequest&& request,
            NYql::TIssues&& issues,
            TAuditDetails<TAuditDetailsObj>&& details,
            std::optional<TString>&& eventId)
        : Session(session)
        , AuditServiceSensors(std::move(auditServiceSensors))
        , PrepareEventDetails(prepareEventDetails)
        , PrepareRequestParameters(prepareRequestParameters)
        , ExtraInfo(std::move(extraInfo))
        , Request(std::move(request))
        , Issues(std::move(issues))
        , Details(std::move(details))
        , EventId(eventId ? std::move(eventId) : CreateGuidAsString())
        , EventType(eventType)
        , MessageName(messageName)
        , RetryState(GetRetryPolicy()->CreateRetryState()) {}

    static constexpr NKikimrServices::TActivity::EType ActorActivityType() { return NKikimrServices::TActivity::YQ_AUDIT_EVENT_SENDER_ACTOR; }

    void Bootstrap(const NActors::TActorContext&) {
        LOG_YQ_AUDIT_SERVICE_TRACE("EventId: " << *EventId << " (" << EventType << ") received ");
        if (!Session) {
            LOG_YQ_AUDIT_SERVICE_TRACE("EventId: " << *EventId << " session is null. Skipping event... ");
            AuditServiceSensors->ReportSkipped();
            Base::PassAway();
            return;
        }

        Base::Become(&TAuditEventSenderActor<TEvent, TRequest, TAuditDetailsObj>::StateFunc);

        FillAuthentication(*CloudEvent.mutable_authentication(), ExtraInfo);
        FillAuthorization(*CloudEvent.mutable_authorization(), Issues);
        FillRequestMetadata(*CloudEvent.mutable_request_metadata(), ExtraInfo, Request);

        FillRequestParameters(Request);
        FillEventMetadata(Details);
        FillDetails(Details);

        FillResponse(CloudEvent, Issues);

        if (ExtraInfo.CloudId) {
            LOG_YQ_AUDIT_SERVICE_TRACE("EventId: " << *EventId << " cloudId is provided. Send now");
            CloudEvent.mutable_event_metadata()->set_cloud_id(ExtraInfo.CloudId);
            SendAndComplete();
            return;
        }

        LOG_YQ_AUDIT_SERVICE_TRACE("EventId: " << *EventId << " resolving cloud id ...");
        Base::Send(NKikimr::NFolderService::FolderServiceActorId(), CreateRequest().release(), 0, 0);
    }

    std::unique_ptr<NKikimr::NFolderService::TEvFolderService::TEvGetFolderRequest> CreateRequest() {
        auto request = std::make_unique<NKikimr::NFolderService::TEvFolderService::TEvGetFolderRequest>();
        request->Request.set_folder_id(ExtraInfo.FolderId);
        request->Token = ExtraInfo.Token;
        return request;
    }

private:
    STRICT_STFUNC(StateFunc,
        hFunc(NKikimr::NFolderService::TEvFolderService::TEvGetFolderResponse, Handle);
    )

    void Handle(NKikimr::NFolderService::TEvFolderService::TEvGetFolderResponse::TPtr& ev) {
        const auto& response = ev->Get()->Response;
        const auto& status = ev->Get()->Status;
        if (!status.Ok() || !response.has_folder()) {
            auto& status = ev->Get()->Status;
            auto delay = RetryState->GetNextRetryDelay(ev);
            if (delay) {
                AuditServiceSensors->ReportCloudIdResolvedRetry();
                LOG_YQ_AUDIT_SERVICE_ERROR("Folder resolve error. Retry with delay " << *delay << ", EventId: " << *EventId << " cloud id resolve error. "
                    << "Status " << status.GRpcStatusCode << " " << status.Msg << " details: " << status.Details);
                NActors::TActivationContext::Schedule(*delay, new IEventHandleFat(NKikimr::NFolderService::FolderServiceActorId(), Base::SelfId(), CreateRequest().release()));
                return;
            }
            AuditServiceSensors->ReportCloudIdResolvedError();
            LOG_YQ_AUDIT_SERVICE_ERROR("EventId: " << *EventId << " cloud id resolve error. "
                << "Status " << status.GRpcStatusCode << " " << status.Msg << " details: " << status.Details);
            LOG_YQ_AUDIT_SERVICE_INFO(MessageName << ": cloud id: [unknown], folder id: [" << CloudEvent.event_metadata().folder_id() << "], user: [" << ExtraInfo.User << "], has issues [" << static_cast<bool>(Issues) << "], details: [" << CloudEvent.details().ShortDebugString() << "]");
            AuditServiceSensors->ReportSkipped();
            Base::PassAway();
            return;
        }

        AuditServiceSensors->ReportCloudIdResolvedSuccess();

        LOG_YQ_AUDIT_SERVICE_TRACE("EventId: " << *EventId << " cloud id resolved");
        const auto cloudId = ev->Get()->Response.folder().cloud_id();
        CloudEvent.mutable_event_metadata()->set_cloud_id(cloudId);
        SendAndComplete();
    }

    void FillRequestParameters(TRequest& request) {
        *CloudEvent.mutable_request_parameters() = PrepareRequestParameters(request);
    }

    template<typename T>
    void FillEventMetadata(TAuditDetails<T>& details) {
        google::protobuf::Timestamp createdAt;
        if (details.Before) {
            createdAt = details.Before->meta().common().created_at();
        } else if (details.After) {
            createdAt = details.After->meta().common().created_at();
        } else {
            auto now = TInstant::Now();
            createdAt.set_seconds(now.Seconds());
            createdAt.set_nanos(now.NanoSeconds() % 1000000000);
        }
        FillEventMetadataImpl(createdAt);
    }

    template<typename T> requires requires (T t) { t.meta().created_at(); }
    void FillEventMetadata(TAuditDetails<T>& details) {
        google::protobuf::Timestamp createdAt;
        if (details.Before) {
            createdAt = details.Before->meta().created_at();
        } else if (details.After) {
            createdAt = details.After->meta().created_at();
        } else {
            auto now = TInstant::Now();
            createdAt.set_seconds(now.Seconds());
            createdAt.set_nanos(now.NanoSeconds() % 1000000000);
        }
        FillEventMetadataImpl(createdAt);
    }

    void FillEventMetadataImpl(const google::protobuf::Timestamp& createdAt) {
        auto* eventMetadata = CloudEvent.mutable_event_metadata();
        eventMetadata->set_event_id(*EventId);
        eventMetadata->set_event_type(EventType);
        *eventMetadata->mutable_created_at() = createdAt;
        eventMetadata->set_folder_id(ExtraInfo.FolderId);
    }

    void FillDetails(TAuditDetails<TAuditDetailsObj>& details) {
        *CloudEvent.mutable_details() = PrepareEventDetails(details);
    }

    void SendAndComplete() {
        TString output;
        google::protobuf::util::JsonPrintOptions printOpts;
        printOpts.preserve_proto_field_names = true;
        google::protobuf::util::MessageToJsonString(CloudEvent, &output, printOpts);
        NUnifiedAgent::TClientMessage message;
        message.Payload = TStringBuilder() << output;

        LOG_YQ_AUDIT_SERVICE_TRACE("EventId: " << *EventId << " sending");
        LOG_YQ_AUDIT_SERVICE_INFO(MessageName << ": cloud id: [" << CloudEvent.event_metadata().cloud_id() << "], folder id: [" << CloudEvent.event_metadata().folder_id() << "], user: [" << ExtraInfo.User << "], has issues [" << static_cast<bool>(Issues) << "], details: [" << CloudEvent.details().ShortDebugString() << "]");
        Session->Send(std::move(message));

        AuditServiceSensors->ReportSent();

        Base::PassAway();
    }

    static const IRetryPolicy::TPtr& GetRetryPolicy() {
        static IRetryPolicy::TPtr policy = IRetryPolicy::GetExponentialBackoffPolicy([](NKikimr::NFolderService::TEvFolderService::TEvGetFolderResponse::TPtr& ev) {
            const auto& response = ev->Get()->Response;
            const auto& status = ev->Get()->Status;
            return !status.Ok() || !response.has_folder() ? ERetryErrorClass::ShortRetry : ERetryErrorClass::NoRetry;
        }, TDuration::MilliSeconds(10), TDuration::MilliSeconds(200), TDuration::Seconds(30), 5);
        return policy;
    }

private:
    NUnifiedAgent::TClientSessionPtr Session;
    std::shared_ptr<TAuditServiceSensors> AuditServiceSensors;
    std::function<typename TEvent::EventDetails(const TAuditDetails<TAuditDetailsObj>&)> PrepareEventDetails;
    std::function<typename TEvent::RequestParameters(const TRequest&)> PrepareRequestParameters;
    TEvAuditService::TExtraInfo ExtraInfo;
    TRequest Request;
    NYql::TIssues Issues;
    TAuditDetails<TAuditDetailsObj> Details;
    std::optional<TString> EventId;
    TString EventType;
    TString MessageName;

    TEvent CloudEvent;
    IRetryPolicy::IRetryState::TPtr RetryState;
};

class TYqCloudAuditServiceActor : public NActors::TActorBootstrapped<TYqCloudAuditServiceActor> {
public:
    using Base = NActors::TActorBootstrapped<TYqCloudAuditServiceActor>;

    TYqCloudAuditServiceActor(const NConfig::TAuditConfig& config, const NMonitoring::TDynamicCounterPtr& counters)
        : NActors::TActorBootstrapped<TYqCloudAuditServiceActor>()
        , Config(config)
        , AuditServiceSensors(std::make_shared<TAuditServiceSensors>(counters)) {}

    static constexpr NKikimrServices::TActivity::EType ActorActivityType() { return NKikimrServices::TActivity::YQ_AUDIT_ACTOR; }

    void Bootstrap(const NActors::TActorContext& ctx) {
        Base::Become(&TYqCloudAuditServiceActor::StateFunc);

        if (!Config.GetEnabled()) {
            LOG_YQ_AUDIT_SERVICE_INFO("Audit service is disabled");
            return;
        }

        auto clientParameters = NUnifiedAgent::TClientParameters(Config.GetUAConfig().GetUri());
        SdkLogger = std::make_unique<TLog>(MakeHolder<TActorLogBackend>(ctx.ActorSystem(), NKikimrServices::EServiceKikimr::YDB_SDK));
        clientParameters.SetLog(*SdkLogger);

        const auto& sharedKey = Config.GetUAConfig().GetSharedSecretKey();
        if (!sharedKey.Empty()) {
            clientParameters.SetSharedSecretKey(sharedKey);
        }
        auto clientPtr = NUnifiedAgent::MakeClient(clientParameters);
        auto sessionParameters = NUnifiedAgent::TSessionParameters()
            .SetCounters(AuditServiceSensors->UACounters);
        Session = clientPtr->CreateSession(sessionParameters);
    }

private:
    STRICT_STFUNC(StateFunc,
        hFunc(TEvAuditService::CreateBindingAuditReport, Handle);
        hFunc(TEvAuditService::ModifyBindingAuditReport, Handle);
        hFunc(TEvAuditService::DeleteBindingAuditReport, Handle);
        hFunc(TEvAuditService::CreateConnectionAuditReport, Handle);
        hFunc(TEvAuditService::ModifyConnectionAuditReport, Handle);
        hFunc(TEvAuditService::DeleteConnectionAuditReport, Handle);
        hFunc(TEvAuditService::CreateQueryAuditReport, Handle);
        hFunc(TEvAuditService::ControlQueryAuditReport, Handle);
        hFunc(TEvAuditService::ModifyQueryAuditReport, Handle);
        hFunc(TEvAuditService::DeleteQueryAuditReport, Handle);
    )

    void Handle(TEvAuditService::CreateBindingAuditReport::TPtr& ev) {
        auto& auditReport = *ev.Get()->Get();

        auto prepareEventDetails = [](const TAuditDetails<YandexQuery::Binding>& details) {
            yandex::cloud::events::yq::CreateBinding::EventDetails eventDetails;
            if (details.After) {
                const auto& after = *details.After;
                eventDetails.set_binding_id(after.meta().id());
                eventDetails.set_name(after.content().name());
                eventDetails.set_visibility(YandexQuery::Acl::Visibility_Name(after.content().acl().visibility()));
                eventDetails.set_connection_id(after.content().connection_id());
                eventDetails.set_type(MapBindingType(after.content().setting().binding_case()));
            }
            return eventDetails;
        };

        auto prepareRequestParameters = [](const YandexQuery::CreateBindingRequest& request) {
            yandex::cloud::events::yq::CreateBinding::RequestParameters requestParameters;
            requestParameters.set_name(request.content().name());
            requestParameters.set_visibility(YandexQuery::Acl::Visibility_Name(request.content().acl().visibility()));
            requestParameters.set_connection_id(request.content().connection_id());
            requestParameters.set_type(MapBindingType(request.content().setting().binding_case()));
            return requestParameters;
        };

        Register(new TAuditEventSenderActor<yandex::cloud::events::yq::CreateBinding, YandexQuery::CreateBindingRequest, YandexQuery::Binding>(
            Session,
            AuditServiceSensors,
            prepareEventDetails,
            prepareRequestParameters,
            "yandex.cloud.events.yq.CreateBinding",
            "CreateBinding",
            std::move(auditReport.ExtraInfo),
            std::move(auditReport.Request),
            std::move(auditReport.Issues),
            std::move(auditReport.Details),
            std::move(auditReport.EventId)
        ));
    }

    void Handle(TEvAuditService::ModifyBindingAuditReport::TPtr& ev) {
        auto& auditReport = *ev.Get()->Get();

        auto prepareEventDetails = [](const TAuditDetails<YandexQuery::Binding>& details) {
            yandex::cloud::events::yq::UpdateBinding::EventDetails eventDetails;
            if (details.After) {
                const auto& after = *details.After;
                eventDetails.set_binding_id(after.meta().id());
                eventDetails.set_name(after.content().name());
                eventDetails.set_visibility(YandexQuery::Acl::Visibility_Name(after.content().acl().visibility()));
                eventDetails.set_connection_id(after.content().connection_id());
                eventDetails.set_type(MapBindingType(after.content().setting().binding_case()));
            }
            return eventDetails;
        };

        auto prepareRequestParameters = [](const YandexQuery::ModifyBindingRequest& request) {
            yandex::cloud::events::yq::UpdateBinding::RequestParameters requestParameters;
            requestParameters.set_binding_id(request.binding_id());
            requestParameters.set_name(request.content().name());
            requestParameters.set_visibility(YandexQuery::Acl::Visibility_Name(request.content().acl().visibility()));
            requestParameters.set_connection_id(request.content().connection_id());
            requestParameters.set_type(MapBindingType(request.content().setting().binding_case()));
            return requestParameters;
        };

        Register(new TAuditEventSenderActor<yandex::cloud::events::yq::UpdateBinding, YandexQuery::ModifyBindingRequest, YandexQuery::Binding>(
            Session,
            AuditServiceSensors,
            prepareEventDetails,
            prepareRequestParameters,
            "yandex.cloud.events.yq.UpdateBinding",
            "ModifyBinding",
            std::move(auditReport.ExtraInfo),
            std::move(auditReport.Request),
            std::move(auditReport.Issues),
            std::move(auditReport.Details),
            std::move(auditReport.EventId)
        ));
    }

    void Handle(TEvAuditService::DeleteBindingAuditReport::TPtr& ev) {
        auto& auditReport = *ev.Get()->Get();

        auto prepareEventDetails = [](const TAuditDetails<YandexQuery::Binding>& details) {
            yandex::cloud::events::yq::DeleteBinding::EventDetails eventDetails;
            if (details.Before) {
                const auto& before = *details.Before;
                eventDetails.set_binding_id(before.meta().id());
                eventDetails.set_name(before.content().name());
                eventDetails.set_visibility(YandexQuery::Acl::Visibility_Name(before.content().acl().visibility()));
                eventDetails.set_connection_id(before.content().connection_id());
                eventDetails.set_type(MapBindingType(before.content().setting().binding_case()));
            }
            return eventDetails;
        };

        auto prepareRequestParameters = [](const YandexQuery::DeleteBindingRequest& request) {
            yandex::cloud::events::yq::DeleteBinding::RequestParameters requestParameters;
            requestParameters.set_binding_id(request.binding_id());
            return requestParameters;
        };

        Register(new TAuditEventSenderActor<yandex::cloud::events::yq::DeleteBinding, YandexQuery::DeleteBindingRequest, YandexQuery::Binding>(
            Session,
            AuditServiceSensors,
            prepareEventDetails,
            prepareRequestParameters,
            "yandex.cloud.events.yq.DeleteBinding",
            "DeleteBinding",
            std::move(auditReport.ExtraInfo),
            std::move(auditReport.Request),
            std::move(auditReport.Issues),
            std::move(auditReport.Details),
            std::move(auditReport.EventId)
        ));
    }

    void Handle(TEvAuditService::CreateConnectionAuditReport::TPtr& ev) {
        auto& auditReport = *ev.Get()->Get();

        auto prepareEventDetails = [](const TAuditDetails<YandexQuery::Connection>& details) {
            yandex::cloud::events::yq::CreateConnection::EventDetails eventDetails;
            if (details.After) {
                const auto& after = *details.After;
                eventDetails.set_connection_id(after.meta().id());
                eventDetails.set_name(after.content().name());
                eventDetails.set_visibility(YandexQuery::Acl::Visibility_Name(after.content().acl().visibility()));
                eventDetails.set_type(MapConnectionType(after.content().setting().connection_case()));
            }
            return eventDetails;
        };

        auto prepareRequestParameters = [](const YandexQuery::CreateConnectionRequest& request) {
            yandex::cloud::events::yq::CreateConnection::RequestParameters requestParameters;
            requestParameters.set_name(request.content().name());
            requestParameters.set_visibility(YandexQuery::Acl::Visibility_Name(request.content().acl().visibility()));
            requestParameters.set_type(MapConnectionType(request.content().setting().connection_case()));
            return requestParameters;
        };

        Register(new TAuditEventSenderActor<yandex::cloud::events::yq::CreateConnection, YandexQuery::CreateConnectionRequest, YandexQuery::Connection>(
            Session,
            AuditServiceSensors,
            prepareEventDetails,
            prepareRequestParameters,
            "yandex.cloud.events.yq.CreateConnection",
            "CreateConnection",
            std::move(auditReport.ExtraInfo),
            std::move(auditReport.Request),
            std::move(auditReport.Issues),
            std::move(auditReport.Details),
            std::move(auditReport.EventId)
        ));
    }

    void Handle(TEvAuditService::ModifyConnectionAuditReport::TPtr& ev) {
        auto& auditReport = *ev.Get()->Get();

        auto prepareEventDetails = [](const TAuditDetails<YandexQuery::Connection>& details) {
            yandex::cloud::events::yq::UpdateConnection::EventDetails eventDetails;
            if (details.After) {
                const auto& after = *details.After;
                eventDetails.set_connection_id(after.meta().id());
                eventDetails.set_name(after.content().name());
                eventDetails.set_visibility(YandexQuery::Acl::Visibility_Name(after.content().acl().visibility()));
                eventDetails.set_type(MapConnectionType(after.content().setting().connection_case()));
            }
            return eventDetails;
        };

        auto prepareRequestParameters = [](const YandexQuery::ModifyConnectionRequest& request) {
            yandex::cloud::events::yq::UpdateConnection::RequestParameters requestParameters;
            requestParameters.set_connection_id(request.connection_id());
            requestParameters.set_name(request.content().name());
            requestParameters.set_visibility(YandexQuery::Acl::Visibility_Name(request.content().acl().visibility()));
            requestParameters.set_type(MapConnectionType(request.content().setting().connection_case()));
            return requestParameters;
        };

        Register(new TAuditEventSenderActor<yandex::cloud::events::yq::UpdateConnection, YandexQuery::ModifyConnectionRequest, YandexQuery::Connection>(
            Session,
            AuditServiceSensors,
            prepareEventDetails,
            prepareRequestParameters,
            "yandex.cloud.events.yq.UpdateConnection",
            "ModifyConnection",
            std::move(auditReport.ExtraInfo),
            std::move(auditReport.Request),
            std::move(auditReport.Issues),
            std::move(auditReport.Details),
            std::move(auditReport.EventId)
        ));
    }

    void Handle(TEvAuditService::DeleteConnectionAuditReport::TPtr& ev) {
        auto& auditReport = *ev.Get()->Get();

        auto prepareEventDetails = [](const TAuditDetails<YandexQuery::Connection>& details) {
            yandex::cloud::events::yq::DeleteConnection::EventDetails eventDetails;
            if (details.Before) {
                const auto& before = *details.Before;
                eventDetails.set_connection_id(before.meta().id());
                eventDetails.set_name(before.content().name());
                eventDetails.set_visibility(YandexQuery::Acl::Visibility_Name(before.content().acl().visibility()));
                eventDetails.set_type(MapConnectionType(before.content().setting().connection_case()));
            }
            return eventDetails;
        };

        auto prepareRequestParameters = [](const YandexQuery::DeleteConnectionRequest& request) {
            yandex::cloud::events::yq::DeleteConnection::RequestParameters requestParameters;
            requestParameters.set_connection_id(request.connection_id());
            return requestParameters;
        };

        Register(new TAuditEventSenderActor<yandex::cloud::events::yq::DeleteConnection, YandexQuery::DeleteConnectionRequest, YandexQuery::Connection>(
            Session,
            AuditServiceSensors,
            prepareEventDetails,
            prepareRequestParameters,
            "yandex.cloud.events.yq.DeleteConnection",
            "DeleteConnection",
            std::move(auditReport.ExtraInfo),
            std::move(auditReport.Request),
            std::move(auditReport.Issues),
            std::move(auditReport.Details),
            std::move(auditReport.EventId)
        ));
    }

    void Handle(TEvAuditService::CreateQueryAuditReport::TPtr& ev) {
        auto& auditReport = *ev.Get()->Get();

        auto prepareEventDetails = [](const TAuditDetails<YandexQuery::Query>& details) {
            yandex::cloud::events::yq::CreateQuery::EventDetails eventDetails;
            if (details.After) {
                const auto& after = *details.After;
                eventDetails.set_query_id(after.meta().common().id());
                eventDetails.set_name(after.content().name());
                eventDetails.set_visibility(YandexQuery::Acl::Visibility_Name(after.content().acl().visibility()));
                eventDetails.set_type(YandexQuery::QueryContent::QueryType_Name(after.content().type()));
                eventDetails.set_execute_mode(YandexQuery::ExecuteMode_Name(after.meta().execute_mode()));
                eventDetails.set_compute_status(YandexQuery::QueryMeta::ComputeStatus_Name(after.meta().status()));
                eventDetails.set_text_length(after.content().text().length());
            }
            return eventDetails;
        };

        auto prepareRequestParameters = [](const YandexQuery::CreateQueryRequest& request) {
            yandex::cloud::events::yq::CreateQuery::RequestParameters requestParameters;
            requestParameters.set_name(request.content().name());
            requestParameters.set_visibility(YandexQuery::Acl::Visibility_Name(request.content().acl().visibility()));
            requestParameters.set_type(YandexQuery::QueryContent::QueryType_Name(request.content().type()));
            requestParameters.set_execute_mode(YandexQuery::ExecuteMode_Name(request.execute_mode()));
            requestParameters.set_text_length(request.content().text().length());
            return requestParameters;
        };

        Register(new TAuditEventSenderActor<yandex::cloud::events::yq::CreateQuery, YandexQuery::CreateQueryRequest, YandexQuery::Query>(
            Session,
            AuditServiceSensors,
            prepareEventDetails,
            prepareRequestParameters,
            "yandex.cloud.events.yq.CreateQuery",
            "CreateQuery",
            std::move(auditReport.ExtraInfo),
            std::move(auditReport.Request),
            std::move(auditReport.Issues),
            std::move(auditReport.Details),
            std::move(auditReport.EventId)
        ));
    }

    void Handle(TEvAuditService::ControlQueryAuditReport::TPtr& ev) {
        auto& auditReport = *ev.Get()->Get();

        auto prepareEventDetails = [](const TAuditDetails<YandexQuery::Query>& details) {
            yandex::cloud::events::yq::ControlQuery::EventDetails eventDetails;
            if (details.Before) {
                const auto& before = *details.Before;
                eventDetails.set_query_id(before.meta().common().id());
                eventDetails.set_name(before.content().name());
                eventDetails.set_visibility(YandexQuery::Acl::Visibility_Name(before.content().acl().visibility()));
                eventDetails.set_type(YandexQuery::QueryContent::QueryType_Name(before.content().type()));
                eventDetails.set_execute_mode(YandexQuery::ExecuteMode_Name(before.meta().execute_mode()));
                eventDetails.set_compute_status(YandexQuery::QueryMeta::ComputeStatus_Name(before.meta().status()));
                eventDetails.set_text_length(before.content().text().length());
            }
            return eventDetails;
        };

        auto prepareRequestParameters = [](const YandexQuery::ControlQueryRequest& request) {
            yandex::cloud::events::yq::ControlQuery::RequestParameters requestParameters;
            requestParameters.set_query_id(request.query_id());
            requestParameters.set_action(YandexQuery::QueryAction_Name(request.action()));
            return requestParameters;
        };

        Register(new TAuditEventSenderActor<yandex::cloud::events::yq::ControlQuery, YandexQuery::ControlQueryRequest, YandexQuery::Query>(
            Session,
            AuditServiceSensors,
            prepareEventDetails,
            prepareRequestParameters,
            "yandex.cloud.events.yq.ControlQuery",
            "ControlQuery",
            std::move(auditReport.ExtraInfo),
            std::move(auditReport.Request),
            std::move(auditReport.Issues),
            std::move(auditReport.Details),
            std::move(auditReport.EventId)
        ));
    }

    void Handle(TEvAuditService::ModifyQueryAuditReport::TPtr& ev) {
        auto& auditReport = *ev.Get()->Get();

        auto prepareEventDetails = [](const TAuditDetails<YandexQuery::Query>& details) {
            yandex::cloud::events::yq::UpdateQuery::EventDetails eventDetails;
            if (details.After) {
                const auto& after = *details.After;
                eventDetails.set_query_id(after.meta().common().id());
                eventDetails.set_name(after.content().name());
                eventDetails.set_visibility(YandexQuery::Acl::Visibility_Name(after.content().acl().visibility()));
                eventDetails.set_type(YandexQuery::QueryContent::QueryType_Name(after.content().type()));
                eventDetails.set_execute_mode(YandexQuery::ExecuteMode_Name(after.meta().execute_mode()));
                eventDetails.set_compute_status(YandexQuery::QueryMeta::ComputeStatus_Name(after.meta().status()));
                eventDetails.set_text_length(after.content().text().length());
            }
            return eventDetails;
        };

        auto prepareRequestParameters = [](const YandexQuery::ModifyQueryRequest& request) {
            yandex::cloud::events::yq::UpdateQuery::RequestParameters requestParameters;
            requestParameters.set_query_id(request.query_id());
            requestParameters.set_name(request.content().name());
            requestParameters.set_visibility(YandexQuery::Acl::Visibility_Name(request.content().acl().visibility()));
            requestParameters.set_type(YandexQuery::QueryContent::QueryType_Name(request.content().type()));
            requestParameters.set_execute_mode(YandexQuery::ExecuteMode_Name(request.execute_mode()));
            requestParameters.set_state_load_mode(YandexQuery::StateLoadMode_Name(request.state_load_mode()));
            requestParameters.set_text_length(request.content().text().length());
            return requestParameters;
        };

        Register(new TAuditEventSenderActor<yandex::cloud::events::yq::UpdateQuery, YandexQuery::ModifyQueryRequest, YandexQuery::Query>(
            Session,
            AuditServiceSensors,
            prepareEventDetails,
            prepareRequestParameters,
            "yandex.cloud.events.yq.UpdateQuery",
            "ModifyQuery",
            std::move(auditReport.ExtraInfo),
            std::move(auditReport.Request),
            std::move(auditReport.Issues),
            std::move(auditReport.Details),
            std::move(auditReport.EventId)
        ));
    }

    void Handle(TEvAuditService::DeleteQueryAuditReport::TPtr& ev) {
        auto& auditReport = *ev.Get()->Get();

        auto prepareEventDetails = [](const TAuditDetails<YandexQuery::Query>& details) {
            yandex::cloud::events::yq::DeleteQuery::EventDetails eventDetails;
            if (details.Before) {
                const auto& before = *details.Before;
                eventDetails.set_query_id(before.meta().common().id());
                eventDetails.set_name(before.content().name());
                eventDetails.set_visibility(YandexQuery::Acl::Visibility_Name(before.content().acl().visibility()));
                eventDetails.set_type(YandexQuery::QueryContent::QueryType_Name(before.content().type()));
                eventDetails.set_execute_mode(YandexQuery::ExecuteMode_Name(before.meta().execute_mode()));
                eventDetails.set_compute_status(YandexQuery::QueryMeta::ComputeStatus_Name(before.meta().status()));
                eventDetails.set_text_length(before.content().text().length());
            }
            return eventDetails;
        };

        auto prepareRequestParameters = [](const YandexQuery::DeleteQueryRequest& request) {
            yandex::cloud::events::yq::DeleteQuery::RequestParameters requestParameters;
            requestParameters.set_query_id(request.query_id());
            return requestParameters;
        };

        Register(new TAuditEventSenderActor<yandex::cloud::events::yq::DeleteQuery, YandexQuery::DeleteQueryRequest, YandexQuery::Query>(
            Session,
            AuditServiceSensors,
            prepareEventDetails,
            prepareRequestParameters,
            "yandex.cloud.events.yq.DeleteQuery",
            "DeleteQuery",
            std::move(auditReport.ExtraInfo),
            std::move(auditReport.Request),
            std::move(auditReport.Issues),
            std::move(auditReport.Details),
            std::move(auditReport.EventId)
        ));
    }

private:
    NConfig::TAuditConfig Config;
    std::shared_ptr<TAuditServiceSensors> AuditServiceSensors;
    std::unique_ptr<TLog> SdkLogger;
    NUnifiedAgent::TClientSessionPtr Session;
};

NActors::IActor* CreateYqCloudAuditServiceActor(const NConfig::TAuditConfig& config, const NMonitoring::TDynamicCounterPtr& counters) {
    return new TYqCloudAuditServiceActor(config, counters);
}

} // namespace NYq
