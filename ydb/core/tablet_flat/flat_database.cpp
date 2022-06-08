#include "defs.h"
#include "flat_abi_evol.h"
#include "flat_database.h"
#include "flat_redo_writer.h"
#include "flat_redo_player.h"
#include "flat_dbase_naked.h"
#include "flat_dbase_apply.h"
#include "flat_dbase_annex.h"
#include "flat_dbase_sz_env.h"
#include "flat_part_shrink.h"
#include "flat_util_misc.h"
#include "flat_sausage_grind.h"
#include <ydb/core/util/pb.h>
#include <ydb/core/scheme_types/scheme_type_registry.h>
#include <util/generic/cast.h>


#define MAX_REDO_BYTES_PER_COMMIT 268435456U // 256MB


namespace NKikimr {
namespace NTable {

TDatabase::TDatabase(TDatabaseImpl *databaseImpl) noexcept
    : DatabaseImpl(databaseImpl ? databaseImpl : new TDatabaseImpl(0, new TScheme, nullptr))
    , NoMoreReadsFlag(true)
{

}

TDatabase::~TDatabase() { }

const TScheme& TDatabase::GetScheme() const noexcept
{
    return *DatabaseImpl->Scheme;
}

TIntrusiveConstPtr<TRowScheme> TDatabase::GetRowScheme(ui32 table) const noexcept
{
    return Require(table)->GetScheme();
}

TAutoPtr<TTableIt> TDatabase::Iterate(ui32 table, TRawVals key, TTagsRef tags, ELookup mode) const noexcept
{
    Y_VERIFY(!NoMoreReadsFlag, "Trying to read after reads prohibited, table %u", table);

    const auto seekBy = [](TRawVals key, ELookup mode) {
        if (!key && mode != ELookup::ExactMatch) {
            /* Compatability mode with outer iterator interface that yields
             * begin() for non-exact lookups with empty key. Inner iterator
             * yields begin() for ({}, Lower) and end() for ({}, Upper).
             */

            return ESeek::Lower;

        } else {
            switch (mode) {
            case ELookup::ExactMatch:
                return ESeek::Exact;
            case ELookup::GreaterThan:
                return ESeek::Upper;
            case ELookup::GreaterOrEqualThan:
                return ESeek::Lower;
            }
        }

        Y_FAIL("Don't know how to convert ELookup to ESeek mode");
    };

    IteratedTables.insert(table);

    return Require(table)->Iterate(key, tags, Env, seekBy(key, mode), TRowVersion::Max());
}

TAutoPtr<TTableIt> TDatabase::IterateExact(ui32 table, TRawVals key, TTagsRef tags,
        TRowVersion snapshot,
        const ITransactionMapPtr& visible,
        const ITransactionObserverPtr& observer) const noexcept
{
    Y_VERIFY(!NoMoreReadsFlag, "Trying to read after reads prohibited, table %u", table);

    IteratedTables.insert(table);

    auto iter = Require(table)->Iterate(key, tags, Env, ESeek::Exact, snapshot, visible, observer);

    // N.B. ESeek::Exact produces iterators with limit=1

    return iter;
}

TAutoPtr<TTableIt> TDatabase::IterateRange(ui32 table, const TKeyRange& range, TTagsRef tags,
        TRowVersion snapshot,
        const ITransactionMapPtr& visible,
        const ITransactionObserverPtr& observer) const noexcept
{
    Y_VERIFY(!NoMoreReadsFlag, "Trying to read after reads prohibited, table %u", table);

    IteratedTables.insert(table);

    ESeek seek = !range.MinKey || range.MinInclusive ? ESeek::Lower : ESeek::Upper;

    auto iter = Require(table)->Iterate(range.MinKey, tags, Env, seek, snapshot, visible, observer);

    if (range.MaxKey) {
        TCelled maxKey(range.MaxKey, *iter->Scheme->Keys, false);

        if (range.MaxInclusive) {
            iter->StopAfter(maxKey);
        } else {
            iter->StopBefore(maxKey);
        }
    }

    return iter;
}

TAutoPtr<TTableReverseIt> TDatabase::IterateRangeReverse(ui32 table, const TKeyRange& range, TTagsRef tags,
        TRowVersion snapshot,
        const ITransactionMapPtr& visible,
        const ITransactionObserverPtr& observer) const noexcept
{
    Y_VERIFY(!NoMoreReadsFlag, "Trying to read after reads prohibited, table %u", table);

    IteratedTables.insert(table);

    ESeek seek = !range.MaxKey || range.MaxInclusive ? ESeek::Lower : ESeek::Upper;

    auto iter = Require(table)->IterateReverse(range.MaxKey, tags, Env, seek, snapshot, visible, observer);

    if (range.MinKey) {
        TCelled minKey(range.MinKey, *iter->Scheme->Keys, false);

        if (range.MinInclusive) {
            iter->StopAfter(minKey);
        } else {
            iter->StopBefore(minKey);
        }
    }

    return iter;
}

template<>
TAutoPtr<TTableIt> TDatabase::IterateRangeGeneric<TTableIt>(ui32 table, const TKeyRange& range, TTagsRef tags,
        TRowVersion snapshot,
        const ITransactionMapPtr& visible,
        const ITransactionObserverPtr& observer) const noexcept
{
    return IterateRange(table, range, tags, snapshot, visible, observer);
}

template<>
TAutoPtr<TTableReverseIt> TDatabase::IterateRangeGeneric<TTableReverseIt>(ui32 table, const TKeyRange& range, TTagsRef tags,
        TRowVersion snapshot,
        const ITransactionMapPtr& visible,
        const ITransactionObserverPtr& observer) const noexcept
{
    return IterateRangeReverse(table, range, tags, snapshot, visible, observer);
}

EReady TDatabase::Select(ui32 table, TRawVals key, TTagsRef tags, TRowState &row, ui64 flg,
        TRowVersion snapshot,
        const ITransactionMapPtr& visible,
        const ITransactionObserverPtr& observer) const noexcept
{
    TSelectStats stats;
    return Select(table, key, tags, row, stats, flg, snapshot, visible, observer);
}

EReady TDatabase::Select(ui32 table, TRawVals key, TTagsRef tags, TRowState &row, TSelectStats& stats, ui64 flg,
        TRowVersion snapshot,
        const ITransactionMapPtr& visible,
        const ITransactionObserverPtr& observer) const noexcept
{
    TempIterators.clear();
    Y_VERIFY(!NoMoreReadsFlag, "Trying to read after reads prohibited, table %u", table);

    auto prevSieved = stats.Sieved;
    auto prevWeeded = stats.Weeded;
    auto prevNoKey = stats.NoKey;
    auto prevInvisible = stats.InvisibleRowSkips;

    auto ready = Require(table)->Select(key, tags, Env, row, flg, snapshot, TempIterators, stats, visible, observer);
    Change->Stats.SelectSieved += stats.Sieved - prevSieved;
    Change->Stats.SelectWeeded += stats.Weeded - prevWeeded;
    Change->Stats.SelectNoKey += stats.NoKey - prevNoKey;
    Change->Stats.SelectInvisible += stats.InvisibleRowSkips - prevInvisible;

    return ready;
}

void TDatabase::CalculateReadSize(TSizeEnv& env, ui32 table, TRawVals minKey, TRawVals maxKey,
                                  TTagsRef tags, ui64 flg, ui64 items, ui64 bytes,
                                  EDirection direction, TRowVersion snapshot)
{
    Y_VERIFY(!NoMoreReadsFlag, "Trying to do precharge after reads prohibited, table %u", table);
    TSelectStats stats;
    Require(table)->Precharge(minKey, maxKey, tags, &env, flg, items, bytes, direction, snapshot, stats);
}

bool TDatabase::Precharge(ui32 table, TRawVals minKey, TRawVals maxKey,
                    TTagsRef tags, ui64 flg, ui64 items, ui64 bytes,
                    EDirection direction, TRowVersion snapshot)
{
    Y_VERIFY(!NoMoreReadsFlag, "Trying to do precharge after reads prohibited, table %u", table);
    TSelectStats stats;
    auto ready = Require(table)->Precharge(minKey, maxKey, tags, Env, flg, items, bytes, direction, snapshot, stats);
    Change->Stats.ChargeSieved += stats.Sieved;
    Change->Stats.ChargeWeeded += stats.Weeded;
    return ready == EReady::Data;
}

void TDatabase::Update(ui32 table, ERowOp rop, TRawVals key, TArrayRef<const TUpdateOp> ops, TRowVersion rowVersion)
{
    Y_VERIFY_DEBUG(rowVersion != TRowVersion::Max(), "Updates cannot have v{max} as row version");

    for (size_t index = 0; index < key.size(); ++index) {
        if (auto error = NScheme::HasUnexpectedValueSize(key[index])) {
            Y_FAIL("Key index %" PRISZT " validation failure: %s", index, error.c_str());
        }
    }
    for (size_t index = 0; index < ops.size(); ++index) {
        if (auto error = NScheme::HasUnexpectedValueSize(ops[index].Value)) {
            Y_FAIL("Op index %" PRISZT " tag %" PRIu32 " validation failure: %s", index, ops[index].Tag, error.c_str());
        }
    }

    Redo->EvUpdate(table, rop, key, ops, rowVersion);
}

void TDatabase::UpdateTx(ui32 table, ERowOp rop, TRawVals key, TArrayRef<const TUpdateOp> ops, ui64 txId)
{
    for (size_t index = 0; index < key.size(); ++index) {
        if (auto error = NScheme::HasUnexpectedValueSize(key[index])) {
            Y_FAIL("Key index %" PRISZT " validation failure: %s", index, error.c_str());
        }
    }
    for (size_t index = 0; index < ops.size(); ++index) {
        if (auto error = NScheme::HasUnexpectedValueSize(ops[index].Value)) {
            Y_FAIL("Op index %" PRISZT " tag %" PRIu32 " validation failure: %s", index, ops[index].Tag, error.c_str());
        }
    }

    Redo->EvUpdateTx(table, rop, key, ops, txId);
}

void TDatabase::RemoveTx(ui32 table, ui64 txId)
{
    Redo->EvRemoveTx(table, txId);
}

void TDatabase::CommitTx(ui32 table, ui64 txId, TRowVersion rowVersion)
{
    Redo->EvCommitTx(table, txId, rowVersion);
}

bool TDatabase::HasOpenTx(ui32 table, ui64 txId) const
{
    return Require(table)->HasOpenTx(txId);
}

void TDatabase::RemoveRowVersions(ui32 table, const TRowVersion& lower, const TRowVersion& upper)
{
    if (Y_LIKELY(lower < upper)) {
        Change->RemovedRowVersions[table].push_back({ lower, upper });
    }
}

const TRowVersionRanges& TDatabase::GetRemovedRowVersions(ui32 table) const
{
    if (auto& wrap = DatabaseImpl->Get(table, false)) {
        return wrap->GetRemovedRowVersions();
    }

    static const TRowVersionRanges empty;
    return empty;
}

void TDatabase::NoMoreReadsForTx() {
    NoMoreReadsFlag = true;
}

void TDatabase::Begin(TTxStamp stamp, IPages& env)
{
    Y_VERIFY(!Redo, "Transaction already in progress");
    Y_VERIFY(!Env);
    Annex = new TAnnex(*DatabaseImpl->Scheme);
    Redo = new NRedo::TWriter{ Annex.Get(), DatabaseImpl->AnnexByteLimit() };
    Change = MakeHolder<TChange>(Stamp = stamp, DatabaseImpl->Serial() + 1);
    Env = &env;
    NoMoreReadsFlag = false;
}

TPartView TDatabase::GetPartView(ui32 tableId, const TLogoBlobID &bundle) const {
    return Require(tableId)->GetPartView(bundle);
}

TVector<TPartView> TDatabase::GetTableParts(ui32 tableId) const {
    return Require(tableId)->GetAllParts();
}

TVector<TIntrusiveConstPtr<TColdPart>> TDatabase::GetTableColdParts(ui32 tableId) const {
    return Require(tableId)->GetColdParts();
}

void TDatabase::EnumerateTableParts(ui32 tableId, const std::function<void(const TPartView&)>& callback) const {
    Require(tableId)->EnumerateParts(std::move(callback));
}

void TDatabase::EnumerateTableColdParts(ui32 tableId, const std::function<void(const TIntrusiveConstPtr<TColdPart>&)>& callback) const {
    Require(tableId)->EnumerateColdParts(callback);
}

void TDatabase::EnumerateTableTxStatusParts(ui32 tableId, const std::function<void(const TIntrusiveConstPtr<TTxStatusPart>&)>& callback) const {
    Require(tableId)->EnumerateTxStatusParts(callback);
}

void TDatabase::EnumerateTxStatusParts(const std::function<void(const TIntrusiveConstPtr<TTxStatusPart>&)>& callback) const {
    DatabaseImpl->EnumerateTxStatusParts(callback);
}

ui64 TDatabase::GetTableMemSize(ui32 tableId, TEpoch epoch) const {
    return Require(tableId)->GetMemSize(epoch);
}

ui64 TDatabase::GetTableMemRowCount(ui32 tableId) const {
    return Require(tableId)->GetMemRowCount();
}

ui64 TDatabase::GetTableMemOpsCount(ui32 tableId) const {
    return Require(tableId)->GetOpsCount();
}

ui64 TDatabase::GetTableIndexSize(ui32 tableId) const {
    return Require(tableId)->Stat().Parts.IndexBytes;
}

ui64 TDatabase::GetTableSearchHeight(ui32 tableId) const {
    return Require(tableId)->GetSearchHeight();
}

ui64 TDatabase::EstimateRowSize(ui32 tableId) const {
    return Require(tableId)->EstimateRowSize();
}

const TDbStats& TDatabase::Counters() const noexcept
{
    return DatabaseImpl->Stats;
}

TDatabase::TChg TDatabase::Head(ui32 table) const noexcept
{
    if (table == Max<ui32>()) {
        return { DatabaseImpl->Serial(), TEpoch::Max() };
    } else {
        auto &wrap = DatabaseImpl->Get(table, true);

        return { wrap.Serial, wrap->Head() };
    }
}

TString TDatabase::SnapshotToLog(ui32 table, TTxStamp stamp)
{
    auto scn = DatabaseImpl->Serial() + 1;
    auto epoch = DatabaseImpl->Get(table, true)->Snapshot();

    DatabaseImpl->Rewind(scn);

    return
        NRedo::TWriter{ }
            .EvBegin(ui32(ECompatibility::Head), ui32(ECompatibility::Edge), scn, stamp)
            .EvFlush(table, stamp, epoch).Dump();
}

ui32 TDatabase::TxSnapTable(ui32 table)
{
    Require(table);
    Change->Snapshots.emplace_back(table);
    return Change->Snapshots.size() - 1;
}

TAutoPtr<TSubset> TDatabase::Subset(ui32 table, TArrayRef<const TLogoBlobID> bundle, TEpoch before) const
{
    return Require(table)->Subset(bundle, before);
}

TAutoPtr<TSubset> TDatabase::Subset(ui32 table, TEpoch before, TRawVals from, TRawVals to) const
{
    auto subset = Require(table)->Subset(before);

    if (from || to) {
        Y_VERIFY(!subset->Frozen, "Got subset with frozens, cannot shrink it");
        Y_VERIFY(!subset->ColdParts, "Got subset with cold parts, cannot shrink it");

        TShrink shrink(Env, subset->Scheme->Keys);

        if (shrink.Put(subset->Flatten, from, to).Skipped) {
            return nullptr; /* Cannot shrink due to lack of some pages */
        } else {
            subset->Flatten = std::move(shrink.PartView);
        }
    }

    return subset;
}

TAutoPtr<TSubset> TDatabase::ScanSnapshot(ui32 table, TRowVersion snapshot)
{
    return Require(table)->ScanSnapshot(snapshot);
}

bool TDatabase::HasBorrowed(ui32 table, ui64 selfTabletId) const
{
    return Require(table)->HasBorrowed(selfTabletId);
}

TBundleSlicesMap TDatabase::LookupSlices(ui32 table, TArrayRef<const TLogoBlobID> bundles) const
{
    return Require(table)->LookupSlices(bundles);
}

void TDatabase::ReplaceSlices(ui32 table, TBundleSlicesMap slices)
{
    return DatabaseImpl->ReplaceSlices(table, std::move(slices));
}

void TDatabase::Replace(ui32 table, TArrayRef<const TPartView> partViews, const TSubset &subset)
{
    return DatabaseImpl->Replace(table, partViews, subset);
}

void TDatabase::ReplaceTxStatus(ui32 table, TArrayRef<const TIntrusiveConstPtr<TTxStatusPart>> txStatus, const TSubset &subset)
{
    return DatabaseImpl->ReplaceTxStatus(table, txStatus, subset);
}

void TDatabase::Merge(ui32 table, TPartView partView)
{
    return DatabaseImpl->Merge(table, std::move(partView));
}

void TDatabase::Merge(ui32 table, TIntrusiveConstPtr<TColdPart> part)
{
    return DatabaseImpl->Merge(table, std::move(part));
}

void TDatabase::Merge(ui32 table, TIntrusiveConstPtr<TTxStatusPart> txStatus)
{
    return DatabaseImpl->Merge(table, std::move(txStatus));
}

TAlter& TDatabase::Alter()
{
    Y_VERIFY(Redo, "Scheme change must be done within a transaction");
    Y_VERIFY(!*Redo, "Scheme change must be done before any data updates");

    return *(Alter_ ? Alter_ : (Alter_ = new TAlter()));
}

void TDatabase::DebugDumpTable(ui32 table, IOutputStream& str, const NScheme::TTypeRegistry& typeRegistry) const {
    str << "Table " << table << Endl;
    if (auto &wrap = DatabaseImpl->Get(table, false))
        wrap->DebugDump(str, Env, typeRegistry);
    else
        str << "unknown" << Endl;
}

void TDatabase::DebugDump(IOutputStream& str, const NScheme::TTypeRegistry& typeRegistry) const {
    for (const auto& it: DatabaseImpl->Scheme->Tables) {
        if (DatabaseImpl->Get(it.first, false)) {
            str << "======= " << it.second.Name << " ======\n";
            DebugDumpTable(it.first, str, typeRegistry);
        }
    }
}

TKeyRangeCache* TDatabase::DebugGetTableErasedKeysCache(ui32 table) const {
    if (auto &wrap = DatabaseImpl->Get(table, false)) {
        return wrap->GetErasedKeysCache();
    } else {
        return nullptr;
    }
}

bool TDatabase::ValidateCommit(TString &err)
{
    if (*Redo && Redo->Bytes() > MAX_REDO_BYTES_PER_COMMIT) {
        err = TStringBuilder()
            << "Redo commit of " << Redo->Bytes()
            << " bytes is more than the allowed limit";
        return false;
    }

    return true;
}

TDatabase::TProd TDatabase::Commit(TTxStamp stamp, bool commit, TCookieAllocator *cookieAllocator)
{
    TempIterators.clear();

    if (IteratedTables) {
        for (ui32 table : IteratedTables) {
            if (auto& wrap = DatabaseImpl->Get(table, false)) {
                if (auto* cache = wrap->GetErasedKeysCache()) {
                    cache->CollectGarbage();
                }
            }
        }

        IteratedTables.clear();
    }

    if (commit && (*Redo || Alter_ || Change->Snapshots || Change->RemovedRowVersions)) {
        Y_VERIFY(stamp >= Change->Stamp);

        /* TODO: Temporary hack fot getting correct Stamp and Serial state
                against invocation of SnapshotToLog() between Begin(...) and
                Commit(...). Read KIKIMR-5366 for details and progress. */

        const_cast<TTxStamp&>(Change->Stamp) = stamp;
        const_cast<ui64&>(Change->Serial) = DatabaseImpl->Serial() + 1;

        NRedo::TWriter prefix{ };

        {
            const ui32 head = ui32(ECompatibility::Head);
            const ui32 edge = ui32(ECompatibility::Edge);

            prefix.EvBegin(head, edge,  Change->Serial, Change->Stamp);
        }

        const auto offset = prefix.Bytes(); /* useful payload starts here */

        if (auto annex = Annex->Unwrap()) {
            Y_VERIFY(cookieAllocator, "Have to provide TCookieAllocator with enabled annex");

            TVector<NPageCollection::TGlobId> blobs;

            blobs.reserve(annex.size());

            for (auto &one: annex) {
                auto glob = cookieAllocator->Do(one.GId.Logo.Channel(), one.Data.size());

                blobs.emplace_back(one.GId = glob);

                Y_VERIFY(glob.Logo.BlobSize(), "Blob cannot have zero bytes");
            }

            prefix.EvAnnex(blobs);
            DatabaseImpl->Assign(std::move(annex));
        }

        DatabaseImpl->Switch(Stamp);

        if (Alter_) {
            auto delta = Alter_->Flush();

            if (DatabaseImpl->Apply(*delta, &prefix))
                Y_PROTOBUF_SUPPRESS_NODISCARD delta->SerializeToString(&Change->Scheme);
        }

        for (auto &one: Change->Snapshots) {
            one.Epoch = Require(one.Table)->Snapshot();
            prefix.EvFlush(one.Table, Stamp, one.Epoch);
        }

        prefix.Join(*Redo);

        Change->Redo = prefix.Dump();

        for (auto &entry: prefix.Unwrap())
            DatabaseImpl->ApplyRedo(entry);

        for (const auto& xpair : Change->RemovedRowVersions) {
            if (auto& wrap = DatabaseImpl->Get(xpair.first, false)) {
                for (const auto& range : xpair.second) {
                    wrap->RemoveRowVersions(range.Lower, range.Upper);
                }
            }
        }

        Change->Garbage = std::move(DatabaseImpl->Garbage);
        Change->Deleted = std::move(DatabaseImpl->Deleted);
        Change->Affects = DatabaseImpl->GrabAffects();
        Change->Annex = DatabaseImpl->GrabAnnex();

        if (Change->Redo.size() == offset && !Change->Affects) {
            std::exchange(Change->Redo, { }); /* omit complete NOOP redo */
        }

        if (Change->Redo.size() > offset && !Change->Affects) {
            Y_Fail(
                NFmt::Do(*Change) << " produced " << (Change->Redo.size() - offset)
                << "b of non technical redo without leaving effects on data");
        } else if (Change->Serial != DatabaseImpl->Serial()) {
            Y_Fail(
                NFmt::Do(*Change) << " serial diverged from current db "
                << DatabaseImpl->Serial() << " after rolling up redo log");
        } else if (Change->Deleted.size() != Change->Garbage.size()) {
            Y_Fail(NFmt::Do(*Change) << " has inconsistent garbage data");
        }
    }

    Redo = nullptr;
    Annex = nullptr;
    Alter_ = nullptr;
    Env = nullptr;

    return { std::move(Change) };
}

TTable* TDatabase::Require(ui32 table) const noexcept
{
    return DatabaseImpl->Get(table, true).Self.Get();
}

TGarbage TDatabase::RollUp(TTxStamp stamp, TArrayRef<const char> delta, TArrayRef<const char> redo,
                                TMemGlobs annex)
{
    Y_VERIFY(!annex || redo, "Annex have to be rolled up with redo log");

    DatabaseImpl->Switch(stamp);

    if (delta) {
        TSchemeChanges changes;
        bool parseOk = ParseFromStringNoSizeLimit(changes, delta);
        Y_VERIFY(parseOk);

        DatabaseImpl->Apply(changes, nullptr);
    }

    if (redo) {
        DatabaseImpl->Assign(std::move(annex));
        DatabaseImpl->ApplyRedo(redo);
        DatabaseImpl->GrabAnnex();
    }

    return std::move(DatabaseImpl->Garbage);
}

void TDatabase::RollUpRemoveRowVersions(ui32 table, const TRowVersion& lower, const TRowVersion& upper)
{
    if (auto& wrap = DatabaseImpl->Get(table, false)) {
        wrap->RemoveRowVersions(lower, upper);
    }
}

TCompactionStats TDatabase::GetCompactionStats(ui32 table) const
{
    return Require(table)->GetCompactionStats();
}

// NOTE: This helper should be used only to dump local DB contents in GDB
void DebugDumpDb(const TDatabase &db) {
    NScheme::TTypeRegistry typeRegistry;
    db.DebugDump(Cout, typeRegistry);
}

}}
