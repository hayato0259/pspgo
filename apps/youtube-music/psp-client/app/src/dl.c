/*
 * オフライン用ダウンロードスレッド。
 *
 * サーバーの /stream?yt= は再生時と同じ変換済み MP3 を返すので、
 * それを offline/<id>.part に書き溜め、完走したら .mp3 へ改名する
 * (途中で切れたファイルを索引に載せないため)。
 * アートワークは 64x64 の生ピクセルをそのまま .art に保存し、
 * art.c がネットワークより先にこのファイルを見る。
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <string.h>
#include <stdio.h>
#include "dl.h"
#include "net.h"
#include "store.h"
#include "art.h"

#define DL_QUEUE 128
#define RXBUF (16 * 1024)

static ApiTrack g_queue[DL_QUEUE];
static volatile int g_qhead = 0, g_qtail = 0;

static SceUID g_thread = -1;
static SceUID g_mutex = -1;
static volatile int g_quit = 0;

static char g_current[128] = "";
static char g_current_id[24] = "";
static volatile int g_done = 0, g_failed = 0;

static unsigned char g_rx[RXBUF];
static unsigned char g_art[ART_SIDE * ART_SIDE * 4 + 64];

static void lock(void)   { if (g_mutex >= 0) sceKernelWaitSema(g_mutex, 1, NULL); }
static void unlock(void) { if (g_mutex >= 0) sceKernelSignalSema(g_mutex, 1); }

static void save_art(const char *id)
{
    char path[128];
    store_art_path(id, path, sizeof(path));

    /* 既にあれば取り直さない */
    FILE *probe = fopen(path, "rb");
    if (probe) {
        fclose(probe);
        return;
    }

    char req[128];
    snprintf(req, sizeof(req), "/art?id=%s&s=%d", id, ART_SIDE);
    int got = http_get_bin(net_server_host(), net_server_port(), req,
                           g_art, sizeof(g_art));
    if (got != ART_SIDE * ART_SIDE * 4)
        return;   /* アートワークは無くても再生には支障がない */

    FILE *fp = fopen(path, "wb");
    if (!fp)
        return;
    fwrite(g_art, 1, (size_t)got, fp);
    fclose(fp);
}

/* 戻り値: 保存したバイト数 / <=0 失敗 */
static int save_mp3(const ApiTrack *t, char *final_path, int final_size)
{
    char part[128], req[128];
    store_mp3_path(t->video_id, final_path, final_size);
    snprintf(part, sizeof(part), "offline/%s.part", t->video_id);
    snprintf(req, sizeof(req), "/stream?yt=%s", t->video_id);

    int sock = http_open_stream(net_server_host(), net_server_port(), req);
    if (sock < 0)
        return sock;

    FILE *fp = fopen(part, "wb");
    if (!fp) {
        net_close(sock);
        return -1;
    }

    int total = 0, ok = 1;
    while (!g_quit) {
        int got = net_recv_wait(sock, g_rx, RXBUF);
        if (got < 0) {
            ok = 0;
            break;
        }
        if (got == 0)
            break;   /* サーバーが閉じた = 曲の終端 */
        if (fwrite(g_rx, 1, (size_t)got, fp) != (size_t)got) {
            ok = 0;   /* 空き容量切れなど */
            break;
        }
        total += got;
    }
    fclose(fp);
    net_close(sock);

    /* 1 分の曲でも約 1MB になる。極端に小さいものはエラー本文とみなす */
    /* ※ sceIoRename/sceIoRemove は相対パスを解決しない (PPSSPP で実測)。
       fopen と同じ経路で cwd を解決する stdio の rename/remove を使う */
    if (!ok || g_quit || total < 64 * 1024) {
        remove(part);
        return -2;
    }
    remove(final_path);   /* 残骸があれば上書きのため消す */
    if (rename(part, final_path) != 0) {
        remove(part);
        return -3;
    }
    return total;
}

static int dl_thread(SceSize args, void *argp)
{
    while (!g_quit) {
        ApiTrack t;
        int has = 0;

        lock();
        if (g_qhead != g_qtail) {
            t = g_queue[g_qhead];
            g_qhead = (g_qhead + 1) % DL_QUEUE;
            snprintf(g_current, sizeof(g_current), "%s", t.title);
            snprintf(g_current_id, sizeof(g_current_id), "%s", t.video_id);
            has = 1;
        } else {
            g_current[0] = '\0';
            g_current_id[0] = '\0';
        }
        unlock();

        if (!has) {
            sceKernelDelayThread(100 * 1000);
            continue;
        }

        if (store_has(t.video_id))
            continue;   /* 積んだ後に別経路で保存済みになった */

        save_art(t.video_id);

        char mp3[128];
        int bytes = save_mp3(&t, mp3, sizeof(mp3));
        if (bytes <= 0) {
            g_failed = g_failed + 1;
            continue;
        }

        /* 長さ不明の曲はファイルサイズから概算する (128kbps = 16000 B/秒) */
        if (t.duration_sec <= 0)
            t.duration_sec = bytes / 16000;

        if (store_add(&t) == 0)
            g_done = g_done + 1;
        else
            g_failed = g_failed + 1;
    }
    return 0;
}

int dl_init(void)
{
    g_mutex = sceKernelCreateSema("dl_sema", 0, 1, 1, NULL);
    if (g_mutex < 0)
        return g_mutex;
    g_quit = 0;
    g_thread = sceKernelCreateThread("dl", dl_thread, 0x1B, 0x8000, 0, 0);
    if (g_thread < 0)
        return g_thread;
    return sceKernelStartThread(g_thread, 0, 0);
}

void dl_shutdown(void)
{
    if (g_thread >= 0) {
        g_quit = 1;
        SceUInt timeout = 15 * 1000 * 1000;
        sceKernelWaitThreadEnd(g_thread, &timeout);
        sceKernelDeleteThread(g_thread);
        g_thread = -1;
    }
    if (g_mutex >= 0) {
        sceKernelDeleteSema(g_mutex);
        g_mutex = -1;
    }
}

int dl_enqueue(const ApiTrack *t)
{
    if (!t->video_id[0])
        return 1;
    if (store_has(t->video_id))
        return 1;

    int rc;
    lock();
    int dup = (strcmp(g_current_id, t->video_id) == 0);
    for (int i = g_qhead; i != g_qtail && !dup; i = (i + 1) % DL_QUEUE)
        if (strcmp(g_queue[i].video_id, t->video_id) == 0)
            dup = 1;
    if (dup) {
        rc = 1;
    } else {
        int next = (g_qtail + 1) % DL_QUEUE;
        if (next == g_qhead) {
            rc = -1;   /* 満杯 */
        } else {
            g_queue[g_qtail] = *t;
            g_qtail = next;
            rc = 0;
        }
    }
    unlock();
    return rc;
}

int dl_pending(void)
{
    lock();
    int n = (g_qtail - g_qhead + DL_QUEUE) % DL_QUEUE;
    if (g_current_id[0])
        n++;
    unlock();
    return n;
}

const char *dl_current_title(void) { return g_current; }
int dl_done(void)   { return g_done; }
int dl_failed(void) { return g_failed; }
