#include "blob_depot_tablet.h"
#include "schema.h"
#include "data.h"
#include "garbage_collection.h"

namespace NKikimr::NBlobDepot {

    void TBlobDepot::Handle(TEvBlobDepot::TEvCommitBlobSeq::TPtr ev) {
        class TTxCommitBlobSeq : public NTabletFlatExecutor::TTransactionBase<TBlobDepot> {
            std::unique_ptr<TEvBlobDepot::TEvCommitBlobSeq::THandle> Request;
            std::unique_ptr<IEventHandle> Response;

        public:
            TTxCommitBlobSeq(TBlobDepot *self, std::unique_ptr<TEvBlobDepot::TEvCommitBlobSeq::THandle> request)
                : TTransactionBase(self)
                , Request(std::move(request))
            {}

            bool Execute(TTransactionContext& txc, const TActorContext&) override {
                NIceDb::TNiceDb db(txc.DB);

                NKikimrBlobDepot::TEvCommitBlobSeqResult *responseRecord;
                std::tie(Response, responseRecord) = TEvBlobDepot::MakeResponseFor(*Request, Self->SelfId());

                TAgent& agent = Self->GetAgent(Request->Recipient);

                for (const auto& item : Request->Get()->Record.GetItems()) {
                    auto *responseItem = responseRecord->AddItems();
                    responseItem->SetStatus(NKikimrProto::OK);

                    NKikimrBlobDepot::TValue value;
                    if (item.HasMeta()) {
                        value.SetMeta(item.GetMeta());
                    }
                    auto *chain = value.AddValueChain();
                    auto *locator = chain->MutableLocator();
                    locator->CopyFrom(item.GetBlobLocator());

                    MarkGivenIdCommitted(agent, TBlobSeqId::FromProto(locator->GetBlobSeqId()));

                    if (!CheckKeyAgainstBarrier(item.GetKey(), responseItem)) {
                        continue;
                    }

                    Self->Data->PutKey(TData::TKey::FromBinaryKey(item.GetKey(), Self->Config), {
                        .Meta = value.GetMeta(),
                        .ValueChain = std::move(*value.MutableValueChain()),
                        .KeepState = value.GetKeepState(),
                        .Public = value.GetPublic(),
                    });

                    TString valueData;
                    const bool success = value.SerializeToString(&valueData);
                    Y_VERIFY(success);

                    db.Table<Schema::Data>().Key(item.GetKey()).Update<Schema::Data::Value>(valueData);
                }

                return true;
            }

            void MarkGivenIdCommitted(TAgent& agent, const TBlobSeqId& blobSeqId) {
                Y_VERIFY(blobSeqId.Generation == Self->Executor()->Generation());
                Y_VERIFY(blobSeqId.Channel < Self->Channels.size());

                auto& channel = Self->Channels[blobSeqId.Channel];

                const ui64 value = blobSeqId.ToSequentialNumber();
                agent.GivenIdRanges[blobSeqId.Channel].RemovePoint(value);
                channel.GivenIdRanges.RemovePoint(value);
            }

            bool CheckKeyAgainstBarrier(const TString& key, NKikimrBlobDepot::TEvCommitBlobSeqResult::TItem *responseItem) {
                if (Self->Config.GetOperationMode() == NKikimrBlobDepot::EOperationMode::VirtualGroup) {
                    if (key.size() != 3 * sizeof(ui64)) {
                        responseItem->SetStatus(NKikimrProto::ERROR);
                        responseItem->SetErrorReason("incorrect BlobId format");
                        return false;
                    }

                    const TLogoBlobID id(reinterpret_cast<const ui64*>(key.data()));
                    if (!Self->BarrierServer->CheckBlobForBarrier(id)) {
                        responseItem->SetStatus(NKikimrProto::ERROR);
                        responseItem->SetErrorReason(TStringBuilder() << "BlobId# " << id << " is being put beyond the barrier");
                        return false;
                    }
                }

                return true;
            }

            void Complete(const TActorContext&) override {
                Self->Data->HandleTrash();
                TActivationContext::Send(Response.release());
            }
        };

        Execute(std::make_unique<TTxCommitBlobSeq>(this, std::unique_ptr<TEvBlobDepot::TEvCommitBlobSeq::THandle>(
            ev.Release())));
    }

} // NKikimr::NBlobDepot
