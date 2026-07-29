/*
 * アプリ共通の UI 部品 (背景・トップバー・再生バー)。
 * 見た目は PC 版 YouTube Music を 480x272 に落とし込んだもの。
 */
#include <pspgu.h>
#include <stdio.h>
#include "ui.h"
#include "gfx.h"
#include "theme.h"
#include "art.h"
#include "dl.h"
#include "common.h"

/* --- 背景 ----------------------------------------------------------------
 * ほぼ黒 + 最上部だけうっすら明るい (本家と同じ構造)。
 * 明るい部分の色は「選択中アートワークの平均色」へ毎フレーム補間し、
 * 本家のヒーローグラデーション (コンテンツ色で染まる背景) を再現する。
 * その上に XMB 風のゆっくり流れる光をごく薄く重ねる。
 */
static float g_amb[3] = { 0x20, 0x24, 0x2A };   /* R,G,B の現在値 */
static unsigned int g_amb_target = 0;

void ui_bg_ambient(unsigned int abgr) { g_amb_target = abgr; }

static unsigned int ambient_top_color(void)
{
    /* 目標色: 既定はニュートラルな暗青。指定があればその色を暗く落とした値 */
    float tr = 0x20, tg = 0x24, tb = 0x2A;
    if (g_amb_target) {
        tr = 10.0f + (float)((g_amb_target)       & 0xFF) * 0.34f;
        tg = 10.0f + (float)((g_amb_target >> 8)  & 0xFF) * 0.34f;
        tb = 10.0f + (float)((g_amb_target >> 16) & 0xFF) * 0.34f;
    }
    g_amb[0] += (tr - g_amb[0]) * 0.05f;
    g_amb[1] += (tg - g_amb[1]) * 0.05f;
    g_amb[2] += (tb - g_amb[2]) * 0.05f;
    return 0xFF000000 |
           ((unsigned)g_amb[2] << 16) | ((unsigned)g_amb[1] << 8) |
           (unsigned)g_amb[0];
}

void ui_frame_begin(void)
{
    gfx_frame_begin();
    draw_vgrad(0, 0, SCR_W, 130, ambient_top_color(), C_BG);
    int t = (int)(gfx_frame / 2) % (SCR_W + 240);
    int x = t - 240;
    draw_hgrad(x, 0, 120, SCR_H, 0x00FFFFFF, 0x08FFFFFF);
    draw_hgrad(x + 120, 0, 120, SCR_H, 0x08FFFFFF, 0x00FFFFFF);
}

/* --- トップバー ---------------------------------------------------------- */

void ui_top_bar(int auth, const char *account)
{
    /* PC 版と同じく帯を敷かず、黒地に直接ロゴ + Music ロゴタイプ */
    gfx_logo(MARGIN, 4, 20);
    text_bold(MARGIN + 26, 19, C_TEXT, 0.82f, "Music");
    /* 右上は本家と同じくアカウントの画像。名前は出さない
       (サーバーが "@me" として円形に切り出したものを配る) */
    if (auth) {
        (void)account;
        art_draw("@me", SCR_W - MARGIN - 22, 4, 22);
    }
}

void ui_chrome(const char *title, const char *hint,
               int auth, const char *account)
{
    gfx_logo(10, 5, 18);
    text_bold(34, 19, C_TEXT, 0.85f, title);
    /* 右上は本家と同じくアカウントの画像。名前は出さない
       (サーバーが "@me" として円形に切り出したものを配る) */
    if (auth) {
        (void)account;
        art_draw("@me", SCR_W - 10 - 20, 4, 20);
    }
    draw_rect(0, 28, SCR_W, 1, C_LINE);
    draw_rect(0, SCR_H - 20, SCR_W, 20, 0xFF0A0A0A);
    draw_rect(0, SCR_H - 20, SCR_W, 1, C_LINE);
    /* 操作の行は細いと読み取りづらい。本体のシステム表示に近い太さにする
       (PGF に太字が無いので二度描きで太らせる) */
    text_bold(10, SCR_H - 6, C_DIM, 0.68f, hint);
}

/* --- 選択中カードの情報パネル -------------------------------------------- */

void ui_selection_info(const char *art_id, const char *title,
                       const char *subtitle, int bottom, int slim)
{
    int h = slim ? INFO_H_SLIM : INFO_H;
    int top = bottom - h;

    /* 上端をフェードさせたスクリム (帯のエッジを見せない) */
    draw_vgrad(0, top - 10, SCR_W, 10, 0x00000000, 0xC0000000);
    draw_vgrad(0, top, SCR_W, h, 0xC0000000, 0xF0000000);

    float tx = MARGIN;
    if (!slim) {
        art_draw(art_id, MARGIN, top + (h - 28) / 2, 28);
        tx = MARGIN + 28 + 10;
    }

    if (slim) {
        /* 1 行: 題名 (白) のみ */
        text_clipped(tx, top + h / 2 + 5, SCR_W - (int)tx - MARGIN,
                     C_TEXT, 0.62f, title);
    } else {
        text_clipped(tx, top + 16, SCR_W - (int)tx - MARGIN,
                     C_TEXT, 0.66f, title);
        if (subtitle && subtitle[0])
            text_clipped(tx, top + 30, SCR_W - (int)tx - MARGIN,
                         C_DIM, 0.55f, subtitle);
    }
}

/* --- リスト共通 ---------------------------------------------------------- */

void scroll_to(int sel, int *scroll)
{
    if (sel < *scroll)
        *scroll = sel;
    if (sel >= *scroll + LIST_ROWS)
        *scroll = sel - LIST_ROWS + 1;
}

void dl_status_line(void)
{
    int n = dl_pending();
    if (n <= 0)
        return;
    char buf[176];
    snprintf(buf, sizeof(buf), "↓%d  %s", n, dl_current_title());
    text_clipped(SCR_W - MARGIN - 150, 30, 150, C_DIM, 0.5f, buf);
}

/* --- 再生バー ------------------------------------------------------------ */

void ui_now_playing(const char *art_id, const char *title, const char *artist,
                    PlayerState st, int elapsed_sec, int duration_sec)
{
    int top = SCR_H - BAR_H;

    draw_vgrad(0, top, SCR_W, BAR_H, 0xF8202020, 0xF80D0D0D);

    /* 進捗は本家と同じくバーの上端に赤で載せる */
    draw_rect(0, top, SCR_W, 1, C_LINE);
    if (duration_sec > 0) {
        int w = SCR_W * elapsed_sec / duration_sec;
        if (w > SCR_W) w = SCR_W;
        draw_rect(0, top - 1, w, 2, C_ACCENT);
    }

    art_draw(art_id, 4, top + 4, 24);

    const char *mark = (st == PLAYER_PAUSED)    ? "II" :
                       (st == PLAYER_BUFFERING) ? "..." :
                       (st == PLAYER_ERROR)     ? "!"  : ">";
    text(36, top + 15, C_ACCENT, 0.7f, mark);

    text_clipped(52, top + 14, 250, C_TEXT, 0.66f, title);
    text_clipped(52, top + 27, 250, C_DIM, 0.56f, artist);

    char tm[32];
    if (duration_sec > 0)
        snprintf(tm, sizeof(tm), "%d:%02d / %d:%02d",
                 elapsed_sec / 60, elapsed_sec % 60,
                 duration_sec / 60, duration_sec % 60);
    else
        snprintf(tm, sizeof(tm), "%d:%02d", elapsed_sec / 60, elapsed_sec % 60);
    text(SCR_W - 82, top + 20, C_DIM, 0.6f, tm);
}
