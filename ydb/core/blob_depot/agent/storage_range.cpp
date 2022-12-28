#include "agent_impl.h"

namespace NKikimr::NBlobDepot {

    template<>
    TBlobDepotAgent::TQuery *TBlobDepotAgent::CreateQuery<TEvBlobStorage::EvRange>(std::unique_ptr<IEventHandle> ev) {
        class TRangeQuery : public TBlobStorageQuery<TEvBlobStorage::TEvRange> {
            std::unique_ptr<TEvBlobStorage::TEvRangeResult> Response;
            ui32 ReadsInFlight = 0;
            ui32 ResolvesInFlight = 0;
            std::map<TLogoBlobID, TString> FoundBlobs;
            std::vector<TLogoBlobID> Reads;
            bool Reverse = false;
            bool Finished = false;

        public:
            using TBlobStorageQuery::TBlobStorageQuery;

            void Initiate() override {
                if (Request.Decommission) {
                    Y_VERIFY(Agent.ProxyId);
                    STLOG(PRI_DEBUG, BLOB_DEPOT_AGENT, BDA26, "forwarding TEvRange", (VirtualGroupId, Agent.VirtualGroupId),
                        (TabletId, Agent.TabletId), (Msg, Request), (ProxyId, Agent.ProxyId));
                    const bool sent = TActivationContext::Send(Event->Forward(Agent.ProxyId));
                    Y_VERIFY(sent);
                    delete this;
                    return;
                }

                BDEV_QUERY(BDEV21, "TEvRange_new", (U.TabletId, Request.TabletId), (U.From, Request.From), (U.To, Request.To),
                    (U.MustRestoreFirst, Request.MustRestoreFirst), (U.IndexOnly, Request.IsIndexOnly));

                Response = std::make_unique<TEvBlobStorage::TEvRangeResult>(NKikimrProto::OK, Request.From, Request.To,
                    Agent.VirtualGroupId);

                IssueResolve();
            }

            void IssueResolve() {
                TString from = Request.From.AsBinaryString();
                TString to = Request.To.AsBinaryString();
                Reverse = Request.To < Request.From;
                if (Reverse) {
                    std::swap(from, to);
                }

                NKikimrBlobDepot::TEvResolve resolve;
                auto *item = resolve.AddItems();
                auto *range = item->MutableKeyRange();
                range->SetBeginningKey(from);
                range->SetIncludeBeginning(true);
                range->SetEndingKey(to);
                range->SetIncludeEnding(true);
                range->SetReverse(Reverse);
                item->SetTabletId(Request.TabletId);
                item->SetMustRestoreFirst(Request.MustRestoreFirst);

                Agent.Issue(std::move(resolve), this, nullptr);
                ++ResolvesInFlight;
            }

            void IssueResolve(TLogoBlobID id) {
                NKikimrBlobDepot::TEvResolve resolve;
                auto *item = resolve.AddItems();
                item->SetExactKey(id.AsBinaryString());
                item->SetTabletId(Request.TabletId);
                item->SetMustRestoreFirst(Request.MustRestoreFirst);

                Agent.Issue(std::move(resolve), this, nullptr);
                ++ResolvesInFlight;
            }

            void ProcessResponse(ui64 id, TRequestContext::TPtr context, TResponse response) override {
                if (auto *p = std::get_if<TEvBlobDepot::TEvResolveResult*>(&response)) {
                    HandleResolveResult(id, std::move(context), (*p)->Record);
                } else if (auto *p = std::get_if<TEvBlobStorage::TEvGetResult*>(&response)) {
                    Agent.HandleGetResult(context, **p);
                } else if (std::holds_alternative<TTabletDisconnected>(response)) {
                    EndWithError(NKikimrProto::ERROR, "BlobDepot tablet disconnected");
                } else {
                    Y_FAIL();
                }
            }

            void HandleResolveResult(ui64 id, TRequestContext::TPtr context, NKikimrBlobDepot::TEvResolveResult& msg) {
                --ResolvesInFlight;

                if (msg.GetStatus() != NKikimrProto::OK && msg.GetStatus() != NKikimrProto::OVERRUN) {
                    return EndWithError(msg.GetStatus(), msg.GetErrorReason());
                }

                for (const auto& key : msg.GetResolvedKeys()) {
                    const TString& blobId = key.GetKey();
                    auto id = TLogoBlobID::FromBinary(blobId);

                    if (key.HasErrorReason()) {
                        return EndWithError(NKikimrProto::ERROR, TStringBuilder() << "failed to resolve blob# " << id
                            << ": " << key.GetErrorReason());
                    } else if (!Request.IsIndexOnly) {
                        TReadArg arg{
                            key.GetValueChain(),
                            NKikimrBlobStorage::EGetHandleClass::FastRead,
                            Request.MustRestoreFirst,
                            this,
                            0,
                            0,
                            Reads.size(),
                            {}};
                        Reads.push_back(id);
                        ++ReadsInFlight;
                        TString error;
                        if (!Agent.IssueRead(arg, error)) {
                            return EndWithError(NKikimrProto::ERROR, TStringBuilder() << "failed to read discovered blob: "
                                << error);
                        }
                    } else if (Request.MustRestoreFirst) {
                        Y_FAIL("not implemented yet");
                    } else {
                        FoundBlobs.try_emplace(id);
                    }
                }

                if (msg.GetStatus() == NKikimrProto::OVERRUN) {
                    Agent.RegisterRequest(id, this, std::move(context), {}, true);
                } else {
                    CheckAndFinish();
                }
            }

            void OnRead(ui64 tag, NKikimrProto::EReplyStatus status, TString dataOrErrorReason) override {
                --ReadsInFlight;

                switch (status) {
                    case NKikimrProto::OK: {
                        const bool inserted = FoundBlobs.try_emplace(Reads[tag], std::move(dataOrErrorReason)).second;
                        Y_VERIFY(inserted);
                        break;
                    }

                    case NKikimrProto::NODATA:
                        IssueResolve(Reads[tag]);
                        break;

                    default:
                        return EndWithError(status, TStringBuilder() << "failed to retrieve BlobId# "
                            << Reads[tag] << " Error# " << dataOrErrorReason);
                }

                CheckAndFinish();
            }

            void CheckAndFinish() {
                if (!ReadsInFlight && !ResolvesInFlight && !Finished) {
                    for (auto& [id, buffer] : FoundBlobs) {
                        if (!Request.IsIndexOnly) {
                            Y_VERIFY_S(buffer.size() == id.BlobSize(), "Id# " << id << " Buffer.size# " << buffer.size());
                        }
                        if (buffer || Request.IsIndexOnly) {
                            Response->Responses.emplace_back(id, std::move(buffer));
                        }
                    }
                    if (Reverse) {
                        std::reverse(Response->Responses.begin(), Response->Responses.end());
                    }
                    EndWithSuccess();
                    Finished = true;
                }
            }

            void EndWithSuccess() {
                if (IS_LOG_PRIORITY_ENABLED(*TlsActivationContext, NLog::PRI_TRACE, NKikimrServices::BLOB_DEPOT_EVENTS)) {
                    for (const auto& r : Response->Responses) {
                        BDEV_QUERY(BDEV22, "TEvRange_item", (BlobId, r.Id), (Buffer.size, r.Buffer.size()));
                    }
                    BDEV_QUERY(BDEV14, "TEvRange_end", (Status, NKikimrProto::OK), (ErrorReason, ""));
                }
                TBlobStorageQuery::EndWithSuccess(std::move(Response));
            }

            void EndWithError(NKikimrProto::EReplyStatus status, const TString& errorReason) {
                BDEV_QUERY(BDEV15, "TEvRange_end", (Status, status), (ErrorReason, errorReason));
                TBlobStorageQuery::EndWithError(status, errorReason);
            }

            ui64 GetTabletId() const override {
                return Request.TabletId;
            }
        };

        return new TRangeQuery(*this, std::move(ev));
    }

} // NKikimr::NBlobDepot
