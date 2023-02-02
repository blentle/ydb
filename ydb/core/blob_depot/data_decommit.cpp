#include "data.h"
#include "coro_tx.h"

namespace NKikimr::NBlobDepot {

    class TBlobDepot::TData::TResolveDecommitActor : public TActorBootstrapped<TResolveDecommitActor> {
        struct TEvPrivate {
            enum {
                EvTxComplete = EventSpaceBegin(TEvents::ES_PRIVATE),
            };
        };

        TBlobDepot* const Self;
        std::weak_ptr<TToken> Token;
        std::vector<TEvBlobStorage::TEvAssimilateResult::TBlob> DecommitBlobs;
        THashSet<TLogoBlobID> ResolutionErrors;
        TEvBlobDepot::TEvResolve::TPtr Ev;

        ui32 RangesInFlight = 0;

        std::deque<std::tuple<TLogoBlobID, bool>> GetQ;
        ui32 GetsInFlight = 0;
        ui32 GetBytesInFlight = 0;
        static constexpr ui32 MaxGetsInFlight = 10;
        static constexpr ui32 MaxGetBytesInFlight = 10'000'000;

        ui32 PutsInFlight = 0;

        THashMap<TLogoBlobID, TKey> IdToKey;

    public:
        TResolveDecommitActor(TBlobDepot *self, TEvBlobDepot::TEvResolve::TPtr ev)
            : Self(self)
            , Token(self->Token)
            , Ev(ev)
        {}

        void Bootstrap() {
            if (Token.expired()) {
                return PassAway();
            }

            STLOG(PRI_DEBUG, BLOB_DEPOT, BDT42, "TResolveDecommitActor::Bootstrap", (Id, Self->GetLogId()),
                (Sender, Ev->Sender), (Cookie, Ev->Cookie));

            Self->Execute(std::make_unique<TCoroTx>(Self, Token, std::bind(&TThis::TxPrepare, this)));
            Become(&TThis::StateFunc);
        }

        void TxPrepare() {
            for (const auto& item : Ev->Get()->Record.GetItems()) {
                switch (item.GetKeyDesignatorCase()) {
                    case NKikimrBlobDepot::TEvResolve::TItem::kKeyRange: {
                        if (!item.HasTabletId()) {
                           return FinishWithError(NLog::PRI_CRIT, "incorrect request");
                        }

                        const ui64 tabletId = item.GetTabletId();
                        const auto& range = item.GetKeyRange();

                        TLogoBlobID minId = range.HasBeginningKey()
                            ? TKey::FromBinaryKey(range.GetBeginningKey(), Self->Config).GetBlobId()
                            : TLogoBlobID(tabletId, 0, 0, 0, 0, 0);

                        TLogoBlobID maxId = range.HasEndingKey()
                            ? TKey::FromBinaryKey(range.GetEndingKey(), Self->Config).GetBlobId()
                            : TLogoBlobID(tabletId, Max<ui32>(), Max<ui32>(), TLogoBlobID::MaxChannel,
                                TLogoBlobID::MaxBlobSize, TLogoBlobID::MaxCookie, TLogoBlobID::MaxPartId,
                                TLogoBlobID::MaxCrcMode);

                        Y_VERIFY(minId <= maxId);

                        if (Self->Data->LastAssimilatedBlobId < maxId) {
                            // adjust minId to skip already assimilated items in range query
                            if (minId < Self->Data->LastAssimilatedBlobId) {
                                if (item.GetMustRestoreFirst()) {
                                    InvokeOtherActor(*this, &TThis::ScanRangeAndIssueGets, TKey(minId),
                                        TKey(*Self->Data->LastAssimilatedBlobId), EScanFlags::INCLUDE_BEGIN);
                                }
                                minId = *Self->Data->LastAssimilatedBlobId;
                            }

                            // issue scan query
                            InvokeOtherActor(*this, &TThis::IssueRange, tabletId, minId, maxId, item.GetMustRestoreFirst());
                        } else if (item.GetMustRestoreFirst()) {
                            InvokeOtherActor(*this, &TThis::ScanRangeAndIssueGets, TKey(minId), TKey(maxId),
                                EScanFlags::INCLUDE_BEGIN | EScanFlags::INCLUDE_END);
                        }

                        break;
                    }

                    case NKikimrBlobDepot::TEvResolve::TItem::kExactKey: {
                        TData::TKey key = TKey::FromBinaryKey(item.GetExactKey(), Self->Config);
                        while (!Self->Data->EnsureKeyLoaded(key, *TCoroTx::GetTxc())) {
                            TCoroTx::RestartTx();
                        }
                        const TValue *value = Self->Data->FindKey(key);
                        const bool doGet = (!value && key.GetBlobId() < Self->Data->LastAssimilatedBlobId) // value not yet assimilated
                            || (value && value->GoingToAssimilate && item.GetMustRestoreFirst()); // value has no local data yet
                        if (doGet) {
                            InvokeOtherActor(*this, &TThis::IssueGet, key.GetBlobId(), item.GetMustRestoreFirst());
                        }
                        break;
                    }

                    case NKikimrBlobDepot::TEvResolve::TItem::KEYDESIGNATOR_NOT_SET:
                        Y_VERIFY_DEBUG(false);
                        break;
                }
            }

            TCoroTx::FinishTx();
            CheckIfDone();
        }

        void ScanRangeAndIssueGets(TKey from, TKey to, TScanFlags flags) {
            bool progress = false;

            auto callback = [&](const TKey& key, const TValue& value) {
                if (value.GoingToAssimilate) {
                    IssueGet(key.GetBlobId(), true /*mustRestoreFirst*/);
                }
                return true;
            };

            TScanRange r{from, to, flags};
            while (!Self->Data->ScanRange(r, TCoroTx::GetTxc(), &progress, callback)) {
                if (std::exchange(progress, false)) {
                    TCoroTx::FinishTx();
                    TCoroTx::RunSuccessorTx();
                } else {
                    TCoroTx::RestartTx();
                }
            }
        }

        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        // RANGE QUERIES are for metadata only -- they scan not yet assimilated parts of the original group and do not
        // recover any data; thus they are IsIndexOnly and not MustRestoreFirst range queries

        void IssueRange(ui64 tabletId, TLogoBlobID from, TLogoBlobID to, bool mustRestoreFirst) {
            STLOG(PRI_DEBUG, BLOB_DEPOT, BDT50, "going to TEvRange", (Id, Self->GetLogId()), (Sender, Ev->Sender),
                (Cookie, Ev->Cookie), (TabletId, tabletId), (From, from), (To, to), (MustRestoreFirst, mustRestoreFirst));
            auto ev = std::make_unique<TEvBlobStorage::TEvRange>(tabletId, from, to, false, TInstant::Max(), true);
            ev->Decommission = true;
            SendToBSProxy(SelfId(), Self->Config.GetVirtualGroupId(), ev.release(), mustRestoreFirst);
            ++RangesInFlight;
        }

        void Handle(TEvBlobStorage::TEvRangeResult::TPtr ev) {
            auto& msg = *ev->Get();
            STLOG(PRI_DEBUG, BLOB_DEPOT, BDT55, "TEvRangeResult", (Id, Self->GetLogId()), (Sender, Ev->Sender),
                (Cookie, Ev->Cookie), (Msg, msg));

            if (msg.Status == NKikimrProto::OK) {
                for (const auto& r : msg.Responses) {
                    if (ev->Cookie) {
                        if (const TValue *value = Self->Data->FindKey(TKey(r.Id)); !value || value->GoingToAssimilate) {
                            IssueGet(r.Id, true /*mustRestoreFirst*/);
                        }
                    } else {
                        DecommitBlobs.push_back({r.Id, r.Keep, r.DoNotKeep});
                    }
                }
            } else {
                return FinishWithError(NLog::PRI_NOTICE, TStringBuilder() << "TEvRange query failed: " << msg.ErrorReason);
            }

            --RangesInFlight;
            CheckIfDone();
        }

        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        // GET QUERIES may contain request either just for metadata, or for the data too; in case we receive data, we
        // have to put it to BlobDepot storage

        void IssueGet(TLogoBlobID id, bool mustRestoreFirst) {
            STLOG(PRI_DEBUG, BLOB_DEPOT, BDT86, "going to TEvGet", (Id, Self->GetLogId()), (Sender, Ev->Sender),
                (Cookie, Ev->Cookie), (BlobId, id), (MustRestoreFirst, mustRestoreFirst));
            GetQ.emplace_back(id, mustRestoreFirst);
            ProcessGetQueue();
        }

        static ui32 GetBytesFor(const std::tuple<TLogoBlobID, bool>& q) {
            const auto& [id, mustRestoreFirst] = q;
            return mustRestoreFirst ? id.BlobSize() : 0;
        }

        void ProcessGetQueue() {
            while (!GetQ.empty() && GetsInFlight < MaxGetsInFlight && GetBytesInFlight + GetBytesFor(GetQ.front()) <= MaxGetBytesInFlight) {
                const auto [id, mustRestoreFirst] = GetQ.front();
                ++GetsInFlight;
                const ui32 bytes = GetBytesFor(GetQ.front());
                GetBytesInFlight += bytes;
                GetQ.pop_front();
                auto ev = std::make_unique<TEvBlobStorage::TEvGet>(id, 0, 0, TInstant::Max(),
                    NKikimrBlobStorage::EGetHandleClass::FastRead, false /*mustRestoreFirst*/,
                    !mustRestoreFirst /*isIndexOnly*/);
                ev->Decommission = true;
                SendToBSProxy(SelfId(), Self->Config.GetVirtualGroupId(), ev.release(), bytes);
            }
        }

        void Handle(TEvBlobStorage::TEvGetResult::TPtr ev) {
            auto& msg = *ev->Get();
            STLOG(PRI_DEBUG, BLOB_DEPOT, BDT87, "TEvGetResult", (Id, Self->GetLogId()), (Sender, Ev->Sender),
                (Cookie, Ev->Cookie), (Msg, msg));

            for (ui32 i = 0; i < msg.ResponseSz; ++i) {
                auto& r = msg.Responses[i];
                if (r.Status == NKikimrProto::OK) {
                    if (r.Buffer) { // wasn't index read
                        IssuePut(TKey(r.Id), std::move(r.Buffer), r.Keep, r.DoNotKeep);
                    } else {
                        DecommitBlobs.push_back({r.Id, r.Keep, r.DoNotKeep});
                    }
                } else if (r.Status == NKikimrProto::NODATA) {
                    Self->Data->ExecuteTxCommitAssimilatedBlob(NKikimrProto::NODATA, TBlobSeqId(), TData::TKey(r.Id),
                        TEvPrivate::EvTxComplete, SelfId(), 0);
                    ++PutsInFlight;
                } else {
                    // mark this specific key as unresolvable
                    ResolutionErrors.emplace(r.Id);
                }
            }

            --GetsInFlight;
            GetBytesInFlight -= ev->Cookie;

            ProcessGetQueue();
            CheckIfDone();
        }

        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        // PUT QUERIES are used to store retrieved MustRestoreFirst blobs in local storage

        void IssuePut(TKey key, TString&& buffer, bool keep, bool doNotKeep) {
            std::vector<ui8> channels(1);
            Self->PickChannels(NKikimrBlobDepot::TChannelKind::Data, channels);
            TChannelInfo& channel = Self->Channels[channels.front()];
            const ui64 value = channel.NextBlobSeqId++;
            const auto blobSeqId = TBlobSeqId::FromSequentalNumber(channel.Index, Self->Executor()->Generation(), value);
            const TLogoBlobID id = blobSeqId.MakeBlobId(Self->TabletID(), EBlobType::VG_DATA_BLOB, 0, buffer.size());
            STLOG(PRI_DEBUG, BLOB_DEPOT, BDT91, "going to TEvPut", (Id, Self->GetLogId()), (Sender, Ev->Sender),
                (Cookie, Ev->Cookie), (Key, key), (BlobId, id));
            SendToBSProxy(SelfId(), channel.GroupId, new TEvBlobStorage::TEvPut(id, std::move(buffer), TInstant::Max()),
                (ui64)keep | (ui64)doNotKeep << 1);
            const bool inserted = channel.AssimilatedBlobsInFlight.insert(value).second; // prevent from barrier advancing
            Y_VERIFY(inserted);
            const bool inserted1 = IdToKey.try_emplace(id, std::move(key)).second;
            Y_VERIFY(inserted1);
            ++PutsInFlight;
        }

        void Handle(TEvBlobStorage::TEvPutResult::TPtr ev) {
            auto& msg = *ev->Get();

            const auto it = IdToKey.find(msg.Id);
            Y_VERIFY(it != IdToKey.end());
            TKey key = std::move(it->second);
            IdToKey.erase(it);

            const bool keep = ev->Cookie & 1;
            const bool doNotKeep = ev->Cookie >> 1 & 1;

            STLOG(PRI_DEBUG, BLOB_DEPOT, BDT88, "got TEvPutResult", (Id, Self->GetLogId()), (Sender, Ev->Sender),
                (Cookie, Ev->Cookie), (Msg, msg), (Key, key), (Keep, keep), (DoNotKeep, doNotKeep));

            Self->Data->ExecuteTxCommitAssimilatedBlob(msg.Status, TBlobSeqId::FromLogoBlobId(msg.Id), std::move(key),
                TEvPrivate::EvTxComplete, SelfId(), 0, keep, doNotKeep);

            if (msg.Status != NKikimrProto::OK) { // do not reply OK to this item
                ResolutionErrors.insert(msg.Id);
            }
        }

        void HandleTxComplete() {
            --PutsInFlight;
            CheckIfDone();
        }

        void CheckIfDone() {
            if (RangesInFlight + GetsInFlight + GetQ.size() + PutsInFlight == 0) {
                FinishWithSuccess();
            }
        }

        void FinishWithSuccess() {
            STLOG(PRI_DEBUG, BLOB_DEPOT, BDT92, "request succeeded", (Id, Self->GetLogId()), (Sender, Ev->Sender),
                (Cookie, Ev->Cookie), (ResolutionErrors.size, ResolutionErrors.size()),
                (DecommitBlobs.size, DecommitBlobs.size()));

            Self->Execute(std::make_unique<TCoroTx>(Self, Token, [self = Self, decommitBlobs = std::move(DecommitBlobs),
                    ev = Ev, resolutionErrors = std::move(ResolutionErrors)]() mutable {
                ui32 numItemsProcessed = 0;
                for (const auto& blob : decommitBlobs) {
                    if (numItemsProcessed == 10'000) {
                        TCoroTx::FinishTx();
                        self->Data->CommitTrash(TCoroTx::CurrentTx());
                        numItemsProcessed = 0;
                        TCoroTx::RunSuccessorTx();
                    }
                    numItemsProcessed += self->Data->AddDataOnDecommit(blob, *TCoroTx::GetTxc(), TCoroTx::CurrentTx());
                }
                TCoroTx::FinishTx();
                self->Data->CommitTrash(TCoroTx::CurrentTx());
                self->Data->ExecuteTxResolve(ev, std::move(resolutionErrors));
            }));

            PassAway();
        }

        void FinishWithError(NLog::EPriority prio, TString errorReason) {
           STLOG(prio, BLOB_DEPOT, BDT89, "request failed", (Id, Self->GetLogId()), (Sender, Ev->Sender),
                (Cookie, Ev->Cookie), (ErrorReason, errorReason));
           auto [response, record] = TEvBlobDepot::MakeResponseFor(*Ev, NKikimrProto::ERROR, std::move(errorReason));
           TActivationContext::Send(response.release());
           PassAway();
        }

        STATEFN(StateFunc) {
            if (Token.expired()) {
                return PassAway();
            }

            switch (const ui32 type = ev->GetTypeRewrite()) {
                hFunc(TEvBlobStorage::TEvGetResult, Handle);
                hFunc(TEvBlobStorage::TEvRangeResult, Handle);
                hFunc(TEvBlobStorage::TEvPutResult, Handle);
                cFunc(TEvPrivate::EvTxComplete, HandleTxComplete);

                default:
                    Y_VERIFY_DEBUG(false, "unexpected event Type# %08" PRIx32, type);
                    STLOG(PRI_CRIT, BLOB_DEPOT, BDT90, "unexpected event", (Id, Self->GetLogId()), (Type, type));
                    break;
            }
        }
    };

    IActor *TBlobDepot::TData::CreateResolveDecommitActor(TEvBlobDepot::TEvResolve::TPtr ev) {
        return new TResolveDecommitActor(Self, ev);
    }

} // NKikimr::NBlobDepot
