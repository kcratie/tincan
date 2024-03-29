/*
 * EdgeVPNio
 * Copyright 2023, University of Florida
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "tincan_exception.h"
#include "turn_descriptor.h"
#include "virtual_link.h"
#include "rtc_base/string_encode.h"
#include "p2p/base/default_ice_transport_factory.h"
#include "rtc_base/bind.h"
namespace tincan
{
    extern BufferPool<Iob> bp;
    using namespace rtc;
    VirtualLink::VirtualLink(
        unique_ptr<VlinkDescriptor> vlink_desc,
        unique_ptr<PeerDescriptor> peer_desc,
        rtc::Thread *signaling_thread,
        rtc::Thread *network_thread) : vlink_desc_(move(vlink_desc)),
                                       peer_desc_(move(peer_desc)),
                                       local_conn_role_(cricket::CONNECTIONROLE_ACTPASS),
                                       dtls_transport_(nullptr),
                                       packet_options_(DSCP_DEFAULT),
                                       signaling_thread_(signaling_thread),
                                       network_thread_(network_thread),
                                       pa_init_(false)
    {
        content_name_.append(vlink_desc_->uid.substr(0, 7));
        local_description_ = make_unique<cricket::SessionDescription>();
        remote_description_ = make_unique<cricket::SessionDescription>();
        ice_transport_factory_ = make_unique<webrtc::DefaultIceTransportFactory>();
        config_.transport_observer = this;
        config_.rtcp_handler = [](const rtc::CopyOnWriteBuffer &packet,
                                  int64_t packet_time_us)
        { RTC_NOTREACHED(); };
        config_.ice_transport_factory = ice_transport_factory_.get();
    }

    string VirtualLink::Name()
    {
        return content_name_;
    }

    void
    VirtualLink::Initialize(
        unique_ptr<SSLIdentity> sslid,
        unique_ptr<SSLFingerprint> local_fingerprint,
        cricket::IceRole ice_role,
        const vector<string> &ignored_list)
    {
        ice_role_ = ice_role;
        net_manager_.set_network_ignore_list(ignored_list);
        port_allocator_.reset(new cricket::BasicPortAllocator(&net_manager_));
        port_allocator_->SetConfiguration(
            SetupSTUN(vlink_desc_->stun_servers),
            SetupTURN(vlink_desc_->turn_descs),
            0, webrtc::PRUNE_BASED_ON_PRIORITY);
        transport_ctlr_ = make_unique<JsepTransportController>(
            signaling_thread_,
            network_thread_,
            port_allocator_.get(),
            /*async_resolver_factory*/ nullptr,
            config_);
        SetupICE(move(sslid), move(local_fingerprint), ice_role);
        dtls_transport_ = transport_ctlr_->GetDtlsTransport(content_name_);
        RegisterLinkEventHandlers();
        return;
    }

    /* Parses the string delimited list of candidates and adds
    them to the P2P transport thereby creating ICE connections
    */
    void
    VirtualLink::AddRemoteCandidates(
        const string &candidates)
    {
        std::istringstream iss(candidates);
        cricket::Candidates cas_vec;
        do
        {
            string candidate_str;
            iss >> candidate_str;
            vector<string> fields;
            size_t len = rtc::split(candidate_str, kCandidateDelim, &fields);
            rtc::SocketAddress sa;
            if (len >= 10)
            {
                sa.FromString(fields[2].append(":").append(fields[3]));
                cricket::Candidate candidate(
                    atoi(fields[0].c_str()), // component
                    fields[1],               // protocol
                    sa,                      // socket address
                    atoi(fields[4].c_str()), // priority
                    fields[5],               // username
                    fields[6],               // password
                    fields[7],               // type
                    atoi(fields[8].c_str()), // generation
                    fields[9]);              // foundation
                cas_vec.push_back(candidate);
            }
        } while (iss);
        webrtc::RTCError err = transport_ctlr_->AddRemoteCandidates(content_name_, cas_vec);
        if (!err.ok())
            RTC_LOG(LS_ERROR) << string("Failed to add remote candidates - ") + err.message();
        return;
    }

    void
    VirtualLink::OnReadPacket(
        PacketTransportInternal *,
        const char *data,
        size_t len,
        const int64_t &,
        int)
    {
        SignalMessageReceived(data, len);
    }

    void
    VirtualLink::OnSentPacket(
        PacketTransportInternal *,
        const rtc::SentPacket &sp)
    {
        // nothing to do atm ...
    }

    void VirtualLink::OnCandidatesGathered(
        const string &,
        const cricket::Candidates &candidates)
    {
        local_candidates_.insert(local_candidates_.end(), candidates.begin(),
                                 candidates.end());
        return;
    }

    void VirtualLink::OnGatheringState(
        cricket::IceGatheringState gather_state)
    {
        // gather_state_ = gather_state;
        if (cas_ready_id_ && gather_state == cricket::kIceGatheringComplete)
        {
            SignalLocalCasReady(cas_ready_id_, Candidates());
            cas_ready_id_ = 0;
        }
        return;
    }

    void VirtualLink::OnWriteableState(
        PacketTransportInternal *transport)
    {
        if (transport->writable())
        {
            RTC_LOG(LS_INFO) << "Connection established to: " << peer_desc_->uid;
            SignalLinkUp(vlink_desc_->uid);
        }
        else
        {
            RTC_LOG(LS_INFO) << "Link NOT writeable: " << peer_desc_->uid;
            SignalLinkDown(vlink_desc_->uid);
        }
    }

    void
    VirtualLink::RegisterLinkEventHandlers()
    {
        dtls_transport_->SignalReadPacket.connect(this, &VirtualLink::OnReadPacket);
        dtls_transport_->SignalSentPacket.connect(this, &VirtualLink::OnSentPacket);
        dtls_transport_->SignalWritableState.connect(this, &VirtualLink::OnWriteableState);

        transport_ctlr_->SignalIceCandidatesGathered.connect(
            this, &VirtualLink::OnCandidatesGathered);
        transport_ctlr_->SignalIceGatheringState.connect(
            this, &VirtualLink::OnGatheringState);
    }

    void VirtualLink::Transmit(Iob&& frame)
    {
        int status = dtls_transport_->SendPacket(frame.data(), frame.size(), packet_options_, 0);
        bp.put(std::move(frame));
        if (status < 0)
            RTC_LOG(LS_INFO) << "Vlink send failed. ERRNO: " << dtls_transport_->GetError();
    }

    string VirtualLink::Candidates()
    {
        std::ostringstream oss;
        for (auto &cnd : local_candidates_)
        {
            oss << cnd.component()
                << kCandidateDelim << cnd.protocol()
                << kCandidateDelim << cnd.address().ToString()
                << kCandidateDelim << cnd.priority()
                << kCandidateDelim << cnd.username()
                << kCandidateDelim << cnd.password()
                << kCandidateDelim << cnd.type()
                << kCandidateDelim << cnd.generation()
                << kCandidateDelim << cnd.foundation()
                << " ";
        }
        return oss.str();
    }

    string VirtualLink::PeerCandidates()
    {
        return peer_desc_->cas;
    }

    void
    VirtualLink::PeerCandidates(
        const string &peer_cas)
    {
        peer_desc_->cas = peer_cas;
        if (peer_desc_->cas.length() != 0)
            AddRemoteCandidates(peer_desc_->cas);
    }

    void
    VirtualLink::GetStats(Json::Value &stats)
    {
        cricket::TransportStats transport_stats;
        transport_ctlr_->GetStats(content_name_, &transport_stats);
        for (const cricket::TransportChannelStats &channel_stat : transport_stats.channel_stats)
            for (const cricket::ConnectionInfo &info : channel_stat.ice_transport_stats.connection_infos)
            {
                Json::Value stat(Json::objectValue);
                stat["best_conn"] = info.best_connection;
                stat["writable"] = info.writable;
                stat["receiving"] = info.receiving;
                stat["timeout"] = info.timeout;
                stat["new_conn"] = info.new_connection;

                stat["rtt"] = (Json::UInt64)info.rtt;
                stat["sent_total_bytes"] = (Json::UInt64)info.sent_total_bytes;
                stat["sent_bytes_second"] = (Json::UInt64)info.sent_bytes_second;
                stat["sent_discarded_packets"] = (Json::UInt64)info.sent_discarded_packets;
                stat["sent_total_packets"] = (Json::UInt64)info.sent_total_packets;
                stat["sent_ping_requests_total"] = (Json::UInt64)info.sent_ping_requests_total;
                stat["sent_ping_requests_before_first_response"] = (Json::UInt64)info.sent_ping_requests_before_first_response;
                stat["sent_ping_responses"] = (Json::UInt64)info.sent_ping_responses;

                stat["recv_total_bytes"] = (Json::UInt64)info.recv_total_bytes;
                stat["recv_bytes_second"] = (Json::UInt64)info.recv_bytes_second;
                stat["recv_ping_requests"] = (Json::UInt64)info.recv_ping_requests;
                stat["recv_ping_responses"] = (Json::UInt64)info.recv_ping_responses;

                stat["local_candidate"] = info.local_candidate.ToString();
                stat["remote_candidate"] = info.remote_candidate.ToString();
                stat["state"] = (Json::UInt)info.state;
                // http://tools.ietf.org/html/rfc5245#section-5.7.4
                stats.append(stat);
            }
    }

    void
    VirtualLink::SetupICE(
        unique_ptr<SSLIdentity> sslid,
        unique_ptr<SSLFingerprint> local_fingerprint,
        cricket::IceRole ice_role)
    {
        if (vlink_desc_->dtls_enabled)
        {
            transport_ctlr_->SetLocalCertificate(RTCCertificate::Create(move(sslid)));

            size_t pos = peer_desc_->fingerprint.find(' ');
            string alg, fp;
            if (pos != string::npos)
            {
                alg = peer_desc_->fingerprint.substr(0, pos);
                fp = peer_desc_->fingerprint.substr(++pos);
                remote_fingerprint_.reset(
                    rtc::SSLFingerprint::CreateFromRfc4572(alg, fp));
            }
        }
        else
        {
            local_fingerprint.release();
            RTC_LOG(LS_INFO) << "Not using DTLS on vlink " << content_name_ << "\n";
        }
        cricket::IceConfig ic;
        ic.continual_gathering_policy = cricket::GATHER_ONCE;
        transport_ctlr_->SetIceConfig(ic);
        cricket::ConnectionRole remote_conn_role = cricket::CONNECTIONROLE_ACTIVE;
        local_conn_role_ = cricket::CONNECTIONROLE_ACTPASS;
        if (cricket::ICEROLE_CONTROLLED == ice_role)
        {
            local_conn_role_ = cricket::CONNECTIONROLE_ACTIVE;
            remote_conn_role = cricket::CONNECTIONROLE_ACTPASS;
        }

        cricket::TransportDescription local_transport_desc(
            vector<string>(), kIceUfrag, kIcePwd,
            cricket::ICEMODE_FULL, local_conn_role_, local_fingerprint.get());

        cricket::TransportDescription remote_transport_desc(
            vector<string>(), kIceUfrag, kIcePwd,
            cricket::ICEMODE_FULL, remote_conn_role, remote_fingerprint_.get());

        cricket::ContentGroup bundle_group(cricket::GROUP_TYPE_BUNDLE);
        bundle_group.AddContentName(content_name_);

        std::unique_ptr<cricket::SctpDataContentDescription> data(
            new cricket::SctpDataContentDescription());
        data->set_rtcp_mux(true);
        local_description_->AddContent(content_name_, cricket::MediaProtocolType::kSctp, move(data));
        local_description_->AddGroup(bundle_group);
        local_description_->AddTransportInfo(cricket::TransportInfo(content_name_, local_transport_desc));

        data.reset(new cricket::SctpDataContentDescription());
        remote_description_->AddContent(content_name_, cricket::MediaProtocolType::kSctp, move(data));
        remote_description_->AddGroup(bundle_group);
        remote_description_->AddTransportInfo(cricket::TransportInfo(content_name_, remote_transport_desc));

        if (ice_role == cricket::ICEROLE_CONTROLLING)
        {
            RTC_LOG(LS_INFO) << "Creating CONTROLLING vlink to peer " << peer_desc_->uid;
            transport_ctlr_->SetLocalDescription(SdpType::kOffer, local_description_.get());
            transport_ctlr_->SetRemoteDescription(SdpType::kAnswer, remote_description_.get());
        }
        else if (ice_role == cricket::ICEROLE_CONTROLLED)
        {
            // when receiving an offer the remote description with the offer must be set first.
            RTC_LOG(LS_INFO) << "Creating CONTROLLED vlink to peer " << peer_desc_->uid;
            transport_ctlr_->SetRemoteDescription(SdpType::kOffer, remote_description_.get());
            transport_ctlr_->SetLocalDescription(SdpType::kAnswer, local_description_.get());
        }
        else
        {
            RTC_LOG(LS_ERROR) << "Invalid ice role specified " << ice_role;
        }
    }

    cricket::ServerAddresses
    VirtualLink::SetupSTUN(
        vector<string> stun_servers)
    {
        cricket::ServerAddresses stun_addrs;
        if (stun_servers.empty())
        {
            RTC_LOG(LS_INFO) << "No STUN Server address provided";
        }
        for (auto stun_server : stun_servers)
        {
            rtc::SocketAddress stun_addr;
            stun_addr.FromString(stun_server);
            stun_addrs.insert(stun_addr);
        }
        return stun_addrs;
    }

    vector<cricket::RelayServerConfig>
    VirtualLink::SetupTURN(
        const vector<TurnDescriptor> turn_descs)
    {
        if (turn_descs.empty())
        {
            RTC_LOG(LS_INFO) << "No TURN Server address provided";
            return vector<cricket::RelayServerConfig>();
        }
        vector<cricket::RelayServerConfig> turn_servers;
        for (auto turn_desc : turn_descs)
        {
            if (turn_desc.username.empty() || turn_desc.password.empty())
            {
                RTC_LOG(LS_WARNING) << "TURN credentials were not provided for hostname " << turn_desc.server_hostname;
                continue;
            }

            vector<string> addr_port;
            rtc::split(turn_desc.server_hostname, ':', &addr_port);
            if (addr_port.size() != 2)
            {
                RTC_LOG(LS_INFO) << "Invalid TURN Server address provided. Address must contain a port number separated by a \":\".";
                continue;
            }
            cricket::RelayServerConfig relay_config_udp(addr_port[0], stoi(addr_port[1]),
                                                        turn_desc.username, turn_desc.password, cricket::PROTO_UDP);
            turn_servers.push_back(relay_config_udp);
        }
        return turn_servers;
    }

    void
    VirtualLink::StartConnections()
    {
        if (!pa_init_)
            InitializePortAllocator();
        if (peer_desc_->cas.length() != 0)
            AddRemoteCandidates(peer_desc_->cas);
        transport_ctlr_->MaybeStartGathering();
    }

    void VirtualLink::Disconnect()
    {
        dtls_transport_->disconnect_all();
    }

    bool VirtualLink::IsReady()
    {
        return dtls_transport_->writable();
    }

    bool VirtualLink::OnTransportChanged(
        const std::string &mid,
        webrtc::RtpTransportInternal *rtp_transport,
        rtc::scoped_refptr<webrtc::DtlsTransport> dtls_transport,
        webrtc::DataChannelTransportInterface *data_channel_transport)
    {
        if (transport_ctlr_)
            dtls_transport_ = transport_ctlr_->GetDtlsTransport(content_name_);
        return true;
    }

    bool
    VirtualLink::InitializePortAllocator()
    {
        port_allocator_->set_flags(port_allocator_->flags() | cricket::PORTALLOCATOR_DISABLE_TCP);
        port_allocator_->Initialize();
        pa_init_ = true;
        return true;
    }

    void VirtualLink::SetCasReadyId(uint64_t id) noexcept
    {
        cas_ready_id_ = id;
    }
} // end namespace tincan
