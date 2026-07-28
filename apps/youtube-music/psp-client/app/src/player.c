/*
 * 再生スレッド: HTTP ストリーム受信 → sceMp3 (Media Engine) → sceAudioSRC。
 * UI スレッドとは volatile なフラグでやり取りする。
 * 構成は poc2-stream で PPSSPP 検証済みのものをスレッド化した形。
 *
 * ダウンロード済みの曲 (offline/<id>.mp3) はサーバーではなく
 * ローカルファイルから読む。供給元が違うだけでデコード経路は同一。
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspaudio.h>
#include <psputility.h>
#include <pspmp3.h>
#include <string.h>
#include <stdio.h>
#include "player.h"
#include "net.h"
#include "store.h"
#include "common.h"

/* 音声チャンネルが既に確保済み。pspsdk のヘッダに定義が無いので自前で持つ */
#define SCE_AUDIO_ERROR_CH_BUSY  ((int)0x80268002)

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
static char g_file_path[128];              /* ローカル再生時のパス ("" = 配信) */
static volatile int g_duration_hint = 0;   /* 曲の長さ (秒)。0 = 不明 */
static volatile int g_start_sec = 0;       /* シークで始めた位置 (秒) */
static volatile int g_gate = 0;            /* 1 = 受信は続けるが音は出さない */
static SceUID g_thread = -1;
static volatile int g_thread_running = 0;

static int decode_thread(SceSize args, void *argp);
static int decode_body(SceSize args, void *argp);

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
        /*
         * スレッドの自然終了を待つ。
         * 待ちきらないまま削除すると、そのスレッドが音声チャンネルと
         * sceMp3 ハンドルを掴んだままになり、次の再生が
         * 「チャンネル使用中」(0x80268002) で失敗する。
         * 受信待ちは中断できるようにしてあるので、通常は即座に終わる。
         * ここは描画スレッドから呼ばれるため、長く待つと画面が固まって見える。
         * 待ちきれなかった場合は新しい再生側でチャンネルを取り直すので、
         * 上限は短くしてよい。
         */
        int spins = 0;
        while (g_thread_running && ++spins < 100)   /* 最大1秒 */
            sceKernelDelayThread(10 * 1000);
        SceUInt timeout = 200 * 1000;
        sceKernelWaitThreadEnd(g_thread, &timeout);
        if (g_thread_running) {
            /* 最後の手段。強制終了させたうえで資源を明示的に解放する */
            sceKernelTerminateThread(g_thread);
            sceAudioSRCChRelease();
            g_thread_running = 0;
        }
        sceKernelDeleteThread(g_thread);
        g_thread = -1;
    }
    g_state = PLAYER_STOPPED;
}

void player_gate(int hold) { g_gate = hold ? 1 : 0; }

int player_start(const char *video_id, int duration_hint_sec, int start_sec)
{
    player_stop();

    snprintf(g_video_id, sizeof(g_video_id), "%s", video_id);

    /* ダウンロード済みならローカルから再生する (通信量ゼロ・即時開始)。
       存在確認は fopen で行う (sceIoOpen は相対パスを解決しない) */
    g_file_path[0] = '\0';
    {
        char path[128];
        store_mp3_path(video_id, path, sizeof(path));
        FILE *probe = fopen(path, "rb");
        if (probe) {
            fclose(probe);
            snprintf(g_file_path, sizeof(g_file_path), "%s", path);
        }
    }

    g_duration_hint = duration_hint_sec;
    g_start_sec = start_sec > 0 ? start_sec : 0;
    /*
     * 音を止める指示は前の再生に紐づくもの。持ち越すと、
     * 動画を一度見たあと別の曲が無音のままになる。必ず開けてから始める
     * (映像が要るなら video_sync が改めて閉じ直す)。
     */
    g_gate = 0;
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
    /* 1152 サンプル/フレーム, 44100Hz。シークした場合はその位置を足す */
    return g_start_sec + (int)((long long)g_frames * 1152 / 44100);
}

int player_elapsed_ms(void)
{
    return g_start_sec * 1000 + (int)((long long)g_frames * 1152 * 1000 / 44100);
}

static int fail(int sock, FILE *fp, int handle, int src_reserved, int code)
{
    if (src_reserved)
        sceAudioSRCChRelease();
    if (handle >= 0)
        sceMp3ReleaseMp3Handle(handle);
    net_close(sock);
    if (fp)
        fclose(fp);
    /*
     * 停止要求で終わった場合は異常ではない。
     * 曲の切り替えやシークでは、前の再生スレッドが受信待ちを打ち切って
     * ここへ来る。これをエラー扱いにすると、新しい再生が始まる直前に
     * 一瞬「エラー」が出てしまう (実際に見えていた)。
     */
    if (!g_cmd_stop) {
        g_last_error = code;
        g_state = PLAYER_ERROR;
    }
    return -1;
}

/*
 * スレッドが本当に終わったか (資源を手放したか) を示す旗。
 * sceKernelWaitThreadEnd はエミュレータで期待通りに待ってくれないことがあり、
 * 終わりきる前に次の再生を始めると音声チャンネルを取り合って失敗する。
 * 実処理を decode_body に分け、戻ってきた時点で必ず 0 にする。
 */
static int decode_thread(SceSize args, void *argp)
{
    g_thread_running = 1;
    int rc = decode_body(args, argp);
    g_thread_running = 0;
    return rc;
}

static int decode_body(SceSize args, void *argp)
{
    int sock = -1;
    FILE *fp = NULL;

    if (g_file_path[0]) {
        fp = fopen(g_file_path, "rb");
        if (!fp) {
            g_last_error = -1;
            g_state = PLAYER_ERROR;
            return 0;
        }
    } else {
        char base_path[96], path[224];
        snprintf(base_path, sizeof(base_path), "/stream?yt=%s&t=%d",
                 g_video_id, g_start_sec);
        if (net_build_path(path, sizeof(path), base_path) < 0) {
            g_last_error = -1;
            g_state = PLAYER_ERROR;
            return 0;
        }
        sock = http_open_stream(net_server_host(), net_server_port(), path);
        if (sock < 0) {
            g_last_error = sock;
            g_state = PLAYER_ERROR;
            return 0;
        }
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
        return fail(sock, fp, -1, 0, handle);

    int inited = 0;
    int src_reserved = 0;
    int eos = 0;
    int stall = 0;
    unsigned int total_rx = 0;
    unsigned int last_rx = 0;   /* 直前に前進を確認した受信量 (供給停止の判定用) */

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
                return fail(sock, fp, handle, src_reserved, rc);
            if (towrite > 0) {
                int got = fp ? (int)fread(dst, 1, (size_t)towrite, fp)
                             : net_recv_wait_abortable(sock, dst, towrite,
                                                       &g_cmd_stop);
                if (got < 0)
                    return fail(sock, fp, handle, src_reserved, got);
                if (got == 0) {
                    eos = 1;
                } else {
                    total_rx += got;
                    rc = sceMp3NotifyAddStreamData(handle, got);
                    if (rc < 0)
                        return fail(sock, fp, handle, src_reserved, rc);
                }
            }
        }

        if (!inited) {
            if (total_rx < 16 * 1024 && !eos)
                continue;
            if (total_rx == 0) {
                /* 接続はできたが中身が空。その曲をサーバーが取得できなかった
                   ということなので、デコーダのエラー番号ではなく理由を返す */
                return fail(sock, fp, handle, src_reserved, PLAYER_ERR_NO_DATA);
            }
            int rc = sceMp3Init(handle);
            if (rc < 0)
                return fail(sock, fp, handle, src_reserved, rc);
            inited = 1;
            g_state = PLAYER_PLAYING;
        }

        if (g_gate) {
            /* 映像の用意待ち。受信は続けているので、開いた瞬間に鳴り始められる */
            sceKernelDelayThread(10 * 1000);
            continue;
        }

        short *out = NULL;
        int bytes = sceMp3Decode(handle, &out);
        if (bytes <= 0) {
            if (eos)
                break; /* データを出し切った */
            /*
             * 負値の大半は「まだデータが足りない」を意味するので待つ。
             * ただし真のデコードエラーで無限ループにならないよう上限を設ける。
             *
             * この上限は当初 2 秒だったが短すぎた: サーバー側は 1 曲ごとに
             * yt-dlp を起動して変換するため、配信開始や曲の切り替わりで
             * 数秒データが来ないことがある。さらに一括ダウンロードと
             * 同時に再生すると回線を分け合うため間隔が伸びる。
             * (Raspberry Pi 4 のサーバーで実際に 0x807F00FD で落ちた)
             * 受信が続いている限りは待ち続け、本当に供給が止まった場合だけ
             * 諦めるよう、受信バイト数が増えていれば待ち時間をリセットする。
             */
            if (total_rx != last_rx) {
                last_rx = total_rx;   /* まだ届いている = 前に進める見込みあり */
                stall = 0;
            } else if (++stall > 4000) { /* 約20秒、無音のまま何も届かない */
                return fail(sock, fp, handle, src_reserved, bytes);
            }
            sceKernelDelayThread(5 * 1000);
            continue;
        }
        stall = 0;
        last_rx = total_rx;

        /*
         * 終端検知。
         * PPSSPP の sceMp3Decode はストリームのデータが尽きた後も
         * 正常値を返し続けることがあり (バッファを繰り返しデコードする)、
         * 「4:47 の曲が 10:45 まで再生中」という状態になる。
         * 受信済みバイト数から期待フレーム数を計算し、
         * それを超えたら曲が終わったとみなす。
         * (128kbps CBR: 1 フレーム = 417.96 バイト)
         */
        if (eos) {
            int expected = (int)(total_rx / 417u) + 4;
            if (g_frames >= expected)
                break;
            if (g_duration_hint > 0 &&
                player_elapsed_sec() >= g_duration_hint + 2)
                break;
        }

        int nsamples = bytes / 4;
        if (!src_reserved) {
            int hz = sceMp3GetSamplingRate(handle);
            int rc = sceAudioSRCChReserve(nsamples, hz, 2);
            if (rc == SCE_AUDIO_ERROR_CH_BUSY) {
                /*
                 * 曲を切り替えた直後、前の再生スレッドが音声チャンネルを
                 * 手放しきれていないことがある。掴んだままだと新しい再生が
                 * 始まらないので、一度解放してから取り直す。
                 */
                sceAudioSRCChRelease();
                sceKernelDelayThread(50 * 1000);
                rc = sceAudioSRCChReserve(nsamples, hz, 2);
            }
            if (rc < 0)
                return fail(sock, fp, handle, src_reserved, rc);
            src_reserved = 1;
        }
        sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, out);
        g_frames = g_frames + 1;
    }

    if (src_reserved)
        sceAudioSRCChRelease();
    sceMp3ReleaseMp3Handle(handle);
    net_close(sock);
    if (fp)
        fclose(fp);
    g_state = g_cmd_stop ? PLAYER_STOPPED : PLAYER_FINISHED;
    return 0;
}
