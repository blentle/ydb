#pragma once

#include "defs.h"

namespace NKikimr::NBlobDepot {

#define ENUMERATE_INCOMING_EVENTS(XX) \
        XX(EvPut) \
        XX(EvGet) \
        XX(EvBlock) \
        XX(EvDiscover) \
        XX(EvRange) \
        XX(EvCollectGarbage) \
        XX(EvStatus) \
        XX(EvPatch) \
        XX(EvAssimilate) \
        // END

    class TBlobDepotAgent;

    struct TRequestContext {
        virtual ~TRequestContext() = default;

        template<typename T>
        T& Obtain() {
            T *sp = static_cast<T*>(this);
            Y_VERIFY_DEBUG(sp && sp == dynamic_cast<T*>(this));
            return *sp;
        }

        using TPtr = std::shared_ptr<TRequestContext>;
    };

    struct TTabletDisconnected {};

    struct TKeyResolved {
        const TResolvedValueChain* ValueChain;
        std::optional<TString> ErrorReason;
    };

    class TRequestSender;

    struct TRequestInFlight
        : TIntrusiveListItem<TRequestInFlight>
        , TNonCopyable
    {
        using TCancelCallback = std::function<void()>;

        const ui64 Id;
        TRequestSender* const Sender = {};
        TRequestContext::TPtr Context;
        TCancelCallback CancelCallback;
        const bool ToBlobDepotTablet = {};

        TRequestInFlight(ui64 id)
            : Id(id)
        {}

        TRequestInFlight(ui64 id, TRequestSender *sender, TRequestContext::TPtr context, TCancelCallback cancelCallback,
                bool toBlobDepotTablet);

        struct THash {
            size_t operator ()(const TRequestInFlight& x) const { return std::hash<ui64>()(x.Id); }
        };

        friend bool operator ==(const TRequestInFlight& x, const TRequestInFlight& y) {
            return x.Id == y.Id;
        }
    };

    class TRequestSender {
        TIntrusiveList<TRequestInFlight> RequestsInFlight;

    protected:
        TBlobDepotAgent& Agent;

    public:
        using TResponse = std::variant<
            // internal events
            TTabletDisconnected,
            TKeyResolved,

            // tablet responses
            TEvBlobDepot::TEvRegisterAgentResult*,
            TEvBlobDepot::TEvAllocateIdsResult*,
            TEvBlobDepot::TEvBlockResult*,
            TEvBlobDepot::TEvQueryBlocksResult*,
            TEvBlobDepot::TEvCollectGarbageResult*,
            TEvBlobDepot::TEvCommitBlobSeqResult*,
            TEvBlobDepot::TEvResolveResult*,

            // underlying DS proxy responses
            TEvBlobStorage::TEvGetResult*,
            TEvBlobStorage::TEvPutResult*
        >;

        static TString ToString(const TResponse& response);

    public:
        TRequestSender(TBlobDepotAgent& agent);
        virtual ~TRequestSender();
        void ClearRequestsInFlight();
        void OnRequestComplete(TRequestInFlight& requestInFlight, TResponse response);

    protected:
        virtual void ProcessResponse(ui64 id, TRequestContext::TPtr context, TResponse response) = 0;

    private:
        friend struct TRequestInFlight;
        void RegisterRequestInFlight(TRequestInFlight *requestInFlight);
    };

    class TBlobDepotAgent
        : public TActorBootstrapped<TBlobDepotAgent>
        , public TRequestSender
    {
        const ui32 VirtualGroupId;
        const TActorId ProxyId;
        const ui64 AgentInstanceId;
        ui64 TabletId = Max<ui64>();
        TActorId PipeId;
        TActorId PipeServerId;
        bool IsConnected = false;

    private:
        struct TEvPrivate {
            enum {
                EvQueryWatchdog = EventSpaceBegin(TEvents::ES_PRIVATE),
                EvProcessPendingEvent,
                EvPendingEventQueueWatchdog,
            };
        };

    public:
        static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
            return NKikimrServices::TActivity::BLOB_DEPOT_AGENT_ACTOR;
        }

        TBlobDepotAgent(ui32 virtualGroupId, TIntrusivePtr<TBlobStorageGroupInfo> info, TActorId proxyId);
        ~TBlobDepotAgent();

        void Bootstrap();

#define FORWARD_STORAGE_PROXY(TYPE) fFunc(TEvBlobStorage::TYPE, HandleStorageProxy);
        void StateFunc(STFUNC_SIG) {
            STRICT_STFUNC_BODY(
                cFunc(TEvents::TSystem::Poison, PassAway);
                hFunc(TEvBlobStorage::TEvConfigureProxy, Handle);

                hFunc(TEvTabletPipe::TEvClientConnected, Handle);
                hFunc(TEvTabletPipe::TEvClientDestroyed, Handle);

                hFunc(TEvBlobDepot::TEvPushNotify, Handle);

                hFunc(TEvBlobDepot::TEvRegisterAgentResult, HandleTabletResponse);
                hFunc(TEvBlobDepot::TEvAllocateIdsResult, HandleTabletResponse);
                hFunc(TEvBlobDepot::TEvBlockResult, HandleTabletResponse);
                hFunc(TEvBlobDepot::TEvQueryBlocksResult, HandleTabletResponse);
                hFunc(TEvBlobDepot::TEvCollectGarbageResult, HandleTabletResponse);
                hFunc(TEvBlobDepot::TEvCommitBlobSeqResult, HandleTabletResponse);
                hFunc(TEvBlobDepot::TEvResolveResult, HandleTabletResponse);

                hFunc(TEvBlobStorage::TEvGetResult, HandleOtherResponse);
                hFunc(TEvBlobStorage::TEvPutResult, HandleOtherResponse);

                ENUMERATE_INCOMING_EVENTS(FORWARD_STORAGE_PROXY)
                hFunc(TEvBlobStorage::TEvBunchOfEvents, Handle);
                cFunc(TEvPrivate::EvProcessPendingEvent, HandlePendingEvent);
                cFunc(TEvPrivate::EvPendingEventQueueWatchdog, HandlePendingEventQueueWatchdog);

                cFunc(TEvPrivate::EvQueryWatchdog, HandleQueryWatchdog);
            )

            DeletePendingQueries.Clear();
        }
#undef FORWARD_STORAGE_PROXY

        void PassAway() override {
            NTabletPipe::CloseAndForgetClient(SelfId(), PipeId);
            TActor::PassAway();
        }

        void Handle(TEvBlobStorage::TEvConfigureProxy::TPtr ev) {
            if (const auto& info = ev->Get()->Info) {
                Y_VERIFY(info->BlobDepotId);
                if (TabletId != *info->BlobDepotId) {
                    TabletId = *info->BlobDepotId;
                    if (TabletId && TabletId != Max<ui64>()) {
                        ConnectToBlobDepot();
                    }
                }
                if (!info->GetTotalVDisksNum()) {
                    TActivationContext::Send(new IEventHandle(TEvents::TSystem::Poison, 0, ProxyId, {}, nullptr, 0));
                    return;
                }
            }

            TActivationContext::Send(ev->Forward(ProxyId));
        }

        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        // Request/response delivery logic

        using TRequestsInFlight = std::unordered_set<TRequestInFlight, TRequestInFlight::THash>;

        ui64 NextTabletRequestId = 1;
        TRequestsInFlight TabletRequestInFlight;
        ui64 NextOtherRequestId = 1;
        TRequestsInFlight OtherRequestInFlight;

        void RegisterRequest(ui64 id, TRequestSender *sender, TRequestContext::TPtr context,
            TRequestInFlight::TCancelCallback cancelCallback, bool toBlobDepotTablet);

        template<typename TEvent>
        void HandleTabletResponse(TAutoPtr<TEventHandle<TEvent>> ev);

        template<typename TEvent>
        void HandleOtherResponse(TAutoPtr<TEventHandle<TEvent>> ev);

        void OnRequestComplete(ui64 id, TRequestSender::TResponse response, TRequestsInFlight& map);
        void DropTabletRequest(ui64 id);

        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////

        struct TAllocateIdsContext : TRequestContext {
            NKikimrBlobDepot::TChannelKind::E ChannelKind;

            TAllocateIdsContext(NKikimrBlobDepot::TChannelKind::E channelKind)
                : ChannelKind(channelKind)
            {}
        };

        ui32 BlobDepotGeneration = 0;
        std::optional<ui32> DecommitGroupId;
        NKikimrBlobStorage::TPDiskSpaceColor::E SpaceColor = {};
        float ApproximateFreeSpaceShare = 0.0f;

        void Handle(TEvTabletPipe::TEvClientConnected::TPtr ev);
        void Handle(TEvTabletPipe::TEvClientDestroyed::TPtr ev);
        void ConnectToBlobDepot();
        void OnConnect();
        void OnDisconnect();

        void ProcessResponse(ui64 id, TRequestContext::TPtr context, TResponse response) override;
        void Handle(TRequestContext::TPtr context, NKikimrBlobDepot::TEvRegisterAgentResult& msg);
        void Handle(TRequestContext::TPtr context, NKikimrBlobDepot::TEvAllocateIdsResult& msg);

        template<typename T, typename = typename TEvBlobDepot::TEventFor<T>::Type>
        ui64 Issue(T msg, TRequestSender *sender, TRequestContext::TPtr context);

        ui64 Issue(std::unique_ptr<IEventBase> ev, TRequestSender *sender, TRequestContext::TPtr context);

        void Handle(TEvBlobDepot::TEvPushNotify::TPtr ev);

        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////

        struct TExecutingQueries {};
        struct TPendingBlockChecks {};
        struct TPendingId {};

        class TQuery
            : public TIntrusiveListItem<TQuery, TExecutingQueries>
            , public TIntrusiveListItem<TQuery, TPendingBlockChecks>
            , public TIntrusiveListItem<TQuery, TPendingId>
            , public TRequestSender
        {
        protected:
            std::unique_ptr<IEventHandle> Event; // original query event
            const ui64 QueryId;
            mutable TString QueryIdString;
            const TMonotonic StartTime;
            std::multimap<TMonotonic, TQuery*>::iterator QueryWatchdogMapIter;
            NLog::EPriority WatchdogPriority = NLog::PRI_WARN;
            bool Destroyed = false;

            static constexpr TDuration WatchdogDuration = TDuration::Seconds(10);

        public:
            TQuery(TBlobDepotAgent& agent, std::unique_ptr<IEventHandle> event);
            virtual ~TQuery();

            void CheckQueryExecutionTime(TMonotonic now);

            void EndWithError(NKikimrProto::EReplyStatus status, const TString& errorReason);
            void EndWithSuccess(std::unique_ptr<IEventBase> response);
            TString GetName() const;
            TString GetQueryId() const;
            virtual ui64 GetTabletId() const { return 0; }
            virtual void Initiate() = 0;

            virtual void OnUpdateBlock() {}
            virtual void OnRead(ui64 /*tag*/, NKikimrProto::EReplyStatus /*status*/, TString /*dataOrErrorReason*/) {}
            virtual void OnIdAllocated() {}
            virtual void OnDestroy(bool /*success*/) {}

        public:
            struct TDeleter {
                static void Destroy(TQuery *query) { delete query; }
            };

        private:
            void DoDestroy();
        };

        template<typename TEvent>
        class TBlobStorageQuery : public TQuery {
        public:
            TBlobStorageQuery(TBlobDepotAgent& agent, std::unique_ptr<IEventHandle> event)
                : TQuery(agent, std::move(event))
                , Request(*Event->Get<TEvent>())
            {}

        protected:
            TEvent& Request;
        };

        struct TPendingEvent {
            std::unique_ptr<IEventHandle> Event;
            size_t Size;
            TMonotonic ExpirationTimestamp;
        };

        std::deque<TPendingEvent> PendingEventQ;
        size_t PendingEventBytes = 0;
        static constexpr size_t MaxPendingEventBytes = 32'000'000; // ~32 MB
        static constexpr TDuration EventExpirationTime = TDuration::Seconds(5);
        TIntrusiveListWithAutoDelete<TQuery, TQuery::TDeleter, TExecutingQueries> ExecutingQueries;
        TIntrusiveListWithAutoDelete<TQuery, TQuery::TDeleter, TExecutingQueries> DeletePendingQueries;
        std::multimap<TMonotonic, TQuery*> QueryWatchdogMap;

        template<ui32 EventType> TQuery *CreateQuery(std::unique_ptr<IEventHandle> ev);
        void HandleStorageProxy(TAutoPtr<IEventHandle> ev);
        void HandlePendingEvent();
        void ProcessStorageEvent(std::unique_ptr<IEventHandle> ev);
        void HandlePendingEventQueueWatchdog();
        void Handle(TEvBlobStorage::TEvBunchOfEvents::TPtr ev);
        void HandleQueryWatchdog();

        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////

        struct TChannelKind
            : NBlobDepot::TChannelKind
        {
            const NKikimrBlobDepot::TChannelKind::E Kind;

            bool IdAllocInFlight = false;

            THashMap<ui8, TGivenIdRange> GivenIdRangePerChannel;
            ui32 NumAvailableItems = 0;

            std::set<TBlobSeqId> WritesInFlight;

            TIntrusiveList<TQuery, TPendingId> QueriesWaitingForId;

            TChannelKind(NKikimrBlobDepot::TChannelKind::E kind)
                : Kind(kind)
            {}

            void IssueGivenIdRange(const NKikimrBlobDepot::TGivenIdRange& proto);
            ui32 GetNumAvailableItems() const;
            std::optional<TBlobSeqId> Allocate(TBlobDepotAgent& agent);
            std::tuple<TLogoBlobID, ui32> MakeBlobId(TBlobDepotAgent& agent, const TBlobSeqId& blobSeqId, EBlobType type,
                    ui32 part, ui32 size) const;
            void Trim(ui8 channel, ui32 generation, ui32 invalidatedStep);
            void RebuildHeap();

            void EnqueueQueryWaitingForId(TQuery *query);
            void ProcessQueriesWaitingForId();
        };

        THashMap<NKikimrBlobDepot::TChannelKind::E, TChannelKind> ChannelKinds;
        THashMap<ui8, TChannelKind*> ChannelToKind;

        void IssueAllocateIdsIfNeeded(TChannelKind& kind);

        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        // DS proxy interaction

        void SendToProxy(ui32 groupId, std::unique_ptr<IEventBase> event, TRequestSender *sender, TRequestContext::TPtr context);

        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        // Blocks

        class TBlocksManager;
        std::unique_ptr<TBlocksManager> BlocksManagerPtr;
        TBlocksManager& BlocksManager;

        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        // Reading

        struct TReadContext;
        struct TReadArg {
            const NProtoBuf::RepeatedPtrField<NKikimrBlobDepot::TResolvedValueChain>& Values;
            NKikimrBlobStorage::EGetHandleClass GetHandleClass;
            bool MustRestoreFirst = false;
            TQuery *Query = nullptr;
            ui64 Offset = 0;
            ui64 Size = 0;
            ui64 Tag = 0;
            std::optional<TEvBlobStorage::TEvGet::TReaderTabletData> ReaderTabletData;
        };

        bool IssueRead(const TReadArg& arg, TString& error);

        void HandleGetResult(const TRequestContext::TPtr& context, TEvBlobStorage::TEvGetResult& msg);

        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        // Blob mapping cache

        class TBlobMappingCache;
        std::unique_ptr<TBlobMappingCache> BlobMappingCachePtr;
        TBlobMappingCache& BlobMappingCache;

        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        // Status flags

        TStorageStatusFlags GetStorageStatusFlags() const;
        float GetApproximateFreeSpaceShare() const;
    };

#define BDEV_QUERY(MARKER, TEXT, ...) BDEV(MARKER, TEXT, (VG, Agent.VirtualGroupId), (BDT, Agent.TabletId), \
                                      (G, Agent.BlobDepotGeneration), (Q, QueryId), __VA_ARGS__)

} // NKikimr::NBlobDepot
