#pragma once

#include "schemeshard_identificators.h"

#include <ydb/core/base/tablet_types.h>
#include <ydb/core/protos/flat_tx_scheme.pb.h>
#include <ydb/core/tablet_flat/flat_cxx_database.h>
#include <util/generic/fwd.h>

namespace NKikimr {
namespace NSchemeShard {

struct TSchemeLimits {
    // path
    ui64 MaxDepth = 32;
    ui64 MaxPaths = 200*1000;
    ui64 MaxChildrenInDir = 100*1000;
    ui64 MaxAclBytesSize = 10 << 10;
    ui64 MaxPathElementLength = 255;
    TString ExtraPathSymbolsAllowed = "!\"#$%&'()*+,-.:;<=>?@[\\]^_`{|}~";

    // table
    ui64 MaxTableColumns = 200;
    ui64 MaxTableColumnNameLength = 255;
    ui64 MaxTableKeyColumns = 20;
    ui64 MaxTableIndices = 20;
    ui64 MaxTableCdcStreams = 5;
    ui64 MaxShards = 200*1000; // In each database
    ui64 MaxShardsInPath = 35*1000; // In each path in database
    ui64 MaxConsistentCopyTargets = 1000;

    // pq group
    ui64 MaxPQPartitions = 1000000;

    TSchemeLimits() = default;
    explicit TSchemeLimits(const NKikimrScheme::TSchemeLimits& proto);

    NKikimrScheme::TSchemeLimits AsProto() const;
};

using ETabletType = TTabletTypes;


struct TGlobalTimestamp {
    TStepId Step = InvalidStepId;
    TTxId TxId = InvalidTxId;

    TGlobalTimestamp(TStepId step, TTxId txId)
        : Step(step)
        , TxId(txId)
    {}

    bool Empty() const {
        return !bool(Step);
    }

    explicit operator bool () const {
        return !Empty();
    }

    bool operator < (const TGlobalTimestamp& ts) const {
        Y_VERIFY_DEBUG(Step, "Comparing with unset timestamp");
        Y_VERIFY_DEBUG(ts.Step, "Comparing with unset timestamp");
        return Step < ts.Step || Step == ts.Step && TxId < ts.TxId;
    }

    bool operator == (const TGlobalTimestamp& ts) const {
        return Step == ts.Step && TxId == ts.TxId;
    }

    TString ToString() const {
        if (Empty()) {
            return "unset";
        }

        return TStringBuilder()
                << "[" << Step << ":" <<  TxId << "]";
    }
};


enum class ETableColumnDefaultKind : ui32 {
    None = 0,
    FromSequence = 1,
};


enum class EAttachChildResult : ui32 {
    Undefined = 0,

    AttachedAsOnlyOne,

    AttachedAsNewerDeleted,
    RejectAsOlderDeleted,

    AttachedAsActual,
    RejectAsDeleted,

    AttachedAsNewerActual,
    RejectAsOlderActual,

    AttachedAsCreatedActual,
    RejectAsInactive,

    AttachedAsOlderUnCreated,
    RejectAsNewerUnCreated
};

}
}
