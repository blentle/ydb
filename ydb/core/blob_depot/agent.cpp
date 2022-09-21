#include "blob_depot_tablet.h"
#include "data.h"

namespace NKikimr::NBlobDepot {

    void TBlobDepot::Handle(TEvTabletPipe::TEvServerConnected::TPtr ev) {
        STLOG(PRI_DEBUG, BLOB_DEPOT, BDT01, "TEvServerConnected", (Id, GetLogId()), (PipeServerId, ev->Get()->ServerId));
        const auto [it, inserted] = PipeServerToNode.emplace(ev->Get()->ServerId, std::nullopt);
        Y_VERIFY(inserted);
    }

    void TBlobDepot::Handle(TEvTabletPipe::TEvServerDisconnected::TPtr ev) {
        STLOG(PRI_DEBUG, BLOB_DEPOT, BDT02, "TEvServerDisconnected", (Id, GetLogId()), (PipeServerId, ev->Get()->ServerId));

        const auto it = PipeServerToNode.find(ev->Get()->ServerId);
        Y_VERIFY(it != PipeServerToNode.end());
        if (const auto& nodeId = it->second) {
            if (const auto agentIt = Agents.find(*nodeId); agentIt != Agents.end()) {
                if (TAgent& agent = agentIt->second; agent.PipeServerId == it->first) {
                    OnAgentDisconnect(agent);
                    agent.PipeServerId.reset();
                    agent.AgentId.reset();
                    agent.ConnectedNodeId = 0;
                    agent.ExpirationTimestamp = TActivationContext::Now() + ExpirationTimeout;
                }
            }
        }
        PipeServerToNode.erase(it);

        RegisterAgentQ.erase(ev->Get()->ServerId);
    }

    void TBlobDepot::OnAgentDisconnect(TAgent& agent) {
        agent.InvalidateStepRequests.clear();
        agent.PushCallbacks.clear();
    }

    void TBlobDepot::Handle(TEvBlobDepot::TEvRegisterAgent::TPtr ev) {
        if (!Configured || (Config.HasDecommitGroupId() && DecommitState < EDecommitState::BlocksFinished)) {
            auto& q = RegisterAgentQ[ev->Recipient];
            Y_VERIFY(q.empty());
            q.emplace_back(ev.Release());
            return;
        }

        const ui32 nodeId = ev->Sender.NodeId();
        const TActorId& pipeServerId = ev->Recipient;
        const auto& req = ev->Get()->Record;

        STLOG(PRI_DEBUG, BLOB_DEPOT, BDT03, "TEvRegisterAgent", (Id, GetLogId()), (Msg, req), (NodeId, nodeId),
            (PipeServerId, pipeServerId), (Id, ev->Cookie));

        const auto it = PipeServerToNode.find(pipeServerId);
        Y_VERIFY(it != PipeServerToNode.end());
        Y_VERIFY(!it->second || *it->second == nodeId);
        it->second = nodeId;
        auto& agent = Agents[nodeId];
        agent.PipeServerId = pipeServerId;
        agent.AgentId = ev->Sender;
        agent.ConnectedNodeId = nodeId;
        agent.ExpirationTimestamp = TInstant::Max();

        if (agent.AgentInstanceId && *agent.AgentInstanceId != req.GetAgentInstanceId()) {
            ResetAgent(agent);
        }
        agent.AgentInstanceId = req.GetAgentInstanceId();

        OnAgentConnect(agent);

        auto [response, record] = TEvBlobDepot::MakeResponseFor(*ev, SelfId(), Executor()->Generation());

        for (const auto& [k, v] : ChannelKinds) {
            auto *proto = record->AddChannelKinds();
            proto->SetChannelKind(k);
            for (const auto& [channel, groupId] : v.ChannelGroups) {
                auto *cg = proto->AddChannelGroups();
                cg->SetChannel(channel);
                cg->SetGroupId(groupId);
            }
        }

        if (Config.HasDecommitGroupId()) {
            record->SetDecommitGroupId(Config.GetDecommitGroupId());
        }

        TActivationContext::Send(response.release());
    }

    void TBlobDepot::OnAgentConnect(TAgent& /*agent*/) {
    }

    void TBlobDepot::Handle(TEvBlobDepot::TEvAllocateIds::TPtr ev) {
        STLOG(PRI_DEBUG, BLOB_DEPOT, BDT04, "TEvAllocateIds", (Id, GetLogId()), (Msg, ev->Get()->Record),
            (PipeServerId, ev->Recipient));

        const ui32 generation = Executor()->Generation();
        auto [response, record] = TEvBlobDepot::MakeResponseFor(*ev, SelfId(), ev->Get()->Record.GetChannelKind(), generation);

        if (const auto it = ChannelKinds.find(record->GetChannelKind()); it != ChannelKinds.end()) {
            auto& kind = it->second;
            auto *givenIdRange = record->MutableGivenIdRange();

            struct TGroupInfo {
                std::vector<TChannelInfo*> Channels;
            };
            std::unordered_map<ui32, TGroupInfo> groups;

            for (const auto& [channel, groupId] : kind.ChannelGroups) {
                Y_VERIFY_DEBUG(channel < Channels.size() && Channels[channel].ChannelKind == it->first);
                groups[groupId].Channels.push_back(&Channels[channel]);
            }

            std::vector<std::tuple<ui64, const TGroupInfo*>> options;

            ui64 accum = 0;
            for (const auto& [groupId, group] : groups) {
                //const ui64 allocatedBytes = Groups[groupId].AllocatedBytes;
                const ui64 groupWeight = 1;
                accum += groupWeight;
                options.emplace_back(accum, &group);
            }

            THashMap<ui8, NKikimrBlobDepot::TGivenIdRange::TChannelRange*> issuedRanges;
            for (ui32 i = 0, count = ev->Get()->Record.GetCount(); i < count; ++i) {
                const ui64 selection = RandomNumber(accum);
                const auto it = std::upper_bound(options.begin(), options.end(), selection,
                    [](ui64 x, const auto& y) { return x < std::get<0>(y); });
                const auto& [_, group] = *it;

                const size_t channelIndex = RandomNumber(group->Channels.size());
                TChannelInfo* const channel = group->Channels[channelIndex];

                const ui64 value = channel->NextBlobSeqId++;

                // fill in range item
                auto& range = issuedRanges[channel->Index];
                if (!range || range->GetEnd() != value) {
                    range = givenIdRange->AddChannelRanges();
                    range->SetChannel(channel->Index);
                    range->SetBegin(value);
                }
                range->SetEnd(value + 1);
            }

            // register issued ranges in agent and global records
            TAgent& agent = GetAgent(ev->Recipient);
            for (const auto& range : givenIdRange->GetChannelRanges()) {
                agent.GivenIdRanges[range.GetChannel()].IssueNewRange(range.GetBegin(), range.GetEnd());

                auto& givenIdRanges = Channels[range.GetChannel()].GivenIdRanges;
                const bool wasEmpty = givenIdRanges.IsEmpty();
                givenIdRanges.IssueNewRange(range.GetBegin(), range.GetEnd());
                if (wasEmpty) {
                    Data->OnLeastExpectedBlobIdChange(range.GetChannel());
                }

                STLOG(PRI_DEBUG, BLOB_DEPOT, BDT05, "IssueNewRange", (Id, GetLogId()),
                    (AgentId, agent.ConnectedNodeId), (Channel, range.GetChannel()),
                    (Begin, range.GetBegin()), (End, range.GetEnd()));
            }
        }

        TActivationContext::Send(response.release());
    }

    TBlobDepot::TAgent& TBlobDepot::GetAgent(const TActorId& pipeServerId) {
        const auto it = PipeServerToNode.find(pipeServerId);
        Y_VERIFY(it != PipeServerToNode.end());
        Y_VERIFY(it->second);
        TAgent& agent = GetAgent(*it->second);
        Y_VERIFY(agent.PipeServerId == pipeServerId);
        return agent;
    }

    TBlobDepot::TAgent& TBlobDepot::GetAgent(ui32 nodeId) {
        const auto agentIt = Agents.find(nodeId);
        Y_VERIFY(agentIt != Agents.end());
        TAgent& agent = agentIt->second;
        return agent;
    }

    void TBlobDepot::ResetAgent(TAgent& agent) {
        for (auto& [channel, agentGivenIdRange] : agent.GivenIdRanges) {
            Channels[channel].GivenIdRanges.Subtract(agentGivenIdRange);
            const ui32 channel_ = channel;
            const auto& agentGivenIdRange_ = agentGivenIdRange;
            STLOG(PRI_DEBUG, BLOB_DEPOT, BDT06, "ResetAgent", (Id, GetLogId()), (AgentId, agent.ConnectedNodeId),
                (Channel, channel_), (GivenIdRanges, Channels[channel_].GivenIdRanges),
                (Agent.GivenIdRanges, agentGivenIdRange_));
            agentGivenIdRange = {};
        }
        Data->HandleTrash();
    }

    void TBlobDepot::InitChannelKinds() {
        STLOG(PRI_DEBUG, BLOB_DEPOT, BDT07, "InitChannelKinds", (Id, GetLogId()));

        TTabletStorageInfo *info = Info();
        const ui32 generation = Executor()->Generation();

        Y_VERIFY(Channels.empty());

        ui32 channel = 0;
        for (const auto& profile : Config.GetChannelProfiles()) {
            for (ui32 i = 0, count = profile.GetCount(); i < count; ++i, ++channel) {
                if (channel >= 2) {
                    const auto kind = profile.GetChannelKind();
                    auto& p = ChannelKinds[kind];
                    p.ChannelToIndex[channel] = p.ChannelGroups.size();
                    p.ChannelGroups.emplace_back(channel, info->GroupFor(channel, generation));
                    Channels.push_back({
                        ui8(channel),
                        kind,
                        &p,
                        {},
                        TBlobSeqId{channel, generation, 1, 0}.ToSequentialNumber(),
                    });
                } else {
                    Channels.push_back({
                        ui8(channel),
                        NKikimrBlobDepot::TChannelKind::System,
                        nullptr,
                        {},
                        0
                    });
                }
            }
        }
    }

    void TBlobDepot::Handle(TEvBlobDepot::TEvPushNotifyResult::TPtr ev) {
        class TTxInvokeCallback : public NTabletFlatExecutor::TTransactionBase<TBlobDepot> {
            TEvBlobDepot::TEvPushNotifyResult::TPtr Ev;

        public:
            TTxInvokeCallback(TBlobDepot *self, TEvBlobDepot::TEvPushNotifyResult::TPtr ev)
                : TTransactionBase(self)
                , Ev(ev)
            {}

            bool Execute(TTransactionContext& /*txc*/, const TActorContext&) override {
                TAgent& agent = Self->GetAgent(Ev->Recipient);
                if (const auto it = agent.PushCallbacks.find(Ev->Cookie); it != agent.PushCallbacks.end()) {
                    auto callback = std::move(it->second);
                    agent.PushCallbacks.erase(it);
                    callback(Ev);
                }
                return true;
            }

            void Complete(const TActorContext&) override {}
        };

        Execute(std::make_unique<TTxInvokeCallback>(this, ev));
    }

    void TBlobDepot::ProcessRegisterAgentQ() {
        if (!Configured || (Config.HasDecommitGroupId() && DecommitState < EDecommitState::BlocksFinished)) {
            return;
        }

        for (auto& [pipeServerId, events] : std::exchange(RegisterAgentQ, {})) {
            for (auto& ev : events) {
                TAutoPtr<IEventHandle> tmp(ev.release());
                Receive(tmp, TActivationContext::AsActorContext());
            }
        }
    }

} // NKikimr::NBlobDepot
