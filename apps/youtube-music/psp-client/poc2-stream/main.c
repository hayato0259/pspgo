/*
 * PoC 2: HTTP ストリーミング受信 → sceMp3(ハードウェアデコード) → sceAudio 再生。
 *
 * 検証内容:
 *   1. sceNetInet の TCP ソケットでプロキシサーバーへ HTTP GET
 *   2. 受信した MP3 ストリームをリングバッファへ蓄積
 *   3. sceMp3 (Media Engine) でデコードし sceAudioSRC で出力
 *
 * サーバー側 (検証当時は server/proxy.py。現在は server/app.py の /stream が同じ
 * 応答を返す) が YouTube Music の音声を MP3 CBR 128kbps に変換して配信する前提。
 *
 * SERVER_HOST は Makefile の -D で差し替え可能。
 * PPSSPP でホスト機のサーバーへ接続する場合は 127.0.0.1 のままで良い。
 */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspaudio.h>
#include <pspctrl.h>
#include <psputility.h>
#include <pspmp3.h>
#include <pspsdk.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

/*
 * BSD ソケットラッパー (socket/connect/...) は新 NID でインポートされ
 * PPSSPP 1.20 が未実装のため、古典的な sceNetInet* を直接使う。
 * 実機 (6.61 CFW) でもこちらの NID が本来の API。
 */
struct in_addr_psp { unsigned int s_addr; };
struct sockaddr_in_psp {
    unsigned char  sin_len;
    unsigned char  sin_family;
    unsigned short sin_port;
    struct in_addr_psp sin_addr;
    char sin_zero[8];
};
#define PSP_AF_INET 2
#define PSP_SOCK_STREAM 1

static unsigned short psp_htons(unsigned short v)
{
    return (unsigned short)((v << 8) | (v >> 8));
}

/* "a.b.c.d" -> network byte order。失敗時 0xFFFFFFFF */
static unsigned int ipv4_aton(const char *s)
{
    unsigned int parts[4] = {0, 0, 0, 0};
    int i = 0;
    while (*s && i < 4) {
        unsigned int v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (unsigned int)(*s - '0');
            s++; digits++;
        }
        if (!digits || v > 255)
            return 0xFFFFFFFFu;
        parts[i++] = v;
        if (*s == '.')
            s++;
        else
            break;
    }
    if (i != 4)
        return 0xFFFFFFFFu;
    return parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
}

PSP_MODULE_INFO("poc2_stream", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(12 * 1024);

#ifndef SERVER_HOST
#define SERVER_HOST "127.0.0.1"
#endif
#ifndef SERVER_PORT
#define SERVER_PORT 8080
#endif
#ifndef STREAM_PATH
#define STREAM_PATH "/stream"
#endif

#define MP3_BUF_SIZE  (128 * 1024)          /* sceMp3 へ渡すストリームバッファ */
#define PCM_BUF_SIZE  (1152 * 2 * 2 * 4)    /* デコード済み PCM (4フレーム分) */

static int g_running = 1;

static int exit_callback(int arg1, int arg2, void *common)
{
    g_running = 0;
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void)
{
    int thid = sceKernelCreateThread("cb_thread", callback_thread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0)
        sceKernelStartThread(thid, 0, 0);
}

static void die(const char *msg, int code)
{
    pspDebugScreenPrintf("FAIL: %s (0x%08X)\n", msg, code);
    pspDebugScreenPrintf("START de shuuryou\n");
    SceCtrlData pad;
    while (g_running) {
        sceCtrlReadBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_START)
            break;
        sceKernelDelayThread(50 * 1000);
    }
    sceKernelExitGame();
}

/* --- ネットワーク初期化 ------------------------------------------------- */

static int net_init(void)
{
    int rc;

    rc = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    if (rc < 0) return rc;
    rc = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
    if (rc < 0) return rc;

    rc = sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024);
    if (rc < 0) return rc;
    rc = sceNetInetInit();
    if (rc < 0) return rc;
    rc = sceNetApctlInit(0x8000, 48);
    if (rc < 0) return rc;

    /* 本体設定の接続 1 番を使う */
    rc = sceNetApctlConnect(1);
    if (rc < 0) return rc;

    pspDebugScreenPrintf("Wi-Fi setsuzoku machi...\n");
    while (g_running) {
        int state;
        rc = sceNetApctlGetState(&state);
        if (rc < 0) return rc;
        if (state == PSP_NET_APCTL_STATE_GOT_IP)
            break;
        sceKernelDelayThread(100 * 1000);
    }
    return 0;
}

/* --- HTTP GET (raw socket) ---------------------------------------------- */

/*
 * EAGAIN を待ち合わせる recv。
 * PPSSPP はソケットを内部でノンブロッキング化しており EAGAIN が普通に返る。
 * 実機のブロッキングソケットでもそのまま動く。
 * 戻り値: >0 受信バイト数 / 0 切断 / <0 エラー
 */
static int recv_wait(int sock, void *buf, int len)
{
    int tries = 0;
    for (;;) {
        int got = sceNetInetRecv(sock, buf, len, 0);
        if (got >= 0)
            return got;
        if (sceNetInetGetErrno() != 11 /* EAGAIN */)
            return got;
        if (++tries > 2000) /* 約10秒でタイムアウト */
            return -1;
        sceKernelDelayThread(5 * 1000);
    }
}

static int http_open_stream(const char *host, int port, const char *path)
{
    int sock = sceNetInetSocket(PSP_AF_INET, PSP_SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    struct sockaddr_in_psp addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_len = sizeof(addr);
    addr.sin_family = PSP_AF_INET;
    addr.sin_port = psp_htons((unsigned short)port);
    addr.sin_addr.s_addr = ipv4_aton(host);
    if (addr.sin_addr.s_addr == 0xFFFFFFFFu) {
        sceNetInetClose(sock);
        return -2;
    }

    if (sceNetInetConnect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        sceNetInetClose(sock);
        return -3;
    }

    char req[512];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.0\r\n"
                     "Host: %s\r\n"
                     "User-Agent: pspgo-poc/0.1\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     path, host);
    if (sceNetInetSend(sock, req, n, 0) != n) {
        sceNetInetClose(sock);
        return -4;
    }

    /* レスポンスヘッダを読み飛ばす（\r\n\r\n まで） */
    char c;
    int matched = 0;
    while (matched < 4) {
        if (recv_wait(sock, &c, 1) != 1) {
            sceNetInetClose(sock);
            return -5;
        }
        if ((matched % 2 == 0 && c == '\r') || (matched % 2 == 1 && c == '\n'))
            matched++;
        else
            matched = (c == '\r') ? 1 : 0;
    }
    return sock;
}

/* --- メイン ------------------------------------------------------------- */

/* 64byte アライン必須 */
static unsigned char g_mp3_buf[MP3_BUF_SIZE] __attribute__((aligned(64)));
static unsigned char g_pcm_buf[PCM_BUF_SIZE] __attribute__((aligned(64)));

int main(void)
{
    int rc;

    pspDebugScreenInit();
    setup_callbacks();
    pspDebugScreenPrintf("PoC2: HTTP -> sceMp3 -> sceAudio\n");
    pspDebugScreenPrintf("server: %s:%d%s\n", SERVER_HOST, SERVER_PORT, STREAM_PATH);

    /* AV モジュール (Media Engine の MP3 デコーダ) */
    rc = sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
    if (rc < 0) die("LoadModule AVCODEC", rc);
    rc = sceUtilityLoadModule(PSP_MODULE_AV_MP3);
    if (rc < 0) die("LoadModule MP3", rc);

    rc = net_init();
    if (rc < 0) die("net_init", rc);
    pspDebugScreenPrintf("Wi-Fi OK\n");

    int sock = http_open_stream(SERVER_HOST, SERVER_PORT, STREAM_PATH);
    if (sock < 0) die("http_open_stream", sock);
    pspDebugScreenPrintf("HTTP OK, stream kaishi\n");

    rc = sceMp3InitResource();
    if (rc < 0) die("sceMp3InitResource", rc);

    SceMp3InitArg init;
    memset(&init, 0, sizeof(init));
    init.mp3StreamStart = 0;
    init.mp3StreamEnd = 0x7FFFFFFF;   /* 終端不明のストリーム */
    init.mp3Buf = g_mp3_buf;
    init.mp3BufSize = MP3_BUF_SIZE;
    init.pcmBuf = g_pcm_buf;
    init.pcmBufSize = PCM_BUF_SIZE;

    int handle = sceMp3ReserveMp3Handle(&init);
    if (handle < 0) die("sceMp3ReserveMp3Handle", handle);

    /* 初回: バッファが埋まるまで受信して sceMp3 に通知 */
    int inited = 0;
    int src_reserved = 0;
    int channel = -1;
    unsigned int total_rx = 0, total_frames = 0;

    while (g_running) {
        /* sceMp3 がデータを要求していれば受信して追加 */
        if (sceMp3CheckStreamDataNeeded(handle) > 0) {
            SceUChar8 *dst;
            SceInt32 towrite, srcpos;
            rc = sceMp3GetInfoToAddStreamData(handle, &dst, &towrite, &srcpos);
            if (rc < 0) die("GetInfoToAddStreamData", rc);
            if (towrite > 0) {
                int got = recv_wait(sock, dst, towrite);
                if (got < 0) die("recv", got);
                if (got == 0) {
                    pspDebugScreenPrintf("stream end (rx=%u frames=%u)\n",
                                         total_rx, total_frames);
                    break;
                }
                total_rx += got;
                rc = sceMp3NotifyAddStreamData(handle, got);
                if (rc < 0) die("NotifyAddStreamData", rc);
            }
        }

        if (!inited) {
            /* ある程度たまってから初期化（ヘッダ解析に必要） */
            if (total_rx < 16 * 1024)
                continue;
            rc = sceMp3Init(handle);
            if (rc < 0) die("sceMp3Init", rc);
            int hz = sceMp3GetSamplingRate(handle);
            int ch_n = sceMp3GetMp3ChannelNum(handle);
            int kbps = sceMp3GetBitRate(handle);
            pspDebugScreenPrintf("mp3: %dHz %dch %dkbps\n", hz, ch_n, kbps);
            inited = 1;
        }

        short *out = NULL;
        int bytes = sceMp3Decode(handle, &out);
        if (bytes == (int)0x80671402 /* ERROR_MP3_LOW_LEVEL_NO_DATA 相当 */) {
            continue; /* データ待ち */
        }
        if (bytes < 0)
            die("sceMp3Decode", bytes);
        if (bytes == 0)
            continue;

        int nsamples = bytes / 4; /* 16bit stereo */
        if (!src_reserved) {
            int hz = sceMp3GetSamplingRate(handle);
            rc = sceAudioSRCChReserve(nsamples, hz, 2);
            if (rc < 0) die("sceAudioSRCChReserve", rc);
            channel = rc;
            src_reserved = 1;
        }
        sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, out);
        total_frames++;

        if ((total_frames & 0x3F) == 0) {
            pspDebugScreenSetXY(0, 10);
            pspDebugScreenPrintf("frames=%u rx=%uKB   ", total_frames, total_rx / 1024);
        }

        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_START)
            break;
    }

    if (src_reserved) {
        (void)channel;
        sceAudioSRCChRelease();
    }
    sceMp3ReleaseMp3Handle(handle);
    sceMp3TermResource();
    sceNetInetClose(sock);
    sceKernelExitGame();
    return 0;
}
