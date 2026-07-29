/*
 * ホーム画面。
 *
 * PC 版 YouTube Music と同じ構造で見せる:
 * セクション見出し + 横並びのカード (アートワーク)。
 * 左右でカード移動、上下でセクション移動。
 * 曲だけのセクションは PC 版の「おすすめ」と同じコンパクトな行リスト形式。
 */
#include <pspkernel.h>
#include <pspctrl.h>
#include <math.h>
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
#include "dl.h"

/* ホームのセクション数。本家のホームは 20 以上あり、
   ここで切るとミュージックビデオの段などが丸ごと出なくなる */
#define MAX_SECTIONS 24
typedef struct {
    char title[128];
    int first;      /* g_home_items 内の最初のカードの位置 */
    int count;
    int cursor;     /* このセクション内で選択中のカード */
} Section;

static ApiItem g_home_items[API_MAX_ITEMS];
static int g_home_count = 0;
static Section g_sections[MAX_SECTIONS];
static int g_section_count = 0;
static int g_section_sel = 0;    /* 選択中のセクション */
static int g_section_top = 0;    /* 画面最上部に表示するセクション */

/*
 * ホームの取得は専用スレッドで行い、待っている間は骨組み (スケルトン) を
 * 描く。取得は 1〜数秒かかるので、描画スレッドで待つと画面が固まる。
 *
 * api_home は共有バッファ (api.c の g_buf) を使うため、取得中は
 * UI スレッドから他の API (検索など) を呼んではいけない。
 * この画面では読み込み中の入力を △ (オフライン) だけに絞って守っている。
 */
typedef enum { HOME_MOUNTED, HOME_LOADING, HOME_READY, HOME_FAILED } HomeLoad;
static volatile HomeLoad g_load_state = HOME_MOUNTED;
static volatile int g_load_result = 0;
static SceUID g_load_thread = -1;

static int home_load_thread(SceSize args, void *argp)
{
    int n = api_home(g_home_items, API_MAX_ITEMS);
    g_load_result = n;
    g_load_state = (n < 0) ? HOME_FAILED : HOME_READY;
    return 0;
}

void home_load_begin(void)
{
    if (g_load_state == HOME_LOADING)
        return;
    if (g_load_thread >= 0) {
        SceUInt timeout = 100 * 1000;
        sceKernelWaitThreadEnd(g_load_thread, &timeout);
        sceKernelDeleteThread(g_load_thread);
        g_load_thread = -1;
    }
    g_load_state = HOME_LOADING;
    g_load_thread = sceKernelCreateThread("home_load", home_load_thread,
                                          0x1B, 0x4000, 0, 0);
    if (g_load_thread < 0) {
        /* スレッドを作れなければ最後の手段としてこの場で取る (描画は止まる) */
        home_load_thread(0, NULL);
        return;
    }
    sceKernelStartThread(g_load_thread, 0, 0);
}

/* 取得済みのデータからセクション構造を組み立て、カーソルを先頭へ置く */
static void home_mount(void)
{
    g_home_count = g_load_result;
    if (g_home_count < 0) {
        g_home_count = 0;
        return;
    }
    g_error[0] = '\0';   /* 接続時などの古いエラー文を残さない */
    /* section 行を見出しに、その後に続く行をカードとしてまとめる */
    g_section_count = 0;
    for (int i = 0; i < g_home_count; i++) {
        ApiItem *it = &g_home_items[i];
        if (it->kind == 'S') {
            if (g_section_count >= MAX_SECTIONS)
                break;
            Section *s = &g_sections[g_section_count++];
            snprintf(s->title, sizeof(s->title), "%s", it->title);
            s->first = i + 1;
            s->count = 0;
            s->cursor = 0;
        } else if (g_section_count > 0) {
            Section *s = &g_sections[g_section_count - 1];
            if (s->first + s->count == i)
                s->count++;
        }
    }
    /* 見出しの無い項目しか無い場合は、ひとまとめにして見せる */
    if (g_section_count == 0 && g_home_count > 0) {
        Section *s = &g_sections[g_section_count++];
        snprintf(s->title, sizeof(s->title), "おすすめ");
        s->first = 0;
        s->count = g_home_count;
        s->cursor = 0;
    }

    g_section_sel = 0;
    g_section_top = 0;
    while (g_section_sel < g_section_count && g_sections[g_section_sel].count == 0)
        g_section_sel++;
    if (g_section_sel >= g_section_count)
        g_section_sel = 0;
}

/*
 * 曲 (kind 'V') だけのセクションは、PC 版の「おすすめ」と同じく
 * コンパクトな行リスト形式で描く。プレイリストが混ざるものはカルーセル。
 */
static int section_compact(const Section *s)
{
    if (s->count == 0)
        return 0;
    for (int i = 0; i < s->count; i++)
        if (g_home_items[s->first + i].kind != 'V')
            return 0;
    return 1;
}

/* 現在選択されているカード。無ければ NULL */
static ApiItem *selected_card(void)
{
    if (g_section_sel < 0 || g_section_sel >= g_section_count)
        return NULL;
    Section *s = &g_sections[g_section_sel];
    if (s->count <= 0)
        return NULL;
    int idx = s->first + s->cursor;
    if (idx < 0 || idx >= g_home_count)
        return NULL;
    return &g_home_items[idx];
}

/* 読み込み中の骨組み (本家のスケルトンと同じ構図)。うっすら明滅させる */
static void draw_home_skeleton(void)
{
    int v = 0x1E + (int)(6.0f * (1.0f + sinf((float)gfx_frame * 0.06f)));
    unsigned int c = 0xFF000000 |
                     ((unsigned)v << 16) | ((unsigned)v << 8) | (unsigned)v;
    for (int row = 0; row < 2; row++) {
        int base_y = ROW_TOP + row * ROW_PITCH;
        draw_rect(MARGIN, base_y - 10, 120, 12, c);          /* 見出しの棒 */
        for (int x = MARGIN; x < SCR_W - MARGIN; x += CARD_PITCH)
            gfx_card_fill((float)x, (float)(base_y + 8), CARD_SIZE, c);
    }
}

Screen screen_home_tick(void)
{
    /* 裏で取得が終わっていたら、このフレームで組み立てて通常表示に移る */
    if (g_load_state == HOME_READY) {
        home_mount();
        g_load_state = HOME_MOUNTED;
    }

    if (g_load_state == HOME_LOADING) {
        /* △ 以外の操作は受けない (取得スレッドが API の共有バッファを
           使用中のため、検索などを重ねて呼べない) */
        if (g_pressed & PSP_CTRL_TRIANGLE) {
            snd_play(SND_OK);
            offline_reset_cursor();
            return SCR_OFFLINE;
        }
        ui_bg_ambient(0);
        ui_frame_begin();
        ui_top_bar(g_auth, g_account);
        draw_home_skeleton();
        now_playing_bar();
        gfx_frame_end();
        return SCR_HOME;
    }

    if (g_load_state == HOME_FAILED) {
        if (g_pressed & PSP_CTRL_CIRCLE) {   /* ○ でやり直す */
            snd_play(SND_OK);
            home_load_begin();
            return SCR_HOME;
        }
        if (g_pressed & PSP_CTRL_TRIANGLE) {
            snd_play(SND_OK);
            offline_reset_cursor();
            return SCR_OFFLINE;
        }
        ui_bg_ambient(0);
        ui_frame_begin();
        ui_chrome("YouTube Music", "○: 再読み込み    △: オフライン",
                  g_auth, g_account);
        text(24, 90, C_ACCENT, 0.85f, "ホームを取得できませんでした");
        {
            const char *reason = api_last_error()[0]
                ? api_last_error() : "サーバーとの通信に失敗しました";
            intraFontSetStyle(gfx_font(), 0.7f, C_DIM, GFX_TEXT_SHADOW, 0.0f, 0);
            intraFontPrintColumn(gfx_font(), 24, 118, SCR_W - 48, reason);
        }
        now_playing_bar();
        gfx_frame_end();
        return SCR_HOME;
    }

    Section *sec = (g_section_sel < g_section_count) ? &g_sections[g_section_sel] : NULL;
    int compact = (sec && section_compact(sec));

    /*
     * 操作系。
     *  - カルーセル: 左右でカード移動、上下でセクション移動
     *  - 行リスト:   上下で曲移動 (端まで行くと隣のセクションへ)、
     *                左右で 1 ページ (4 件) 送り
     *  - L/R トリガー: どちらの形式でも 1 ページ分まとめて送る
     */
    int page_step = compact ? 4
                  : (SCR_W - MARGIN * 2 - CARD_SIZE) / CARD_PITCH + 1;

    if (sec && (g_pressed & PSP_CTRL_LEFT) && sec->cursor > 0) {
        sec->cursor -= compact ? page_step : 1;
        if (sec->cursor < 0) sec->cursor = 0;
        snd_play(SND_MOVE);
    }
    if (sec && (g_pressed & PSP_CTRL_RIGHT) && sec->cursor < sec->count - 1) {
        sec->cursor += compact ? page_step : 1;
        if (sec->cursor > sec->count - 1) sec->cursor = sec->count - 1;
        snd_play(SND_MOVE);
    }
    if (sec && (g_pressed & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER))) {
        int c = sec->cursor +
                ((g_pressed & PSP_CTRL_RTRIGGER) ? page_step : -page_step);
        if (c > sec->count - 1) c = sec->count - 1;
        if (c < 0) c = 0;
        if (c != sec->cursor) {
            sec->cursor = c;
            snd_play(SND_MOVE);
        }
    }
    /* 上下は「いま見えているページの中」だけを移動し、
       ページの上端/下端 (4件目・8件目など) では隣のセクションへ抜ける */
    if (g_pressed & PSP_CTRL_UP) {
        if (compact && (sec->cursor % page_step) > 0) {
            sec->cursor--;
            snd_play(SND_MOVE);
        } else {
            int i = g_section_sel - 1;
            while (i >= 0 && g_sections[i].count == 0) i--;
            if (i >= 0) { g_section_sel = i; snd_play(SND_MOVE); }
        }
    }
    if (g_pressed & PSP_CTRL_DOWN) {
        if (compact && (sec->cursor % page_step) < page_step - 1 &&
            sec->cursor < sec->count - 1) {
            sec->cursor++;
            snd_play(SND_MOVE);
        } else {
            int i = g_section_sel + 1;
            while (i < g_section_count && g_sections[i].count == 0) i++;
            if (i < g_section_count) { g_section_sel = i; snd_play(SND_MOVE); }
        }
    }
    /* 選択中セクションが画面に入るようにする (同時に 2 段まで表示) */
    if (g_section_sel < g_section_top)
        g_section_top = g_section_sel;
    if (g_section_sel > g_section_top + 1)
        g_section_top = g_section_sel - 1;

    /* ホームの SELECT は認証状態にかかわらず検索を優先する。 */
    if (g_pressed & PSP_CTRL_SELECT) {
        snd_play(SND_OK);
        begin_search_input(1);
        return SCR_SEARCH;
    }

    /* △: オフライン ライブラリへ */
    if (g_pressed & PSP_CTRL_TRIANGLE) {
        snd_play(SND_OK);
        offline_reset_cursor();
        return SCR_OFFLINE;
    }

    ApiItem *cur = selected_card();

    /* □: 選択中の項目をダウンロード (曲はそのまま、プレイリストは全曲) */
    if ((g_pressed & PSP_CTRL_SQUARE) && cur) {
        if (cur->kind == 'V') {
            ApiTrack t;
            memset(&t, 0, sizeof(t));
            snprintf(t.video_id, sizeof(t.video_id), "%s", cur->id);
            snprintf(t.title, sizeof(t.title), "%s", cur->title);
            snprintf(t.artist, sizeof(t.artist), "%s", cur->subtitle);
            if (dl_enqueue(&t) == 0)
                snd_play(SND_OK);
        } else if (cur->kind == 'P') {
            /* 再生キュー (g_tracks) を壊さないよう別バッファへ取得する */
            static ApiTrack tmp[API_MAX_TRACKS];
            char title[128];
            int n = api_playlist(cur->id, title, sizeof(title),
                                 tmp, API_MAX_TRACKS);
            int queued = 0;
            for (int i = 0; i < n; i++)
                if (dl_enqueue(&tmp[i]) == 0)
                    queued++;
            if (queued > 0)
                snd_play(SND_OK);
        }
    }

    if ((g_pressed & PSP_CTRL_CIRCLE) && cur) {
        snd_play(SND_OK);
        g_playlist_from_search = 0;
        if (cur->kind == 'P') {
            g_track_count = api_playlist(cur->id, g_pl_title, sizeof(g_pl_title),
                                         g_tracks, API_MAX_TRACKS);
            if (g_track_count >= 0) {
                g_track_sel = 0;
                g_track_scroll = 0;
                g_playing_index = -1;
                shuffle_history_reset();
                return SCR_PLAYLIST;
            }
        } else if (cur->kind == 'V') {
            queue_from_single(cur);
            return SCR_PLAYER;
        }
    }

    /* --- 描画 --- */
    /* 背景の環境光を選択中アートワークの平均色へ寄せる (本家のヒーロー背景) */
    ui_bg_ambient(cur ? art_avg_color(cur->id) : 0);
    ui_frame_begin();
    ui_top_bar(g_auth, g_account);

    if (g_home_count == 0) {
        text(24, 90, C_ACCENT, 0.85f, "表示できる項目がありませんでした");
        if (g_error[0]) {
            intraFontSetStyle(gfx_font(), 0.7f, C_DIM, GFX_TEXT_SHADOW, 0.0f, 0);
            intraFontPrintColumn(gfx_font(), 24, 118, SCR_W - 48, g_error);
        }
        text(24, 176, C_DIM, 0.7f, "サーバーのログを確認してください");
        now_playing_bar();
        gfx_frame_end();
        return SCR_HOME;
    }

    /* セクションを最大 2 段、横並びのカードで描く (XMB 風の滑らかスクロール) */
    static float scrollf[MAX_SECTIONS];   /* セクションごとの表示上のスクロール位置 */
    for (int row = 0; row < 2; row++) {
        int si = g_section_top + row;
        if (si >= g_section_count)
            break;
        Section *s = &g_sections[si];
        int base_y = ROW_TOP + row * ROW_PITCH;
        int active = (si == g_section_sel);

        /* 見出し。PC 版の大きな太字タイトル + 右端にページ位置 (n/m) */
        if (active) {
            text_bold(MARGIN, base_y, C_TEXT, 0.72f, s->title);
            char pos[16];
            snprintf(pos, sizeof(pos), "%d / %d", s->cursor + 1, s->count);
            text(SCR_W - MARGIN - gfx_text_width(0.55f, pos), base_y,
                 C_DIM, 0.55f, pos);
        } else {
            text(MARGIN, base_y, C_DIM, 0.6f, s->title);
        }

        if (section_compact(s)) {
            /*
             * --- コンパクトな行リスト (PC 版の「おすすめ」形式) ---
             * 4 行を 1 ページとし、ページ単位で横にスライドする
             * (カルーセルと同じ動き)。カーソルが境界を越えると
             * ページ全体が滑らかに切り替わる。
             */
            const int lrow = 18;      /* 1 行の高さ */
            const int pgrows = 4;     /* 1 ページの行数 */
            int page = s->cursor / pgrows;
            scrollf[si] += ((float)page - scrollf[si]) * 0.18f;
            float pf = scrollf[si];

            int pages = (s->count + pgrows - 1) / pgrows;
            for (int p = 0; p < pages; p++) {
                float px = ((float)p - pf) * (float)SCR_W;
                if (px < -(float)SCR_W || px > (float)SCR_W)
                    continue;   /* 画面に少しも掛からないページは飛ばす */
                for (int r = 0; r < pgrows; r++) {
                    int item = p * pgrows + r;
                    if (item >= s->count)
                        break;
                    int idx = s->first + item;
                    if (idx >= g_home_count)
                        break;
                    ApiItem *it = &g_home_items[idx];
                    float rx = MARGIN + px;
                    int ry = base_y + 8 + r * lrow;
                    int selrow = active && item == s->cursor;

                    if (selrow) {
                        draw_rect((int)rx - 8, ry,
                                  SCR_W - (MARGIN - 8) * 2, lrow, C_SEL_BG);
                        draw_rect((int)rx - 8, ry, 3, lrow, C_ACCENT);
                    }
                    art_draw_ex(it->id, rx, ry + 1, lrow - 2,
                                active ? 0xFFFFFFFF : C_CARD_DIM);
                    /* 1 行構成: 左に曲名、右の固定位置にアーティスト等 */
                    text_clipped(rx + 24, ry + 13, 202,
                                 selrow ? C_TEXT :
                                 (active ? 0xFFDDDDDD : C_DIM),
                                 0.56f, it->title);
                    text_clipped(rx + 234, ry + 13, SCR_W - MARGIN - 234,
                                 C_DIM, 0.5f, it->subtitle);
                }
            }
            continue;   /* このセクションはカルーセルを描かない */
        }

        /* --- カルーセル (プレイリストを含むセクション) --- */
        /* 目標スクロール位置へ滑らかに寄せる (XMB 風の慣性)。
           visible は「完全に画面へ収まる枚数」。切り上げで数えると
           終端で最後のカードが右端に隠れてしまう */
        int visible = (SCR_W - MARGIN * 2 - CARD_SIZE) / CARD_PITCH + 1;
        if (visible < 1) visible = 1;
        int target = s->cursor - visible / 2;
        if (target > s->count - visible) target = s->count - visible;
        if (target < 0) target = 0;
        scrollf[si] += ((float)target - scrollf[si]) * 0.22f;
        if (scrollf[si] < 0) scrollf[si] = 0;

        int first = (int)scrollf[si];
        float frac = scrollf[si] - (float)first;
        int sel_x = -1, sel_idx = -1;

        for (int c = 0; c <= visible && first + c < s->count; c++) {
            int idx = s->first + first + c;
            if (idx >= g_home_count)
                break;
            ApiItem *it = &g_home_items[idx];
            int x = MARGIN + (int)((float)c * CARD_PITCH - frac * CARD_PITCH);
            int y = base_y + 8;
            if (x > SCR_W)
                break;

            if (active && (first + c) == s->cursor) {
                sel_x = x;      /* 選択カードは最後に大きく描く */
                sel_idx = idx;
                continue;
            }
            gfx_shadow(x, y, CARD_SIZE, CARD_SIZE, 0x50);
            /* 非アクティブ段は減光してフォーカスの階層を作る */
            art_draw_ex(it->id, x, y, CARD_SIZE,
                        active ? 0xFFFFFFFF : C_CARD_DIM);
        }

        if (sel_idx >= 0) {
            /*
             * 選択中カード:
             *  - 選択が変わった瞬間から 1.0 → 1.16 倍へイージングで拡大
             *  - 枠線ではなく、XMB 的な白い光彩 (ごくゆっくり呼吸) をまとわせる
             * 座標・サイズは float のまま描く (整数に丸めるとカクつく)。
             */
            static int prev_sec = -1, prev_cur = -1;
            static float grow = 0.0f;
            if (prev_sec != g_section_sel || prev_cur != s->cursor) {
                prev_sec = g_section_sel;
                prev_cur = s->cursor;
                grow = 0.0f;
            }
            grow += (1.0f - grow) * 0.16f;

            float size = (float)CARD_SIZE + 10.0f * grow;
            float cx = sel_x + CARD_SIZE / 2.0f;
            float cy = base_y + 8 + CARD_SIZE / 2.0f;
            float x = cx - size / 2.0f, y = cy - size / 2.0f;

            int glow_a = 130 + (int)(20.0f * sinf((float)gfx_frame * 0.05f));

            gfx_glow(x, y, size, size, glow_a);
            gfx_shadow(x, y, size, size, 0x70);
            art_draw_ex(g_home_items[sel_idx].id, x, y, size, 0xFFFFFFFF);
        }
    }

    /*
     * 選択中カードの曲情報は画面下の固定パネルに出す (PC 版の再生バーと
     * 同じ構図)。再生中は再生バーの上に細い帯として重ねる。
     */
    {
        int playing = (player_state() != PLAYER_STOPPED && g_playing_index >= 0);
        if (cur)
            ui_selection_info(cur->id, cur->title, cur->subtitle,
                              playing ? SCR_H - BAR_H : SCR_H, playing);
    }

    dl_status_line();
    now_playing_bar();
    gfx_frame_end();
    return SCR_HOME;
}
