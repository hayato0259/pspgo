/*
 * アートワーク (アルバム/プレイリスト画像) の取得と描画。
 *
 * PSP に画像デコーダを持たせず、サーバーが 64x64 の生ピクセル (RGBA) に
 * 変換したものを受け取ってテクスチャとして貼る。
 *
 * 取得はワーカースレッドで行い、UI スレッドは
 * 「あるなら描く、無ければプレースホルダ + 予約」だけをする。
 * これにより一覧をスクロールしても画面が止まらない。
 */
#include <pspkernel.h>
#include <pspgu.h>
#include <string.h>
#include <stdio.h>
#include "art.h"
#include "net.h"
#include "gfx.h"
#include "theme.h"
#include "common.h"

#define SLOTS 20          /* 同時に保持する枚数 (2段分 + 再生中) */
#define QUEUE 24          /* 読み込み待ち行列 (画面内のカード数より多く) */

enum { SLOT_EMPTY = 0, SLOT_LOADING, SLOT_READY, SLOT_MISSING };

typedef struct {
    char id[48];
    int state;
    unsigned int use;                         /* LRU 用の使用時刻 */
    unsigned int avg;                         /* 平均色 (ABGR)。環境光用 */
    unsigned int pixels[ART_SIDE * ART_SIDE] __attribute__((aligned(16)));
} Slot;

static Slot g_slots[SLOTS];
static volatile unsigned int g_clock = 0;

/* 読み込み待ち行列 (UI スレッドが積み、ワーカーが取り出す) */
static char g_queue[QUEUE][48];
static volatile int g_qhead = 0, g_qtail = 0;

static SceUID g_thread = -1;
static SceUID g_mutex = -1;
static volatile int g_quit = 0;

static unsigned char g_rx[ART_SIDE * ART_SIDE * 4 + 64];

static void lock(void)   { if (g_mutex >= 0) sceKernelWaitSema(g_mutex, 1, NULL); }
static void unlock(void) { if (g_mutex >= 0) sceKernelSignalSema(g_mutex, 1); }

static Slot *find_slot(const char *id)
{
    for (int i = 0; i < SLOTS; i++)
        if (g_slots[i].state != SLOT_EMPTY && strcmp(g_slots[i].id, id) == 0)
            return &g_slots[i];
    return NULL;
}

/* 空き、無ければ最も長く使われていない READY/MISSING を再利用する */
static Slot *claim_slot(const char *id)
{
    Slot *best = NULL;
    for (int i = 0; i < SLOTS; i++) {
        if (g_slots[i].state == SLOT_EMPTY) {
            best = &g_slots[i];
            break;
        }
        if (g_slots[i].state == SLOT_LOADING)
            continue;   /* 読み込み中は奪わない */
        if (!best || g_slots[i].use < best->use)
            best = &g_slots[i];
    }
    if (!best)
        return NULL;
    snprintf(best->id, sizeof(best->id), "%s", id);
    best->state = SLOT_LOADING;
    best->use = g_clock;
    return best;
}

static void enqueue(const char *id)
{
    int next = (g_qtail + 1) % QUEUE;
    if (next == g_qhead)
        return;   /* 行列が満杯: 今回は諦める (次のフレームで再度予約される) */
    snprintf(g_queue[g_qtail], sizeof(g_queue[0]), "%s", id);
    g_qtail = next;
}

static int art_thread(SceSize args, void *argp)
{
    while (!g_quit) {
        char id[48] = "";

        lock();
        if (g_qhead != g_qtail) {
            snprintf(id, sizeof(id), "%s", g_queue[g_qhead]);
            g_qhead = (g_qhead + 1) % QUEUE;
        }
        unlock();

        if (!id[0]) {
            sceKernelDelayThread(30 * 1000);
            continue;
        }

        char path[128];
        snprintf(path, sizeof(path), "/art?id=%s&s=%d", id, ART_SIDE);
        int got = http_get_bin(net_server_host(), net_server_port(), path,
                              g_rx, sizeof(g_rx));

        lock();
        Slot *s = find_slot(id);
        if (s) {
            if (got == ART_SIDE * ART_SIDE * 4) {
                memcpy(s->pixels, g_rx, (size_t)got);
                /* GPU は CPU キャッシュを見ないので明示的に書き戻す */
                sceKernelDcacheWritebackRange(s->pixels, sizeof(s->pixels));

                /* 環境光用の平均色 (角のアルファ 0 画素も含むが誤差の範囲) */
                unsigned long long r = 0, g = 0, b = 0;
                for (int i = 0; i < ART_SIDE * ART_SIDE; i++) {
                    unsigned int p = s->pixels[i];
                    r += p & 0xFF;
                    g += (p >> 8) & 0xFF;
                    b += (p >> 16) & 0xFF;
                }
                int n = ART_SIDE * ART_SIDE;
                s->avg = 0xFF000000 |
                         ((unsigned)(b / n) << 16) |
                         ((unsigned)(g / n) << 8) |
                         (unsigned)(r / n);
                s->state = SLOT_READY;
            } else {
                s->state = SLOT_MISSING;   /* 取得できない画像は再試行しない */
            }
        }
        unlock();
    }
    return 0;
}

int art_init(void)
{
    memset(g_slots, 0, sizeof(g_slots));
    g_mutex = sceKernelCreateSema("art_sema", 0, 1, 1, NULL);
    if (g_mutex < 0)
        return g_mutex;
    g_quit = 0;
    g_thread = sceKernelCreateThread("art", art_thread, 0x1A, 0x4000, 0, 0);
    if (g_thread < 0)
        return g_thread;
    return sceKernelStartThread(g_thread, 0, 0);
}

void art_shutdown(void)
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

/* --- 描画 --------------------------------------------------------------- */

void art_draw_ex(const char *id, float x, float y, float size,
                 unsigned int tint)
{
    g_clock++;

    if (!id || !id[0]) {
        gfx_card_fill(x, y, size, C_CARD_MISS);
        return;
    }

    lock();
    Slot *s = find_slot(id);
    if (!s) {
        s = claim_slot(id);
        if (s)
            enqueue(id);
        unlock();
        gfx_card_fill(x, y, size, C_CARD_LOAD);   /* 読み込み中のプレースホルダ */
        return;
    }
    s->use = g_clock;
    int state = s->state;
    const unsigned int *px = s->pixels;
    unlock();

    if (state == SLOT_READY)
        gfx_blit_raw_ex(px, ART_SIDE, x, y, size, size, tint);
    else
        gfx_card_fill(x, y, size,
                      state == SLOT_MISSING ? C_CARD_MISS : C_CARD_LOAD);
}

void art_draw(const char *id, int x, int y, int size)
{
    art_draw_ex(id, (float)x, (float)y, (float)size, 0xFFFFFFFF);
}

unsigned int art_avg_color(const char *id)
{
    if (!id || !id[0])
        return 0;
    lock();
    Slot *s = find_slot(id);
    unsigned int avg = (s && s->state == SLOT_READY) ? s->avg : 0;
    unlock();
    return avg;
}
