﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xiongziliang/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_RTPRECEIVER_H
#define ZLMEDIAKIT_RTPRECEIVER_H

#include <map>
#include <string>
#include <memory>
#include "RtpCodec.h"
#include "RtspMediaSource.h"
using namespace std;
using namespace toolkit;

namespace mediakit {

template<typename T, typename SEQ = uint16_t, uint32_t kMax = 256, uint32_t kMin = 10>
class PacketSortor {
public:
    PacketSortor() = default;
    ~PacketSortor() = default;

    void setOnSort(function<void(SEQ seq, T &packet)> cb) {
        _cb = std::move(cb);
    }

    /**
     * 清空状态
     */
    void clear() {
        _seq_cycle_count = 0;
        _rtp_sort_cache_map.clear();
        _next_seq_out = 0;
        _max_sort_size = kMin;
    }

    /**
     * 获取排序缓存长度
     */
    int getJitterSize() {
        return _rtp_sort_cache_map.size();
    }

    /**
     * 获取seq回环次数
     */
    int getCycleCount() {
        return _seq_cycle_count;
    }

    /**
     * 输入并排序
     * @param seq 序列号
     * @param packet 包负载
     */
    void sortPacket(SEQ seq, T packet) {
        if (seq < _next_seq_out && _next_seq_out - seq > kMax) {
            //回环
            ++_seq_cycle_count;
        }
        //放入排序缓存
        _rtp_sort_cache_map.emplace(seq, std::move(packet));
        //尝试输出排序后的包
        tryPopPacket();
    }

private:
    void popPacket() {
        auto it = _rtp_sort_cache_map.begin();
        _cb(it->first, it->second);
        _next_seq_out = it->first + 1;
        _rtp_sort_cache_map.erase(it);
    }

    void tryPopPacket() {
        bool flag = false;
        while ((!_rtp_sort_cache_map.empty() && _rtp_sort_cache_map.begin()->first == _next_seq_out)) {
            //找到下个包，直接输出
            popPacket();
            flag = true;
        }

        if (flag) {
            setSortSize();
        } else if (_rtp_sort_cache_map.size() > _max_sort_size) {
            //排序缓存溢出，不再继续排序
            popPacket();
            setSortSize();
        }
    }

    void setSortSize() {
        _max_sort_size = 2 * _rtp_sort_cache_map.size();
        if (_max_sort_size > kMax) {
            _max_sort_size = kMax;
        } else if (_max_sort_size < kMin) {
            _max_sort_size = kMin;
        }
    }

private:
    //下次应该输出的SEQ
    SEQ _next_seq_out = 0;
    //seq回环次数计数
    uint32_t _seq_cycle_count = 0;
    //排序缓存长度
    uint32_t _max_sort_size = kMin;
    //rtp排序缓存，根据seq排序
    map<SEQ, T> _rtp_sort_cache_map;
    //回调
    function<void(SEQ seq, T &packet)> _cb;
};

class RtpReceiver {
public:
    RtpReceiver();
    virtual ~RtpReceiver();

protected:
    /**
     * 输入数据指针生成并排序rtp包
     * @param track_index track下标索引
     * @param type track类型
     * @param samplerate rtp时间戳基准时钟，视频为90000，音频为采样率
     * @param rtp_raw_ptr rtp数据指针
     * @param rtp_raw_len rtp数据指针长度
     * @return 解析成功返回true
     */
    bool handleOneRtp(int track_index, TrackType type, int samplerate, unsigned char *rtp_raw_ptr, unsigned int rtp_raw_len);

    /**
     * rtp数据包排序后输出
     * @param rtp rtp数据包
     * @param track_index track索引
     */
    virtual void onRtpSorted(const RtpPacket::Ptr &rtp, int track_index) {}

    void clear();
    void setPoolSize(int size);
    int getJitterSize(int track_index);
    int getCycleCount(int track_index);

private:
    void sortRtp(const RtpPacket::Ptr &rtp , int track_index);

private:
    uint32_t _ssrc[2] = {0, 0};
    //ssrc不匹配计数
    uint32_t _ssrc_err_count[2] = {0, 0};
    //rtp排序缓存，根据seq排序
    PacketSortor<RtpPacket::Ptr> _rtp_sortor[2];
    //rtp循环池
    RtspMediaSource::PoolType _rtp_pool;
};

}//namespace mediakit


#endif //ZLMEDIAKIT_RTPRECEIVER_H
