#pragma once
#include <ydb/library/yql/dq/common/dq_common.h>
#include <ydb/library/yql/dq/actors/dq_events_ids.h>
#include <ydb/library/yql/minikql/computation/mkql_computation_node_holders.h>
#include <ydb/library/yql/public/issue/yql_issue.h>

#include <util/generic/ptr.h>

#include <memory>
#include <utility>

namespace NYql::NDqProto {
class TCheckpoint;
class TTaskInput;
class TSourceState;
} // namespace NYql::NDqProto

namespace NActors {
class IActor;
} // namespace NActors

namespace NYql::NDq {

// Source/transform.
// Must be IActor.
//
// Protocol:
// 1. CA starts source/transform.
// 2. CA calls IDqComputeActorAsyncInput::GetAsyncInputData(batch, FreeSpace).
// 3. Source/transform sends TEvNewAsyncInputDataArrived when it has data to process.
// 4. CA calls IDqComputeActorAsyncInput::GetAsyncInputData(batch, FreeSpace) to get data when it is ready to process it.
//
// In case of error source/transform sends TEvAsyncInputError
//
// Checkpointing:
// 1. InjectCheckpoint event arrives to CA.
// 2. ...
// 3. CA calls IDqComputeActorAsyncInput::SaveState() and IDqTaskRunner::SaveGraphState() and uses this pair as state for CA.
// 3. ...
// 5. CA calls IDqComputeActorAsyncInput::CommitState() to apply all side effects.
struct IDqComputeActorAsyncInput {
    struct TEvNewAsyncInputDataArrived : public NActors::TEventLocal<TEvNewAsyncInputDataArrived, TDqComputeEvents::EvNewAsyncInputDataArrived> {
        const ui64 InputIndex;
        explicit TEvNewAsyncInputDataArrived(ui64 inputIndex)
            : InputIndex(inputIndex)
        {}
    };

    struct TEvAsyncInputError : public NActors::TEventLocal<TEvAsyncInputError, TDqComputeEvents::EvAsyncInputError> {
        TEvAsyncInputError(ui64 inputIndex, const TIssues& issues, bool isFatal)
            : InputIndex(inputIndex)
            , Issues(issues)
            , IsFatal(isFatal)
        {}

        const ui64 InputIndex;
        const TIssues Issues;
        const bool IsFatal;
    };

    virtual ui64 GetInputIndex() const = 0;

    // Gets data and returns space used by filled data batch.
    // Method should be called under bound mkql allocator.
    // Could throw YQL errors.
    virtual i64 GetAsyncInputData(NKikimr::NMiniKQL::TUnboxedValueVector& batch, bool& finished, i64 freeSpace) = 0;

    // Checkpointing.
    virtual void SaveState(const NDqProto::TCheckpoint& checkpoint, NDqProto::TSourceState& state) = 0;
    virtual void CommitState(const NDqProto::TCheckpoint& checkpoint) = 0; // Apply side effects related to this checkpoint.
    virtual void LoadState(const NDqProto::TSourceState& state) = 0;

    virtual void PassAway() = 0; // The same signature as IActor::PassAway()

    virtual ~IDqComputeActorAsyncInput() = default;
};

struct IDqSourceFactory : public TThrRefBase {
    using TPtr = TIntrusivePtr<IDqSourceFactory>;

    struct TArguments {
        const NDqProto::TTaskInput& InputDesc;
        ui64 InputIndex;
        TTxId TxId;
        const THashMap<TString, TString>& SecureParams;
        const THashMap<TString, TString>& TaskParams;
        const NActors::TActorId& ComputeActorId;
        const NKikimr::NMiniKQL::TTypeEnvironment& TypeEnv;
        const NKikimr::NMiniKQL::THolderFactory& HolderFactory;
    };

    // Creates source.
    // Could throw YQL errors.
    // IActor* and IDqComputeActorAsyncInput* returned by method must point to the objects with consistent lifetime.
    virtual std::pair<IDqComputeActorAsyncInput*, NActors::IActor*> CreateDqSource(TArguments&& args) const = 0;
};

} // namespace NYql::NDq
