/*
 * XMB 風の UI 操作音。
 *
 * 起動時に短い PCM をソフトウェア合成し、専用の音声チャンネルで鳴らす。
 * 音楽再生は sceAudioSRC チャンネル、こちらは通常チャンネルなので
 * ハードウェアミキサーが自然に重ねてくれる。
 * 出力はブロッキングだが専用スレッドで行うため UI は止まらない。
 */
#include <pspkernel.h>
#include <pspaudio.h>
#include <math.h>
#include "snd.h"

#define BLK 1024                       /* チャンネルのブロックサイズ (サンプル) */
#define VOL 0x2400                     /* 音楽より控えめに */

static short g_move[BLK];
static short g_ok[BLK * 2];
static short g_cancel[BLK];

static volatile int g_pending = -1;
static volatile int g_quit = 0;
static SceUID g_thread = -1;
static int g_channel = -1;

/* f0 -> f1 へ滑らかに変わる減衰音を合成する */
static void synth(short *buf, int n, float f0, float f1, float amp, float decay)
{
    float phase = 0.0f;
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)n;
        float f = f0 + (f1 - f0) * t;
        phase += 2.0f * (float)M_PI * f / 44100.0f;
        float env = expf(-decay * t) * (1.0f - t * 0.15f);
        /* ごく短いアタックでクリックノイズを避ける */
        if (i < 64)
            env *= (float)i / 64.0f;
        buf[i] = (short)(amp * env * sinf(phase));
    }
}

static int snd_thread(SceSize args, void *argp)
{
    while (!g_quit) {
        int e = g_pending;
        if (e < 0) {
            sceKernelDelayThread(15 * 1000);
            continue;
        }
        g_pending = -1;
        switch (e) {
        case SND_MOVE:
            sceAudioOutputBlocking(g_channel, VOL, g_move);
            break;
        case SND_OK:
            sceAudioOutputBlocking(g_channel, VOL, g_ok);
            sceAudioOutputBlocking(g_channel, VOL, g_ok + BLK);
            break;
        case SND_CANCEL:
            sceAudioOutputBlocking(g_channel, VOL, g_cancel);
            break;
        }
    }
    return 0;
}

int snd_init(void)
{
    /* XMB のティックを意識した音作り */
    synth(g_move, BLK, 2093.0f, 2093.0f, 14000.0f, 9.0f);        /* C7 の短いティック */
    synth(g_ok, BLK, 1046.5f, 1046.5f, 12000.0f, 3.0f);          /* C6 → */
    synth(g_ok + BLK, BLK, 1568.0f, 1568.0f, 12000.0f, 5.0f);    /* G6 の上昇二音 */
    synth(g_cancel, BLK, 830.6f, 620.0f, 12000.0f, 6.0f);        /* 下降する戻り音 */

    g_channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, BLK,
                                  PSP_AUDIO_FORMAT_MONO);
    if (g_channel < 0)
        return g_channel;

    g_quit = 0;
    g_thread = sceKernelCreateThread("uisnd", snd_thread, 0x16, 0x2000, 0, 0);
    if (g_thread < 0)
        return g_thread;
    return sceKernelStartThread(g_thread, 0, 0);
}

void snd_shutdown(void)
{
    if (g_thread >= 0) {
        g_quit = 1;
        SceUInt timeout = 2 * 1000 * 1000;
        sceKernelWaitThreadEnd(g_thread, &timeout);
        sceKernelDeleteThread(g_thread);
        g_thread = -1;
    }
    if (g_channel >= 0) {
        sceAudioChRelease(g_channel);
        g_channel = -1;
    }
}

void snd_play(SndEffect e)
{
    g_pending = (int)e;
}
