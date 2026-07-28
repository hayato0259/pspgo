/*
 * 描画基盤の実装。main.c から分離したもの。
 *
 * PSP 固有の注意 (詳細はリポジトリの CLAUDE.md):
 *  - intraFont が残す深度/アルファテストは gu_state_2d() で毎回解除する
 *  - テクスチャは GU_TCC_RGBA + sceGuColor(0xFFFFFFFF)
 *  - CPU で書いた画素は sceKernelDcacheWritebackRange が必要
 *  - jpn0.pgf を主フォントにする (altFont 構成は日本語が欠ける)
 */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspge.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "gfx.h"
#include "theme.h"
#include "common.h"

unsigned int gfx_frame = 0;

static unsigned int __attribute__((aligned(16))) g_gu_list[262144];
static intraFont *g_font = NULL;

#ifdef SHOTDUMP
/* 目視確認用のオフスクリーン描画先 (メインメモリ) */
static unsigned int __attribute__((aligned(64))) g_sysfb[512 * SCR_H];
static char g_dump_req[32] = "";
#endif

/* --- 手続きテクスチャ ---------------------------------------------------
 * 画像アセットを持たず、起動時に CPU で生成する。
 *  - ロゴ: 赤い円 + 白いリング + 白い再生三角 (公式アイコンと同じ構成)
 *  - 光彩: 角丸ボックスから柔らかく減衰する白 (XMB 的な選択表現)
 *  - 角丸: アートワークと同じ丸みの角丸矩形 (プレースホルダと影の元)
 */
#define LOGO_SIDE 32
#define GLOW_SIDE 64
#define CARD_TEX_SIDE 64
#define THUMB_SIDE 32
static unsigned int g_logo[LOGO_SIDE * LOGO_SIDE] __attribute__((aligned(16)));
static unsigned int g_glow[GLOW_SIDE * GLOW_SIDE] __attribute__((aligned(16)));
static unsigned int g_card[CARD_TEX_SIDE * CARD_TEX_SIDE] __attribute__((aligned(16)));
static unsigned int g_thumb[THUMB_SIDE * THUMB_SIDE] __attribute__((aligned(16)));

static float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* 角丸の箱までの距離。0 未満が内側 */
static float box_dist(float px, float py, float cx, float cy,
                      float hx, float hy, float r)
{
    float dx = fabsf(px - cx) - (hx - r);
    float dy = fabsf(py - cy) - (hy - r);
    float ax = dx > 0.0f ? dx : 0.0f;
    float ay = dy > 0.0f ? dy : 0.0f;
    float outside = sqrtf(ax * ax + ay * ay);
    float inside = (dx > dy ? dx : dy);
    if (inside > 0.0f)
        inside = 0.0f;
    return outside + inside - r;
}

/*
 * 高評価・低評価の親指。
 * 握りこぶし・立てた親指・袖の 3 つの角丸の箱を重ねて作る。
 * 画像を持たずに起動時に生成するのは、ロゴや光彩と同じ方針。
 */
static void make_thumb_texture(void)
{
    for (int y = 0; y < THUMB_SIDE; y++) {
        for (int x = 0; x < THUMB_SIDE; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float fist = box_dist(px, py, 19.0f, 20.0f, 9.0f, 8.0f, 3.5f);
            float thumb = box_dist(px, py, 12.5f, 11.0f, 3.5f, 7.0f, 3.0f);
            float cuff = box_dist(px, py, 6.0f, 22.0f, 4.0f, 6.0f, 1.5f);
            float d = fist;
            if (thumb < d) d = thumb;
            if (cuff < d) d = cuff;
            float a = clamp01(0.5f - d);          /* 1px ぶんなだらかに減衰 */
            unsigned int alpha = (unsigned int)(a * 255.0f);
            g_thumb[y * THUMB_SIDE + x] = (alpha << 24) | 0x00FFFFFF;
        }
    }
}

static void make_ui_textures(void)
{
    make_thumb_texture();
    /*
     * ロゴ: 中心 (15.5,15.5)、円の半径 13.5、白いリングは半径 8.0 (太さ 2.2)。
     * 各要素は距離関数で 1px ぶんなだらかに減衰させてアンチエイリアスする。
     */
    for (int y = 0; y < LOGO_SIDE; y++) {
        for (int x = 0; x < LOGO_SIDE; x++) {
            float dx = x - 15.5f, dy = y - 15.5f;
            float d = sqrtf(dx * dx + dy * dy);

            float alpha = clamp01(13.5f - d);      /* 円の外形 */
            if (alpha <= 0.0f) {
                g_logo[y * LOGO_SIDE + x] = 0x00000000;
                continue;
            }

            /* 白の被覆率: リングと三角のうち近い方 */
            float ring = 1.1f - fabsf(d - 8.0f);
            float tri = -1.0f;
            float t = (x - 13.1f) / 7.2f;          /* 三角: x 13.1..20.3 */
            if (t >= 0.0f && t <= 1.0f)
                tri = 3.6f * (1.0f - t) - fabsf(dy);
            float w = clamp01((ring > tri) ? ring : tri);

            /* 赤と白を被覆率で混ぜる (ABGR) */
            unsigned int c = (unsigned)(w * 255.0f);
            g_logo[y * LOGO_SIDE + x] =
                ((unsigned)(alpha * 255.0f) << 24) | (c << 16) | (c << 8) | 0xFF;
        }
    }

    /* 光彩: 中央の角丸ボックス (±16, r=8) からの距離で減衰する白 */
    for (int y = 0; y < GLOW_SIDE; y++) {
        for (int x = 0; x < GLOW_SIDE; x++) {
            float dx = fabsf(x - 31.5f) - 16.0f;
            float dy = fabsf(y - 31.5f) - 16.0f;
            if (dx < 0) dx = 0;
            if (dy < 0) dy = 0;
            float d = sqrtf(dx * dx + dy * dy);
            float a = clamp01(1.0f - d / 15.0f);
            a = a * a;                   /* 端ほど急に落とすと柔らかく見える */
            g_glow[y * GLOW_SIDE + x] =
                ((unsigned)(a * 255.0f) << 24) | 0x00FFFFFF;
        }
    }

    /*
     * 角丸矩形: 64x64 いっぱいの角丸 (r=7) を白で。
     * サーバーが送るアートワークの角丸と同じ丸みに合わせてある。
     * tint で任意の色のプレースホルダや選択背景として使う。
     */
    for (int y = 0; y < CARD_TEX_SIDE; y++) {
        for (int x = 0; x < CARD_TEX_SIDE; x++) {
            float qx = fabsf(x - 31.5f) - (31.5f - 7.0f);
            float qy = fabsf(y - 31.5f) - (31.5f - 7.0f);
            if (qx < 0) qx = 0;
            if (qy < 0) qy = 0;
            float d = sqrtf(qx * qx + qy * qy) - 7.0f;
            float a = clamp01(0.5f - d);
            g_card[y * CARD_TEX_SIDE + x] =
                ((unsigned)(a * 255.0f) << 24) | 0x00FFFFFF;
        }
    }

    sceKernelDcacheWritebackRange(g_logo, sizeof(g_logo));
    sceKernelDcacheWritebackRange(g_glow, sizeof(g_glow));
    sceKernelDcacheWritebackRange(g_card, sizeof(g_card));
}

/* --- GU ----------------------------------------------------------------- */

void gu_state_2d(void)
{
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_STENCIL_TEST);
    sceGuDisable(GU_CULL_FACE);
}

static void gu_init(void)
{
    sceGuInit();
    sceGuStart(GU_DIRECT, g_gu_list);
#ifdef SHOTDUMP
    /*
     * ダンプ用ビルドでは描画先をメインメモリ上の配列にする。
     * VRAM は GPU 側の都合で CPU から読んだ内容が最新とは限らないが、
     * メインメモリなら描画結果をそのまま読み出せる。
     */
    sceGuDrawBuffer(GU_PSM_8888, g_sysfb, 512);
#else
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, 512);
#endif
    sceGuDispBuffer(SCR_W, SCR_H, (void *)0x88000, 512);
    sceGuDepthBuffer((void *)0x110000, 512);
    sceGuOffset(2048 - (SCR_W / 2), 2048 - (SCR_H / 2));
    sceGuViewport(2048, 2048, SCR_W, SCR_H);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuFrontFace(GU_CW);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuShadeModel(GU_SMOOTH);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

/* --- フォント ------------------------------------------------------------
 * 実機は flash0:/font/ に PGF フォントがある。
 * PPSSPP は flash0 が仮想FSで intraFont から開けないことがあるため、
 * アプリ同梱パス (相対) もフォールバックとして試す。
 * ※同梱フォントはリポジトリにコミットしない (著作物)。README 参照。
 */
static const char *FONT_JPN[] = { "flash0:/font/jpn0.pgf", "font/jpn0.pgf", NULL };
static const char *FONT_LTN[] = { "flash0:/font/ltn8.pgf", "font/ltn8.pgf", NULL };

static intraFont *load_first(const char **paths)
{
    for (int i = 0; paths[i]; i++) {
        intraFont *f = intraFontLoad(paths[i], INTRAFONT_STRING_UTF8);
        if (f)
            return f;
    }
    return NULL;
}

static int font_init(void)
{
    if (intraFontInit() < 0)
        return -1;
    g_font = load_first(FONT_JPN);       /* 日本語フォントを主に使う */
    if (!g_font)
        g_font = load_first(FONT_LTN);   /* 日本語フォントが無い環境向けの保険 */
    return g_font ? 0 : -2;
}

intraFont *gfx_font(void) { return g_font; }

int gfx_init(void)
{
    gu_init();
    make_ui_textures();
    return font_init();
}

void gfx_shutdown(void)
{
    if (g_font)
        intraFontUnload(g_font);
    intraFontShutdown();
    sceGuTerm();
}

/*
 * 表と裏のどちらへ描いているか。
 * sceGuSwapBuffers のたびに入れ替わる。Media Engine に渡す
 * 書き込み先を正確に知る必要があるので、推測せず自分で数える。
 */
static int g_draw_parity = 0;

void *gfx_draw_buffer(void)
{
    unsigned int base = (unsigned int)sceGeEdramGetAddr();
    return (void *)(base + (g_draw_parity ? 0x88000u : 0u));
}

void gfx_frame_begin(void)
{
    gfx_frame++;
    sceGuStart(GU_DIRECT, g_gu_list);
    gu_state_2d();
    sceGuClearColor(C_BG);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
}

void gfx_frame_begin_keep(void)
{
    gfx_frame++;
    sceGuStart(GU_DIRECT, g_gu_list);
    gu_state_2d();
    /* 色は消さない。深度だけ揃えておく (文字描画が深度テストを残すため) */
    sceGuClearDepth(0);
    sceGuClear(GU_DEPTH_BUFFER_BIT);
}

#ifdef SHOTDUMP
void gfx_request_dump(const char *name)
{
    snprintf(g_dump_req, sizeof(g_dump_req), "%s", name);
}

/*
 * 開発時の目視確認用。
 * ダブルバッファのどちらに描いたかを自分で追跡し、スワップ前に
 * 「今描き終えたバッファ」を raw で書き出す。表示中バッファを
 * 後から読む方法では、どちらを掴んだか確定できず古いフレームが出てしまう。
 */
static void dump_drawn_buffer(void)
{
    /* GPU が書いた内容を読むため、CPU キャッシュを捨ててから読む */
    sceKernelDcacheInvalidateRange(g_sysfb, sizeof(g_sysfb));
    FILE *fp = fopen(g_dump_req, "wb");
    if (!fp)
        return;
    fwrite(g_sysfb, 4, (size_t)512 * SCR_H, fp);
    fclose(fp);
}
#endif

void gfx_frame_end(void)
{
    sceGuFinish();
    sceGuSync(0, 0);
#ifdef SHOTDUMP
    if (g_dump_req[0]) {
        dump_drawn_buffer();
        g_dump_req[0] = '\0';
    }
#endif
    sceDisplayWaitVblankStart();
#ifndef SHOTDUMP
    sceGuSwapBuffers();   /* ダンプ用ビルドは常に同じオフスクリーンへ描く */
    g_draw_parity ^= 1;
#endif
}

/* --- 2D プリミティブ ----------------------------------------------------- */

typedef struct { unsigned int color; short x, y, z; } RectVtx;
typedef struct { short u, v; short x, y, z; } TexVtx;
typedef struct { float u, v; float x, y, z; } TexVtxF;   /* サブピクセル用 */

void draw_rect(int x, int y, int w, int h, unsigned int color)
{
    RectVtx *v = sceGuGetMemory(2 * sizeof(RectVtx));
    v[0].color = color; v[0].x = x;     v[0].y = y;     v[0].z = 0;
    v[1].color = color; v[1].x = x + w; v[1].y = y + h; v[1].z = 0;
    gu_state_2d();
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_SPRITES,
                   GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
    sceGuEnable(GU_TEXTURE_2D);
}

void draw_vgrad(int x, int y, int w, int h,
                unsigned int top, unsigned int bottom)
{
    RectVtx *v = sceGuGetMemory(4 * sizeof(RectVtx));
    v[0].color = top;    v[0].x = x;     v[0].y = y;     v[0].z = 0;
    v[1].color = top;    v[1].x = x + w; v[1].y = y;     v[1].z = 0;
    v[2].color = bottom; v[2].x = x;     v[2].y = y + h; v[2].z = 0;
    v[3].color = bottom; v[3].x = x + w; v[3].y = y + h; v[3].z = 0;
    gu_state_2d();
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_TRIANGLE_STRIP,
                   GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 4, 0, v);
    sceGuEnable(GU_TEXTURE_2D);
}

void draw_hgrad(int x, int y, int w, int h,
                unsigned int left, unsigned int right)
{
    RectVtx *v = sceGuGetMemory(4 * sizeof(RectVtx));
    v[0].color = left;  v[0].x = x;     v[0].y = y;     v[0].z = 0;
    v[1].color = right; v[1].x = x + w; v[1].y = y;     v[1].z = 0;
    v[2].color = left;  v[2].x = x;     v[2].y = y + h; v[2].z = 0;
    v[3].color = right; v[3].x = x + w; v[3].y = y + h; v[3].z = 0;
    gu_state_2d();
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_TRIANGLE_STRIP,
                   GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 4, 0, v);
    sceGuEnable(GU_TEXTURE_2D);
}

/* --- テクスチャ描画 ------------------------------------------------------ */

/*
 * テクスチャ描画の共通実装。座標は float (サブピクセル)。
 * 整数座標で拡大アニメーションを描くと 1px 刻みでカクつくため、
 * すべての blit をこの float 版に統一する。
 */
static void blit_tex(const unsigned int *tex, int texside,
                     float x, float y, float w, float h, unsigned int tint)
{
    gu_state_2d();
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, texside, texside, texside, tex);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuColor(tint);
    TexVtxF *v = sceGuGetMemory(2 * sizeof(TexVtxF));
    v[0].u = 0.0f;           v[0].v = 0.0f;
    v[0].x = x;              v[0].y = y;              v[0].z = 0.0f;
    v[1].u = (float)texside; v[1].v = (float)texside;
    v[1].x = x + w;          v[1].y = y + h;          v[1].z = 0.0f;
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                   2, 0, v);
}

void gfx_blit_raw_ex(const unsigned int *pixels, int texside,
                     float x, float y, float w, float h, unsigned int tint)
{
    /*
     * GU_TCC_RGB だとアルファが直前のマテリアル色に依存し、
     * 0 のままだと完全に透明になって何も見えない。
     * RGBA + MODULATE で tint による減光もここで行う (blit_tex がその構成)。
     */
    blit_tex(pixels, texside, x, y, w, h, tint);
}

void gfx_blit_raw(const unsigned int *pixels, int texside,
                  int x, int y, int w, int h)
{
    blit_tex(pixels, texside, (float)x, (float)y, (float)w, (float)h,
             0xFFFFFFFF);
}

void gfx_logo(int x, int y, int size)
{
    blit_tex(g_logo, LOGO_SIDE, x, y, size, size, 0xFFFFFFFF);
}

void gfx_glow(float x, float y, float w, float h, int alpha)
{
    blit_tex(g_glow, GLOW_SIDE, x - w / 4, y - h / 4, w + w / 2, h + h / 2,
             ((unsigned)alpha << 24) | 0x00FFFFFF);
}

/*
 * 柔らかい落ち影。光彩テクスチャを黒くティントし、
 * 少し下へずらして対象より一回り大きく描く。
 * 角丸カードの裏に draw_rect の影を敷くと角から黒がはみ出すため、
 * 影も必ずこれを使う。
 */
void gfx_shadow(float x, float y, float w, float h, int alpha)
{
    blit_tex(g_glow, GLOW_SIDE, x - w / 8, y - h / 8 + 3, w + w / 4, h + h / 4,
             ((unsigned)alpha << 24) | 0x00000000);
}

void gfx_card_fill(float x, float y, float size, unsigned int color)
{
    blit_tex(g_card, CARD_TEX_SIDE, x, y, size, size, color);
}

void gfx_thumb(float x, float y, float size, int up, unsigned int color)
{
    /* 下向きは同じ絵を 180 度回して使う (テクスチャを2枚持たない)。
       上下だけ反転すると親指の向きが本家と逆になる */
    if (up)
        blit_tex(g_thumb, THUMB_SIDE, x, y, size, size, color);
    else
        blit_tex(g_thumb, THUMB_SIDE, x + size, y + size, -size, -size, color);
}

/* --- テキスト ------------------------------------------------------------ */

void text(float x, float y, unsigned int color, float size, const char *s)
{
    intraFontSetStyle(g_font, size, color, 0, 0.0f, 0);
    intraFontPrint(g_font, x, y, s);
}

/* PGF に太字が無いので、0.6px ずらした 2 度描きで太らせる */
void text_bold(float x, float y, unsigned int color, float size, const char *s)
{
    text(x, y, color, size, s);
    text(x + 0.6f, y, color, size, s);
}

void text_clipped(float x, float y, int w, unsigned int color, float size,
                  const char *s)
{
    /* ベースラインの上 14px / 下 6px をクリップ領域にする */
    sceGuScissor((int)x, (int)y - 14, w, 20);
    text(x, y, color, size, s);
    sceGuScissor(0, 0, SCR_W, SCR_H);
}

float gfx_text_width(float size, const char *s)
{
    intraFontSetStyle(g_font, size, 0xFFFFFFFF, 0, 0.0f, 0);
    return intraFontMeasureText(g_font, s);
}

/* --- QR ------------------------------------------------------------------ */

void draw_qr(const unsigned char *qr, int size, int x, int y, int scale)
{
    int quiet = 2 * scale;                 /* QR に必要な余白 */
    int side = size * scale;

    /* 余白を含めた白地 */
    draw_rect(x - quiet, y - quiet, side + quiet * 2, side + quiet * 2, 0xFFFFFFFF);

    /* 黒マスを数える (頂点バッファのサイズ決定用) */
    int dark = 0;
    for (int i = 0; i < size * size; i++)
        if (qr[i])
            dark++;
    if (dark == 0)
        return;

    RectVtx *v = sceGuGetMemory(2 * dark * sizeof(RectVtx));
    int n = 0;
    for (int row = 0; row < size; row++) {
        for (int col = 0; col < size; col++) {
            if (!qr[row * size + col])
                continue;
            short px = (short)(x + col * scale);
            short py = (short)(y + row * scale);
            v[n].color = 0xFF000000; v[n].x = px;         v[n].y = py;         v[n].z = 0; n++;
            v[n].color = 0xFF000000; v[n].x = px + scale; v[n].y = py + scale; v[n].z = 0; n++;
        }
    }
    gu_state_2d();
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_SPRITES,
                   GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, n, 0, v);
    sceGuEnable(GU_TEXTURE_2D);
}
