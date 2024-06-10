/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "WebRtcPlayer.h"

#include "Common/config.h"
#include "Extension/Factory.h"
#include "Util/base64.h"

using namespace std;

namespace mediakit {

WebRtcPlayer::Ptr WebRtcPlayer::create(const EventPoller::Ptr &poller,
                                       const RtspMediaSource::Ptr &src,
                                       const MediaInfo &info) {
    WebRtcPlayer::Ptr ret(new WebRtcPlayer(poller, src, info), [](WebRtcPlayer *ptr) {
        ptr->onDestory();
        delete ptr;
    });
    ret->onCreate();
    return ret;
}

WebRtcPlayer::WebRtcPlayer(const EventPoller::Ptr &poller,
                           const RtspMediaSource::Ptr &src,
                           const MediaInfo &info) : WebRtcTransportImp(poller) {
    _media_info = info;
    _play_src = src;
    CHECK(src);

    GET_CONFIG(bool, direct_proxy, Rtsp::kDirectProxy);
    if (direct_proxy) {
        do {
            auto video_track = src->getTrack(mediakit::TrackVideo);
            if (!video_track) {
                break;
            }
            if (video_track->getCodecId() != mediakit::CodecH264 && video_track->getCodecId() != mediakit::CodecH265) {
                break;
            }

            auto sdp_parser = mediakit::SdpParser(video_track->getSdp(96)->getSdp());
            auto sdp_video_track = sdp_parser.getTrack(mediakit::TrackVideo);
            CHECK(sdp_video_track);
            std::vector<std::string> config_frames;
            for (const auto &attr : sdp_video_track->_attr) {
                if (attr.first != "fmtp" || attr.second.find("sprop") == std::string::npos) {
                    continue;
                }

                auto pos = attr.second.find(' ');
                if (pos == std::string::npos) {
                    continue;
                }

                auto format_parameters = toolkit::split(attr.second.substr(pos), ";");
                for (auto fp : format_parameters) {
                    toolkit::trim(fp);
                    pos = fp.find('=');
                    if (pos == std::string::npos) {
                        continue;
                    }
                    if (!strncmp(fp.data(), "sprop-parameter-sets", 20)) {
                        // h264
                        pos = fp.find('=');
                        auto parameters = toolkit::split(fp.substr(pos + 1), ",");
                        for (const auto &p : parameters) {
                            config_frames.emplace_back(decodeBase64(p));
                        }
                    } else if (   !strncmp(fp.data(), "sprop-vps", 9)
                               || !strncmp(fp.data(), "sprop-sps", 9)
                               || !strncmp(fp.data(), "sprop-pps", 9)) {
                        // h265
                        config_frames.emplace_back(decodeBase64(fp.substr(pos + 1)));
                    }
                }
            }

            if (auto encoder = mediakit::Factory::getRtpEncoderByCodecId(video_track->getCodecId(), sdp_video_track->_pt)) {
                GET_CONFIG(uint32_t, video_mtu, Rtp::kVideoMtuSize);
                encoder->setRtpInfo(sdp_video_track->_ssrc, video_mtu, sdp_video_track->_samplerate,
                                    sdp_video_track->_pt, 2 * video_track->getTrackType(), video_track->getIndex());

                for (const auto &f : config_frames) {
                    _config_packets.emplace_back(
                        encoder->getRtpInfo().makeRtp(TrackVideo, f.data(), f.size(), false, 0));
                }
                _send_config_packets = !_config_packets.empty();
            }
        } while (false);
    }
}

void WebRtcPlayer::onStartWebRTC() {
    auto playSrc = _play_src.lock();
    if(!playSrc){
        onShutdown(SockException(Err_shutdown, "rtsp media source was shutdown"));
        return ;
    }
    WebRtcTransportImp::onStartWebRTC();
    if (canSendRtp()) {
        playSrc->pause(false);
        _reader = playSrc->getRing()->attach(getPoller(), true);
        weak_ptr<WebRtcPlayer> weak_self = static_pointer_cast<WebRtcPlayer>(shared_from_this());
        weak_ptr<Session> weak_session = static_pointer_cast<Session>(getSession());
        _reader->setGetInfoCB([weak_session]() {
            Any ret;
            ret.set(static_pointer_cast<SockInfo>(weak_session.lock()));
            return ret;
        });
        _reader->setReadCB([weak_self](const RtspMediaSource::RingDataType &pkt) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            if (strong_self->_send_config_packets && !strong_self->_config_packets.empty() && !pkt->empty()) {
                const auto &first_rtp = pkt->front();
                auto seq = first_rtp->getSeq() - strong_self->_config_packets.size();
                for (const auto &rtp : strong_self->_config_packets) {
                    auto header = rtp->getHeader();
                    header->seq = htons(seq++);
                    header->stamp = htonl(first_rtp->getStamp());
                    rtp->ntp_stamp = first_rtp->ntp_stamp;
                    strong_self->onSendRtp(rtp, false);
                }
                // sent config frames once
                strong_self->_send_config_packets = false;
            }
            size_t i = 0;
            pkt->for_each([&](const RtpPacket::Ptr &rtp) {
                //TraceL<<"send track type:"<<rtp->type<<" ts:"<<rtp->getStamp()<<" ntp:"<<rtp->ntp_stamp<<" size:"<<rtp->getPayloadSize()<<" i:"<<i;
                strong_self->onSendRtp(rtp, ++i == pkt->size());
            });
        });
        _reader->setDetachCB([weak_self]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            strong_self->onShutdown(SockException(Err_shutdown, "rtsp ring buffer detached"));
        });

        _reader->setMessageCB([weak_self] (const toolkit::Any &data) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            if (data.is<Buffer>()) {
                auto &buffer = data.get<Buffer>();
                // PPID 51: 文本string
                // PPID 53: 二进制
                strong_self->sendDatachannel(0, 51, buffer.data(), buffer.size());
            } else {
                WarnL << "Send unknown message type to webrtc player: " << data.type_name();
            }
        });
    }
}
void WebRtcPlayer::onDestory() {
    auto duration = getDuration();
    auto bytes_usage = getBytesUsage();
    //流量统计事件广播
    GET_CONFIG(uint32_t, iFlowThreshold, General::kFlowThreshold);
    if (_reader && getSession()) {
        WarnL << "RTC播放器(" << _media_info.shortUrl() << ")结束播放,耗时(s):" << duration;
        if (bytes_usage >= iFlowThreshold * 1024) {
            NOTICE_EMIT(BroadcastFlowReportArgs, Broadcast::kBroadcastFlowReport, _media_info, bytes_usage, duration, true, *getSession());
        }
    }
    WebRtcTransportImp::onDestory();
}

void WebRtcPlayer::onRtcConfigure(RtcConfigure &configure) const {
    auto playSrc = _play_src.lock();
    if(!playSrc){
        return ;
    }
    WebRtcTransportImp::onRtcConfigure(configure);
    //这是播放
    configure.audio.direction = configure.video.direction = RtpDirection::sendonly;
    configure.setPlayRtspInfo(playSrc->getSdp());
}

}// namespace mediakit