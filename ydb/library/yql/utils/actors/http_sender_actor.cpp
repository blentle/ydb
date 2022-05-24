#include "http_sender_actor.h"
#include "http_sender.h"

#include <library/cpp/actors/core/events.h>

constexpr std::optional<ui32> MaxRetries = std::nullopt;
constexpr TDuration BaseRetryDelay = TDuration::Seconds(1);
constexpr double ScaleFactor = 1.5;
constexpr TDuration MaxDelay = TDuration::Seconds(10);

TDuration RandomizeDelay(TDuration baseDelay) {
    const TDuration::TValue half = baseDelay.GetValue() / 2;
    return TDuration::FromValue(half + RandomNumber<TDuration::TValue>(half));
}

using namespace NActors;

namespace NYql::NDq {

class THttpSenderActor : public NActors::TActor<THttpSenderActor> {
public:
    static constexpr char ActorName[] = "YQL_SOLOMON_HTTP_SENDER_ACTOR";

    THttpSenderActor(
            const TActorId senderId,
            const TActorId httpProxyId)
        : TActor<THttpSenderActor>(&THttpSenderActor::StateFunc)
        , HttpProxyId(httpProxyId)
        , SenderId(senderId)
    { }

private:
    STRICT_STFUNC(StateFunc,
        hFunc(NHttp::TEvHttpProxy::TEvHttpOutgoingRequest, Handle);
        hFunc(NHttp::TEvHttpProxy::TEvHttpIncomingResponse, Handle);
        hFunc(TEvents::TEvPoison, Handle);
        hFunc(TEvents::TEvWakeup, Handle);
    )

    void SendRequestToProxy() {
        Send(HttpProxyId, new NHttp::TEvHttpProxy::TEvHttpOutgoingRequest(Request->Duplicate(), Timeout));
    }

    void Handle(NHttp::TEvHttpProxy::TEvHttpOutgoingRequest::TPtr& ev) {
        Request = ev->Get()->Request;
        Timeout = ev->Get()->Timeout;
        Cookie = ev->Cookie;
        SendRequestToProxy();
    }

    void Handle(NHttp::TEvHttpProxy::TEvHttpIncomingResponse::TPtr& ev) {
        const auto* res = ev->Get();
        const TString& error = res->GetError();

        const bool isTerminal = error.empty() || MaxRetries && RetryCount >= *MaxRetries;
        Send(SenderId, new TEvHttpBase::TEvSendResult(ev, RetryCount++, isTerminal), /*flags=*/0, Cookie);

        if (isTerminal) {
            PassAway();
            return;
        }

        Schedule(GetRetryDelay(), new TEvents::TEvWakeup());
    }

    void Handle(TEvents::TEvPoison::TPtr&) {
        PassAway();
    }

    void Handle(TEvents::TEvWakeup::TPtr&) {
        SendRequestToProxy();
    }

private:
    TDuration GetRetryDelay() {
        const TDuration delay = RandomizeDelay(CurrentDelay);
        CurrentDelay = Min(CurrentDelay * ScaleFactor, MaxDelay);
        return delay;
    }

private:
    const TActorId HttpProxyId;
    const TActorId SenderId;

    ui32 RetryCount = 0;
    TDuration CurrentDelay = BaseRetryDelay;
    NHttp::THttpOutgoingRequestPtr Request;
    TDuration Timeout;
    ui64 Cookie = 0;
};

NActors::IActor* CreateHttpSenderActor(TActorId senderId, TActorId httpProxyId) {
    return new THttpSenderActor(senderId, httpProxyId);
}

} // NYql::NDq
