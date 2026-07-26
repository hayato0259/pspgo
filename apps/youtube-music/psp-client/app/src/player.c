/*
 * 再生スレッド: HTTP ストリーム受信 → sceMp3 (Media Engine) → sceAudioSRC。
 * UI スレッドとは volatile なフラグでやり取りする。
 * 構成は poc2-stream で PPSSPP 検証済みのものをスレッド化した形。
 */
#include <pspkernel.h>
#include <pspaudio.h>
#include <psputility.h>
#include <pspmp3.h>
#include <string.h>
#include <stdio.h>
#include "player.h"
#include "net.h"
#include "common.h"

#define MP3_BUF_SIZE  (128 * 1024)
#define PCM_BUF_SIZE  (1152 * 2 * 2 * 4)

static unsigned char g_mp3_buf[MP3_BUF_SIZE] __attribute__((aligned(64)));
static unsigned char g_pcm_buf[PCM_BUF_SIZE] __attribute__((aligned(64)));

static volatile PlayerState g_state = PLAYER_STOPPED;
static volatile int g_cmd_stop = 0;
static volatile int g_paused = 0;
static volatile int g_frames = 0;
static volatile int g_last_error = 0;

static char g_video_id[24];
static SceUID g_thread = -1;

static int decode_thread(SceSize args, void *argp);

int player_global_init(void)
{
    int rc = sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
    if (rc < 0) return rc;
    rc = sceUtilityLoadModule(PSP_MODULE_AV_MP3);
    if (rc < 0) return rc;
    return sceMp3InitResource();
}

void player_stop(void)
{
    if (g_thread >= 0) {
        g_cmd_stop = 1;
        /* スレッドの自然終了を待つ (最大5秒) */
        SceUInt timeout = 5 * 1000 * 1000;
        sceKernelWaitThreadEnd(g_thread, &timeout);
        sceKernelDeleteThread(g_thread);
        g_thread = -1;
    }
    g_state = PLAYER_STOPPED;
}

int player_start(const char *video_id)
{
    player_stop();

    snprintf(g_video_id, sizeof(g_video_id), "%s", video_id);
    g_cmd_stop = 0;
    g_paused = 0;
    g_frames = 0;
    g_last_error = 0;
    g_state = PLAYER_BUFFERING;

    g_thread = sceKernelCreateThread("player", decode_thread, 0x12, 0x8000, 0, 0);
    if (g_thread < 0) {
        g_state = PLAYER_ERROR;
        return g_thread;
    }
    return sceKernelStartThread(g_thread, 0, 0);
}

void player_toggle_pause(void)
{
    if (g_state == PLAYER_PLAYING || g_state == PLAYER_PAUSED) {
        g_paused = !g_paused;
        g_state = g_paused ? PLAYER_PAUSED : PLAYER_PLAYING;
    }
}

PlayerState player_state(void) { return g_state; }
int player_last_error(void) { return g_last_error; }

int player_elapsed_sec(void)
{
    /* 1152 サンプル/フレーム, 44100Hz */
    return (int)((long long)g_frames * 1152 / 44100);
}

static int fail(int sock, int handle, int src_reserved, int code)
{
    if (src_reserved)
        sceAudioSRCChRelease();
    if (handle >= 0)
        sceMp3ReleaseMp3Handle(handle);
    net_close(sock);
    g_last_error = code;
    g_state = PLAYER_ERROR;
    return -1;
}

static int decode_thread(SceSize args, void *argp)
{
    char path[96];
    snprintf(path, sizeof(path), "/stream?yt=%s", g_video_id);

    int sock = http_open_stream(SERVER_HOST, SERVER_PORT, path);
    if (sock < 0) {
        g_last_error = sock;
        g_state = PLAYER_ERROR;
        return 0;
    }

    SceMp3InitArg init;
    memset(&init, 0, sizeof(init));
    init.mp3StreamStart = 0;
    init.mp3StreamEnd = 0x7FFFFFFF;
    init.mp3Buf = g_mp3_buf;
    init.mp3BufSize = MP3_BUF_SIZE;
    init.pcmBuf = g_pcm_buf;
    init.pcmBufSize = PCM_BUF_SIZE;

    int handle = sceMp3ReserveMp3Handle(&init);
    if (handle < 0)
        return fail(sock, -1, 0, handle);

    int inited = 0;
    int src_reserved = 0;
    int eos = 0;
    int stall = 0;
    unsigned int total_rx = 0;

    while (!g_cmd_stop) {
        if (g_paused) {
            sceKernelDelayThread(50 * 1000);
            continue;
        }

        if (!eos && sceMp3CheckStreamDataNeeded(handle) > 0) {
            SceUChar8 *dst;
            SceInt32 towrite, srcpos;
            int rc = sceMp3GetInfoToAddStreamData(handle, &dst, &towrite, &srcpos);
            if (rc < 0)
                return fail(sock, handle, src_reserved, rc);
            if (towrite > 0) {
                int got = net_recv_wait(sock, dst, towrite);
                if (got < 0)
                    return fail(sock, handle, src_reserved, got);
                if (got == 0) {
                    eos = 1;
                } else {
                    total_rx += got;
                    rc = sceMp3NotifyAddStreamData(handle, got);
                    if (rc < 0)
                        return fail(sock, handle, src_reserved, rc);
                }
            }
        }

        if (!inited) {
            if (total_rx < 16 * 1024 && !eos)
                continue;
            int rc = sceMp3Init(handle);
            if (rc < 0)
                return fail(sock, handle, src_reserved, rc);
            inited = 1;
            g_state = PLAYER_PLAYING;
        }

        short *out = NULL;
        int bytes = sceMp3Decode(handle, &out);
        if (bytes <= 0) {
            if (eos)
                break; /* データを出し切った */
            /*
             * 負値の大半は「まだデータが足りない」を意味するので待つ。
             * ただし真のデコードエラーで無限ループにならないよう上限を設ける
             * (16KB/s 以上で届いていれば数十回で必ず前に進む)。
             */
            if (++stall > 400) /* 約2秒 */
                return fail(sock, handle, src_reserved, bytes);
            sceKernelDelayThread(5 * 1000);
            continue;
        }
        stall = 0;

        int nsamples = bytes / 4;
        if (!src_reserved) {
            int hz = sceMp3GetSamplingRate(handle);
            int rc = sceAudioSRCChReserve(nsamples, hz, 2);
            if (rc < 0)
                return fail(sock, handle, src_reserved, rc);
            src_reserved = 1;
        }
        sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, out);
        g_frames = g_frames + 1;
    }

    if (src_reserved)
        sceAudioSRCChRelease();
    sceMp3ReleaseMp3Handle(handle);
    net_close(sock);
    g_state = g_cmd_stop ? PLAYER_STOPPED : PLAYER_FINISHED;
    return 0;
}
