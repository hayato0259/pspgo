/*
 * トラック一覧の画面: プレイリストとオフライン ライブラリ。
 * 行の見た目は共通 (track_row)。オフラインはアーティスト列を足した形。
 */
#include <pspctrl.h>
#include <string.h>
#include <stdio.h>
#include "app.h"
#include "queue.h"
#include "common.h"
#include "theme.h"
#include "gfx.h"
#include "ui.h"
#include "api.h"
#include "player.h"
#include "art.h"
#include "snd.h"
#include "store.h"
#include "dl.h"

static int g_off_sel = 0, g_off_scroll = 0;   /* オフライン画面のカーソル */

void offline_reset_cursor(void)
{
    g_off_sel = 0;
    g_off_scroll = 0;
}

/*
 * 一覧の 1 行。artist が NULL なら題名の欄を右端 (時間の手前) まで使う。
 * saved が真なら「保存済み」の印を出す。
 */
static void track_row(int y, const ApiTrack *t, int selected, int playing,
                      const char *artist, int saved)
{
    if (selected) {
        draw_rect(0, y, SCR_W, ROW_H, C_SEL_BG);
        draw_rect(0, y, 3, ROW_H, C_ACCENT);   /* 左端のアクセント */
    }
    art_draw(t->video_id, 8, y + 2, ROW_H - 4);
    char line[200];
    snprintf(line, sizeof(line), "%s%s", playing ? "♪ " : "", t->title);
    text_clipped(30, y + 13, artist ? 230 : SCR_W - 30 - 48,
                 selected ? C_TEXT : C_DIM,
                 selected ? 0.72f : 0.65f, line);
    if (artist)
        text_clipped(268, y + 13, SCR_W - 268 - 48, C_DIM, 0.55f, artist);
    if (t->duration_sec > 0) {
        char dur[16];
        snprintf(dur, sizeof(dur), "%d:%02d",
                 t->duration_sec / 60, t->duration_sec % 60);
        text(SCR_W - 42, y + 13, C_DIM, 0.6f, dur);
    }
    if (saved)
        text(SCR_W - 56, y + 13, C_DIM, 0.55f, "↓");
}

/* --- プレイリスト --- */

Screen screen_playlist_tick(void)
{
    if ((g_pressed & PSP_CTRL_UP) && g_track_sel > 0) {
        g_track_sel--;
        snd_play(SND_MOVE);
    }
    if ((g_pressed & PSP_CTRL_DOWN) && g_track_sel < g_track_count - 1) {
        g_track_sel++;
        snd_play(SND_MOVE);
    }
    scroll_to(g_track_sel, &g_track_scroll);

    if (g_pressed & PSP_CTRL_CROSS) {
        snd_play(SND_CANCEL);
        if (g_playlist_from_search) {
            g_playlist_from_search = 0;
            return SCR_SEARCH;
        }
        return SCR_HOME;
    }
    if ((g_pressed & PSP_CTRL_CIRCLE) && g_track_count > 0) {
        snd_play(SND_OK);
        /* 手動で曲を選び直したらシャッフルの一巡をやり直す */
        g_playing_index = -1;
        shuffle_history_reset();
        start_track(g_track_sel);
        return SCR_PLAYER;
    }
    /* □: このプレイリストの全曲をオフライン用にダウンロード */
    if ((g_pressed & PSP_CTRL_SQUARE) && g_track_count > 0) {
        int queued = 0;
        for (int i = 0; i < g_track_count; i++)
            if (dl_enqueue(&g_tracks[i]) == 0)
                queued++;
        if (queued > 0)
            snd_play(SND_OK);
    }

    if (g_track_count > 0)
        ui_bg_ambient(art_avg_color(g_tracks[g_track_sel].video_id));
    ui_frame_begin();
    ui_chrome(g_pl_title, "○: 再生    □: 全曲ダウンロード    ×: 戻る",
              g_auth, g_account);
    int y = LIST_TOP;
    for (int i = g_track_scroll;
         i < g_track_count && i < g_track_scroll + LIST_ROWS; i++) {
        ApiTrack *t = &g_tracks[i];
        track_row(y, t, i == g_track_sel, i == g_playing_index,
                  NULL, store_has(t->video_id));
        y += ROW_H;
    }
    dl_status_line();
    now_playing_bar();
    gfx_frame_end();
    return SCR_PLAYLIST;
}

/* --- オフライン ライブラリ --- */

/*
 * ダウンロード済みの曲を再生キュー (g_tracks) に積んで再生を始める。
 * 100 曲 (API_MAX_TRACKS) を超えて保存している場合は選択曲以降を積む。
 */
static Screen offline_play(int sel)
{
    int n = store_count();
    if (n <= 0)
        return SCR_OFFLINE;

    int start = 0;
    if (n > API_MAX_TRACKS) {
        start = sel;
        if (start > n - API_MAX_TRACKS)
            start = n - API_MAX_TRACKS;
    }
    g_track_count = 0;
    for (int i = start; i < n && g_track_count < API_MAX_TRACKS; i++)
        if (store_get(i, &g_tracks[g_track_count]) == 0)
            g_track_count++;

    snprintf(g_pl_title, sizeof(g_pl_title), "オフライン ライブラリ");
    g_track_sel = sel - start;
    g_track_scroll = 0;
    g_playing_index = -1;
    shuffle_history_reset();
    start_track(sel - start);
    return SCR_PLAYER;
}

Screen screen_offline_tick(void)
{
    int n = store_count();
    if (g_off_sel >= n)
        g_off_sel = (n > 0) ? n - 1 : 0;

    if ((g_pressed & PSP_CTRL_UP) && g_off_sel > 0) {
        g_off_sel--;
        snd_play(SND_MOVE);
    }
    if ((g_pressed & PSP_CTRL_DOWN) && g_off_sel < n - 1) {
        g_off_sel++;
        snd_play(SND_MOVE);
    }
    scroll_to(g_off_sel, &g_off_scroll);

    /* ネットワーク無しで起動した場合は戻る先が無いので × は効かせない */
    if ((g_pressed & PSP_CTRL_CROSS) && g_net_ok) {
        snd_play(SND_CANCEL);
        return SCR_HOME;
    }
    if ((g_pressed & PSP_CTRL_CIRCLE) && n > 0) {
        snd_play(SND_OK);
        return offline_play(g_off_sel);
    }
    /* □: 選択中の曲を削除 (再生中なら止めてから) */
    if ((g_pressed & PSP_CTRL_SQUARE) && n > 0) {
        ApiTrack t;
        if (store_get(g_off_sel, &t) == 0) {
            if (g_playing_index >= 0 && g_playing_index < g_track_count &&
                strcmp(g_tracks[g_playing_index].video_id, t.video_id) == 0) {
                player_stop();
                g_playing_index = -1;
            }
            store_remove(g_off_sel);
            snd_play(SND_CANCEL);
        }
    }

    ApiTrack sel_t;
    int has_sel = (n > 0 && store_get(g_off_sel, &sel_t) == 0);

    ui_bg_ambient(has_sel ? art_avg_color(sel_t.video_id) : 0);
    ui_frame_begin();
    char title[64];
    snprintf(title, sizeof(title), "オフライン ライブラリ (%d曲)", n);
    ui_chrome(title,
              g_net_ok ? "○: 再生    □: 削除    ×: 戻る"
                       : "○: 再生    □: 削除",
              g_auth, g_account);

    if (n == 0) {
        text(24, 100, C_DIM, 0.8f, "まだダウンロードした曲がありません");
        text(24, 128, C_DIM, 0.65f,
             "プレイリスト画面で □ を押すと全曲を保存できます");
        text(24, 148, C_DIM, 0.65f,
             "保存した曲はサーバーが無くても再生できます");
    }

    int y = LIST_TOP;
    for (int i = g_off_scroll;
         i < n && i < g_off_scroll + LIST_ROWS; i++) {
        ApiTrack t;
        if (store_get(i, &t) != 0)
            break;
        int playing = (g_playing_index >= 0 && g_playing_index < g_track_count &&
                       strcmp(g_tracks[g_playing_index].video_id,
                              t.video_id) == 0 &&
                       player_state() != PLAYER_STOPPED);
        track_row(y, &t, i == g_off_sel, playing, t.artist, 0);
        y += ROW_H;
    }
    dl_status_line();
    now_playing_bar();
    gfx_frame_end();
    return SCR_OFFLINE;
}
