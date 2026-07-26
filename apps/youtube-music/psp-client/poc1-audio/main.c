/*
 * PoC 1: sceAudio で純音（440Hz サイン波）を鳴らす最小検証。
 * 目的: ツールチェーン → EBOOT.PBP → PPSSPP/実機 の経路と、
 *       PCM 出力 API が使えることの確認。
 */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspaudio.h>
#include <pspctrl.h>
#include <math.h>
#include <string.h>

PSP_MODULE_INFO("poc1_audio", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

#define SAMPLE_RATE   44100
#define SAMPLES_PER_BUF 1024  /* sceAudio は 64 の倍数 */

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

int main(void)
{
    pspDebugScreenInit();
    setup_callbacks();

    pspDebugScreenPrintf("PoC1: sceAudio sine tone\n");

    int ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, SAMPLES_PER_BUF,
                               PSP_AUDIO_FORMAT_STEREO);
    if (ch < 0) {
        pspDebugScreenPrintf("sceAudioChReserve failed: 0x%08X\n", ch);
        sceKernelSleepThread();
    }
    pspDebugScreenPrintf("channel=%d  440Hz  START/HOME de shuuryou\n", ch);

    static short buf[2][SAMPLES_PER_BUF * 2]; /* ダブルバッファ, LR interleave */
    float phase = 0.0f;
    const float step = 2.0f * (float)M_PI * 440.0f / (float)SAMPLE_RATE;
    int flip = 0;

    SceCtrlData pad;
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

    while (g_running) {
        short *p = buf[flip];
        int i;
        for (i = 0; i < SAMPLES_PER_BUF; i++) {
            short s = (short)(sinf(phase) * 12000.0f);
            p[i * 2] = s;
            p[i * 2 + 1] = s;
            phase += step;
            if (phase > 2.0f * (float)M_PI)
                phase -= 2.0f * (float)M_PI;
        }
        /* ブロッキング出力: 前バッファの再生完了を待つ */
        sceAudioOutputBlocking(ch, PSP_AUDIO_VOLUME_MAX, p);
        flip ^= 1;

        sceCtrlPeekBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_START)
            break;
    }

    sceAudioChRelease(ch);
    sceKernelExitGame();
    return 0;
}
