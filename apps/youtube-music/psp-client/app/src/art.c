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
#include "store.h"

#define SLOTS 48          /* メモリに保持する枚数 (数段ぶん + 再生中) */
#define QUEUE 24          /* 読み込み待ち行列 (画面内のカード数より多く) */

/*
 * 画像のディスクキャッシュ。
 *
 * メモリの枠は有限なので、スクロールして戻ると押し出された画像を
 * 取り直すことになり、そのたびに通信が発生していた。
 * 一度取った画素を本体に置いておき、次からはそこから読む。
 * アプリを再起動しても残る。
 *
 * ファイルは 1 本にまとめ、固定数の枠を使い回す:
 *   [見出し 256 枠 x 64 バイト][画素 256 枠 x 16KB]  = 約 4MB
 * ディレクトリに小さなファイルを大量に作らずに済み、
 * 容量が青天井にならない (最も長く使われていない枠から上書きする)。
 */
#define DISK_SLOTS 256
#define ART_BYTES  (ART_SIDE * ART_SIDE * 4)
#define DISK_HDR   64                       /* 1 枠ぶんの見出しの大きさ */
#define DISK_FILE  "artcache.bin"

typedef struct {
    char id[48];
    unsigned int valid;
    unsigned int use;      /* 追い出す枠を選ぶための使用時刻 */
} DiskHead;

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

/* --- ディスクキャッシュ (art_thread からのみ触る) ----------------------- */

static DiskHead g_disk[DISK_SLOTS];
static unsigned int g_disk_clock = 0;
static int g_disk_ok = 0;

static long disk_head_offset(int i) { return (long)i * DISK_HDR; }
static long disk_data_offset(int i)
{
    return (long)DISK_SLOTS * DISK_HDR + (long)i * ART_BYTES;
}

/* 見出しだけ読み込む (16KB)。ファイルが無ければ作る */
static void disk_init(void)
{
    memset(g_disk, 0, sizeof(g_disk));

    /* 相対パスのファイル操作は stdio を使う (sceIo* は解決しない) */
    FILE *fp = fopen(DISK_FILE, "r+b");
    if (!fp) {
        fp = fopen(DISK_FILE, "w+b");
        if (!fp)
            return;                 /* 書けない環境ならキャッシュ無しで動く */
        static unsigned char zero[DISK_HDR];
        memset(zero, 0, sizeof(zero));
        for (int i = 0; i < DISK_SLOTS; i++)
            fwrite(zero, 1, sizeof(zero), fp);
        fclose(fp);
        g_disk_ok = 1;
        return;
    }

    for (int i = 0; i < DISK_SLOTS; i++) {
        unsigned char buf[DISK_HDR];
        if (fread(buf, 1, sizeof(buf), fp) != sizeof(buf))
            break;
        memcpy(&g_disk[i], buf, sizeof(DiskHead));
        if (g_disk[i].valid && g_disk[i].use > g_disk_clock)
            g_disk_clock = g_disk[i].use;
    }
    fclose(fp);
    g_disk_ok = 1;
}

static int disk_find(const char *id)
{
    for (int i = 0; i < DISK_SLOTS; i++)
        if (g_disk[i].valid && strcmp(g_disk[i].id, id) == 0)
            return i;
    return -1;
}

/* 見つかれば画素を out に読み込んで 1 を返す */
static int disk_load(const char *id, unsigned char *out)
{
    if (!g_disk_ok)
        return 0;
    int i = disk_find(id);
    if (i < 0)
        return 0;

    FILE *fp = fopen(DISK_FILE, "r+b");
    if (!fp)
        return 0;
    int ok = 0;
    if (fseek(fp, disk_data_offset(i), SEEK_SET) == 0 &&
        fread(out, 1, ART_BYTES, fp) == ART_BYTES)
        ok = 1;

    if (ok) {
        /* 使った印を残す。次に追い出す枠を選ぶときに使う */
        g_disk[i].use = ++g_disk_clock;
        if (fseek(fp, disk_head_offset(i), SEEK_SET) == 0)
            fwrite(&g_disk[i], 1, sizeof(DiskHead), fp);
    }
    fclose(fp);
    return ok;
}

/* 空き枠、無ければ最も長く使われていない枠に書く */
static void disk_store(const char *id, const unsigned char *pixels)
{
    if (!g_disk_ok)
        return;

    int target = -1;
    for (int i = 0; i < DISK_SLOTS; i++) {
        if (!g_disk[i].valid) { target = i; break; }
        if (target < 0 || g_disk[i].use < g_disk[target].use)
            target = i;
    }
    if (target < 0)
        return;

    FILE *fp = fopen(DISK_FILE, "r+b");
    if (!fp)
        return;
    if (fseek(fp, disk_data_offset(target), SEEK_SET) == 0 &&
        fwrite(pixels, 1, ART_BYTES, fp) == ART_BYTES) {
        memset(&g_disk[target], 0, sizeof(DiskHead));
        snprintf(g_disk[target].id, sizeof(g_disk[target].id), "%s", id);
        g_disk[target].valid = 1;
        g_disk[target].use = ++g_disk_clock;
        if (fseek(fp, disk_head_offset(target), SEEK_SET) == 0)
            fwrite(&g_disk[target], 1, sizeof(DiskHead), fp);
    }
    fclose(fp);
}

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

        /* ダウンロード済みならローカルの .art を優先する
           (オフライン時はこれが唯一の供給源になる) */
        int got = 0;
        {
            char local[128];
            store_art_path(id, local, sizeof(local));
            FILE *fp = fopen(local, "rb");
            if (fp) {
                got = (int)fread(g_rx, 1, ART_SIDE * ART_SIDE * 4, fp);
                fclose(fp);
            }
        }
        /* 次にディスクキャッシュ。通信せずに済む */
        int from_disk = 0;
        if (got != ART_BYTES && disk_load(id, g_rx)) {
            got = ART_BYTES;
            from_disk = 1;
        }

        if (got != ART_SIDE * ART_SIDE * 4) {
            char base_path[128], path[256];
            snprintf(base_path, sizeof(base_path), "/art?id=%s&s=%d",
                     id, ART_SIDE);
            if (net_build_path(path, sizeof(path), base_path) < 0)
                path[0] = '\0';
            if (!path[0])
                got = -1;
            else
                got = http_get_bin(net_server_host(), net_server_port(), path,
                                   g_rx, sizeof(g_rx));
        }

        /* 通信して取れたものだけ書く (ディスクから読んだものは書き直さない) */
        if (got == ART_BYTES && !from_disk)
            disk_store(id, g_rx);

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
    disk_init();
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

/*
 * ぼかした背景。再生画面の下地に使う。
 *
 * ぼかし処理は書かない。64x64 を 16x16 まで平均で縮めてから画面いっぱいに
 * 引き伸ばすと、GU の線形補間が 30 倍に拡大する過程でそのままぼかしになる。
 * 正方形のまま縦横比を保って「覆う」ので、左右は画面幅、上下ははみ出す。
 *
 * 角のアルファが 0 のままだと背景に穴が空くため、不透明度は 255 に固定する。
 */
#define BLUR_SIDE 16

void art_draw_blur_bg(const char *id, unsigned int tint)
{
    static unsigned int g_blur[BLUR_SIDE * BLUR_SIDE] __attribute__((aligned(16)));
    static char g_blur_id[48] = "";

    if (!id || !id[0])
        return;

    lock();
    Slot *s = find_slot(id);
    if (s && s->state == SLOT_READY && strcmp(g_blur_id, id) != 0) {
        const int step = ART_SIDE / BLUR_SIDE;
        for (int by = 0; by < BLUR_SIDE; by++) {
            for (int bx = 0; bx < BLUR_SIDE; bx++) {
                unsigned int r = 0, g = 0, b = 0;
                for (int y = 0; y < step; y++) {
                    const unsigned int *row =
                        &s->pixels[(by * step + y) * ART_SIDE + bx * step];
                    for (int x = 0; x < step; x++) {
                        unsigned int p = row[x];
                        r += p & 0xFF;
                        g += (p >> 8) & 0xFF;
                        b += (p >> 16) & 0xFF;
                    }
                }
                unsigned int n = (unsigned int)(step * step);
                g_blur[by * BLUR_SIDE + bx] = 0xFF000000 |
                    ((b / n) << 16) | ((g / n) << 8) | (r / n);
            }
        }
        snprintf(g_blur_id, sizeof(g_blur_id), "%s", id);
        sceKernelDcacheWritebackRange(g_blur, sizeof(g_blur));
    }
    unlock();

    if (strcmp(g_blur_id, id) != 0)
        return;   /* 画像がまだ来ていない (取得は art_draw 側が予約する) */

    float side = (float)SCR_W;
    gfx_blit_raw_ex(g_blur, BLUR_SIDE, 0.0f, ((float)SCR_H - side) / 2.0f,
                    side, side, tint);
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
