#pragma once
#include "agent.h"
#include "client.h"

#include <library/cpp/monlib/dynamic_counters/counters.h>
#include <util/system/mutex.h>
#include <deque>

namespace NKikimr::NColumnShard {

class TCommonCountersOwner {
private:
    ::NMonitoring::TDynamicCounterPtr SubGroup;
    const TString ModuleId;
    TString NormalizeSignalName(const TString& name) const;
protected:
    std::shared_ptr<TValueAggregationAgent> GetValueAutoAggregations(const TString& name) const;
    std::shared_ptr<TValueAggregationClient> GetValueAutoAggregationsClient(const TString& name) const;

    TCommonCountersOwner(const TCommonCountersOwner& sameAs)
        : SubGroup(sameAs.SubGroup)
        , ModuleId(sameAs.ModuleId) {

    }

    TCommonCountersOwner(const TCommonCountersOwner& sameAs, const TString& componentName)
        : SubGroup(sameAs.SubGroup)
        , ModuleId(sameAs.ModuleId)
    {
        DeepSubGroup("component", componentName);
    }
public:
    const TString& GetModuleId() const {
        return ModuleId;
    }

    NMonitoring::TDynamicCounters::TCounterPtr GetAggregationValue(const TString& name) const;
    NMonitoring::TDynamicCounters::TCounterPtr GetValue(const TString& name) const;
    NMonitoring::TDynamicCounters::TCounterPtr GetDeriviative(const TString& name) const;
    void DeepSubGroup(const TString& id, const TString& value);
    void DeepSubGroup(const TString& componentName);
    NMonitoring::THistogramPtr GetHistogram(const TString& name, NMonitoring::IHistogramCollectorPtr&& hCollector) const;

    TCommonCountersOwner(const TString& module, TIntrusivePtr<::NMonitoring::TDynamicCounters> baseSignals = nullptr);
};

}
