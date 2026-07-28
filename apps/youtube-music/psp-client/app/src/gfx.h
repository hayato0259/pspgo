#ifndef GFX_H
#define GFX_H

#include <intraFont.h>
#include "icons.h"

/*
 * 描画基盤。GU の初期化、フレーム制御、2D プリミティブ、
 * 手続きテクスチャ (ロゴ・光彩・角丸)、フォントとテキスト描画をまとめる。
 * アプリ固有の部品 (トップバーなど) は ui.h に置く。
 */

/* フレームカウンタ。gfx_frame_begin() のたびに 1 増える (アニメーション用) */
extern unsigned int gfx_frame;

/* GU + フォント + テクスチャ生成。0=成功、負=フォントが無い等 */
int  gfx_init(void);
void gfx_shutdown(void);

/* 画面クリアまで行う。背景は ui_frame_begin() が重ねる */
void gfx_frame_begin(void);
/*
 * 画面を消さずに描き始める。
 * 映像をフレームバッファへ直接書いた後、その上に文字を重ねるときに使う
 * (消してしまうと映像が消える)。
 */
void gfx_frame_begin_keep(void);
void gfx_frame_end(void);

/*
 * これから描き込むバッファの先頭アドレス。
 * Media Engine に映像の書き込み先として渡すために使う。
 */
void *gfx_draw_buffer(void);

/*
 * intraFont は文字描画時に深度/アルファテストを有効化したまま戻さないため、
 * 平面塗りやテクスチャ描画の直前に毎回この関数で 2D 用の状態に戻す。
 */
void gu_state_2d(void);

/*
 * 文字の影。本体のシステム画面と同じ見え方にするため既定で付ける。
 * intraFont の関数を直接呼ぶ場所でも同じ値を渡すこと。
 */
#define GFX_TEXT_SHADOW 0x90000000

/* 主フォント (jpn0.pgf)。intraFont の関数を直接使う画面向け */
intraFont *gfx_font(void);

/* --- 2D プリミティブ --- */
void draw_rect(int x, int y, int w, int h, unsigned int color);
void draw_vgrad(int x, int y, int w, int h, unsigned int top, unsigned int bottom);
void draw_hgrad(int x, int y, int w, int h, unsigned int left, unsigned int right);

/* 生 RGBA ピクセル (辺 texside の正方形) をテクスチャとして描く */
void gfx_blit_raw(const unsigned int *pixels, int texside,
                  int x, int y, int w, int h);

/*
 * tint 付き・サブピクセル座標版。
 * アニメーション (拡大など) は整数座標だと 1px 刻みでカクつくため、
 * 動くものは必ずこちらで描く。tint 0xFFFFFFFF で原色。
 */
void gfx_blit_raw_ex(const unsigned int *pixels, int texside,
                     float x, float y, float w, float h, unsigned int tint);

/* --- 手続きテクスチャによる部品 (座標は float。動くものもそのまま渡せる) --- */
void gfx_logo(int x, int y, int size);                    /* YT Music ロゴ */
void gfx_glow(float x, float y, float w, float h, int alpha);   /* 白い光彩 */
void gfx_shadow(float x, float y, float w, float h, int alpha); /* 柔らかい落ち影 */
void gfx_card_fill(float x, float y, float size, unsigned int color); /* 角丸ベタ塗り */
void gfx_circle_fill(float x, float y, float size, unsigned int color); /* 円のベタ塗り */
/*
 * Material Symbols のアイコン。本家と同じ絵柄を使うため焼き込んである
 * (tools/make_icons.py が生成)。color で色を指定する。
 */
void gfx_icon(IconId id, float x, float y, float size, unsigned int color);

/* --- テキスト --- */
void text(float x, float y, unsigned int color, float size, const char *s);
void text_bold(float x, float y, unsigned int color, float size, const char *s);
/* ベースライン (x,y) から幅 w px でクリップして 1 行だけ描く (折り返さない) */
void text_clipped(float x, float y, int w, unsigned int color, float size,
                  const char *s);

/*
 * 幅 w の箱でクリップしたまま、中身を dx だけ左へずらして 1 行描く。
 * 箱に収まらない題名を流す (マーキー) ために使う。dx=0 は text_clipped と同じ。
 */
void text_scroll(float x, float y, int w, float dx, unsigned int color,
                 float size, const char *s, int bold);

/*
 * サイズ size で描いたときの幅 (px)。
 * intraFontMeasureText は「直前に SetStyle したサイズ」で測るため、
 * 素で呼ぶと直前の描画に引きずられる。必ずこちらを使う。
 */
float gfx_text_width(float size, const char *s);

/* QR コード (1 バイト 1 マスのビット行列) */
void draw_qr(const unsigned char *qr, int size, int x, int y, int scale);

#ifdef SHOTDUMP
/* 次の gfx_frame_end で描画済みバッファを raw で書き出す */
void gfx_request_dump(const char *name);
#endif

#endif
