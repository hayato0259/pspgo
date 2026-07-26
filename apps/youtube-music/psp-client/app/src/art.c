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
#include "common.h"

#define SLOTS 16          /* 同時に保持する枚数 */
#define QUEUE 8           /* 読み込み待ち行列 */

enum { SLOT_EMPTY = 0, SLOT_LOADING, SLOT_READY, SLOT_MISSING };

typedef struct {
    char id[48];
    int state;
    unsigned int use;                         /* LRU 用の使用時刻 */
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
        int got = http_get_bin(SERVER_HOST, SERVER_PORT, path,
                              g_rx, sizeof(g_rx));

        lock();
        Slot *s = find_slot(id);
        if (s) {
            if (got == ART_SIDE * ART_SIDE * 4) {
                memcpy(s->pixels, g_rx, (size_t)got);
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

typedef struct { short u, v; short x, y, z; } TexVtx;
typedef struct { unsigned int color; short x, y, z; } FlatVtx;

static void fill(int x, int y, int w, int h, unsigned int color)
{
    FlatVtx *v = sceGuGetMemory(2 * sizeof(FlatVtx));
    v[0].color = color; v[0].x = x;     v[0].y = y;     v[0].z = 0;
    v[1].color = color; v[1].x = x + w; v[1].y = y + h; v[1].z = 0;
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_SPRITES,
                   GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
    sceGuEnable(GU_TEXTURE_2D);
}

static void blit(const unsigned int *pixels, int x, int y, int size)
{
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);          /* スウィズルなし */
    sceGuTexImage(0, ART_SIDE, ART_SIDE, ART_SIDE, pixels);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);

    TexVtx *v = sceGuGetMemory(2 * sizeof(TexVtx));
    v[0].u = 0;         v[0].v = 0;         v[0].x = x;        v[0].y = y;        v[0].z = 0;
    v[1].u = ART_SIDE;  v[1].v = ART_SIDE;  v[1].x = x + size; v[1].y = y + size; v[1].z = 0;
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
}

void art_draw(const char *id, int x, int y, int size)
{
    g_clock++;

    if (!id || !id[0]) {
        fill(x, y, size, size, 0xFF3A2A22);
        return;
    }

    lock();
    Slot *s = find_slot(id);
    if (!s) {
        s = claim_slot(id);
        if (s)
            enqueue(id);
        unlock();
        fill(x, y, size, size, 0xFF3A2A22);   /* 読み込み中のプレースホルダ */
        return;
    }
    s->use = g_clock;
    int state = s->state;
    const unsigned int *px = s->pixels;
    unlock();

    if (state == SLOT_READY)
        blit(px, x, y, size);
    else
        fill(x, y, size, size, state == SLOT_MISSING ? 0xFF2E2018 : 0xFF3A2A22);
}
