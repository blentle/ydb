#pragma once

#include "defs.h"

#include "cluster_info.h"

#include <ydb/core/erasure/erasure.h>
#include <ydb/core/protos/cms.pb.h>

namespace NKikimr {
namespace NCms {

using namespace NKikimrCms;

class IErasureCounter {
public:
    virtual ~IErasureCounter() = default;

    virtual bool GroupAlreadyHasLockedDisks(TErrorInfo& error) const = 0;
    virtual bool CheckForMaxAvailability(TErrorInfo& error, TInstant& defaultDeadline) const = 0;
    virtual bool CheckForKeepAvailability(TClusterInfoPtr info, TErrorInfo& error, TInstant& defaultDeadline) const = 0;
    virtual void CountGroupState(TClusterInfoPtr info,  
                                 TDuration retryTime, 
                                 TDuration duration, 
                                 TErrorInfo& error) = 0;
    virtual void CountVDisk(const TVDiskInfo& vdisk,
                            TClusterInfoPtr info,
                            TDuration retryTime,
                            TDuration duration,
                            TErrorInfo& error) = 0;
};

class TErasureCounterBase: public IErasureCounter {
protected:
    ui32 Down;
    ui32 Locked;
    const TVDiskInfo& VDisk;
    const ui32 GroupId;

protected:
    bool IsDown(const TVDiskInfo& vdisk, 
                TClusterInfoPtr info,
                TDuration& retryTime, 
                TErrorInfo& error);
    bool IsLocked(const TVDiskInfo& vdisk, 
                  TClusterInfoPtr info,
                  TDuration& retryTime, 
                  TDuration& duration, 
                  TErrorInfo& error);

public:
    TErasureCounterBase(const TVDiskInfo& vdisk, ui32 groupId)
        : Down(0)
        , Locked(0)
        , VDisk(vdisk)
        , GroupId(groupId)
    {
    }

    bool GroupAlreadyHasLockedDisks(TErrorInfo& error) const final;
    bool CheckForMaxAvailability(TErrorInfo& error, TInstant& defaultDeadline) const final;
};

class TDefaultErasureCounter: public TErasureCounterBase {
public:
    TDefaultErasureCounter(const TVDiskInfo& vdisk, ui32 groupId)
        : TErasureCounterBase(vdisk, groupId)
    {
    }

    void CountGroupState(TClusterInfoPtr info, TDuration retryTime,
                         TDuration duration, TErrorInfo &error) override;
    bool CheckForKeepAvailability(TClusterInfoPtr info, 
                                  TErrorInfo& error, 
                                  TInstant& defaultDeadline) const override;
    void CountVDisk(const TVDiskInfo& vdisk, 
                    TClusterInfoPtr info,
                    TDuration retryTime, 
                    TDuration duration, 
                    TErrorInfo& error) override;
};

class TMirror3dcCounter: public TErasureCounterBase {
private:
    THashMap<ui8, ui32> DataCenterDisabledNodes;

public:
    TMirror3dcCounter(const TVDiskInfo& vdisk, ui32 groupId)
        : TErasureCounterBase(vdisk, groupId)
    {
    }

    void CountGroupState(TClusterInfoPtr info, TDuration retryTime,
                         TDuration duration, TErrorInfo &error) override;
    bool CheckForKeepAvailability(TClusterInfoPtr info, 
                                  TErrorInfo& error, 
                                  TInstant& defaultDeadline) const override;
    void CountVDisk(const TVDiskInfo& vdisk, 
                    TClusterInfoPtr info,
                    TDuration retryTime, 
                    TDuration duration, 
                    TErrorInfo& error) override;
};

TSimpleSharedPtr<IErasureCounter>
CreateErasureCounter(TErasureType::EErasureSpecies es, 
                     const TVDiskInfo& vdisk, ui32 groupId);
}
}
