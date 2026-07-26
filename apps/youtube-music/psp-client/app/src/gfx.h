#ifndef GFX_H
#define GFX_H

#include <intraFont.h>

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
void gfx_frame_end(void);

/*
 * intraFont は文字描画時に深度/アルファテストを有効化したまま戻さないため、
 * 平面塗りやテクスチャ描画の直前に毎回この関数で 2D 用の状態に戻す。
 */
void gu_state_2d(void);

/* 主フォント (jpn0.pgf)。intraFont の関数を直接使う画面向け */
intraFont *gfx_font(void);

/* --- 2D プリミティブ --- */
void draw_rect(int x, int y, int w, int h, unsigned int color);
void draw_vgrad(int x, int y, int w, int h, unsigned int top, unsigned int bottom);
void draw_hgrad(int x, int y, int w, int h, unsigned int left, unsigned int right);

/* 生 RGBA ピクセル (辺 texside の正方形) をテクスチャとして描く */
void gfx_blit_raw(const unsigned int *pixels, int texside,
                  int x, int y, int w, int h);

/* --- 手続きテクスチャによる部品 --- */
void gfx_logo(int x, int y, int size);                    /* YT Music ロゴ */
void gfx_glow(int x, int y, int w, int h, int alpha);     /* 白い光彩 (選択) */
void gfx_shadow(int x, int y, int w, int h, int alpha);   /* 柔らかい落ち影 */
void gfx_card_fill(int x, int y, int size, unsigned int color); /* 角丸ベタ塗り */

/* --- テキスト --- */
void text(float x, float y, unsigned int color, float size, const char *s);
void text_bold(float x, float y, unsigned int color, float size, const char *s);
/* ベースライン (x,y) から幅 w px でクリップして 1 行だけ描く (折り返さない) */
void text_clipped(float x, float y, int w, unsigned int color, float size,
                  const char *s);

/* QR コード (1 バイト 1 マスのビット行列) */
void draw_qr(const unsigned char *qr, int size, int x, int y, int scale);

#ifdef SHOTDUMP
/* 次の gfx_frame_end で描画済みバッファを raw で書き出す */
void gfx_request_dump(const char *name);
#endif

#endif
