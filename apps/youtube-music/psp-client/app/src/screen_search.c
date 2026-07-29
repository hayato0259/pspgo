/*
 * 検索画面。入力は本体の OSK (osk.c)、結果はホームと同じ ApiItem の一覧。
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
#include "art.h"
#include "snd.h"
#include "osk.h"

static ApiItem g_search_items[API_MAX_ITEMS];
static int g_search_count = 0;
static int g_search_sel = -1, g_search_scroll = 0;
static int g_search_editing = 0;
static int g_search_first_prompt = 0;
static char g_search_query[192] = "";

int begin_search_input(int first_prompt)
{
    if (osk_begin() < 0) {
        snprintf(g_error, sizeof(g_error), "キーボードを開けませんでした");
        g_search_editing = 0;
        g_search_count = 0;
        g_search_sel = -1;
        return -1;
    }
    g_search_editing = 1;
    g_search_first_prompt = first_prompt;
    g_error[0] = '\0';
    return 0;
}

static int search_item_selectable(int index)
{
    return index >= 0 && index < g_search_count &&
           (g_search_items[index].kind == 'V' ||
            g_search_items[index].kind == 'P');
}

static void search_select_first(void)
{
    g_search_sel = -1;
    for (int i = 0; i < g_search_count; i++) {
        if (search_item_selectable(i)) {
            g_search_sel = i;
            break;
        }
    }
    g_search_scroll = 0;
    if (g_search_sel >= 0)
        scroll_to(g_search_sel, &g_search_scroll);
}

Screen screen_search_tick(void)
{
    if (g_search_editing) {
        int rc = osk_update(g_search_query, sizeof(g_search_query));
        if (rc == OSK_RUNNING)
            return SCR_SEARCH;

        g_search_editing = 0;
        if (rc == OSK_CANCELLED) {
            if (g_search_first_prompt)
                return SCR_HOME;
            return SCR_SEARCH;
        }
        if (rc == OSK_ERROR) {
            snprintf(g_error, sizeof(g_error),
                     "キーボードの処理に失敗しました");
            g_search_count = 0;
            g_search_sel = -1;
            return SCR_SEARCH;
        }

        g_error[0] = '\0';
        if (g_search_query[0] == '\0') {
            g_search_count = 0;
        } else {
            g_search_count = api_search(g_search_query, g_search_items,
                                        API_MAX_ITEMS);
            if (g_search_count < 0) {
                if (api_last_error()[0])
                    snprintf(g_error, sizeof(g_error), "%s",
                             api_last_error());
                else
                    snprintf(g_error, sizeof(g_error),
                             "検索できませんでした (%d)", g_search_count);
                g_search_count = 0;
            }
        }
        search_select_first();
        return SCR_SEARCH;
    }

    if (g_pressed & PSP_CTRL_CROSS) {
        snd_play(SND_CANCEL);
        return SCR_HOME;
    }
    if (g_pressed & PSP_CTRL_TRIANGLE) {
        snd_play(SND_OK);
        begin_search_input(0);
        return SCR_SEARCH;
    }
    if (g_pressed & PSP_CTRL_UP) {
        int next = g_search_sel - 1;
        while (next >= 0 && !search_item_selectable(next))
            next--;
        if (next >= 0) {
            g_search_sel = next;
            snd_play(SND_MOVE);
        }
    }
    if (g_pressed & PSP_CTRL_DOWN) {
        int next = g_search_sel + 1;
        while (next < g_search_count && !search_item_selectable(next))
            next++;
        if (next < g_search_count) {
            g_search_sel = next;
            snd_play(SND_MOVE);
        }
    }
    if (g_search_sel >= 0)
        scroll_to(g_search_sel, &g_search_scroll);

    ApiItem *cur = search_item_selectable(g_search_sel)
                       ? &g_search_items[g_search_sel] : NULL;
    if ((g_pressed & PSP_CTRL_CIRCLE) && cur) {
        snd_play(SND_OK);
        if (cur->kind == 'P') {
            g_track_count = api_playlist(cur->id, g_pl_title,
                                         sizeof(g_pl_title), g_tracks,
                                         API_MAX_TRACKS);
            if (g_track_count >= 0) {
                g_playlist_from_search = 1;
                g_track_sel = 0;
                g_track_scroll = 0;
                g_playing_index = -1;
                shuffle_history_reset();
                return SCR_PLAYLIST;
            }
        } else {
            g_playlist_from_search = 1;
            queue_from_single(cur);
            return SCR_PLAYER;
        }
    }

    ui_bg_ambient(cur ? art_avg_color(cur->id) : 0);
    ui_frame_begin();
    {
        char title[224];
        snprintf(title, sizeof(title), "検索: %s", g_search_query);
        ui_chrome(title, "○: 決定    △: 再検索    ×: ホーム",
                  g_auth, g_account);
    }

    if (g_search_sel < 0) {
        text(24, 112, C_DIM, 0.8f, "見つかりませんでした");
        if (g_error[0]) {
            intraFontSetStyle(gfx_font(), 0.65f, C_ACCENT, GFX_TEXT_SHADOW, 0.0f, 0);
            intraFontPrintColumn(gfx_font(), 24, 140, SCR_W - 48, g_error);
        }
    } else {
        int y = LIST_TOP;
        for (int i = g_search_scroll;
             i < g_search_count && i < g_search_scroll + LIST_ROWS; i++) {
            ApiItem *it = &g_search_items[i];
            if (it->kind == 'S') {
                text_bold(12, y + 14, C_TEXT, 0.65f, it->title);
                gu_state_2d();
                draw_rect(12, y + ROW_H - 2, SCR_W - 24, 1, C_LINE);
            } else {
                if (i == g_search_sel) {
                    draw_rect(0, y, SCR_W, ROW_H, C_SEL_BG);
                    draw_rect(0, y, 3, ROW_H, C_ACCENT);
                }
                art_draw(it->id, 8, y + 2, ROW_H - 4);
                text_clipped(30, y + 13, 250,
                             (i == g_search_sel) ? C_TEXT : C_DIM,
                             (i == g_search_sel) ? 0.7f : 0.63f,
                             it->title);
                text_clipped(292, y + 13, SCR_W - 300, C_DIM, 0.55f,
                             it->subtitle);
            }
            y += ROW_H;
        }
    }
    now_playing_bar();
    gfx_frame_end();
    return SCR_SEARCH;
}
