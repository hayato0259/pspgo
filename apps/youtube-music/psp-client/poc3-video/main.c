/*
 * PoC 3: PV (ミュージックビデオ) をアプリ内で再生できるかの検証。
 *
 * 答えを出したい問いは 3 つ:
 *   1. sceMpeg で H.264 Baseline をデコードして画面に出せるか
 *   2. 音声の sceMp3 と映像の sceMpeg を同時に使えるか
 *      (Media Engine は排他資源。ここが駄目ならアプリ内再生は成立しない)
 *   3. 480x272 で実用的なフレームレートが出るか
 *
 * 通信は一切しない。ローカルに置いた変換済みファイルを読むだけに絞っている
 * (通信と混ぜると、失敗したときにどちらが原因か切り分けられなくなるため)。
 *
 * 入力ファイル (EBOOT.PBP と同じフォルダに置く):
 *   test.pmf … 映像。tools/make_psmf.py で作る PSMF コンテナ
 *   test.mp3 … 音声。既存の配信と同じ MP3 CBR 128kbps / 44.1kHz / stereo
 *
 * 起動直後にボタンで動作モードを選ぶ。3 モードを比べると、
 * 失敗したときに「映像単体で駄目」なのか「同時が駄目」なのかが分かる。
 *
 * 画面表示は pspDebugScreen を使う。intraFont や GU を持ち込まないのは、
 * フォントファイルへの依存を無くして PoC を単体で動かせるようにするため。
 * 映像は sceMpegAvcDecode がフレームバッファへ直接書き、その上に文字を重ねる。
 */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <psputility.h>
#include <pspmp3.h>
#include <pspmpeg.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("poc3_video", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(20 * 1024);   /* sceMpeg の作業領域が大きいので広めに取る */

#define VIDEO_FILE     "test.pmf"
#define AUDIO_FILE     "test.mp3"

#define PSMF_HEADER_SIZE  2048      /* PSMF ヘッダの長さ */
#define PACK_SIZE         2048      /* MPEG プログラムストリームの 1 パックの長さ */
#define RING_PACKETS      128       /* リングバッファのパック数 */
#define FRAME_WIDTH       512       /* フレームバッファの 1 行の画素数 */

#define MP3_BUF_SIZE   (128 * 1024)
#define PCM_BUF_SIZE   (1152 * 2 * 2 * 4)

/* --- 動作モード --- */
enum {
    MODE_VIDEO_ONLY = 0,
    MODE_AUDIO_ONLY,
    MODE_BOTH
};
static int g_mode = MODE_BOTH;
static const char *mode_name(int m)
{
    if (m == MODE_VIDEO_ONLY) return "VIDEO ONLY";
    if (m == MODE_AUDIO_ONLY) return "AUDIO ONLY";
    return "VIDEO + AUDIO";
}

static volatile int g_running = 1;    /* 0 = アプリ終了 (HOME / START) */
static volatile int g_playing = 1;    /* 0 = 再生終了。音声スレッドの停止に使う */

/* ------------------------------------------------------------------
 * 手順の記録
 *
 * 「動いた / 動かない」だけでは次に触る人が同じ道を辿ることになる。
 * 呼んだ API と戻り値を順に残し、どこで止まったかを画面から読み取れるようにする。
 * ------------------------------------------------------------------ */
#define STEP_MAX 24
static struct {
    const char *name;
    int rc;
} g_step[STEP_MAX];
static int g_steps;

static int step(const char *name, int rc)
{
    if (g_steps < STEP_MAX) {
        g_step[g_steps].name = name;
        g_step[g_steps].rc = rc;
        g_steps++;
    }
    return rc;
}

/* ------------------------------------------------------------------
 * 終了コールバック (HOME ボタン)
 * ------------------------------------------------------------------ */
static int exit_callback(int arg1, int arg2, void *common)
{
    (void)arg1; (void)arg2; (void)common;
    g_running = 0;
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
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

/* ------------------------------------------------------------------
 * 音声: test.mp3 を sceMp3 でデコードして sceAudioSRC に流す。
 * 本体アプリの player.c と同じ経路。ファイルから読む点だけが違う。
 * ------------------------------------------------------------------ */
static unsigned char g_mp3_buf[MP3_BUF_SIZE] __attribute__((aligned(64)));
static unsigned char g_pcm_buf[PCM_BUF_SIZE] __attribute__((aligned(64)));

static volatile int g_audio_frames;
static volatile int g_audio_err;      /* 0 = 異常なし */
static volatile int g_audio_done;
static const char *volatile g_audio_stage = "not started";

static int audio_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;

    FILE *fp = fopen(AUDIO_FILE, "rb");
    if (!fp) {
        g_audio_stage = "fopen " AUDIO_FILE;
        g_audio_err = -1;
        g_audio_done = 1;
        return 0;
    }

    SceMp3InitArg init;
    memset(&init, 0, sizeof(init));
    init.mp3StreamStart = 0;
    init.mp3StreamEnd = 0x7FFFFFFF;   /* 終端が分からないストリームの作法 */
    init.mp3Buf = g_mp3_buf;
    init.mp3BufSize = MP3_BUF_SIZE;
    init.pcmBuf = g_pcm_buf;
    init.pcmBufSize = PCM_BUF_SIZE;

    g_audio_stage = "sceMp3ReserveMp3Handle";
    int handle = sceMp3ReserveMp3Handle(&init);
    step("sceMp3ReserveMp3Handle", handle);
    if (handle < 0) {
        g_audio_err = handle;
        g_audio_done = 1;
        fclose(fp);
        return 0;
    }

    int inited = 0, src_reserved = 0, eos = 0, stall = 0;
    unsigned int total_rx = 0;

    while (g_running && g_playing) {
        if (!eos && sceMp3CheckStreamDataNeeded(handle) > 0) {
            SceUChar8 *dst;
            SceInt32 towrite, srcpos;
            int rc = sceMp3GetInfoToAddStreamData(handle, &dst, &towrite, &srcpos);
            if (rc < 0) { g_audio_err = rc; g_audio_stage = "GetInfoToAddStreamData"; break; }
            if (towrite > 0) {
                int got = (int)fread(dst, 1, (size_t)towrite, fp);
                if (got <= 0) {
                    eos = 1;
                } else {
                    total_rx += got;
                    rc = sceMp3NotifyAddStreamData(handle, got);
                    if (rc < 0) { g_audio_err = rc; g_audio_stage = "NotifyAddStreamData"; break; }
                }
            }
        }

        if (!inited) {
            if (total_rx < 16 * 1024 && !eos)
                continue;
            g_audio_stage = "sceMp3Init";
            int rc = sceMp3Init(handle);
            step("sceMp3Init", rc);
            if (rc < 0) { g_audio_err = rc; break; }
            inited = 1;
            g_audio_stage = "playing";
        }

        short *out = NULL;
        int bytes = sceMp3Decode(handle, &out);
        if (bytes <= 0) {
            if (eos) break;
            /* 負値の大半は「データが足りない」の意味なので待つ。
               真のデコードエラーで無限に回らないよう上限は設ける */
            if (++stall > 2000) { g_audio_err = bytes; g_audio_stage = "sceMp3Decode"; break; }
            sceKernelDelayThread(5 * 1000);
            continue;
        }
        stall = 0;

        if (!src_reserved) {
            int hz = sceMp3GetSamplingRate(handle);
            int rc = sceAudioSRCChReserve(bytes / 4, hz, 2);
            step("sceAudioSRCChReserve", rc);
            if (rc < 0) { g_audio_err = rc; g_audio_stage = "sceAudioSRCChReserve"; break; }
            src_reserved = 1;
        }
        sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, out);
        g_audio_frames++;
    }

    if (src_reserved)
        sceAudioSRCChRelease();
    sceMp3ReleaseMp3Handle(handle);
    fclose(fp);
    if (!g_audio_err)
        g_audio_stage = "finished";
    g_audio_done = 1;
    return 0;
}

/* ------------------------------------------------------------------
 * 映像: PSMF を sceMpeg に食わせて 1 フレームずつデコードする
 * ------------------------------------------------------------------ */
static FILE *g_pmf;
static SceMpeg g_mpeg;
static SceMpegRingbuffer g_ring;
static SceMpegStream *g_vstream;
static SceMpegAu g_au;
static void *g_ring_data;
static void *g_mpeg_data;
static void *g_es_buf;
static unsigned char g_header[PSMF_HEADER_SIZE] __attribute__((aligned(64)));
/* 描画先のアドレスを保持する変数。sceMpegAvcDecode にはこの変数のアドレスを渡す */
static void *g_display_buf;

/*
 * リングバッファの補給。sceMpegRingbufferPut の内部から呼ばれる。
 * 要求されたパック数だけファイルから読んで、実際に読めたパック数を返す。
 */
static SceInt32 ringbuffer_cb(ScePVoid pData, SceInt32 iNumPackets, ScePVoid pParam)
{
    (void)pParam;
    if (!g_pmf)
        return 0;
    int want = iNumPackets * PACK_SIZE;
    int got = (int)fread(pData, 1, (size_t)want, g_pmf);
    if (got <= 0)
        return 0;
    return got / PACK_SIZE;
}

/* 0 = 成功。負値ならその時点で中断している (詳細は g_step に残る) */
static int video_open(void)
{
    g_pmf = fopen(VIDEO_FILE, "rb");
    if (step("fopen " VIDEO_FILE, g_pmf ? 0 : -1) < 0)
        return -1;

    if (fread(g_header, 1, PSMF_HEADER_SIZE, g_pmf) != PSMF_HEADER_SIZE)
        return step("read psmf header", -1);
    if (memcmp(g_header, "PSMF", 4) != 0)
        return step("psmf magic", -1);

    if (step("sceMpegInit", sceMpegInit()) < 0)
        return -1;

    int ring_size = step("sceMpegRingbufferQueryMemSize",
                         sceMpegRingbufferQueryMemSize(RING_PACKETS));
    if (ring_size < 0)
        return -1;
    g_ring_data = memalign(64, ring_size);
    if (step("alloc ringbuffer", g_ring_data ? 0 : -1) < 0)
        return -1;
    if (step("sceMpegRingbufferConstruct",
             sceMpegRingbufferConstruct(&g_ring, RING_PACKETS, g_ring_data,
                                        ring_size, ringbuffer_cb, NULL)) < 0)
        return -1;

    int mpeg_size = step("sceMpegQueryMemSize", sceMpegQueryMemSize(0));
    if (mpeg_size < 0)
        return -1;
    g_mpeg_data = memalign(64, mpeg_size);
    if (step("alloc mpeg", g_mpeg_data ? 0 : -1) < 0)
        return -1;
    if (step("sceMpegCreate",
             sceMpegCreate(&g_mpeg, g_mpeg_data, mpeg_size, &g_ring,
                           FRAME_WIDTH, 0, 0)) < 0)
        return -1;

    /* ヘッダから本体の開始位置と長さを読む。自作ヘッダが正しいかの確認も兼ねる */
    SceInt32 offset = 0, stream_size = 0;
    step("sceMpegQueryStreamOffset",
         sceMpegQueryStreamOffset(&g_mpeg, g_header, &offset));
    step("  -> offset", (int)offset);
    step("sceMpegQueryStreamSize",
         sceMpegQueryStreamSize(g_header, &stream_size));
    step("  -> stream size", (int)stream_size);

    if (offset <= 0)
        offset = PSMF_HEADER_SIZE;
    fseek(g_pmf, offset, SEEK_SET);

    g_vstream = sceMpegRegistStream(&g_mpeg, 0, 0);   /* 0 = 映像 */
    if (step("sceMpegRegistStream", g_vstream ? 0 : -1) < 0)
        return -1;

    g_es_buf = sceMpegMallocAvcEsBuf(&g_mpeg);
    if (step("sceMpegMallocAvcEsBuf", g_es_buf ? 0 : -1) < 0)
        return -1;
    if (step("sceMpegInitAu", sceMpegInitAu(&g_mpeg, g_es_buf, &g_au)) < 0)
        return -1;

    SceMpegAvcMode mode;
    mode.iUnk0 = -1;
    mode.iPixelFormat = SCE_MPEG_AVC_FORMAT_8888;   /* 画面と同じ 32bit RGBA */
    step("sceMpegAvcDecodeMode", sceMpegAvcDecodeMode(&g_mpeg, &mode));
    return 0;
}

static void video_close(void)
{
    if (g_es_buf) sceMpegFreeAvcEsBuf(&g_mpeg, g_es_buf);
    if (g_mpeg_data) { sceMpegDelete(&g_mpeg); free(g_mpeg_data); }
    if (g_ring_data) { sceMpegRingbufferDestruct(&g_ring); free(g_ring_data); }
    sceMpegFinish();
    if (g_pmf) fclose(g_pmf);
}

/* ------------------------------------------------------------------ */

static void print_steps(void)
{
    pspDebugScreenSetXY(0, 0);
    pspDebugScreenPrintf("PoC3 video  mode: %s\n", mode_name(g_mode));
    int i;
    for (i = 0; i < g_steps; i++)
        pspDebugScreenPrintf("%-28s %d (0x%08X)\n",
                             g_step[i].name, g_step[i].rc, (unsigned)g_step[i].rc);
}

/* 起動直後にモードを選ばせる */
static void select_mode(void)
{
    SceCtrlData pad;

#ifdef AUTOMODE
    /* PPSSPP での自動確認用。ボタン入力なしで指定モードに入る
       (エミュレータへの入力送信は本体の操作を奪うため使わない方針) */
    g_mode = AUTOMODE;
    return;
#endif

    pspDebugScreenSetXY(0, 0);
    pspDebugScreenPrintf("PoC3: PV playback check\n\n");
    pspDebugScreenPrintf("  CIRCLE   : video only\n");
    pspDebugScreenPrintf("  CROSS    : audio only\n");
    pspDebugScreenPrintf("  TRIANGLE : video + audio  (Media Engine kyouzon test)\n\n");
    pspDebugScreenPrintf("  START    : quit\n");

    while (g_running) {
        sceCtrlReadBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_CIRCLE)   { g_mode = MODE_VIDEO_ONLY; break; }
        if (pad.Buttons & PSP_CTRL_CROSS)    { g_mode = MODE_AUDIO_ONLY; break; }
        if (pad.Buttons & PSP_CTRL_TRIANGLE) { g_mode = MODE_BOTH; break; }
        if (pad.Buttons & PSP_CTRL_START)    { g_running = 0; return; }
        sceKernelDelayThread(20 * 1000);
    }
    /* ボタンを離すまで待つ (次のループで拾い直さないため) */
    do {
        sceCtrlReadBufferPositive(&pad, 1);
        sceKernelDelayThread(20 * 1000);
    } while (g_running && pad.Buttons);
}

int main(void)
{
    pspDebugScreenInit();
    setup_callbacks();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

    /* デコーダは両方とも avcodec モジュールに入っている */
    step("LoadModule AVCODEC", sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC));
    step("LoadModule MP3", sceUtilityLoadModule(PSP_MODULE_AV_MP3));
    step("sceMp3InitResource", sceMp3InitResource());

    select_mode();
    if (!g_running) {
        sceKernelExitGame();
        return 0;
    }
    pspDebugScreenClear();

    /* 描画先。sceMpegAvcDecode はここへ直接フレームを書く */
    void *topaddr = NULL;
    int bufwidth = 0, pixelformat = 0;
    sceDisplayGetFrameBuf(&topaddr, &bufwidth, &pixelformat, PSP_DISPLAY_SETBUF_IMMEDIATE);
    g_display_buf = topaddr;
    step("frame buffer", (int)(unsigned long)topaddr);

    SceUID audio_thid = -1;
    if (g_mode != MODE_VIDEO_ONLY) {
        audio_thid = sceKernelCreateThread("poc3_audio", audio_thread,
                                           0x12, 0x10000, 0, NULL);
        step("create audio thread", audio_thid);
        if (audio_thid >= 0)
            sceKernelStartThread(audio_thid, 0, NULL);
    }

    int video_ok = 0;
    if (g_mode != MODE_AUDIO_ONLY)
        video_ok = (video_open() == 0);

    int frames = 0;
    int decode_err = 0;   /* sceMpegAvcDecode が返した負値 */
    int au_err = 0;       /* sceMpegGetAvcAu が返した負値 (供給待ちでも出る) */
    int au_retry = 0;     /* 連続して Au を取れなかった回数 */
    SceInt32 init_flag = 0;
    unsigned int t0 = sceKernelGetSystemTimeLow();
    unsigned int t_last = t0;
    int frames_last = 0;
    float fps = 0.0f;

    SceCtrlData pad;
    while (g_running) {
        sceCtrlPeekBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_START)
            break;

        if (video_ok && !decode_err) {
            int avail = sceMpegRingbufferAvailableSize(&g_ring);
            if (avail > 0)
                sceMpegRingbufferPut(&g_ring, avail, avail);

            SceInt32 unk = 0;
            int rc = sceMpegGetAvcAu(&g_mpeg, g_vstream, &g_au, &unk);
            if (rc < 0) {
                /* 「まだ供給が足りない」と「もう終わり」を戻り値だけでは
                   区別できないので、連続して取れない状態が続いたら終端とみなす */
                au_err = rc;
                if (++au_retry < 60) {
                    sceKernelDelayThread(10 * 1000);
                    continue;
                }
                break;
            }
            au_retry = 0;
            /*
             * 第 4 引数は「描画先そのもの」ではなく
             * 「描画先のアドレスを入れた変数」へのポインタ。
             * ここを間違えるとデコードは 0 (成功) を返すのに画面は真っ黒のままになる。
             */
            rc = sceMpegAvcDecode(&g_mpeg, &g_au, FRAME_WIDTH, &g_display_buf, &init_flag);
            if (rc < 0) {
                step("sceMpegAvcDecode", rc);
                decode_err = rc;
                break;
            }
            frames++;
        } else {
            sceKernelDelayThread(16 * 1000);
        }

        /* 1 秒ごとに実測フレームレートを更新する。
           PPSSPP では画面を読み出せないため、判定は目視ではなく数値で行う */
        unsigned int now = sceKernelGetSystemTimeLow();
        if (now - t_last >= 1000 * 1000) {
            fps = (float)(frames - frames_last) *
                  1000000.0f / (float)(now - t_last);
            t_last = now;
            frames_last = frames;
        }

        pspDebugScreenSetXY(0, 0);
        pspDebugScreenPrintf("%-14s  frames %5d  fps %5.1f\n", mode_name(g_mode), frames, fps);
        pspDebugScreenPrintf("audio: %-22s frames %5d err 0x%08X\n",
                             g_audio_stage, g_audio_frames, (unsigned)g_audio_err);
        pspDebugScreenPrintf("video: au 0x%08X  dec 0x%08X  elapsed %ds\n",
                             (unsigned)au_err, (unsigned)decode_err,
                             (int)((now - t0) / 1000000));
    }

    /* 終了時の結果を残す。ここが検証レポートの元になる */
    unsigned int total_us = sceKernelGetSystemTimeLow() - t0;
    step("frames decoded", frames);
    step("elapsed ms", (int)(total_us / 1000));
    step("average fps x100", total_us ? (int)((float)frames * 100000000.0f / (float)total_us) : 0);
    step("audio frames", g_audio_frames);
    step("audio err", g_audio_err);

    g_playing = 0;
    if (audio_thid >= 0) {
        SceUInt timeout = 3 * 1000 * 1000;
        sceKernelWaitThreadEnd(audio_thid, &timeout);
        sceKernelDeleteThread(audio_thid);
    }
    if (video_ok)
        video_close();

    /*
     * 結果表示。
     * 成功したときは画面を消さない。最後にデコードしたフレームが
     * 残ったままになるので、数値と一緒に「実際に絵が出ているか」も確認できる。
     * 失敗したときは呼んだ API の一覧を出す (どこで止まったかを見るため)。
     */
    int failed = (g_mode != MODE_AUDIO_ONLY && (!video_ok || decode_err < 0));
    if (failed) {
        pspDebugScreenClear();
        print_steps();
    } else {
        float avg = total_us ? (float)frames * 1000000.0f / (float)total_us : 0.0f;
        pspDebugScreenSetXY(0, 0);
        pspDebugScreenPrintf("%-14s DONE                        \n", mode_name(g_mode));
        pspDebugScreenPrintf("video frames %5d in %5d ms  avg %5.1f fps\n",
                             frames, (int)(total_us / 1000), avg);
        pspDebugScreenPrintf("audio frames %5d  stage %-12s err 0x%08X\n",
                             g_audio_frames, g_audio_stage, (unsigned)g_audio_err);
        pspDebugScreenPrintf("last au 0x%08X  dec 0x%08X            \n",
                             (unsigned)au_err, (unsigned)decode_err);
    }
    pspDebugScreenPrintf("START de shuuryou\n");

    while (g_running) {
        sceCtrlReadBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_START)
            break;
        sceKernelDelayThread(50 * 1000);
    }

    sceKernelExitGame();
    return 0;
}
