#pragma once

#include <ydb/core/protos/kqp.pb.h>
#include <ydb/core/base/appdata.h>

#include <library/cpp/actors/core/actor.h>

namespace NKikimr::NKqp {

struct TEvKqpRunScriptActor {
};

NActors::IActor* CreateRunScriptActor(const TString& executionId, const NKikimrKqp::TEvQueryRequest& request, const TString& database, ui64 leaseGeneration, TDuration leaseDuration,  NKikimrConfig::TQueryServiceConfig queryServiceConfig);

} // namespace NKikimr::NKqp
