#include "agent_impl.h"
#include "blocks.h"

namespace NKikimr::NBlobDepot {

    template<>
    TBlobDepotAgent::TQuery *TBlobDepotAgent::CreateQuery<TEvBlobStorage::EvPut>(std::unique_ptr<IEventHandle> ev) {
        class TPutQuery : public TQuery {
            const bool SuppressFooter = true;
            const bool IssueUncertainWrites = true;

            std::vector<ui32> BlockChecksRemain;
            ui32 PutsInFlight = 0;
            bool PutsIssued = false;
            bool WaitingForCommitBlobSeq = false;
            bool IsInFlight = false;
            NKikimrBlobDepot::TEvCommitBlobSeq CommitBlobSeq;
            TBlobSeqId BlobSeqId;

        public:
            using TQuery::TQuery;

            void OnDestroy(bool success) override {
                if (IsInFlight) {
                    Y_VERIFY(!success);
                    RemoveBlobSeqFromInFlight();
                    NKikimrBlobDepot::TEvDiscardSpoiledBlobSeq msg;
                    BlobSeqId.ToProto(msg.AddItems());
                    Agent.Issue(std::move(msg), this, nullptr);
                }
            }

            void Initiate() override {
                auto& msg = GetQuery();
                if (msg.Buffer.size() > MaxBlobSize) {
                    return EndWithError(NKikimrProto::ERROR, "blob is way too big");
                } else if (msg.Buffer.size() != msg.Id.BlobSize()) {
                    return EndWithError(NKikimrProto::ERROR, "blob size mismatch");
                } else if (!msg.Buffer) {
                    return EndWithError(NKikimrProto::ERROR, "no blob data");
                } else if (!msg.Id) {
                    return EndWithError(NKikimrProto::ERROR, "blob id is zero");
                }

                BlockChecksRemain.resize(1 + msg.ExtraBlockChecks.size(), 3); // set number of tries for every block
                CheckBlocks();
            }

            void CheckBlocks() {
                auto& msg = GetQuery();
                bool someBlocksMissing = false;
                for (size_t i = 0; i <= msg.ExtraBlockChecks.size(); ++i) {
                    const auto *blkp = i ? &msg.ExtraBlockChecks[i - 1] : nullptr;
                    const ui64 tabletId = blkp ? blkp->first : msg.Id.TabletID();
                    const ui32 generation = blkp ? blkp->second : msg.Id.Generation();
                    const auto status = msg.Decommission
                        ? NKikimrProto::OK // suppress blocks check when copying blob from decommitted group
                        : Agent.BlocksManager.CheckBlockForTablet(tabletId, generation, this, nullptr);
                    if (status == NKikimrProto::OK) {
                        continue;
                    } else if (status != NKikimrProto::UNKNOWN) {
                        return EndWithError(status, "block race detected");
                    } else if (!--BlockChecksRemain[i]) {
                        return EndWithError(NKikimrProto::ERROR, "failed to acquire blocks");
                    } else {
                        someBlocksMissing = true;
                    }
                }
                if (!someBlocksMissing) {
                    IssuePuts();
                }
            }

            void IssuePuts() {
                Y_VERIFY(!PutsIssued);

                auto& msg = GetQuery();

                const auto it = Agent.ChannelKinds.find(NKikimrBlobDepot::TChannelKind::Data);
                if (it == Agent.ChannelKinds.end()) {
                    return EndWithError(NKikimrProto::ERROR, "no Data channels");
                }
                auto& kind = it->second;

                std::optional<TBlobSeqId> blobSeqId = kind.Allocate(Agent);
                STLOG(PRI_DEBUG, BLOB_DEPOT_AGENT, BDA21, "allocated BlobSeqId", (VirtualGroupId, Agent.VirtualGroupId),
                    (QueryId, GetQueryId()), (BlobSeqId, blobSeqId));
                if (!blobSeqId) {
                    return kind.EnqueueQueryWaitingForId(this);
                }
                BlobSeqId = *blobSeqId;
                if (!IssueUncertainWrites) {
                    // small optimization -- do not put this into WritesInFlight as it will be deleted right after in
                    // this function
                    kind.WritesInFlight.insert(BlobSeqId);
                    IsInFlight = true;
                }

                Y_VERIFY(CommitBlobSeq.ItemsSize() == 0);
                auto *commitItem = CommitBlobSeq.AddItems();
                commitItem->SetKey(msg.Id.AsBinaryString());
                auto *locator = commitItem->MutableBlobLocator();
                BlobSeqId.ToProto(locator->MutableBlobSeqId());
                //locator->SetChecksum(Crc32c(msg.Buffer.data(), msg.Buffer.size()));
                locator->SetTotalDataLen(msg.Buffer.size());
                if (!SuppressFooter) {
                    locator->SetFooterLen(sizeof(TVirtualGroupBlobFooter));
                }

                TString footerData;
                if (!SuppressFooter) {
                    footerData = TString::Uninitialized(sizeof(TVirtualGroupBlobFooter));
                    auto& footer = *reinterpret_cast<TVirtualGroupBlobFooter*>(footerData.Detach());
                    memset(&footer, 0, sizeof(footer));
                    footer.StoredBlobId = msg.Id;
                }

                auto put = [&](EBlobType type, TRope&& buffer) {
                    const auto& [id, groupId] = kind.MakeBlobId(Agent, BlobSeqId, type, 0, buffer.size());
                    Y_VERIFY(!locator->HasGroupId() || locator->GetGroupId() == groupId);
                    locator->SetGroupId(groupId);
                    auto ev = std::make_unique<TEvBlobStorage::TEvPut>(id, std::move(buffer), msg.Deadline, msg.HandleClass, msg.Tactic);
                    ev->ExtraBlockChecks = msg.ExtraBlockChecks;
                    if (!msg.Decommission) { // do not check original blob against blocks when writing decommission copy
                        ev->ExtraBlockChecks.emplace_back(msg.Id.TabletID(), msg.Id.Generation());
                    }
                    Agent.SendToProxy(groupId, std::move(ev), this, nullptr);
                    ++PutsInFlight;
                };

                if (SuppressFooter) {
                    // write the blob as is, we don't need footer for this kind
                    put(EBlobType::VG_DATA_BLOB, TRope(std::move(msg.Buffer)));
                } else if (msg.Buffer.size() + sizeof(TVirtualGroupBlobFooter) <= MaxBlobSize) {
                    // write single blob with footer
                    TRope buffer = std::move(msg.Buffer);
                    buffer.Insert(buffer.End(), TRope(std::move(footerData)));
                    buffer.Compact();
                    put(EBlobType::VG_COMPOSITE_BLOB, std::move(buffer));
                } else {
                    // write data blob and blob with footer
                    put(EBlobType::VG_DATA_BLOB, TRope(std::move(msg.Buffer)));
                    put(EBlobType::VG_FOOTER_BLOB, TRope(std::move(footerData)));
                }

                if (IssueUncertainWrites) {
                    IssueCommitBlobSeq(true);
                }

                PutsIssued = true;
            }

            void IssueCommitBlobSeq(bool uncertainWrite) {
                auto *item = CommitBlobSeq.MutableItems(0);
                if (uncertainWrite) {
                    item->SetUncertainWrite(true);
                } else {
                    item->ClearUncertainWrite();
                }
                Agent.Issue(CommitBlobSeq, this, nullptr);

                Y_VERIFY(!WaitingForCommitBlobSeq);
                WaitingForCommitBlobSeq = true;
            }

            void RemoveBlobSeqFromInFlight() {
                Y_VERIFY(IsInFlight);
                IsInFlight = false;

                // find and remove the write in flight record to ensure it won't be reported upon TEvPushNotify
                // reception AND to check that it wasn't already trimmed by answering TEvPushNotifyResult
                const auto it = Agent.ChannelKinds.find(NKikimrBlobDepot::TChannelKind::Data);
                if (it == Agent.ChannelKinds.end()) {
                    return EndWithError(NKikimrProto::ERROR, "no Data channels");
                }
                auto& kind = it->second;
                const size_t numErased = kind.WritesInFlight.erase(BlobSeqId);
                Y_VERIFY(numErased);
            }

            void OnUpdateBlock(bool success) override {
                if (success) {
                    CheckBlocks(); // just restart request
                } else {
                    EndWithError(NKikimrProto::ERROR, "BlobDepot tablet disconnected");
                }
            }

            void OnIdAllocated() override {
                IssuePuts();
            }

            void ProcessResponse(ui64 /*id*/, TRequestContext::TPtr context, TResponse response) override {
                if (auto *p = std::get_if<TEvBlobStorage::TEvPutResult*>(&response)) {
                    HandlePutResult(std::move(context), **p);
                } else if (auto *p = std::get_if<TEvBlobDepot::TEvCommitBlobSeqResult*>(&response)) {
                    HandleCommitBlobSeqResult(std::move(context), (*p)->Record);
                } else if (std::holds_alternative<TTabletDisconnected>(response)) {
                    EndWithError(NKikimrProto::ERROR, "BlobDepot tablet disconnected");
                } else {
                    Y_FAIL("unexpected response");
                }
            }

            void HandlePutResult(TRequestContext::TPtr /*context*/, TEvBlobStorage::TEvPutResult& msg) {
                STLOG(PRI_DEBUG, BLOB_DEPOT_AGENT, BDA22, "TEvPutResult", (VirtualGroupId, Agent.VirtualGroupId),
                    (QueryId, GetQueryId()), (Msg, msg));

                --PutsInFlight;
                if (msg.Status != NKikimrProto::OK) {
                    EndWithError(msg.Status, std::move(msg.ErrorReason));
                } else if (PutsInFlight) {
                    // wait for all puts to complete
                } else if (BlobSeqId.Generation != Agent.BlobDepotGeneration) {
                    // FIXME: although this is error now, we can handle this in the future, when BlobDepot picks records
                    // on restarts; it may have scanned written record and already updated it in its local database;
                    // however, if it did not, we can't try to commit this records as it may be already scheduled for
                    // garbage collection by the tablet
                    EndWithError(NKikimrProto::ERROR, "BlobDepot tablet was restarting during write");
                } else if (!IssueUncertainWrites) { // proceed to second phase
                    IssueCommitBlobSeq(false);
                    RemoveBlobSeqFromInFlight();
                } else {
                    CheckIfFinished();
                }
            }

            void HandleCommitBlobSeqResult(TRequestContext::TPtr /*context*/, NKikimrBlobDepot::TEvCommitBlobSeqResult& msg) {
                Y_VERIFY(WaitingForCommitBlobSeq);
                WaitingForCommitBlobSeq = false;

                Y_VERIFY(msg.ItemsSize() == 1);
                auto& item = msg.GetItems(0);
                if (const auto status = item.GetStatus(); status != NKikimrProto::OK && status != NKikimrProto::RACE) {
                    EndWithError(item.GetStatus(), item.GetErrorReason());
                } else {
                    // it's okay to treat RACE as OK here since values are immutable in Virtual Group mode
                    CheckIfFinished();
                }
            }

            void CheckIfFinished() {
                if (!PutsInFlight && !WaitingForCommitBlobSeq) {
                    EndWithSuccess();
                }
            }

            void EndWithSuccess() {
                if (IssueUncertainWrites) { // send a notification
                    auto *item = CommitBlobSeq.MutableItems(0);
                    item->SetCommitNotify(true);
                    IssueCommitBlobSeq(false);
                }

                auto& msg = GetQuery();
                TQuery::EndWithSuccess(std::make_unique<TEvBlobStorage::TEvPutResult>(NKikimrProto::OK, msg.Id,
                    Agent.GetStorageStatusFlags(), Agent.VirtualGroupId, Agent.GetApproximateFreeSpaceShare()));
            }

            TEvBlobStorage::TEvPut& GetQuery() const {
                return *Event->Get<TEvBlobStorage::TEvPut>();
            }
        };

        return new TPutQuery(*this, std::move(ev));
    }

} // NKikimr::NBlobDepot
