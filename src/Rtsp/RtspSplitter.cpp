﻿/*
 * MIT License
 *
 * Copyright (c) 2016 xiongziliang <771730766@qq.com>
 *
 * This file is part of ZLMediaKit(https://github.com/xiongziliang/ZLMediaKit).
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "RtspSplitter.h"

namespace mediakit{

const char *RtspSplitter::onSearchPacketTail(const char *data, int len) {
    if(!_enableRecvRtp){
        _isRtpPacket = false;
        return HttpRequestSplitter::onSearchPacketTail(data, len);
    }
    if(data[0] != '$'){
        //这是rtsp包
        _isRtpPacket = false;
        return HttpRequestSplitter::onSearchPacketTail(data, len);
    }
    //这是rtp包
    if(len < 4){
        //数据不够
        return nullptr;
    }
    uint16_t length = (((uint8_t *)data)[2] << 8) | ((uint8_t *)data)[3];
    if(len < length + 4){
        //数据不够
        return nullptr;
    }
    //返回rtp包末尾
    _isRtpPacket = true;
    return data + 4 + length;
}

int64_t RtspSplitter::onRecvHeader(const char *data, uint64_t len) {
    if(_isRtpPacket){
        onRtpPacket(data,len);
        return 0;
    }
    _parser.Parse(data);
    auto ret = atoi(_parser["Content-Length"].data());
    if(ret == 0){
        onWholeRtspPacket(_parser);
    }
    return ret;
}

void RtspSplitter::onRecvContent(const char *data, uint64_t len) {
    _parser.setContent(string(data,len));
    onWholeRtspPacket(_parser);
}

void RtspSplitter::enableRecvRtp(bool enable) {
    _enableRecvRtp = enable;
}


}//namespace mediakit



