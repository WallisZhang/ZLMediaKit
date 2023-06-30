/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef HLSFMP4RECORDER_H
#define HLSFMP4RECORDER_H

#include "Common/MediaSource.h"
#include "Common/config.h"
#include "FMP4/FMP4MediaSource.h"
#include "HlsMakerImp.h"
#include <memory>

using namespace std;
using namespace toolkit;

namespace mediakit {

class HlsFMP4Recorder final
    : public MediaSourceEventInterceptor
    , public MediaSinkInterface
    , public std::enable_shared_from_this<HlsFMP4Recorder> {
public:
    using Ptr = std::shared_ptr<HlsFMP4Recorder>;

    HlsFMP4Recorder(const std::string &m3u8_file, const std::string &params, const ProtocolOption &option) {
        GET_CONFIG(uint32_t, hlsNum, Hls::kSegmentNum);
        GET_CONFIG(bool, hlsKeep, Hls::kSegmentKeep);
        GET_CONFIG(uint32_t, hlsBufSize, Hls::kFileBufSize);
        GET_CONFIG(float, hlsDuration, Hls::kSegmentDuration);

        _option = option;
        _hls = std::make_shared<HlsMakerImp>(m3u8_file, params, hlsBufSize, hlsDuration, hlsNum, hlsKeep);
        _hls->setSchema(HLS_FMP4_SCHEMA);

        //����ϴεĲ����ļ�
        _hls->clearCache();
    }

    ~HlsFMP4Recorder() {};

    void setMediaSource(const MediaSource::Ptr &src) { _fmp4_src = dynamic_pointer_cast<FMP4MediaSource>(src); }

    void setMediaSource(const MediaTuple &tuple) {
        WarnL << "vhost:" << tuple.vhost << ", app:" << tuple.app << ", stream:" << tuple.stream;
        _hls->setMediaSource(tuple.vhost, tuple.app, tuple.stream);
    }

    void setListener(const std::weak_ptr<MediaSourceEvent> &listener) {
        setDelegate(listener);
        _hls->getMediaSource()->setListener(shared_from_this());
    }

    int readerCount() { return _hls->getMediaSource()->readerCount(); }

    void onReaderChanged(MediaSource &sender, int size) override {
        // hls������Ƭ����Ϊ0ʱ����Ϊhls¼��(��ɾ����Ƭ)����ô�������޹ۿ��߶�һֱ����hls
        _enabled = _option.hls_demand ? (_hls->isLive() ? size : true) : true;
        if (!size && _hls->isLive() && _option.hls_demand) {
            // hlsֱ��ʱ��������˹ۿ���ɾ����Ƶ���棬Ŀ����Ϊ�˷�ֹ��Ƶ��Ծ
            _clear_cache = true;
        }
        MediaSourceEventInterceptor::onReaderChanged(sender, size);
    }

    bool addTrack(const Track::Ptr &track) override { return true; }

    void addTrackCompleted() {
        const string init_seg = _fmp4_src->getInitSegment();
        _hls->inputData((char *)init_seg.c_str(), init_seg.size(), 0, true);

        weak_ptr<HlsFMP4Recorder> weak_self = static_pointer_cast<HlsFMP4Recorder>(shared_from_this());

        _fmp4_reader = _fmp4_src->getRing()->attach(EventPollerPool::Instance().getPoller());
        _fmp4_reader->setGetInfoCB([weak_self]() { return weak_self.lock(); });
        _fmp4_reader->setDetachCB([weak_self]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                // �������Ѿ�����
                return;
            }
        });
        _fmp4_reader->setReadCB([weak_self](const FMP4MediaSource::RingDataType &fmp4_list) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                // �������Ѿ�����
                return;
            }
            size_t i = 0;
            auto size = fmp4_list->size();
            fmp4_list->for_each([&](const FMP4Packet::Ptr &ts) { strong_self->onWrite(ts, ++i == size); });
        });
    };

    void resetTracks() { WarnL << "resetTracks"; };

    bool inputFrame(const Frame::Ptr &frame) override { return true; }

    void flush() {};

    bool isEnabled() {
        //������δ���ʱ����������inputFrame�������Ա㼰ʱ��ջ���
        return _option.hls_demand ? (_clear_cache ? true : _enabled) : true;
    }

private:
    void onWrite(FMP4Packet::Ptr packet, bool key) { _hls->inputData(packet->data(), packet->size(), packet->time_stamp, key); }

private:
    bool _enabled = true;
    bool _clear_cache = false;
    ProtocolOption _option;
    FMP4MediaSource::Ptr _fmp4_src;
    FMP4MediaSource::RingType::RingReader::Ptr _fmp4_reader;
    std::shared_ptr<HlsMakerImp> _hls;
};
} // namespace mediakit
#endif // HLSFMP4RECORDER_H
