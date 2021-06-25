﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "WebRtcTransport.h"
#include <iostream>
#include "RtpExt.h"
#include "Rtcp/Rtcp.h"
#include "Rtcp/RtcpFCI.h"
#include "Rtsp/RtpReceiver.h"

#define RTX_SSRC_OFFSET 2
#define RTP_CNAME "zlmediakit-rtp"
#define RTP_LABEL "zlmediakit-label"
#define RTP_MSLABEL "zlmediakit-mslabel"
#define RTP_MSID RTP_MSLABEL " " RTP_LABEL

//RTC配置项目
namespace RTC {
#define RTC_FIELD "rtc."
//rtp和rtcp接受超时时间
const string kTimeOutSec = RTC_FIELD"timeoutSec";
//服务器外网ip
const string kExternIP = RTC_FIELD"externIP";
//设置remb比特率，非0时关闭twcc并开启remb。该设置在rtc推流时有效，可以控制推流画质
const string kRembBitRate = RTC_FIELD"rembBitRate";

static onceToken token([]() {
    mINI::Instance()[kTimeOutSec] = 15;
    mINI::Instance()[kExternIP] = "";
    mINI::Instance()[kRembBitRate] = 0;
});

}//namespace RTC

WebRtcTransport::WebRtcTransport(const EventPoller::Ptr &poller) {
    _poller = poller;
    _dtls_transport = std::make_shared<RTC::DtlsTransport>(poller, this);
    _ice_server = std::make_shared<RTC::IceServer>(this, makeRandStr(4), makeRandStr(28).substr(4));
}

void WebRtcTransport::onCreate(){

}

void WebRtcTransport::onDestory(){
    _dtls_transport = nullptr;
    _ice_server = nullptr;
}

const EventPoller::Ptr& WebRtcTransport::getPoller() const{
    return _poller;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void WebRtcTransport::OnIceServerSendStunPacket(const RTC::IceServer *iceServer, const RTC::StunPacket *packet, RTC::TransportTuple *tuple) {
    onSendSockData((char *) packet->GetData(), packet->GetSize(), (struct sockaddr_in *) tuple);
}

void WebRtcTransport::OnIceServerSelectedTuple(const RTC::IceServer *iceServer, RTC::TransportTuple *tuple) {
    InfoL;
}

void WebRtcTransport::OnIceServerConnected(const RTC::IceServer *iceServer) {
    InfoL;
}

void WebRtcTransport::OnIceServerCompleted(const RTC::IceServer *iceServer) {
    InfoL;
    if (_answer_sdp->media[0].role == DtlsRole::passive) {
        _dtls_transport->Run(RTC::DtlsTransport::Role::SERVER);
    } else {
        _dtls_transport->Run(RTC::DtlsTransport::Role::CLIENT);
    }
}

void WebRtcTransport::OnIceServerDisconnected(const RTC::IceServer *iceServer) {
    InfoL;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void WebRtcTransport::OnDtlsTransportConnected(
        const RTC::DtlsTransport *dtlsTransport,
        RTC::SrtpSession::CryptoSuite srtpCryptoSuite,
        uint8_t *srtpLocalKey,
        size_t srtpLocalKeyLen,
        uint8_t *srtpRemoteKey,
        size_t srtpRemoteKeyLen,
        std::string &remoteCert) {
    InfoL;
    _srtp_session_send = std::make_shared<RTC::SrtpSession>(RTC::SrtpSession::Type::OUTBOUND, srtpCryptoSuite, srtpLocalKey, srtpLocalKeyLen);
    _srtp_session_recv = std::make_shared<RTC::SrtpSession>(RTC::SrtpSession::Type::INBOUND, srtpCryptoSuite, srtpRemoteKey, srtpRemoteKeyLen);
    onStartWebRTC();
}

void WebRtcTransport::OnDtlsTransportSendData(const RTC::DtlsTransport *dtlsTransport, const uint8_t *data, size_t len) {
    onSendSockData((char *)data, len);
}

void WebRtcTransport::OnDtlsTransportConnecting(const RTC::DtlsTransport *dtlsTransport) {
    InfoL;
}

void WebRtcTransport::OnDtlsTransportFailed(const RTC::DtlsTransport *dtlsTransport) {
    InfoL;
    onShutdown(SockException(Err_shutdown, "dtls transport failed"));
}

void WebRtcTransport::OnDtlsTransportClosed(const RTC::DtlsTransport *dtlsTransport) {
    InfoL;
    onShutdown(SockException(Err_shutdown, "dtls close notify received"));
}

void WebRtcTransport::OnDtlsTransportApplicationDataReceived(const RTC::DtlsTransport *dtlsTransport, const uint8_t *data, size_t len) {
    InfoL << hexdump(data, len);
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void WebRtcTransport::onSendSockData(const char *buf, size_t len, bool flush){
    auto tuple = _ice_server->GetSelectedTuple();
    assert(tuple);
    onSendSockData(buf, len, (struct sockaddr_in *) tuple, flush);
}

const RtcSession& WebRtcTransport::getSdp(SdpType type) const{
    switch (type) {
        case SdpType::offer: return *_offer_sdp;
        case SdpType::answer: return *_answer_sdp;
        default: throw std::invalid_argument("不识别的sdp类型");
    }
}

RTC::TransportTuple* WebRtcTransport::getSelectedTuple() const{
    return  _ice_server->GetSelectedTuple();
}

void WebRtcTransport::sendRtcpRemb(uint32_t ssrc, size_t bit_rate) {
    auto remb = FCI_REMB::create({ssrc}, (uint32_t)bit_rate);
    auto fb = RtcpFB::create(PSFBType::RTCP_PSFB_REMB, remb.data(), remb.size());
    fb->ssrc = htonl(0);
    fb->ssrc_media = htonl(ssrc);
    sendRtcpPacket((char *) fb.get(), fb->getSize(), true);
    TraceL << ssrc << " " << bit_rate;
}

void WebRtcTransport::sendRtcpPli(uint32_t ssrc) {
    auto pli = RtcpFB::create(PSFBType::RTCP_PSFB_PLI);
    pli->ssrc = htonl(0);
    pli->ssrc_media = htonl(ssrc);
    sendRtcpPacket((char *) pli.get(), pli->getSize(), true);
}

string getFingerprint(const string &algorithm_str, const std::shared_ptr<RTC::DtlsTransport> &transport){
    auto algorithm = RTC::DtlsTransport::GetFingerprintAlgorithm(algorithm_str);
    for (auto &finger_prints : transport->GetLocalFingerprints()) {
        if (finger_prints.algorithm == algorithm) {
            return finger_prints.value;
        }
    }
    throw std::invalid_argument(StrPrinter << "不支持的加密算法:" << algorithm_str);
}

void WebRtcTransport::setRemoteDtlsFingerprint(const RtcSession &remote){
    //设置远端dtls签名
    RTC::DtlsTransport::Fingerprint remote_fingerprint;
    remote_fingerprint.algorithm = RTC::DtlsTransport::GetFingerprintAlgorithm(_offer_sdp->media[0].fingerprint.algorithm);
    remote_fingerprint.value = _offer_sdp->media[0].fingerprint.hash;
    _dtls_transport->SetRemoteFingerprint(remote_fingerprint);
}

void WebRtcTransport::onCheckSdp(SdpType type, RtcSession &sdp){
    for (auto &m : sdp.media) {
        if (m.type != TrackApplication && !m.rtcp_mux) {
            throw std::invalid_argument("只支持rtcp-mux模式");
        }
    }
    if (sdp.group.mids.empty()) {
        throw std::invalid_argument("只支持group BUNDLE模式");
    }
    if (type == SdpType::offer) {
        sdp.checkValidSSRC();
    }
}

void WebRtcTransport::onRtcConfigure(RtcConfigure &configure) const {
    //开启remb后关闭twcc，因为开启twcc后remb无效
    GET_CONFIG(size_t, remb_bit_rate, RTC::kRembBitRate);
    configure.enableTWCC(!remb_bit_rate);
}

std::string WebRtcTransport::getAnswerSdp(const string &offer){
    try {
        //// 解析offer sdp ////
        _offer_sdp = std::make_shared<RtcSession>();
        _offer_sdp->loadFrom(offer);
        onCheckSdp(SdpType::offer, *_offer_sdp);
        setRemoteDtlsFingerprint(*_offer_sdp);

        //// sdp 配置 ////
        SdpAttrFingerprint fingerprint;
        fingerprint.algorithm = _offer_sdp->media[0].fingerprint.algorithm;
        fingerprint.hash = getFingerprint(fingerprint.algorithm, _dtls_transport);
        RtcConfigure configure;
        configure.setDefaultSetting(_ice_server->GetUsernameFragment(), _ice_server->GetPassword(),
                                    RtpDirection::sendrecv, fingerprint);
        onRtcConfigure(configure);

        //// 生成answer sdp ////
        _answer_sdp = configure.createAnswer(*_offer_sdp);
        onCheckSdp(SdpType::answer, *_answer_sdp);
        return _answer_sdp->toString();
    } catch (exception &ex) {
        onShutdown(SockException(Err_shutdown, ex.what()));
        throw;
    }
}

bool is_dtls(char *buf) {
    return ((*buf > 19) && (*buf < 64));
}

bool is_rtp(char *buf) {
    RtpHeader *header = (RtpHeader *) buf;
    return ((header->pt < 64) || (header->pt >= 96));
}

bool is_rtcp(char *buf) {
    RtpHeader *header = (RtpHeader *) buf;
    return ((header->pt >= 64) && (header->pt < 96));
}

void WebRtcTransport::inputSockData(char *buf, size_t len, RTC::TransportTuple *tuple) {
    if (RTC::StunPacket::IsStun((const uint8_t *) buf, len)) {
        RTC::StunPacket *packet = RTC::StunPacket::Parse((const uint8_t *) buf, len);
        if (packet == nullptr) {
            WarnL << "parse stun error" << std::endl;
            return;
        }
        _ice_server->ProcessStunPacket(packet, tuple);
        return;
    }
    if (is_dtls(buf)) {
        _dtls_transport->ProcessDtlsData((uint8_t *) buf, len);
        return;
    }
    if (is_rtp(buf)) {
        if (_srtp_session_recv->DecryptSrtp((uint8_t *) buf, &len)) {
            onRtp(buf, len);
        } else {
            RtpHeader *rtp = (RtpHeader *) buf;
            WarnL << "srtp_unprotect rtp failed, pt:" << (int)rtp->pt;
        }
        return;
    }
    if (is_rtcp(buf)) {
        if (_srtp_session_recv->DecryptSrtcp((uint8_t *) buf, &len)) {
            onRtcp(buf, len);
        } else {
            WarnL;
        }
        return;
    }
}

void WebRtcTransport::sendRtpPacket(const char *buf, size_t len, bool flush, void *ctx) {
    if (_srtp_session_send) {
        //预留rtx加入的两个字节
        CHECK(len + SRTP_MAX_TRAILER_LEN + 2 <= sizeof(_srtp_buf));
        memcpy(_srtp_buf, buf, len);
        onBeforeEncryptRtp((char *) _srtp_buf, len, ctx);
        if (_srtp_session_send->EncryptRtp(_srtp_buf, &len)) {
            onSendSockData((char *) _srtp_buf, len, flush);
        }
    }
}

void WebRtcTransport::sendRtcpPacket(const char *buf, size_t len, bool flush, void *ctx){
    if (_srtp_session_send) {
        CHECK(len + SRTP_MAX_TRAILER_LEN <= sizeof(_srtp_buf));
        memcpy(_srtp_buf, buf, len);
        onBeforeEncryptRtcp((char *) _srtp_buf, len, ctx);
        if (_srtp_session_send->EncryptRtcp(_srtp_buf, &len)) {
            onSendSockData((char *) _srtp_buf, len, flush);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////
WebRtcTransportImp::Ptr WebRtcTransportImp::create(const EventPoller::Ptr &poller){
    WebRtcTransportImp::Ptr ret(new WebRtcTransportImp(poller), [](WebRtcTransportImp *ptr){
        ptr->onDestory();
       delete ptr;
    });
    ret->onCreate();
    return ret;
}

void WebRtcTransportImp::onCreate(){
    WebRtcTransport::onCreate();
    _socket = Socket::createSocket(getPoller(), false);
    //随机端口，绑定全部网卡
    _socket->bindUdpSock(0);
    weak_ptr<WebRtcTransportImp> weak_self = shared_from_this();
    _socket->setOnRead([weak_self](const Buffer::Ptr &buf, struct sockaddr *addr, int addr_len) mutable {
        auto strong_self = weak_self.lock();
        if (strong_self) {
            strong_self->inputSockData(buf->data(), buf->size(), addr);
        }
    });
    _self = shared_from_this();

    GET_CONFIG(float, timeoutSec, RTC::kTimeOutSec);
    _timer = std::make_shared<Timer>(timeoutSec / 2, [weak_self]() {
        auto strong_self = weak_self.lock();
        if (!strong_self) {
            return false;
        }
        if (strong_self->_alive_ticker.elapsedTime() > timeoutSec * 1000) {
            strong_self->onShutdown(SockException(Err_timeout, "接受rtp和rtcp超时"));
        }
        return true;
    }, getPoller());
}

WebRtcTransportImp::WebRtcTransportImp(const EventPoller::Ptr &poller) : WebRtcTransport(poller) {
    InfoL << this;
}

WebRtcTransportImp::~WebRtcTransportImp() {
    InfoL << this;
}

void WebRtcTransportImp::onDestory() {
    WebRtcTransport::onDestory();
    uint64_t duration = _alive_ticker.createdTime() / 1000;

    //流量统计事件广播
    GET_CONFIG(uint32_t, iFlowThreshold, General::kFlowThreshold);

    if (_reader) {
        WarnL << "RTC播放器("
              << _media_info._vhost << "/"
              << _media_info._app << "/"
              << _media_info._streamid
              << ")结束播放,耗时(s):" << duration;
        if (_bytes_usage >= iFlowThreshold * 1024) {
            NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastFlowReport, _media_info, _bytes_usage, duration, true, static_cast<SockInfo &>(*_socket));
        }
    }

    if (_push_src) {
        WarnL << "RTC推流器("
              << _media_info._vhost << "/"
              << _media_info._app << "/"
              << _media_info._streamid
              << ")结束推流,耗时(s):" << duration;
        if (_bytes_usage >= iFlowThreshold * 1024) {
            NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastFlowReport, _media_info, _bytes_usage, duration, false, static_cast<SockInfo &>(*_socket));
        }
    }
}

void WebRtcTransportImp::attach(const RtspMediaSource::Ptr &src, const MediaInfo &info, bool is_play) {
    assert(src);
    _media_info = info;
    if (is_play) {
        _play_src = src;
    } else {
        _push_src = src;
    }
}

void WebRtcTransportImp::onSendSockData(const char *buf, size_t len, struct sockaddr_in *dst, bool flush) {
    auto ptr = BufferRaw::create();
    ptr->assign(buf, len);
    _socket->send(ptr, (struct sockaddr *)(dst), sizeof(struct sockaddr), flush);
}

///////////////////////////////////////////////////////////////////

bool WebRtcTransportImp::canSendRtp() const{
    auto &sdp = getSdp(SdpType::answer);
    return _play_src && (sdp.media[0].direction == RtpDirection::sendrecv || sdp.media[0].direction == RtpDirection::sendonly);
}

bool WebRtcTransportImp::canRecvRtp() const{
    auto &sdp = getSdp(SdpType::answer);
    return _push_src && (sdp.media[0].direction == RtpDirection::sendrecv || sdp.media[0].direction == RtpDirection::recvonly);
}

void WebRtcTransportImp::onStartWebRTC() {
    //获取ssrc和pt相关信息,届时收到rtp和rtcp时分别可以根据pt和ssrc找到相关的信息
    for (auto &m_answer : getSdp(SdpType::answer).media) {
        auto m_offer = getSdp(SdpType::offer).getMedia(m_answer.type);
        auto info = std::make_shared<RtpPayloadInfo>();

        info->media = &m_answer;
        info->answer_ssrc_rtp = m_answer.getRtpSSRC();
        info->answer_ssrc_rtx = m_answer.getRtxSSRC();
        info->offer_ssrc_rtp = m_offer->getRtpSSRC();
        info->offer_ssrc_rtx = m_offer->getRtxSSRC();
        info->plan_rtp = &m_answer.plan[0];;
        info->plan_rtx = m_answer.getRelatedRtxPlan(info->plan_rtp->pt);
        info->rtcp_context_send = std::make_shared<RtcpContext>(info->plan_rtp->sample_rate, false);

        //send ssrc --> RtpPayloadInfo
        _rtp_info_ssrc[info->answer_ssrc_rtp] = info;

        //recv ssrc --> RtpPayloadInfo
        _rtp_info_ssrc[info->offer_ssrc_rtp] = info;

        //rtp pt --> RtpPayloadInfo
        _rtp_info_pt.emplace(info->plan_rtp->pt, std::make_pair(false, info));
        if (info->plan_rtx) {
            //rtx pt --> RtpPayloadInfo
            _rtp_info_pt.emplace(info->plan_rtx->pt, std::make_pair(true, info));
        }
        if (m_offer->type != TrackApplication) {
            //记录rtp ext类型与id的关系，方便接收或发送rtp时修改rtp ext id
            for (auto &ext : m_offer->extmap) {
                auto ext_type = RtpExt::getExtType(ext.ext);
                _rtp_ext_id_to_type.emplace(ext.id, ext_type);
                _rtp_ext_type_to_id.emplace(ext_type, ext.id);
            }
        }
    }

    if (canRecvRtp()) {
        _push_src->setSdp(getSdp(SdpType::answer).toRtspSdp());
    }
    if (canSendRtp()) {
        _reader = _play_src->getRing()->attach(getPoller(), true);
        weak_ptr<WebRtcTransportImp> weak_self = shared_from_this();
        _reader->setReadCB([weak_self](const RtspMediaSource::RingDataType &pkt) {
            auto strongSelf = weak_self.lock();
            if (!strongSelf) {
                return;
            }
            size_t i = 0;
            pkt->for_each([&](const RtpPacket::Ptr &rtp) {
                strongSelf->onSendRtp(rtp, ++i == pkt->size());
            });
        });
        _reader->setDetachCB([weak_self](){
            auto strongSelf = weak_self.lock();
            if (!strongSelf) {
                return;
            }
            strongSelf->onShutdown(SockException(Err_eof, "rtsp ring buffer detached"));
        });

        RtcSession rtsp_send_sdp;
        rtsp_send_sdp.loadFrom(_play_src->getSdp(), false);
        for (auto &m : getSdp(SdpType::answer).media) {
            if (m.type == TrackApplication) {
                continue;
            }
            auto rtsp_media = rtsp_send_sdp.getMedia(m.type);
            if (rtsp_media && getCodecId(rtsp_media->plan[0].codec) == getCodecId(m.plan[0].codec)) {
                auto it = _rtp_info_pt.find(m.plan[0].pt);
                CHECK(it != _rtp_info_pt.end());
                //记录发送rtp时约定的信息，届时发送rtp时需要修改pt和ssrc
                _send_rtp_info[m.type] = it->second.second;
            }
        }
    }
    //使用完毕后，释放强引用，这样确保推流器断开后能及时注销媒体
    _play_src = nullptr;
}

void WebRtcTransportImp::onCheckSdp(SdpType type, RtcSession &sdp){
    WebRtcTransport::onCheckSdp(type, sdp);
    if (type != SdpType::answer) {
        //我们只修改answer sdp
        return;
    }

    //修改answer sdp的ip、端口信息
    GET_CONFIG(string, extern_ip, RTC::kExternIP);
    for (auto &m : sdp.media) {
        m.addr.reset();
        m.addr.address = extern_ip.empty() ? SockUtil::get_local_ip() : extern_ip;
        m.rtcp_addr.reset();
        m.rtcp_addr.address = m.addr.address;
        m.rtcp_addr.port = _socket->get_local_port();
        m.port = m.rtcp_addr.port;
        sdp.origin.address = m.addr.address;
    }

    if (!canSendRtp()) {
        //设置我们发送的rtp的ssrc
        return;
    }

    for (auto &m : sdp.media) {
        if (m.type == TrackApplication) {
            continue;
        }
        //添加answer sdp的ssrc信息
        m.rtp_rtx_ssrc.emplace_back();
        m.rtp_rtx_ssrc[0].ssrc = _play_src->getSsrc(m.type);
        m.rtp_rtx_ssrc[0].cname = RTP_CNAME;
        m.rtp_rtx_ssrc[0].label = RTP_LABEL;
        m.rtp_rtx_ssrc[0].mslabel = RTP_MSLABEL;
        m.rtp_rtx_ssrc[0].msid = RTP_MSID;

        if (m.getRelatedRtxPlan(m.plan[0].pt)) {
            m.rtp_rtx_ssrc.emplace_back();
            m.rtp_rtx_ssrc[1] = m.rtp_rtx_ssrc[0];
            m.rtp_rtx_ssrc[1].ssrc += RTX_SSRC_OFFSET;
        }
    }
}

void WebRtcTransportImp::onRtcConfigure(RtcConfigure &configure) const {
    WebRtcTransport::onRtcConfigure(configure);

    if (_play_src) {
        //这是播放,同时也可能有推流
        configure.video.direction = _push_src ? RtpDirection::sendrecv : RtpDirection::sendonly;
        configure.audio.direction = configure.video.direction;
        configure.setPlayRtspInfo(_play_src->getSdp());
    } else if (_push_src) {
        //这只是推流
        configure.video.direction = RtpDirection::recvonly;
        configure.audio.direction = RtpDirection::recvonly;
    } else {
        throw std::invalid_argument("未设置播放或推流的媒体源");
    }

    //添加接收端口candidate信息
    configure.addCandidate(*getIceCandidate());
}

SdpAttrCandidate::Ptr WebRtcTransportImp::getIceCandidate() const{
    auto candidate = std::make_shared<SdpAttrCandidate>();
    candidate->foundation = "udpcandidate";
    //rtp端口
    candidate->component = 1;
    candidate->transport = "udp";
    //优先级，单candidate时随便
    candidate->priority = 100;
    GET_CONFIG(string, extern_ip, RTC::kExternIP);
    candidate->address = extern_ip.empty() ? SockUtil::get_local_ip() : extern_ip;
    candidate->port = _socket->get_local_port();
    candidate->type = "host";
    return candidate;
}

///////////////////////////////////////////////////////////////////

struct PacketContainer {
  explicit PacketContainer(TrackType t): type(t) {}
  const TrackType type;
  //rtp排序缓存，根据seq排序
  PacketSortor<RtpPacket::Ptr> rtp_sortor;
  void sortPacket(int sample_rate,
                  const uint8_t *ptr,
                  size_t len);
};

using OnRtpSortedCallback = std::function<void(RtpPacket::Ptr rtp, int track_index)>;

struct TrackRtpHandlerInterface {
  virtual bool handleRtp(int sample_rate, const uint8_t *ptr, size_t len, const RtpHeader *header) = 0;
  virtual void setOnRtpSorted(OnRtpSortedCallback callback) = 0;
};

struct TrackRtpHandler : TrackRtpHandlerInterface {
  explicit TrackRtpHandler(TrackType t): track{t} {}
  PacketContainer track;

  bool handleRtp(int sample_rate, const uint8_t *ptr, size_t len, const RtpHeader *header) override {
    //比对缓存ssrc
    auto ssrc = ntohl(header->ssrc);
    track.sortPacket(sample_rate, ptr, len);
    return true;
  }
  void setOnRtpSorted(OnRtpSortedCallback callback) override {
    track.rtp_sortor.setOnSort([this, callback](uint16_t seq, RtpPacket::Ptr &packet) {
      callback(move(packet), static_cast<int>(track.type));
    });
  }
};

class RtpReceiverImp : public RtpReceiver {
  const RtcMedia& media;
public:
  RtpReceiverImp(const RtcMedia *m, function<void(RtpPacket::Ptr rtp)> cb)
      : media(*m), track_rtp_handler_(new TrackRtpHandler(m->type)) {
    _on_sort = std::move(cb);
    track_rtp_handler_->setOnRtpSorted(
        [&](RtpPacket::Ptr rtp, int track_index) {
          _on_sort(std::move(rtp));
        });
  }

  ~RtpReceiverImp() override = default;

  bool inputRtp(int samplerate, uint8_t *ptr, size_t len) {
    return handleRtp(samplerate, ptr, len);
  }

protected:
  /**
   * 输入数据指针生成并排序rtp包
   * @param index track下标索引
   * @param type track类型
   * @param sample_rate rtp时间戳基准时钟，视频为90000，音频为采样率
   * @param ptr rtp数据指针
   * @param len rtp数据指针长度
   * @return 解析成功返回true
   */
  bool handleRtp(int sample_rate, uint8_t *ptr, size_t len);

  void onRtpSorted(RtpPacket::Ptr rtp, int track_index) override {
    _on_sort(std::move(rtp));
  }

private:
  std::unique_ptr<TrackRtpHandlerInterface> track_rtp_handler_;
  function<void(RtpPacket::Ptr rtp)> _on_sort;
};

constexpr size_t RTP_MAX_SIZE = 10*1024;

bool RtpReceiverImp::handleRtp(int sample_rate, uint8_t *ptr, size_t len) {
  if (len < RtpPacket::kRtpHeaderSize) {
    WarnL << "rtp包太小:" << len;
    return false;
  }
  if (len > RTP_MAX_SIZE) {
    WarnL << "超大的rtp包:" << len << " > " << RTP_MAX_SIZE;
    return false;
  }
  if (!sample_rate) {
    //无法把时间戳转换成毫秒
    return false;
  }
  RtpHeader *header = (RtpHeader *) ptr;
  if (header->version != RtpPacket::kRtpVersion) {
    throw BadRtpException("非法的rtp，version字段非法");
  }
  if (!header->getPayloadSize(len)) {
    //无有效负载的rtp包
    return false;
  }

  return track_rtp_handler_->handleRtp(sample_rate, ptr, len, header);
}

void PacketContainer::sortPacket(
    int sample_rate,
    const uint8_t *ptr,
    size_t len) {
  auto rtp = RtpPacket::create();
  //需要添加4个字节的rtp over tcp头
  rtp->setCapacity(RtpPacket::kRtpTcpHeaderSize + len);
  rtp->setSize(RtpPacket::kRtpTcpHeaderSize + len);
  rtp->sample_rate = sample_rate;
  rtp->type = type;

  //赋值4个字节的rtp over tcp头
  uint8_t *data = (uint8_t *) rtp->data();
  data[0] = '$';
  data[1] = 2 * static_cast<int>(type);
  data[2] = (len >> 8) & 0xFF;
  data[3] = len & 0xFF;
  //拷贝rtp
  memcpy(&data[4], ptr, len);

  auto seq = rtp->getSeq();
  rtp_sortor.sortPacket(seq, move(rtp));
}


void WebRtcTransportImp::onRtcp(const char *buf, size_t len) {
    _bytes_usage += len;
    auto rtcps = RtcpHeader::loadFromBytes((char *) buf, len);
    for (auto rtcp : rtcps) {
        switch ((RtcpType) rtcp->pt) {
            case RtcpType::RTCP_SR : {
                //对方汇报rtp发送情况
                RtcpSR *sr = (RtcpSR *) rtcp;
                auto it = _rtp_info_ssrc.find(sr->ssrc);
                if (it != _rtp_info_ssrc.end()) {
                    auto &info = it->second;
                    auto it = info->rtcp_context_recv.find(sr->ssrc);
                    if (it != info->rtcp_context_recv.end()) {
                        it->second->onRtcp(sr);
                        auto rr = it->second->createRtcpRR(info->answer_ssrc_rtp, sr->ssrc);
                        sendRtcpPacket(rr->data(), rr->size(), true);
                    } else {
                        WarnL << "未识别的sr rtcp包:" << rtcp->dumpString();
                    }
                } else {
                    WarnL << "未识别的sr rtcp包:" << rtcp->dumpString();
                }
                break;
            }
            case RtcpType::RTCP_RR : {
                _alive_ticker.resetTime();
                //对方汇报rtp接收情况
                RtcpRR *rr = (RtcpRR *) rtcp;
                for (auto item : rr->getItemList()) {
                    auto it = _rtp_info_ssrc.find(item->ssrc);
                    if (it != _rtp_info_ssrc.end()) {
                        auto &info = it->second;
                        auto sr = info->rtcp_context_send->createRtcpSR(info->answer_ssrc_rtp);
                        sendRtcpPacket(sr->data(), sr->size(), true);
                    } else {
                        WarnL << "未识别的rr rtcp包:" << rtcp->dumpString();
                    }
                }
                break;
            }
            case RtcpType::RTCP_BYE : {
                //对方汇报停止发送rtp
                RtcpBye *bye = (RtcpBye *) rtcp;
                for (auto ssrc : bye->getSSRC()) {
                    auto it = _rtp_info_ssrc.find(*ssrc);
                    if (it == _rtp_info_ssrc.end()) {
                        WarnL << "未识别的bye rtcp包:" << rtcp->dumpString();
                        continue;
                    }
                    _rtp_info_ssrc.erase(it);
                }
                onShutdown(SockException(Err_eof, "rtcp bye message received"));
                break;
            }
            case RtcpType::RTCP_PSFB:
            case RtcpType::RTCP_RTPFB: {
                if ((RtcpType) rtcp->pt == RtcpType::RTCP_PSFB) {
                    break;
                }
                //RTPFB
                switch ((RTPFBType) rtcp->report_count) {
                    case RTPFBType::RTCP_RTPFB_NACK : {
                        RtcpFB *fb = (RtcpFB *) rtcp;
                        auto it = _rtp_info_ssrc.find(fb->ssrc_media);
                        if (it == _rtp_info_ssrc.end()) {
                            WarnL << "未识别的 rtcp包:" << rtcp->dumpString();
                            return;
                        }
                        auto &info = it->second;
                        auto &fci = fb->getFci<FCI_NACK>();
                        info->nack_list.for_each_nack(fci, [&](const RtpPacket::Ptr &rtp) {
                            //rtp重传
                            onSendRtp(rtp, true, true);
                        });
                        break;
                    }
                    default: break;
                }
                break;
            }
            default: break;
        }
    }
}

///////////////////////////////////////////////////////////////////

void WebRtcTransportImp::changeRtpExtId(RtpPayloadInfo &info, const RtpHeader *header, bool is_recv, string *rid_ptr) const{
    string rid, repaired_rid;
    auto ext_map = RtpExt::getExtValue(header);
    for (auto &pr : ext_map) {
        if (is_recv) {
            auto it = _rtp_ext_id_to_type.find(pr.first);
            if (it == _rtp_ext_id_to_type.end()) {
                WarnL << "接收rtp时,忽略不识别的rtp ext, id=" << (int) pr.first;
                pr.second.clearExt();
                continue;
            }
            pr.second.setType(it->second);
            //重新赋值ext id为 ext type，作为后面处理ext的统一中间类型
            pr.second.setExtId((uint8_t) it->second);
            switch (it->second) {
                case RtpExtType::sdes_rtp_stream_id : rid = pr.second.getRtpStreamId(); break;
                case RtpExtType::sdes_repaired_rtp_stream_id : repaired_rid = pr.second.getRepairedRtpStreamId(); break;
                default : break;
            }
        } else {
            pr.second.setType((RtpExtType) pr.first);
            auto it = _rtp_ext_type_to_id.find((RtpExtType) pr.first);
            if (it == _rtp_ext_type_to_id.end()) {
                WarnL << "发送rtp时, 忽略不被客户端支持rtp ext:" << pr.second.dumpString();
                pr.second.clearExt();
                continue;
            }
            //重新赋值ext id为客户端sdp声明的类型
            pr.second.setExtId(it->second);
        }
    }

    if (!is_recv) {
        return;
    }
    if (rid.empty()) {
        rid = repaired_rid;
    }
    auto ssrc = ntohl(header->ssrc);
    if (rid.empty()) {
        //获取rid
        rid = info.ssrc_to_rid[ssrc];
    } else {
        //设置rid
        info.ssrc_to_rid[ssrc] = rid;
    }
    if (rid_ptr) {
        *rid_ptr = rid;
    }
}

std::shared_ptr<RtpReceiverImp> WebRtcTransportImp::createRtpReceiver(const string &rid, uint32_t ssrc, bool is_rtx, const RtpPayloadInfo::Ptr &info){
    auto ref = std::make_shared<RtpReceiverImp>(info->media,[info, this, rid](RtpPacket::Ptr rtp) mutable {
        onSortedRtp(*info, rid, std::move(rtp));
    });

    if (!is_rtx) {
        //rtx没nack
        info->nack_ctx[rid].setOnNack([info, this, ssrc](const FCI_NACK &nack) mutable {
            onSendNack(*info, nack, ssrc);
        });
        //ssrc --> RtpPayloadInfo
        _rtp_info_ssrc[ssrc] = info;
    }

    InfoL << "receive rtp of ssrc:" << ssrc << ", rid:" << rid << ", is rtx:" << is_rtx << ", codec:" << info->plan_rtp->codec;
    return ref;
}

void WebRtcTransportImp::onRtp(const char *buf, size_t len) {
    _bytes_usage += len;
    _alive_ticker.resetTime();

    RtpHeader *rtp = (RtpHeader *) buf;
    //根据接收到的rtp的pt信息，找到该流的信息
    auto it = _rtp_info_pt.find(rtp->pt);
    if (it == _rtp_info_pt.end()) {
        WarnL << "unknown rtp pt:" << (int)rtp->pt;
        return;
    }
    bool is_rtx = it->second.first;
    auto ssrc = ntohl(rtp->ssrc);
    auto &info = it->second.second;

    //修改ext id至统一
    string rid;
    changeRtpExtId(*info, rtp, true, &rid);

#if 0
    if (rid.empty() && info->media->type == TrackVideo) {
        WarnL << "ssrc:" << ssrc << ", rtx:" << is_rtx << ",seq:" << ntohs((uint16_t) rtp->seq);
    }
#endif
    auto &ref = info->receiver[rid];
    if (!ref) {
        ref = createRtpReceiver(rid, ssrc, is_rtx, info);
    }

    if (!is_rtx) {
        //这是普通的rtp数据
        auto seq = ntohs(rtp->seq);
#if 0
        if (info->media->type == TrackVideo && seq % 100 == 0) {
            //此处模拟接受丢包
            DebugL << "recv dropped:" << seq;
            return;
        }
#endif
        //统计rtp接受情况，便于生成nack rtcp包
        info->nack_ctx[rid].received(seq);

        auto &cxt_ref = info->rtcp_context_recv[ssrc];
        if (!cxt_ref) {
            cxt_ref = std::make_shared<RtcpContext>(info->plan_rtp->sample_rate, true);
            info->rid_to_ssrc[rid] = ssrc;
        }
        //时间戳转换成毫秒
        auto stamp_ms = ntohl(rtp->stamp) * uint64_t(1000) / info->plan_rtp->sample_rate;
        //统计rtp收到的情况，好做rr汇报
        cxt_ref->onRtp(seq, stamp_ms, len);

        //解析并排序rtp
        ref->inputRtp(info->plan_rtp->sample_rate, (uint8_t *) buf, len);
        return;
    }

    //这里是rtx重传包
    //https://datatracker.ietf.org/doc/html/rfc4588#section-4
    auto payload = rtp->getPayloadData();
    auto size = rtp->getPayloadSize(len);
    if (size < 2) {
        return;
    }

    //前两个字节是原始的rtp的seq
    auto origin_seq = payload[0] << 8 | payload[1];
    //rtx的seq转换为rtp的seq
    rtp->seq = htons(origin_seq);
    //rtx的ssrc转换为rtp的ssrc
    rtp->ssrc = htonl(info->rid_to_ssrc[rid]);
    //rtx的pt转换为rtp的pt
    rtp->pt = info->plan_rtp->pt;
    memmove((uint8_t *) buf + 2, buf, payload - (uint8_t *) buf);
    buf += 2;
    len -= 2;
    ref->inputRtp(info->plan_rtp->sample_rate, (uint8_t *) buf, len);
}

void WebRtcTransportImp::onSendNack(RtpPayloadInfo &info, const FCI_NACK &nack, uint32_t ssrc) {
    auto rtcp = RtcpFB::create(RTPFBType::RTCP_RTPFB_NACK, &nack, FCI_NACK::kSize);
    rtcp->ssrc = htons(info.answer_ssrc_rtp);
    rtcp->ssrc_media = htonl(ssrc);
    DebugL << htonl(ssrc) << " " << nack.getPid();
    sendRtcpPacket((char *) rtcp.get(), rtcp->getSize(), true);
}

///////////////////////////////////////////////////////////////////

void WebRtcTransportImp::onSortedRtp(RtpPayloadInfo &info, const string &rid, RtpPacket::Ptr rtp) {
    if (info.media->type == TrackVideo && _pli_ticker.elapsedTime() > 2000) {
        //定期发送pli请求关键帧，方便非rtc等协议
        _pli_ticker.resetTime();
        sendRtcpPli(rtp->getSSRC());

        //开启remb，则发送remb包调节比特率
        GET_CONFIG(size_t, remb_bit_rate, RTC::kRembBitRate);
        if (remb_bit_rate && getSdp(SdpType::answer).supportRtcpFb(SdpConst::kRembRtcpFb)) {
            sendRtcpRemb(rtp->getSSRC(), remb_bit_rate);
        }
    }

    if (_push_src) {
        if (rtp->type == TrackAudio) {
            //音频
            for (auto &pr : _push_src_simulcast) {
                pr.second->onWrite(rtp, false);
            }
        } else {
            //视频
            auto &src = _push_src_simulcast[rid];
            if (!src) {
                auto stream_id = rid.empty() ? _push_src->getId() : _push_src->getId() + "_" + rid;
                auto src_imp = std::make_shared<RtspMediaSourceImp>(_push_src->getVhost(), _push_src->getApp(), stream_id);
                src_imp->setSdp(_push_src->getSdp());
                src_imp->setProtocolTranslation(_push_src->isRecording(Recorder::type_hls),_push_src->isRecording(Recorder::type_mp4));
                src_imp->setListener(shared_from_this());
                src = src_imp;
            }
            src->onWrite(std::move(rtp), false);
        }
    }
}

///////////////////////////////////////////////////////////////////

void WebRtcTransportImp::onSendRtp(const RtpPacket::Ptr &rtp, bool flush, bool rtx){
    auto &info = _send_rtp_info[rtp->type];
    if (!info) {
        //忽略，对方不支持该编码类型
        return;
    }
    if (!rtx) {
        //统计rtp发送情况，好做sr汇报
        info->rtcp_context_send->onRtp(rtp->getSeq(), rtp->getStampMS(), rtp->size() - RtpPacket::kRtpTcpHeaderSize);
        info->nack_list.push_back(rtp);
#if 0
        //此处模拟发送丢包
        if (rtp->type == TrackVideo && rtp->getSeq() % 100 == 0) {
            DebugL << "send dropped:" << rtp->getSeq();
            return;
        }
#endif
    } else {
        WarnL << "send rtx rtp:" << rtp->getSeq();
    }
    pair<bool/*rtx*/, RtpPayloadInfo *> ctx{rtx, info.get()};
    sendRtpPacket(rtp->data() + RtpPacket::kRtpTcpHeaderSize, rtp->size() - RtpPacket::kRtpTcpHeaderSize, flush, &ctx);
    _bytes_usage += rtp->size() - RtpPacket::kRtpTcpHeaderSize;
}

void WebRtcTransportImp::onBeforeEncryptRtp(const char *buf, size_t &len, void *ctx) {
    auto pr = (pair<bool/*rtx*/, RtpPayloadInfo *> *) ctx;
    auto header = (RtpHeader *) buf;

    if (!pr->first || !pr->second->plan_rtx) {
        //普通的rtp,或者不支持rtx, 修改目标pt和ssrc
        changeRtpExtId(*pr->second, header, false);
        header->pt = pr->second->plan_rtp->pt;
        header->ssrc = htonl(pr->second->answer_ssrc_rtp);
    } else {
        //重传的rtp, rtx
        changeRtpExtId(*pr->second, header, false);
        header->pt = pr->second->plan_rtx->pt;
        if (pr->second->answer_ssrc_rtx) {
            //有rtx单独的ssrc,有些情况下，浏览器支持rtx，但是未指定rtx单独的ssrc
            header->ssrc = htonl(pr->second->answer_ssrc_rtx);
        } else {
            //未单独指定rtx的ssrc，那么使用rtp的ssrc
            header->ssrc = htonl(pr->second->answer_ssrc_rtp);
        }

        auto origin_seq = ntohs(header->seq);
        //seq跟原来的不一样
        header->seq = htons(_rtx_seq[pr->second->media->type]++);
        auto payload = header->getPayloadData();
        auto payload_size = header->getPayloadSize(len);
        if (payload_size) {
            //rtp负载后移两个字节，这两个字节用于存放osn
            //https://datatracker.ietf.org/doc/html/rfc4588#section-4
            memmove(payload + 2, payload, payload_size);
        }
        payload[0] = origin_seq >> 8;
        payload[1] = origin_seq & 0xFF;
        len += 2;
    }
}

void WebRtcTransportImp::onShutdown(const SockException &ex){
    WarnL << ex.what();
    _self = nullptr;
}

/////////////////////////////////////////////////////////////////////////////////////////////

bool WebRtcTransportImp::close(MediaSource &sender, bool force) {
    //此回调在其他线程触发
    if (!force && totalReaderCount(sender)) {
        return false;
    }
    string err = StrPrinter << "close media:" << sender.getSchema() << "/" << sender.getVhost() << "/" << sender.getApp() << "/" << sender.getId() << " " << force;
    onShutdown(SockException(Err_shutdown,err));
    return true;
}

int WebRtcTransportImp::totalReaderCount(MediaSource &sender) {
    auto total_count = 0;
    for (auto &src : _push_src_simulcast) {
        total_count += src.second->totalReaderCount();
    }
    return total_count;
}

MediaOriginType WebRtcTransportImp::getOriginType(MediaSource &sender) const {
    return MediaOriginType::rtc_push;
}

string WebRtcTransportImp::getOriginUrl(MediaSource &sender) const {
    return "";
}

std::shared_ptr<SockInfo> WebRtcTransportImp::getOriginSock(MediaSource &sender) const {
    return const_cast<WebRtcTransportImp *>(this)->shared_from_this();
}

/////////////////////////////////////////////////////////////////////////////////////////////

string WebRtcTransportImp::get_local_ip() {
    return getSdp(SdpType::answer).media[0].candidate[0].address;
}

uint16_t WebRtcTransportImp::get_local_port() {
    return _socket->get_local_port();
}

string WebRtcTransportImp::get_peer_ip() {
    return SockUtil::inet_ntoa(((struct sockaddr_in *) getSelectedTuple())->sin_addr);
}

uint16_t WebRtcTransportImp::get_peer_port() {
    return ntohs(((struct sockaddr_in *) getSelectedTuple())->sin_port);
}

string WebRtcTransportImp::getIdentifier() const {
    return StrPrinter << this;
}