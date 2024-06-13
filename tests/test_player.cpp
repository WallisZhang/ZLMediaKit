
#include "Codec/Transcode.h"
#include "Common/Device.h"
#include "Common/config.h"
#include "Common/macros.h"
#include "Player/MediaPlayer.h"
#include "Record/Recorder.h"
#include "Rtsp/UDPServer.h"
#include "Util/logger.h"
#include "Util/onceToken.h"
#include "Util/util.h"
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <signal.h>
#include <unistd.h>

#include "Network/TcpServer.h"
#include "Poller/EventPoller.h"
#include "Util/MD5.h"
#include "Util/SSLBox.h"
#include "Util/logger.h"
#include "Util/onceToken.h"

#include "Common/config.h"
#include "Http/WebSocketSession.h"
#include "Player/PlayerProxy.h"
#include "Rtmp/FlvMuxer.h"
#include "Rtmp/RtmpSession.h"
#include "Rtsp/RtspSession.h"
#include "Rtsp/UDPServer.h"
#include "Shell/ShellSession.h"

using namespace std;
using namespace toolkit;
using namespace mediakit;

static std::shared_ptr<DevChannel> g_dev;

void test_player(const std::string &url) {

    // ������־
    Logger::Instance().add(std::make_shared<ConsoleChannel>());

    g_dev = std::make_shared<mediakit::DevChannel>(mediakit::MediaTuple { DEFAULT_VHOST, "live", "test" });

    auto player = std::make_shared<MediaPlayer>();
    // sdlҪ����main�̳߳�ʼ��
    weak_ptr<MediaPlayer> weakPlayer = player;
    player->setOnPlayResult([weakPlayer](const SockException &ex) {
        InfoL << "OnPlayResult:" << ex.what();
        auto strongPlayer = weakPlayer.lock();
        if (ex || !strongPlayer) {
            return;
        }

        auto videoTrack = dynamic_pointer_cast<VideoTrack>(strongPlayer->getTrack(TrackVideo, false));
        auto audioTrack = dynamic_pointer_cast<AudioTrack>(strongPlayer->getTrack(TrackAudio, false));

        if (videoTrack) {
            VideoInfo info;
            info.codecId = videoTrack->getCodecId();
            info.iWidth = videoTrack->getVideoWidth();
            info.iHeight = videoTrack->getVideoHeight();
            info.iFrameRate = videoTrack->getVideoFps();

            g_dev->initVideo(info);
            videoTrack->addDelegate([](const Frame::Ptr &frame) { return g_dev->inputFrame(frame); });
        }

        if (audioTrack) {
            AudioInfo info;
            info.codecId = audioTrack->getCodecId();
            info.iChannel = audioTrack->getAudioChannel();
            info.iSampleBit = audioTrack->getAudioSampleBit();
            info.iSampleRate = audioTrack->getAudioSampleRate();
            g_dev->initAudio(info);

            audioTrack->addDelegate([](const Frame::Ptr &frame) { return g_dev->inputFrame(frame); });
        }
    });

    player->setOnShutdown([](const SockException &ex) { WarnL << "play shutdown: " << ex.what(); });

    (*player)[Client::kRtpType] = mediakit::Rtsp::RTP_TCP;
    // ���ȴ�track ready�ٻص����ųɹ��¼����������Լӿ��뿪�ٶ�
    (*player)[Client::kWaitTrackReady] = true;
    player->play(url);

    getchar();
}

namespace mediakit {
////////////HTTP����///////////
namespace Http {
#define HTTP_FIELD "http."
#define HTTP_PORT 80
const string kPort = HTTP_FIELD "port";
#define HTTPS_PORT 443
const string kSSLPort = HTTP_FIELD "sslport";
onceToken token1(
    []() {
        mINI::Instance()[kPort] = HTTP_PORT;
        mINI::Instance()[kSSLPort] = HTTPS_PORT;
    },
    nullptr);
} // namespace Http

////////////SHELL����///////////
namespace Shell {
#define SHELL_FIELD "shell."
#define SHELL_PORT 9000
const string kPort = SHELL_FIELD "port";
onceToken token1([]() { mINI::Instance()[kPort] = SHELL_PORT; }, nullptr);
} // namespace Shell

////////////RTSP����������///////////
namespace Rtsp {
#define RTSP_FIELD "rtsp."
#define RTSP_PORT 554
#define RTSPS_PORT 322
const string kPort = RTSP_FIELD "port";
const string kSSLPort = RTSP_FIELD "sslport";
onceToken token1(
    []() {
        mINI::Instance()[kPort] = RTSP_PORT;
        mINI::Instance()[kSSLPort] = RTSPS_PORT;
    },
    nullptr);

} // namespace Rtsp

////////////RTMP����������///////////
namespace Rtmp {
#define RTMP_FIELD "rtmp."
#define RTMP_PORT 3935
const string kPort = RTMP_FIELD "port";
onceToken token1([]() { mINI::Instance()[kPort] = RTMP_PORT; }, nullptr);
} // namespace Rtmp
} // namespace mediakit

#define REALM "realm_zlmediakit"
static map<string, FlvRecorder::Ptr> s_mapFlvRecorder;
static mutex s_mtxFlvRecorder;

void initEventListener() {
    static onceToken s_token(
        []() {
            // ����kBroadcastOnGetRtspRealm�¼�����rtsp�����Ƿ���Ҫ��Ȩ(��ͳ��rtsp��Ȩ����)���ܷ���
            NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastOnGetRtspRealm, [](BroadcastOnGetRtspRealmArgs) {
                DebugL << "RTSP�Ƿ���Ҫ��Ȩ�¼���" << args.getUrl() << " " << args.params;
                if (string("1") == args.stream) {
                    // live/1��Ҫ��֤
                    // ������Ҫ��֤����������realm
                    invoker(REALM);
                } else {
                    // ��ʱ����Ҫ��ѯredis�����ݿ����жϸ����Ƿ���Ҫ��֤��ͨ��invoker�ķ�ʽ����������ȫ�첽
                    // �������ǲ���Ҫ��֤
                    invoker("");
                }
            });

            // ����kBroadcastOnRtspAuth�¼�������ȷ��rtsp��Ȩ�û�����
            NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastOnRtspAuth, [](BroadcastOnRtspAuthArgs) {
                DebugL << "RTSP���ż�Ȩ:" << args.getUrl() << " " << args.params;
                DebugL << "RTSP�û���" << user_name << (must_no_encrypt ? " Base64" : " MD5") << " ��ʽ��¼";
                string user = user_name;
                // ���������첽��ȡ���ݿ�
                if (user == "test0") {
                    // �������ݿⱣ���������
                    invoker(false, "pwd0");
                    return;
                }

                if (user == "test1") {
                    // �������ݿⱣ���������
                    auto encrypted_pwd = MD5(user + ":" + REALM + ":" + "pwd1").hexdigest();
                    invoker(true, encrypted_pwd);
                    return;
                }
                if (user == "test2" && must_no_encrypt) {
                    // �����¼����test2,������base64��ʽ��¼����ʱ�����ṩ�������룬��ô�ᵼ����֤ʧ��
                    // ����ͨ�������ʽ����base64���ֲ���ȫ�ļ��ܷ�ʽ
                    invoker(true, "pwd2");
                    return;
                }

                // �����û�������û���һ��
                invoker(false, user);
            });

            // ����rtsp/rtmp�����¼������ؽ����֪�Ƿ�������Ȩ��
            NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastMediaPublish, [](BroadcastMediaPublishArgs) {
                DebugL << "������Ȩ��" << args.getUrl() << " " << args.params;
                invoker("", ProtocolOption()); // ��Ȩ�ɹ�
                                               // invoker("this is auth failed message");//��Ȩʧ��
            });

            // ����rtsp/rtsps/rtmp/http-flv�����¼������ؽ����֪�Ƿ��в���Ȩ��(rtspͨ��kBroadcastOnRtspAuth����¼�������ʵ�ּ�Ȩ)
            NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastMediaPlayed, [](BroadcastMediaPlayedArgs) {
                DebugL << "���ż�Ȩ:" << args.getUrl() << " " << args.params;
                invoker(""); // ��Ȩ�ɹ�
                             // invoker("this is auth failed message");//��Ȩʧ��
            });

            // shell��¼�¼���ͨ��shell���Ե�¼��������ִ��һЩ����
            NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastShellLogin, [](BroadcastShellLoginArgs) {
                DebugL << "shell login:" << user_name << " " << passwd;
                invoker(""); // ��Ȩ�ɹ�
                             // invoker("this is auth failed message");//��Ȩʧ��
            });

            // ����rtsp��rtmpԴע���ע���¼����˴����ڲ���rtmp����Ϊflv¼�񣬱�����http��Ŀ¼��
            NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastMediaChanged, [](BroadcastMediaChangedArgs) {
                auto tuple = sender.getMediaTuple();
                if (sender.getSchema() == RTMP_SCHEMA && tuple.app == "live") {
                    lock_guard<mutex> lck(s_mtxFlvRecorder);
                    auto key = tuple.shortUrl();
                    if (bRegist) {
                        DebugL << "��ʼ¼��RTMP��" << sender.getUrl();
                        GET_CONFIG(string, http_root, Http::kRootPath);
                        auto path = http_root + "/" + key + "_" + to_string(time(NULL)) + ".flv";
                        FlvRecorder::Ptr recorder(new FlvRecorder);
                        try {
                            recorder->startRecord(
                                EventPollerPool::Instance().getPoller(), dynamic_pointer_cast<RtmpMediaSource>(sender.shared_from_this()), path);
                            s_mapFlvRecorder[key] = recorder;
                        } catch (std::exception &ex) {
                            WarnL << ex.what();
                        }
                    } else {
                        s_mapFlvRecorder.erase(key);
                    }
                }
            });

            // ��������ʧ��(δ�ҵ��ض�����)�¼�
            NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastNotFoundStream, [](BroadcastNotFoundStreamArgs) {
                /**
                 * �����������¼�����ʱ��ȥ�����������Ϳ���ʵ�ְ�������
                 * �����ɹ���ZLMediaKit���������ת����������(���ȴ�ʱ��ԼΪ5�룬���5�붼δ�����ɹ����������Ქ��ʧ��)
                 */
                DebugL << "δ�ҵ����¼�:" << args.getUrl() << " " << args.params;
            });

            // �������Ż���������ʱ���������¼�
            NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastFlowReport, [](BroadcastFlowReportArgs) {
                DebugL << "������(������)�Ͽ������¼�:" << args.getUrl() << " " << args.params << "\r\nʹ������:" << totalBytes
                       << " bytes,����ʱ��:" << totalDuration << "��";
            });
        },
        nullptr);
}

#if !defined(SIGHUP)
#define SIGHUP 1
#endif

int main(int argc, char *argv[]) {
    // ������־
    Logger::Instance().add(std::make_shared<ConsoleChannel>());
    Logger::Instance().add(std::make_shared<FileChannel>());
    Logger::Instance().setWriter(std::make_shared<AsyncLogWriter>());

    if (argc < 2) {
        ErrorL << "\r\n���Է�����./test_player rtxp_url\r\n";

        return 0;
    }

    std::string url = argv[1];
    // ���������ļ�����������ļ������ھʹ���һ��
    loadIniConfig();
    initEventListener();

    // ����֤�飬֤�������Կ��˽Կ
    SSL_Initor::Instance().loadCertificate((exeDir() + "ssl.p12").data());
    // ����ĳ����ǩ��֤��
    SSL_Initor::Instance().trustCertificate((exeDir() + "ssl.p12").data());
    // ��������Ч֤��֤��(������ǩ�������֤��)
    SSL_Initor::Instance().ignoreInvalidCertificate(false);

    uint16_t shellPort = mINI::Instance()[Shell::kPort];
    uint16_t rtspPort = mINI::Instance()[Rtsp::kPort];
    uint16_t rtspsPort = mINI::Instance()[Rtsp::kSSLPort];
    uint16_t rtmpPort = 2935;
    uint16_t httpPort = 8088;
    uint16_t httpsPort = mINI::Instance()[Http::kSSLPort];

    // �򵥵�telnet�������������ڷ��������ԣ����ǲ���ʹ��23�˿ڣ�����telnet����Ī�����������
    // ���Է���:telnet 127.0.0.1 9000
    TcpServer::Ptr shellSrv(new TcpServer());
    TcpServer::Ptr rtspSrv(new TcpServer());
    TcpServer::Ptr rtmpSrv(new TcpServer());
    TcpServer::Ptr httpSrv(new TcpServer());

    shellSrv->start<ShellSession>(shellPort);
    rtspSrv->start<RtspSession>(rtspPort); // Ĭ��554
    rtmpSrv->start<RtmpSession>(rtmpPort); // Ĭ��1935
    // http������
    httpSrv->start<HttpSession>(httpPort); // Ĭ��80

    // ���֧��ssl�������Կ���https������
    TcpServer::Ptr httpsSrv(new TcpServer());
    // https������
    httpsSrv->start<HttpsSession>(httpsPort); // Ĭ��443

    // ֧��ssl���ܵ�rtsp����������������������ѷecho show�������豸����
    TcpServer::Ptr rtspSSLSrv(new TcpServer());
    rtspSSLSrv->start<RtspSessionWithSSL>(rtspsPort); // Ĭ��322

    // ������֧�ֶ�̬�л��˿�(��Ӱ����������)
    NoticeCenter::Instance().addListener(ReloadConfigTag, Broadcast::kBroadcastReloadConfig, [&](BroadcastReloadConfigArgs) {
        // ���´���������
        if (shellPort != mINI::Instance()[Shell::kPort].as<uint16_t>()) {
            shellPort = mINI::Instance()[Shell::kPort];
            shellSrv->start<ShellSession>(shellPort);
            InfoL << "����shell������:" << shellPort;
        }
        if (rtspPort != mINI::Instance()[Rtsp::kPort].as<uint16_t>()) {
            rtspPort = mINI::Instance()[Rtsp::kPort];
            rtspSrv->start<RtspSession>(rtspPort);
            InfoL << "����rtsp������" << rtspPort;
        }
        if (rtmpPort != mINI::Instance()[Rtmp::kPort].as<uint16_t>()) {
            rtmpPort = mINI::Instance()[Rtmp::kPort];
            rtmpSrv->start<RtmpSession>(rtmpPort);
            InfoL << "����rtmp������" << rtmpPort;
        }
        if (httpPort != mINI::Instance()[Http::kPort].as<uint16_t>()) {
            httpPort = mINI::Instance()[Http::kPort];
            httpSrv->start<HttpSession>(httpPort);
            InfoL << "����http������" << httpPort;
        }
        if (httpsPort != mINI::Instance()[Http::kSSLPort].as<uint16_t>()) {
            httpsPort = mINI::Instance()[Http::kSSLPort];
            httpsSrv->start<HttpsSession>(httpsPort);
            InfoL << "����https������" << httpsPort;
        }

        if (rtspsPort != mINI::Instance()[Rtsp::kSSLPort].as<uint16_t>()) {
            rtspsPort = mINI::Instance()[Rtsp::kSSLPort];
            rtspSSLSrv->start<RtspSessionWithSSL>(rtspsPort);
            InfoL << "����rtsps������" << rtspsPort;
        }
    });

    test_player(url);
    // �����˳��źŴ�����
    static semaphore sem;
    signal(SIGINT, [](int) { sem.post(); }); // �����˳��ź�
    signal(SIGHUP, [](int) { loadIniConfig(); });
    sem.wait();

    lock_guard<mutex> lck(s_mtxFlvRecorder);
    s_mapFlvRecorder.clear();
    return 0;
}
