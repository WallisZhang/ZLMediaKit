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

#ifndef SRC_HTTP_HTTPSSESSION_H_
#define SRC_HTTP_HTTPSSESSION_H_

#include "HttpSession.h"
#include "Util/SSLBox.h"
#include "Util/TimeTicker.h"

using namespace toolkit;

namespace mediakit {

class HttpsSession: public HttpSession {
public:
	HttpsSession(const std::shared_ptr<ThreadPool> &pTh, const Socket::Ptr &pSock):
		HttpSession(pTh,pSock){
		_sslBox.setOnEncData([&](const char *data, uint32_t len){
#if defined(__GNUC__) && (__GNUC__ < 5)
			public_send(data,len);
#else//defined(__GNUC__) && (__GNUC__ < 5)
			HttpSession::send(obtainBuffer(data,len));
#endif//defined(__GNUC__) && (__GNUC__ < 5)
		});
		_sslBox.setOnDecData([&](const char *data, uint32_t len){
#if defined(__GNUC__) && (__GNUC__ < 5)
			public_onRecv(data,len);
#else//defined(__GNUC__) && (__GNUC__ < 5)
			HttpSession::onRecv(data,len);
#endif//defined(__GNUC__) && (__GNUC__ < 5)
		});
	}
	virtual ~HttpsSession(){
		//_sslBox.shutdown();
	}
	void onRecv(const Buffer::Ptr &pBuf) override{
		TimeTicker();
		_sslBox.onRecv(pBuf->data(), pBuf->size());
	}
#if defined(__GNUC__) && (__GNUC__ < 5)
	int public_send(const char *data, uint32_t len){
		return HttpSession::send(obtainBuffer(data,len));
	}
	void public_onRecv(const char *data, uint32_t len){
		HttpSession::onRecv(data,len);
	}
#endif//defined(__GNUC__) && (__GNUC__ < 5)
protected:
	virtual int send(const Buffer::Ptr &buf) override{
		TimeTicker();
		_sslBox.onSend(buf->data(), buf->size());
		return buf->size();
	}
	SSL_Box _sslBox;
};



/**
* 通过该模板类可以透明化WebSocket协议，
* 用户只要实现WebSock协议下的具体业务协议，譬如基于WebSocket协议的Rtmp协议等
* @tparam SessionType 业务协议的TcpSession类
*/
template <class SessionType,class HttpSessionType = HttpSession>
class WebSocketSession : public HttpSessionType {
public:
	WebSocketSession(const std::shared_ptr<ThreadPool> &pTh, const Socket::Ptr &pSock) : HttpSessionType(pTh,pSock){}
	virtual ~WebSocketSession(){}

	//收到eof或其他导致脱离TcpServer事件的回调
	void onError(const SockException &err) override{
		HttpSessionType::onError(err);
		if(_session){
			_session->onError(err);
		}
	}
	//每隔一段时间触发，用来做超时管理
	void onManager() override{
		if(_session){
			_session->onManager();
		}else{
            HttpSessionType::onManager();
		}
	}

	void attachServer(const TcpServer &server) override{
		HttpSessionType::attachServer(server);
		_weakServer = const_cast<TcpServer &>(server).shared_from_this();
	}
protected:
	/**
	 * 开始收到一个webSocket数据包
	 * @param packet
	 */
	void onWebSocketDecodeHeader(const WebSocketHeader &packet) override{
		//新包，原来的包残余数据清空掉
		_remian_data.clear();

		if(_firstPacket){
			//这是个WebSocket会话而不是普通的Http会话
			_firstPacket = false;
			_session = std::make_shared<SessionImp>(HttpSessionType::getIdentifier(),nullptr,HttpSessionType::_sock);

			auto strongServer = _weakServer.lock();
			if(strongServer){
				_session->attachServer(*strongServer);
			}

			//此处截取数据并进行websocket协议打包
			weak_ptr<WebSocketSession> weakSelf = dynamic_pointer_cast<WebSocketSession>(HttpSessionType::shared_from_this());
			_session->setOnBeforeSendCB([weakSelf](const Buffer::Ptr &buf){
				auto strongSelf = weakSelf.lock();
				if(strongSelf){
                    WebSocketHeader header;
                    header._fin = true;
                    header._reserved = 0;
                    header._opcode = WebSocketHeader::TEXT;
                    header._mask_flag = false;
                    strongSelf->WebSocketSplitter::encode(header,(uint8_t *)buf->data(),buf->size());
				}
				return buf->size();
			});
		}
	}

	/**
	 * 收到websocket数据包负载
	 * @param packet
	 * @param ptr
	 * @param len
	 * @param recved
	 */
	void onWebSocketDecodePlayload(const WebSocketHeader &packet,const uint8_t *ptr,uint64_t len,uint64_t recved) override {
		_remian_data.append((char *)ptr,len);
	}

	/**
	 * 接收到完整的一个webSocket数据包后回调
	 * @param header 数据包包头
	 */
	void onWebSocketDecodeComplete(const WebSocketHeader &header_in) override {
        WebSocketHeader& header = const_cast<WebSocketHeader&>(header_in);
        auto  flag = header._mask_flag;
		header._mask_flag = false;

		switch (header._opcode){
            case WebSocketHeader::CLOSE:{
                HttpSessionType::encode(header,nullptr,0);
			}
				break;
			case WebSocketHeader::PING:{
				const_cast<WebSocketHeader&>(header)._opcode = WebSocketHeader::PONG;
                HttpSessionType::encode(header,(uint8_t *)_remian_data.data(),_remian_data.size());
			}
				break;
			case WebSocketHeader::CONTINUATION:{

			}
				break;
			case WebSocketHeader::TEXT:
			case WebSocketHeader::BINARY:{
				BufferString::Ptr buffer = std::make_shared<BufferString>(_remian_data);
				_session->onRecv(buffer);
			}
				break;
			default:
				break;
		}
		_remian_data.clear();
		header._mask_flag = flag;
	}

	/**
	* 发送数据进行websocket协议打包后回调
	* @param ptr
	* @param len
	*/
	void onWebSocketEncodeData(const uint8_t *ptr,uint64_t len) override{
        SocketHelper::send((char *)ptr,len);
	}
private:
	typedef function<int(const Buffer::Ptr &buf)> onBeforeSendCB;
	/**
	 * 该类实现了TcpSession派生类发送数据的截取
	 * 目的是发送业务数据前进行websocket协议的打包
	 */
	class SessionImp : public SessionType{
	public:
		SessionImp(const string &identifier,
				   const std::shared_ptr<ThreadPool> &pTh,
				   const Socket::Ptr &pSock) :
				_identifier(identifier),SessionType(pTh,pSock){}

		~SessionImp(){}

		/**
		 * 设置发送数据截取回调函数
		 * @param cb 截取回调函数
		 */
		void setOnBeforeSendCB(const onBeforeSendCB &cb){
			_beforeSendCB = cb;
		}
	protected:
		/**
		 * 重载send函数截取数据
		 * @param buf 需要截取的数据
		 * @return 数据字节数
		 */
		int send(const Buffer::Ptr &buf) override {
			if(_beforeSendCB){
				return _beforeSendCB(buf);
			}
			return SessionType::send(buf);
		}
		string getIdentifier() const override{
			return _identifier;
		}
	private:
		onBeforeSendCB _beforeSendCB;
		string _identifier;
	};
private:
	bool _firstPacket = true;
	string _remian_data;
	weak_ptr<TcpServer> _weakServer;
	std::shared_ptr<SessionImp> _session;
};

/**
* 回显会话
*/
class EchoSession : public TcpSession {
public:
	EchoSession(const std::shared_ptr<ThreadPool> &pTh, const Socket::Ptr &pSock) :
			TcpSession(pTh,pSock){
		DebugL;
	}
	virtual ~EchoSession(){
		DebugL;
	}

	void attachServer(const TcpServer &server) override{
		DebugL << getIdentifier() << " " << TcpSession::getIdentifier();
	}
	void onRecv(const Buffer::Ptr &buffer) override {
		send(buffer);
	}
	void onError(const SockException &err) override{
		WarnL << err.what();
	}
	//每隔一段时间触发，用来做超时管理
	void onManager() override{
		DebugL;
	}
};


typedef WebSocketSession<EchoSession,HttpSession> EchoWebSocketSession;
typedef WebSocketSession<EchoSession,HttpsSession> SSLEchoWebSocketSession;


} /* namespace mediakit */

#endif /* SRC_HTTP_HTTPSSESSION_H_ */
