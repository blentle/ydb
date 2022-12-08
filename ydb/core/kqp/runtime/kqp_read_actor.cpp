#include "kqp_read_actor.h"

#include <ydb/core/kqp/runtime/kqp_scan_data.h>
#include <ydb/core/base/tablet_pipecache.h>
#include <ydb/core/engine/minikql/minikql_engine_host.h>
#include <ydb/core/kqp/common/kqp_yql.h>
#include <ydb/core/protos/tx_datashard.pb.h>
#include <ydb/core/tx/datashard/datashard.h>
#include <ydb/core/tx/datashard/range_ops.h>
#include <ydb/core/tx/scheme_cache/scheme_cache.h>

#include <ydb/library/yql/dq/actors/compute/dq_compute_actor_impl.h>

#include <library/cpp/actors/core/interconnect.h>
#include <library/cpp/actors/core/actorsystem.h>

#include <util/generic/intrlist.h>

namespace {

static constexpr ui64 EVREAD_MAX_ROWS = 32767;
static constexpr ui64 EVREAD_MAX_BYTES = 200_MB;

static constexpr ui64 MAX_SHARD_RETRIES = 5;
static constexpr ui64 MAX_SHARD_RESOLVES = 3;

bool IsDebugLogEnabled(const NActors::TActorSystem* actorSystem, NActors::NLog::EComponent component) {
    auto* settings = actorSystem->LoggerSettings();
    return settings && settings->Satisfies(NActors::NLog::EPriority::PRI_DEBUG, component);
}

}

namespace NKikimr {
namespace NKqp {

using namespace NYql;
using namespace NYql::NDq;
using namespace NKikimr;
using namespace NKikimr::NDataShard;


class TKqpReadActor : public TActorBootstrapped<TKqpReadActor>, public NYql::NDq::IDqComputeActorAsyncInput {
    using TBase = TActorBootstrapped<TKqpReadActor>;
public:
    struct TShardState : public TIntrusiveListItem<TShardState> {
        TSmallVec<TSerializedTableRange> Ranges;
        TSmallVec<TSerializedCellVec> Points;

        TOwnedCellVec LastKey;
        ui32 FirstUnprocessedRequest = 0;
        TMaybe<ui32> ReadId;
        ui64 TabletId;

        size_t ResolveAttempt = 0;
        size_t RetryAttempt = 0;

        bool NeedResolve = false;

        TShardState(ui64 tabletId)
            : TabletId(tabletId)
        {
        }

        TTableRange GetBounds() {
            if (Ranges.empty()) {
                YQL_ENSURE(!Points.empty());
                return TTableRange(
                    Points.front().GetCells(), true,
                    Points.back().GetCells(), true);
            } else {
                return TTableRange(
                    Ranges.front().From.GetCells(), Ranges.front().FromInclusive,
                    Ranges.back().To.GetCells(), Ranges.back().ToInclusive);
            }
        }

        static void MakePrefixRange(TSerializedTableRange& range, size_t keyColumns) {
            if (keyColumns == 0) {
                return;
            }
            bool fromInclusive = range.FromInclusive;
            TConstArrayRef<TCell> from = range.From.GetCells();

            bool toInclusive = range.ToInclusive;
            TConstArrayRef<TCell> to = range.To.GetCells();

            bool noop = true;
            // Recognize and remove padding made here https://a.yandex-team.ru/arcadia/ydb/core/kqp/executer/kqp_partition_helper.cpp?rev=r10109549#L284

            // Absent cells mean infinity. So in prefix notation `From` should be exclusive.
            // For example x >= (Key1, Key2, +infinity) is equivalent to x > (Key1, Key2) where x is arbitrary tuple
            if (range.From.GetCells().size() < keyColumns) {
                fromInclusive = false;
                noop = range.FromInclusive;
            } else if (range.FromInclusive) {
                // Nulls are minimum values so we should remove null padding.
                // x >= (Key1, Key2, null) is equivalent to x >= (Key1, Key2)
                ssize_t i = range.From.GetCells().size();
                while (i > 0 && range.From.GetCells()[i - 1].IsNull()) {
                    --i;
                    noop = false;
                }
                from = range.From.GetCells().subspan(0, i);
            }

            // Absent cells mean infinity. So in prefix notation `To` should be inclusive.
            // For example x < (Key1, Key2, +infinity) is equivalent to x <= (Key1, Key2) where x is arbitrary tuple
            if (range.To.GetCells().size() < keyColumns) {
                toInclusive = true;
                // Nulls are minimum values so we should remove null padding.
                // For example x < (Key1, Key2, null) is equivalent to x < (Key1, Key2)
                ssize_t i = range.To.GetCells().size();
                while (i > 0 && range.To.GetCells()[i - 1].IsNull()) {
                    --i;
                    noop = false;
                }
                to = range.To.GetCells().subspan(0, i);
            }

            if (!noop) {
                return;
            }

            range = TSerializedTableRange(from, fromInclusive, to, toInclusive);
        }

        void FillUnprocessedRanges(
            TVector<TSerializedTableRange>& result,
            TConstArrayRef<NScheme::TTypeInfo> keyTypes) const
        {
            // Form new vector. Skip ranges already read.
            bool lastKeyEmpty = LastKey.DataSize() == 0;

            if (!lastKeyEmpty) {
                YQL_ENSURE(keyTypes.size() == LastKey.size(), "Key columns size != last key");
            }

            auto rangeIt = Ranges.begin() + FirstUnprocessedRequest;

            if (!lastKeyEmpty) {
                // It is range, where read was interrupted. Restart operation from last read key.
                result.emplace_back(std::move(TSerializedTableRange(
                    TSerializedCellVec::Serialize(LastKey), rangeIt->To.GetBuffer(), false, rangeIt->ToInclusive
                    )));
                ++rangeIt;
            }

            // And push all others
            result.insert(result.end(), rangeIt, Ranges.end());
            for (auto& range : result) {
                MakePrefixRange(range, keyTypes.size());
            }
        }

        void FillUnprocessedPoints(TVector<TSerializedCellVec>& result) const {
            result.insert(result.begin(), Points.begin() + FirstUnprocessedRequest, Points.end());
        }

        void FillEvRead(TEvDataShard::TEvRead& ev, TConstArrayRef<NScheme::TTypeInfo> keyTypes) {
            if (Ranges.empty()) {
                FillUnprocessedPoints(ev.Keys);
            } else {
                FillUnprocessedRanges(ev.Ranges, keyTypes);
            }
        }

        TString ToString(TConstArrayRef<NScheme::TTypeInfo> keyTypes) const {
            TStringBuilder sb;
            sb << "TShardState{ TabletId: " << TabletId << ", Last Key " << PrintLastKey(keyTypes)
                << ", Ranges: [";
            for (size_t i = 0; i < Ranges.size(); ++i) {
                sb << "#" << i << ": " << DebugPrintRange(keyTypes, Ranges[i].ToTableRange(), *AppData()->TypeRegistry);
                if (i + 1 != Ranges.size()) {
                    sb << ", ";
                }
            }
            sb << "], "
                << ", RetryAttempt: " << RetryAttempt << ", ResolveAttempt: " << ResolveAttempt << " }";
            return sb;
        }

        TString PrintLastKey(TConstArrayRef<NScheme::TTypeInfo> keyTypes) const {
            if (LastKey.empty()) {
                return "<none>";
            }
            return DebugPrintPoint(keyTypes, LastKey, *AppData()->TypeRegistry);
        }
    };

    using TShardQueue = TIntrusiveListWithAutoDelete<TShardState, TDelete>;

    struct TReadState {
        TShardState* Shard = nullptr;
        bool Finished = false;
        ui64 LastSeqNo;
        TMaybe<TString> SerializedContinuationToken;

        void RegisterMessage(const TEvDataShard::TEvReadResult& result) {
            LastSeqNo = result.Record.GetSeqNo();
            Finished = result.Record.GetFinished();
        }

        bool IsLastMessage(const TEvDataShard::TEvReadResult& result) {
            return result.Record.GetFinished() || (Finished && result.Record.GetSeqNo() == LastSeqNo);
        }

        operator bool () {
            return Shard;
        }

        void Reset() {
            Shard = nullptr;
        }
    };

public:
    TKqpReadActor(NKikimrTxDataShard::TKqpReadRangesSourceSettings&& settings, const NYql::NDq::TDqAsyncIoFactory::TSourceArguments& args)
        : Settings(std::move(settings))
        , LogPrefix(TStringBuilder() << "SelfId: " << this->SelfId() << ", TxId: " << args.TxId << ", task: " << args.TaskId << ". ")
        , ComputeActorId(args.ComputeActorId)
        , InputIndex(args.InputIndex)
        , TypeEnv(args.TypeEnv)
        , HolderFactory(args.HolderFactory)
    {
        TableId = TTableId(
            Settings.GetTable().GetTableId().GetOwnerId(),
            Settings.GetTable().GetTableId().GetTableId(),
            Settings.GetTable().GetSysViewInfo(),
            Settings.GetTable().GetTableId().GetSchemaVersion()
        );

        KeyColumnTypes.reserve(Settings.GetKeyColumnTypes().size());
        for (auto typeId : Settings.GetKeyColumnTypes()) {
            KeyColumnTypes.push_back(NScheme::TTypeInfo((NScheme::TTypeId)typeId));
        }
    }

    STFUNC(ReadyState) {
        Y_UNUSED(ctx);
        try {
            switch (ev->GetTypeRewrite()) {
                hFunc(TEvDataShard::TEvReadResult, HandleRead);
                hFunc(TEvTxProxySchemeCache::TEvResolveKeySetResult, HandleResolve);
                hFunc(TEvPipeCache::TEvDeliveryProblem, HandleError);
                IgnoreFunc(TEvInterconnect::TEvNodeConnected);
                IgnoreFunc(TEvTxProxySchemeCache::TEvInvalidateTableResult);
            }
        } catch (const yexception& e) {
            RuntimeError(e.what(), NYql::NDqProto::StatusIds::INTERNAL_ERROR);
        }
    }

    void Bootstrap() {
        THolder<TShardState> stateHolder = MakeHolder<TShardState>(Settings.GetShardIdHint());
        PendingShards.PushBack(stateHolder.Get());
        auto& state = *stateHolder.Release();

        if (Settings.HasFullRange()) {
            state.Ranges.push_back(TSerializedTableRange(Settings.GetFullRange()));
        } else {
            YQL_ENSURE(Settings.HasRanges());
            if (Settings.GetRanges().KeyRangesSize() > 0) {
                YQL_ENSURE(Settings.GetRanges().KeyPointsSize() == 0);
                for (const auto& range : Settings.GetRanges().GetKeyRanges()) {
                    state.Ranges.push_back(TSerializedTableRange(range));
                }
            } else {
                for (const auto& point : Settings.GetRanges().GetKeyPoints()) {
                    state.Points.push_back(TSerializedCellVec(point));
                }
            }
        }

        if (!Settings.HasShardIdHint()) {
            state.NeedResolve = true;
            ResolveShard(&state);
        } else {
            StartTableScan();
        }
        Become(&TKqpReadActor::ReadyState);
    }

    bool StartTableScan() {
        const ui32 maxAllowedInFlight = MaxInFlight;
        bool isFirst = true;
        while (!PendingShards.Empty() && RunningReads() + 1 <= maxAllowedInFlight) {
            if (isFirst) {
                CA_LOG_D("BEFORE: " << PendingShards.Size() << "." << RunningReads());
                isFirst = false;
            }
            auto state = THolder<TShardState>(PendingShards.PopFront());
            InFlightShards.PushFront(state.Get());
            StartRead(state.Release());
        }
        if (!isFirst) {
            CA_LOG_D("AFTER: " << PendingShards.Size() << "." << RunningReads());
        }

        CA_LOG_D("Scheduled table scans, in flight: " << RunningReads() << " shards. "
            << "pending shards to read: " << PendingShards.Size() << ", ");

        return RunningReads() > 0 || !PendingShards.Empty();
    }

    void ResolveShard(TShardState* state) {
        if (state->ResolveAttempt >= MAX_SHARD_RESOLVES) {
            RuntimeError(TStringBuilder() << "Table '" << Settings.GetTable().GetTablePath() << "' resolve limit exceeded",
                NDqProto::StatusIds::UNAVAILABLE);
            return;
        }

        state->ResolveAttempt++;

        auto range = state->GetBounds();
        TVector<TKeyDesc::TColumnOp> columns;
        columns.reserve(Settings.GetColumns().size());
        for (const auto& column : Settings.GetColumns()) {
            TKeyDesc::TColumnOp op;
            op.Column = column.GetId();
            op.Operation = TKeyDesc::EColumnOperation::Read;
            op.ExpectedType = NScheme::TTypeInfo((NScheme::TTypeId)column.GetType());
            columns.emplace_back(std::move(op));
        }

        auto keyDesc = MakeHolder<TKeyDesc>(TableId, range, TKeyDesc::ERowOperation::Read,
            KeyColumnTypes, columns);

        CA_LOG_D("Sending TEvResolveKeySet update for table '" << Settings.GetTable().GetTablePath() << "'"
            << ", range: " << DebugPrintRange(KeyColumnTypes, range, *AppData()->TypeRegistry)
            << ", attempt #" << state->ResolveAttempt);

        auto request = MakeHolder<NSchemeCache::TSchemeCacheRequest>();
        request->ResultSet.emplace_back(std::move(keyDesc));

        request->ResultSet.front().UserData = ResolveShardId;
        ResolveShards[ResolveShardId] = state;
        ResolveShardId += 1;

        Send(MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvInvalidateTable(TableId, {}));
        Send(MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvResolveKeySet(request));
    }

    void HandleResolve(TEvTxProxySchemeCache::TEvResolveKeySetResult::TPtr& ev) {
        CA_LOG_D("Received TEvResolveKeySetResult update for table '" << Settings.GetTable().GetTablePath() << "'");

        auto* request = ev->Get()->Request.Get();
        if (request->ErrorCount > 0) {
            CA_LOG_E("Resolve request failed for table '" << Settings.GetTable().GetTablePath() << "', ErrorCount# " << request->ErrorCount);

            auto statusCode = NDqProto::StatusIds::UNAVAILABLE;
            auto issueCode = TIssuesIds::KIKIMR_TEMPORARILY_UNAVAILABLE;
            TString error;

            for (const auto& x : request->ResultSet) {
                if ((ui32)x.Status < (ui32)NSchemeCache::TSchemeCacheRequest::EStatus::OkScheme) {
                    // invalidate table
                    Send(MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvInvalidateTable(TableId, {}));

                    switch (x.Status) {
                        case NSchemeCache::TSchemeCacheRequest::EStatus::PathErrorNotExist:
                            statusCode = NDqProto::StatusIds::SCHEME_ERROR;
                            issueCode = TIssuesIds::KIKIMR_SCHEME_MISMATCH;
                            error = TStringBuilder() << "Table '" << Settings.GetTable().GetTablePath() << "' not exists.";
                            break;
                        case NSchemeCache::TSchemeCacheRequest::EStatus::TypeCheckError:
                            statusCode = NDqProto::StatusIds::SCHEME_ERROR;
                            issueCode = TIssuesIds::KIKIMR_SCHEME_MISMATCH;
                            error = TStringBuilder() << "Table '" << Settings.GetTable().GetTablePath() << "' scheme changed.";
                            break;
                        case NSchemeCache::TSchemeCacheRequest::EStatus::LookupError:
                            statusCode = NDqProto::StatusIds::UNAVAILABLE;
                            issueCode = TIssuesIds::KIKIMR_TEMPORARILY_UNAVAILABLE;
                            error = TStringBuilder() << "Failed to resolve table '" << Settings.GetTable().GetTablePath() << "'.";
                            break;
                        default:
                            statusCode = NDqProto::StatusIds::SCHEME_ERROR;
                            issueCode = TIssuesIds::KIKIMR_SCHEME_MISMATCH;
                            error = TStringBuilder() << "Unresolved table '" << Settings.GetTable().GetTablePath() << "'. Status: " << x.Status;
                            break;
                    }
                }
            }

            return RuntimeError(error, statusCode);
        }

        auto keyDesc = std::move(request->ResultSet[0].KeyDescription);
        THolder<TShardState> state;
        if (auto ptr = ResolveShards[request->ResultSet[0].UserData]) {
            state = THolder<TShardState>(ptr);
            ResolveShards.erase(request->ResultSet[0].UserData);
        } else {
            return;
        }

        if (keyDesc->GetPartitions().size() == 1 && !state->NeedResolve) {
            // we re-resolved the same shard
            RuntimeError(TStringBuilder() << "too many retries for shard " << state->TabletId, NDqProto::StatusIds::StatusIds::INTERNAL_ERROR);
            PendingShards.PushBack(state.Release());
            return;
        }

        if (keyDesc->GetPartitions().empty()) {
            TString error = TStringBuilder() << "No partitions to read from '" << Settings.GetTable().GetTablePath() << "'";
            CA_LOG_E(error);
            return RuntimeError(error, NDqProto::StatusIds::SCHEME_ERROR);
        }

        const auto& tr = *AppData()->TypeRegistry;

        TVector<THolder<TShardState>> newShards;
        newShards.reserve(keyDesc->GetPartitions().size());

        for (ui64 idx = 0, i = 0; idx < keyDesc->GetPartitions().size(); ++idx) {
            const auto& partition = keyDesc->GetPartitions()[idx];

            TTableRange partitionRange{
                idx == 0 ? state->Ranges.front().From.GetCells() : keyDesc->GetPartitions()[idx - 1].Range->EndKeyPrefix.GetCells(),
                idx == 0 ? state->Ranges.front().FromInclusive : !keyDesc->GetPartitions()[idx - 1].Range->IsInclusive,
                keyDesc->GetPartitions()[idx].Range->EndKeyPrefix.GetCells(),
                keyDesc->GetPartitions()[idx].Range->IsInclusive
            };

            CA_LOG_D("Processing resolved ShardId# " << partition.ShardId
                << ", partition range: " << DebugPrintRange(KeyColumnTypes, partitionRange, tr)
                << ", i: " << i << ", state ranges: " << state->Ranges.size());

            auto newShard = MakeHolder<TShardState>(partition.ShardId);

            for (ui64 j = i; j < state->Ranges.size(); ++j) {
                CA_LOG_D("Intersect state range #" << j << " " << DebugPrintRange(KeyColumnTypes, state->Ranges[j].ToTableRange(), tr)
                    << " with partition range " << DebugPrintRange(KeyColumnTypes, partitionRange, tr));

                auto intersection = Intersect(KeyColumnTypes, partitionRange, state->Ranges[j].ToTableRange());

                if (!intersection.IsEmptyRange(KeyColumnTypes)) {
                    CA_LOG_D("Add range to new shardId: " << partition.ShardId
                        << ", range: " << DebugPrintRange(KeyColumnTypes, intersection, tr));

                    newShard->Ranges.emplace_back(TSerializedTableRange(intersection));
                } else {
                    CA_LOG_D("empty intersection");
                    if (j > i) {
                        i = j - 1;
                    }
                    break;
                }
            }

            if (!newShard->Ranges.empty()) {
                newShards.push_back(std::move(newShard));
            }
        }

        YQL_ENSURE(!newShards.empty());
        for (int i = newShards.ysize() - 1; i >= 0; --i) {
            PendingShards.PushFront(newShards[i].Release());
        }

        if (!state->LastKey.empty()) {
            PendingShards.Front()->LastKey = std::move(state->LastKey);
        }

        if (IsDebugLogEnabled(TlsActivationContext->ActorSystem(), NKikimrServices::KQP_COMPUTE)
            && PendingShards.Size() + RunningReads() > 0)
        {
            TStringBuilder sb;
            if (!PendingShards.Empty()) {
                sb << "Pending shards States: ";
                for (auto& st : PendingShards) {
                    sb << st.ToString(KeyColumnTypes) << "; ";
                }
            }

            if (!InFlightShards.Empty()) {
                sb << "In Flight shards States: ";
                for (auto& st : InFlightShards) {
                    sb << st.ToString(KeyColumnTypes) << "; ";
                }
            }
            CA_LOG_D(sb);
        }
        StartTableScan();
    }

    void RetryRead(ui64 id) {
        if (!Reads[id]) {
            return;
        }

        auto state = Reads[id].Shard;
        Reads[id].Finished = true;

        state->RetryAttempt += 1;
        if (state->RetryAttempt >= MAX_SHARD_RETRIES) {
            return ResolveShard(state);
        }
        CA_LOG_D("Retrying read #" << id);

        auto cancel = MakeHolder<TEvDataShard::TEvReadCancel>();
        cancel->Record.SetReadId(id);
        Send(MakePipePeNodeCacheID(false), new TEvPipeCache::TEvForward(cancel.Release(), state->TabletId), IEventHandle::FlagTrackDelivery);

        if (Reads[id].SerializedContinuationToken) {
            NKikimrTxDataShard::TReadContinuationToken token;
            Y_VERIFY(token.ParseFromString(*(Reads[id].SerializedContinuationToken)), "Failed to parse continuation token");
            state->FirstUnprocessedRequest = token.GetFirstUnprocessedQuery();

            if (token.GetLastProcessedKey()) {
                TSerializedCellVec vec(token.GetLastProcessedKey());
                state->LastKey = TOwnedCellVec(vec.GetCells());
            }
        }

        StartRead(state);
    }

    void StartRead(TShardState* state) {
        THolder<TEvDataShard::TEvRead> ev(new TEvDataShard::TEvRead());
        auto& record = ev->Record;

        state->FillEvRead(*ev, KeyColumnTypes);
        for (const auto& column : Settings.GetColumns()) {
            record.AddColumns(column.GetId());
        }

        {
            record.MutableSnapshot()->SetTxId(Settings.GetSnapshot().GetTxId());
            record.MutableSnapshot()->SetStep(Settings.GetSnapshot().GetStep());
        }

        //if (RuntimeSettings.Timeout) {
        //    ev->Record.SetTimeoutMs(RuntimeSettings.Timeout.Get()->MilliSeconds());
        //}
        //ev->Record.SetStatsMode(RuntimeSettings.StatsMode);
        //ev->Record.SetTxId(std::get<ui64>(TxId));

        auto id = ReadId++;
        Reads.resize(ReadId);
        Reads[id].Shard = state;
        state->ReadId = id;

        record.SetReadId(id);

        record.MutableTableId()->SetOwnerId(Settings.GetTable().GetTableId().GetOwnerId());
        record.MutableTableId()->SetTableId(Settings.GetTable().GetTableId().GetTableId());
        record.MutableTableId()->SetSchemaVersion(Settings.GetTable().GetSchemaVersion());

        record.SetReverse(Settings.GetReverse());
        if (Settings.GetItemsLimit()) {
            record.SetMaxRows(Settings.GetItemsLimit());
        } else {
            record.SetMaxRows(EVREAD_MAX_ROWS);
        }
        record.SetMaxBytes(EVREAD_MAX_BYTES);

        record.SetResultFormat(Settings.GetDataFormat());

        CA_LOG_D(TStringBuilder() << "Send EvRead to shardId: " << state->TabletId << ", tablePath: " << Settings.GetTable().GetTablePath()
            << ", ranges: " << DebugPrintRanges(KeyColumnTypes, ev->Ranges, *AppData()->TypeRegistry)
            << ", readId = " << id);

        ReadIdByTabletId[state->TabletId].push_back(id);
        Send(MakePipePeNodeCacheID(false), new TEvPipeCache::TEvForward(ev.Release(), state->TabletId, true),
            IEventHandle::FlagTrackDelivery);
    }

    void HandleRead(TEvDataShard::TEvReadResult::TPtr ev) {
        const auto& record = ev->Get()->Record;
        auto id = record.GetReadId();
        Y_VERIFY(id < ReadId);
        if (!Reads[id] || Reads[id].Finished) {
            // dropped read
            return;
        }

        Reads[id].SerializedContinuationToken = record.GetContinuationToken();
        if (record.GetStatus().GetCode() != Ydb::StatusIds::SUCCESS) {
            for (auto& issue : record.GetStatus().GetIssues()) {
                CA_LOG_D("read id #" << id << " got issue " << issue.Getmessage());
            }
            return RetryRead(id);
        }

        Reads[id].RegisterMessage(*ev->Get());

        YQL_ENSURE(record.GetResultFormat() == NKikimrTxDataShard::EScanDataFormat::CELLVEC);

        Results.push({Reads[id].Shard->TabletId, THolder<TEventHandle<TEvDataShard::TEvReadResult>>(ev.Release())});
        CA_LOG_D(TStringBuilder() << "new data for read #" << id << " pushed");
        Send(ComputeActorId, new TEvNewAsyncInputDataArrived(InputIndex));
    }

    void HandleError(TEvPipeCache::TEvDeliveryProblem::TPtr& ev) {
        auto& msg = *ev->Get();

        for (auto& read : ReadIdByTabletId[msg.TabletId]) {
            CA_LOG_W("Got EvDeliveryProblem, TabletId: " << msg.TabletId << ", NotDelivered: " << msg.NotDelivered);
            RetryRead(read);
        }
    }

    size_t RunningReads() const {
        return Reads.size() - ResetReads;
    }

    ui64 GetInputIndex() const override {
        return InputIndex;
    }

    NMiniKQL::TBytesStatistics GetRowSize(const NUdf::TUnboxedValue* row) {
        NMiniKQL::TBytesStatistics rowStats{0, 0};
        for (size_t i = 0; i < Settings.ColumnsSize(); ++i) {
            if (IsSystemColumn(Settings.GetColumns(i).GetId())) {
                rowStats.AllocatedBytes += sizeof(NUdf::TUnboxedValue);
            } else {
                rowStats.AddStatistics(NMiniKQL::GetUnboxedValueSize(row[i], NScheme::TTypeInfo((NScheme::TTypeId)Settings.GetColumns(i).GetType())));
            }
        }
        if (Settings.ColumnsSize() == 0) {
            rowStats.AddStatistics({sizeof(ui64), sizeof(ui64)});
        }
        return rowStats;
    }

    TGuard<NKikimr::NMiniKQL::TScopedAlloc> BindAllocator() {
        return TypeEnv.BindAllocator();
    }

    NMiniKQL::TBytesStatistics PackArrow(
        THolder<TEventHandle<TEvDataShard::TEvReadResult>>& result,
        ui64 shardId,
        NKikimr::NMiniKQL::TUnboxedValueVector& batch)
    {
        NMiniKQL::TBytesStatistics stats;
        bool hasResultColumns = false;
        if (Settings.ColumnsSize() == 0) {
            batch.resize(result->Get()->GetRowsCount(), HolderFactory.GetEmptyContainer());
        } else {
            TVector<NUdf::TUnboxedValue*> editAccessors(result->Get()->GetRowsCount());
            batch.reserve(result->Get()->GetRowsCount());

            for (ui64 rowIndex = 0; rowIndex < result->Get()->GetRowsCount(); ++rowIndex) {
                batch.emplace_back(HolderFactory.CreateDirectArrayHolder(
                    Settings.columns_size(),
                    editAccessors[rowIndex])
                );
            }

            for (size_t columnIndex = 0; columnIndex < Settings.ColumnsSize(); ++columnIndex) {
                auto tag = Settings.GetColumns(columnIndex).GetId();
                auto type = NScheme::TTypeInfo((NScheme::TTypeId)Settings.GetColumns(columnIndex).GetType());
                if (IsSystemColumn(tag)) {
                    for (ui64 rowIndex = 0; rowIndex < result->Get()->GetRowsCount(); ++rowIndex) {
                        NMiniKQL::FillSystemColumn(editAccessors[rowIndex][columnIndex], shardId, tag, type);
                        stats.AllocatedBytes += sizeof(NUdf::TUnboxedValue);
                    }
                } else {
                    hasResultColumns = true;
                    stats.AddStatistics(
                        NMiniKQL::WriteColumnValuesFromArrow(editAccessors, *result->Get()->ArrowBatch, columnIndex, type)
                    );
                }
            }
        }

        if (!hasResultColumns) {
            auto rowsCnt = result->Get()->GetRowsCount();
            stats.AddStatistics({sizeof(ui64) * rowsCnt, sizeof(ui64) * rowsCnt});
        }
        return stats;
    }

    NMiniKQL::TBytesStatistics PackCells(
        THolder<TEventHandle<TEvDataShard::TEvReadResult>>& result,
        ui64 shardId,
        NKikimr::NMiniKQL::TUnboxedValueVector& batch)
    {
        NMiniKQL::TBytesStatistics stats;
        batch.reserve(batch.size());
        for (size_t rowIndex = 0; rowIndex < result->Get()->GetRowsCount(); ++rowIndex) {
            const auto& row = result->Get()->GetCells(rowIndex);
            NUdf::TUnboxedValue* rowItems = nullptr;
            batch.emplace_back(HolderFactory.CreateDirectArrayHolder(Settings.ColumnsSize(), rowItems));
            for (size_t i = 0; i < Settings.ColumnsSize(); ++i) {
                auto tag = Settings.GetColumns(i).GetId();
                auto type = NScheme::TTypeInfo((NScheme::TTypeId)Settings.GetColumns(i).GetType());
                if (IsSystemColumn(tag)) {
                    NMiniKQL::FillSystemColumn(rowItems[i], shardId, tag, type);
                } else {
                    rowItems[i] = NMiniKQL::GetCellValue(row[i], type);
                }
            }
            stats.AddStatistics(GetRowSize(rowItems));
        }
        return stats;
    }

    i64 GetAsyncInputData(
        NKikimr::NMiniKQL::TUnboxedValueVector& resultVector,
        TMaybe<TInstant>&,
        bool& finished,
        i64 freeSpace) override
    {
        ui64 bytes = 0;
        while (!Results.empty()) {
            auto& [shardId, result, batch, processedRows] = Results.front();
            auto& msg = *result->Get();
            if (!batch.Defined()) {
                batch.ConstructInPlace();
                switch (msg.Record.GetResultFormat()) {
                    case NKikimrTxDataShard::EScanDataFormat::ARROW:
                        PackArrow(result, shardId, *batch);
                        break;
                    case NKikimrTxDataShard::EScanDataFormat::UNSPECIFIED:
                    case NKikimrTxDataShard::EScanDataFormat::CELLVEC:
                        PackCells(result, shardId, *batch);
                }
            }

            auto id = result->Get()->Record.GetReadId();
            if (!Reads[id]) {
                Results.pop();
                continue;
            }
            auto* state = Reads[id].Shard;

            for (; processedRows < batch->size(); ++processedRows) {
                NMiniKQL::TBytesStatistics rowSize = GetRowSize((*batch)[processedRows].GetElements());
                if (static_cast<ui64>(freeSpace) < bytes + rowSize.AllocatedBytes) {
                    break;
                }
                resultVector.push_back(std::move((*batch)[processedRows]));
                RowCount += 1;
                bytes += rowSize.AllocatedBytes;
            }
            CA_LOG_D(TStringBuilder() << "returned " << resultVector.size() << " rows");

            if (batch->size() == processedRows) {
                auto& record = msg.Record;
                if (Reads[id].IsLastMessage(msg)) {
                    Reads[id].Reset();
                    ResetReads++;
                } else if (!Reads[id].Finished) {
                    THolder<TEvDataShard::TEvReadAck> request(new TEvDataShard::TEvReadAck());
                    request->Record.SetReadId(record.GetReadId());
                    request->Record.SetSeqNo(record.GetSeqNo());
                    request->Record.SetMaxRows(EVREAD_MAX_ROWS);
                    request->Record.SetMaxBytes(EVREAD_MAX_BYTES);
                    Send(MakePipePeNodeCacheID(false), new TEvPipeCache::TEvForward(request.Release(), state->TabletId, true),
                        IEventHandle::FlagTrackDelivery);
                }

                StartTableScan();
                if (PendingShards.Size() > 0) {
                    return bytes;
                }

                Results.pop();
                CA_LOG_D("dropping batch");

                if (RunningReads() == 0 || (Settings.HasItemsLimit() && RowCount >= Settings.GetItemsLimit())) {
                    finished = true;
                }
            } else {
                break;
            }
        }

        return bytes;
    }

    void SaveState(const NYql::NDqProto::TCheckpoint&, NYql::NDqProto::TSourceState&) override {}
    void CommitState(const NYql::NDqProto::TCheckpoint&) override {}
    void LoadState(const NYql::NDqProto::TSourceState&) override {}

    void PassAway() override {
        {
            auto guard = BindAllocator();
            Results.clear();
        }
        TBase::PassAway();
    }

    void RuntimeError(const TString& message, NYql::NDqProto::StatusIds::StatusCode statusCode, const NYql::TIssues& subIssues = {}) {
        NYql::TIssue issue(message);
        for (const auto& i : subIssues) {
            issue.AddSubIssue(MakeIntrusive<NYql::TIssue>(i));
        }

        NYql::TIssues issues;
        issues.AddIssue(std::move(issue));
        Send(ComputeActorId, new TEvAsyncInputError(InputIndex, std::move(issues), statusCode));
    }

private:
    NKikimrTxDataShard::TKqpReadRangesSourceSettings Settings;

    TVector<NScheme::TTypeInfo> KeyColumnTypes;

    size_t RowCount = 0;
    ui64 ResetReads = 0;
    ui64 ReadId = 0;
    TVector<TReadState> Reads;
    THashMap<ui64, TVector<ui32>> ReadIdByTabletId;

    THashMap<ui64, TShardState*> ResolveShards;
    ui64 ResolveShardId = 0;

    TShardQueue InFlightShards;
    TShardQueue PendingShards;

    struct TResult {
        ui64 ShardId;
        THolder<TEventHandle<TEvDataShard::TEvReadResult>> ReadResult;
        TMaybe<NKikimr::NMiniKQL::TUnboxedValueVector> Batch;
        size_t ProcessedRows = 0;

        TResult(ui64 shardId, THolder<TEventHandle<TEvDataShard::TEvReadResult>> readResult)
            : ShardId(shardId)
            , ReadResult(std::move(readResult))
        {
        }
    };
    TQueue<TResult> Results;

    ui32 MaxInFlight = 1024;
    const TString LogPrefix;
    TTableId TableId;

    const TActorId ComputeActorId;
    const ui64 InputIndex;
    const NMiniKQL::TTypeEnvironment& TypeEnv;
    const NMiniKQL::THolderFactory& HolderFactory;
};


void RegisterKqpReadActor(NYql::NDq::TDqAsyncIoFactory& factory) {
    factory.RegisterSource<NKikimrTxDataShard::TKqpReadRangesSourceSettings>(
        TString(NYql::KqpReadRangesSourceName),
        [] (NKikimrTxDataShard::TKqpReadRangesSourceSettings&& settings, NYql::NDq::TDqAsyncIoFactory::TSourceArguments&& args) {
            auto* actor = new TKqpReadActor(std::move(settings), args);
            return std::make_pair<NYql::NDq::IDqComputeActorAsyncInput*, IActor*>(actor, actor);
        });
}

} // namespace NKqp
} // namespace NKikimr
