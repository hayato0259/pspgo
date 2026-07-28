/*
 * 映像再生: HTTP で PSMF を受け取り → sceMpeg (Media Engine) → 画面に直接書く。
 *
 * 役割を 2 つのスレッドに分ける。
 *   受信スレッド : ソケットから読んでメモリの環状バッファに積むだけ
 *   描画スレッド : 環状バッファから 1 フレームぶんデコードする (video_decode)
 *
 * こう分けるのは、デコード結果が「今から描くバッファ」に直接書かれるため。
 * 描画スレッド以外でデコードすると、書いている最中に画面が切り替わってしまう。
 * 一方で受信を描画スレッドでやると通信待ちで画面が止まるので、そちらは分ける。
 */
#include <pspkernel.h>
#include <pspmpeg.h>
#include <psputility.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include "video.h"
#include "net.h"
#include "common.h"

#define PSMF_HEADER_SIZE 2048
#define PACK_SIZE        2048
#define RING_PACKETS     96              /* sceMpeg 側のリングバッファ */
#define FRAME_WIDTH      512             /* フレームバッファの 1 行の画素数 */

/* 受信をためておく環状バッファ。回線が詰まってもしばらく持ちこたえる長さにする */
#define RX_SIZE (512 * 1024)

static unsigned char g_rx[RX_SIZE];
static volatile unsigned int g_rx_head = 0;   /* 受信スレッドが書く位置 */
static volatile unsigned int g_rx_tail = 0;   /* デコード側が読んだ位置 */
static volatile int g_rx_eof = 0;

static volatile VideoState g_state = VIDEO_STOPPED;
static volatile int g_last_error = 0;
static volatile int g_frames = 0;
static volatile int g_cmd_stop = 0;
static volatile int g_thread_running = 0;

static char g_video_id[24];
static int g_seconds;
static SceUID g_thread = -1;
static int g_sock = -1;

/* sceMpeg 側 */
static SceMpeg g_mpeg;
static SceMpegRingbuffer g_ring;
static SceMpegStream *g_vstream;
static SceMpegAu g_au;
static void *g_ring_data;
static void *g_mpeg_data;
static void *g_es_buf;
static void *g_display_buf;                  /* デコード先のアドレスを保持する変数 */
static int g_mpeg_ready = 0;
static SceInt32 g_init_flag = 0;
static int g_au_retry = 0;
static unsigned char g_header[PSMF_HEADER_SIZE] __attribute__((aligned(64)));

static unsigned int rx_available(void)
{
    return g_rx_head - g_rx_tail;   /* 符号なしの差なので巻き戻りも正しく出る */
}

/* 環状バッファから len バイト取り出す。足りなければ 0 を返して何もしない */
static int rx_take(unsigned char *dst, unsigned int len)
{
    if (rx_available() < len)
        return 0;
    unsigned int pos = g_rx_tail % RX_SIZE;
    unsigned int first = RX_SIZE - pos;
    if (first > len)
        first = len;
    memcpy(dst, g_rx + pos, first);
    if (len > first)
        memcpy(dst + first, g_rx, len - first);
    g_rx_tail += len;
    return 1;
}

/*
 * sceMpeg がデータを欲しがったときに呼ばれる。
 * 環状バッファにある分だけ渡す。無ければ 0 を返す (次の呼び出しで再度試される)。
 */
static SceInt32 ringbuffer_cb(ScePVoid pData, SceInt32 iNumPackets, ScePVoid pParam)
{
    (void)pParam;
    unsigned int want = (unsigned int)iNumPackets * PACK_SIZE;
    unsigned int have = rx_available();
    if (have < PACK_SIZE)
        return 0;
    if (want > have)
        want = (have / PACK_SIZE) * PACK_SIZE;
    if (!rx_take((unsigned char *)pData, want))
        return 0;
    return (SceInt32)(want / PACK_SIZE);
}

/* --- 受信スレッド ------------------------------------------------------- */

static int recv_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    g_thread_running = 1;

    char base_path[128], path[256];
    snprintf(base_path, sizeof(base_path), "/video?yt=%s&sec=%d",
             g_video_id, g_seconds);
    if (net_build_path(path, sizeof(path), base_path) < 0) {
        g_last_error = -1;
        g_state = VIDEO_ERROR;
        g_thread_running = 0;
        return 0;
    }

    g_sock = http_open_stream(net_server_host(), net_server_port(), path);
    if (g_sock < 0) {
        g_last_error = g_sock;
        g_state = VIDEO_ERROR;
        g_thread_running = 0;
        return 0;
    }

    while (!g_cmd_stop) {
        /* 空きが少ないうちは受け取らない (デコードが追いつくのを待つ) */
        unsigned int used = rx_available();
        if (used > RX_SIZE - PACK_SIZE * 8) {
            sceKernelDelayThread(10 * 1000);
            continue;
        }

        unsigned int pos = g_rx_head % RX_SIZE;
        unsigned int room = RX_SIZE - pos;               /* 折り返しまで */
        unsigned int free_bytes = RX_SIZE - used;
        if (room > free_bytes)
            room = free_bytes;

        int got = net_recv_wait_abortable(g_sock, g_rx + pos, (int)room,
                                          &g_cmd_stop);
        if (got < 0) {
            g_last_error = got;
            break;
        }
        if (got == 0) {
            g_rx_eof = 1;    /* サーバーが閉じた = 最後まで受け取った */
            break;
        }
        g_rx_head += (unsigned int)got;
    }

    net_close(g_sock);
    g_sock = -1;
    g_thread_running = 0;
    return 0;
}

/* --- sceMpeg の準備 (最初のデータが届いてから行う) ---------------------- */

static int mpeg_setup(void)
{
    /* PSMF ヘッダを取り出す。これが揃うまでは何も始められない */
    if (!rx_take(g_header, PSMF_HEADER_SIZE))
        return 0;
    if (memcmp(g_header, "PSMF", 4) != 0) {
        g_last_error = -10;
        g_state = VIDEO_ERROR;
        return -1;
    }

    if (sceMpegInit() < 0) {
        g_last_error = -11;
        g_state = VIDEO_ERROR;
        return -1;
    }

    int ring_size = sceMpegRingbufferQueryMemSize(RING_PACKETS);
    g_ring_data = memalign(64, ring_size);
    if (!g_ring_data ||
        sceMpegRingbufferConstruct(&g_ring, RING_PACKETS, g_ring_data,
                                   ring_size, ringbuffer_cb, NULL) < 0) {
        g_last_error = -12;
        g_state = VIDEO_ERROR;
        return -1;
    }

    int mpeg_size = sceMpegQueryMemSize(0);
    g_mpeg_data = memalign(64, mpeg_size);
    if (!g_mpeg_data ||
        sceMpegCreate(&g_mpeg, g_mpeg_data, mpeg_size, &g_ring,
                      FRAME_WIDTH, 0, 0) < 0) {
        g_last_error = -13;
        g_state = VIDEO_ERROR;
        return -1;
    }

    SceInt32 offset = 0;
    sceMpegQueryStreamOffset(&g_mpeg, g_header, &offset);

    g_vstream = sceMpegRegistStream(&g_mpeg, 0, 0);   /* 0 = 映像 */
    g_es_buf = sceMpegMallocAvcEsBuf(&g_mpeg);
    if (!g_vstream || !g_es_buf ||
        sceMpegInitAu(&g_mpeg, g_es_buf, &g_au) < 0) {
        g_last_error = -14;
        g_state = VIDEO_ERROR;
        return -1;
    }

    SceMpegAvcMode mode;
    mode.iUnk0 = -1;
    mode.iPixelFormat = SCE_MPEG_AVC_FORMAT_8888;      /* 画面と同じ 32bit */
    sceMpegAvcDecodeMode(&g_mpeg, &mode);

    g_mpeg_ready = 1;
    return 1;
}

static void mpeg_teardown(void)
{
    if (g_es_buf) { sceMpegFreeAvcEsBuf(&g_mpeg, g_es_buf); g_es_buf = NULL; }
    if (g_mpeg_data) { sceMpegDelete(&g_mpeg); free(g_mpeg_data); g_mpeg_data = NULL; }
    if (g_ring_data) { sceMpegRingbufferDestruct(&g_ring); free(g_ring_data); g_ring_data = NULL; }
    if (g_mpeg_ready)
        sceMpegFinish();
    g_mpeg_ready = 0;
    g_vstream = NULL;
}

/* --- 公開関数 ----------------------------------------------------------- */

int video_start(const char *video_id, int seconds)
{
    video_stop();

    snprintf(g_video_id, sizeof(g_video_id), "%s", video_id);
    g_seconds = seconds;
    g_rx_head = g_rx_tail = 0;
    g_rx_eof = 0;
    g_frames = 0;
    g_last_error = 0;
    g_cmd_stop = 0;
    g_init_flag = 0;
    g_au_retry = 0;
    g_state = VIDEO_BUFFERING;

    g_thread = sceKernelCreateThread("video_rx", recv_thread, 0x14, 0x8000, 0, 0);
    if (g_thread < 0) {
        g_state = VIDEO_ERROR;
        return g_thread;
    }
    return sceKernelStartThread(g_thread, 0, 0);
}

void video_stop(void)
{
    if (g_thread >= 0) {
        g_cmd_stop = 1;
        int spins = 0;
        while (g_thread_running && ++spins < 100)   /* 最大1秒 */
            sceKernelDelayThread(10 * 1000);
        SceUInt timeout = 200 * 1000;
        sceKernelWaitThreadEnd(g_thread, &timeout);
        sceKernelDeleteThread(g_thread);
        g_thread = -1;
    }
    mpeg_teardown();
    g_state = VIDEO_STOPPED;
}

VideoState video_state(void) { return g_state; }
int video_last_error(void)   { return g_last_error; }
int video_frames(void)       { return g_frames; }

int video_decode(void *draw_buf)
{
    if (g_state == VIDEO_STOPPED || g_state == VIDEO_ERROR ||
        g_state == VIDEO_FINISHED)
        return -1;

    if (!g_mpeg_ready) {
        /* ヘッダ + しばらく分が溜まるまで待つ。
           足りないうちに始めると出だしで途切れる */
        if (rx_available() < PSMF_HEADER_SIZE + PACK_SIZE * 32 && !g_rx_eof)
            return 0;
        int rc = mpeg_setup();
        if (rc <= 0)
            return rc < 0 ? -1 : 0;
    }

    g_display_buf = draw_buf;

    int avail = sceMpegRingbufferAvailableSize(&g_ring);
    if (avail > 0)
        sceMpegRingbufferPut(&g_ring, avail, avail);

    SceInt32 unk = 0;
    int rc = sceMpegGetAvcAu(&g_mpeg, g_vstream, &g_au, &unk);
    if (rc < 0) {
        /* 「まだ供給が足りない」と「もう終わり」は戻り値では区別できない。
           受信が終わっていて、かつ何度取っても取れないなら終端とみなす */
        if (++g_au_retry > (g_rx_eof ? 10 : 400)) {
            g_state = VIDEO_FINISHED;
            return -1;
        }
        return 0;
    }
    g_au_retry = 0;

    /* 第 4 引数は描画先そのものではなく「描画先のアドレスを入れた変数」への
       ポインタ。直接渡すと戻り値 0 のまま画面が真っ黒になる */
    rc = sceMpegAvcDecode(&g_mpeg, &g_au, FRAME_WIDTH, &g_display_buf,
                          &g_init_flag);
    if (rc < 0) {
        g_last_error = rc;
        g_state = VIDEO_ERROR;
        return -1;
    }

    g_frames++;
    g_state = VIDEO_PLAYING;
    return 1;
}
