/*
 * 再生画面と歌詞画面。
 *
 * テレビ版と同じ中央 1 カラム (アートワーク → 題名 → アーティスト)。
 * シークバー・再生制御・評価・キューは常設せず、
 * 十字キーの上下で呼び出す操作パネルに入れる (テレビ版と同じ構成)。
 * 動画版の再生中は映像をフレームバッファへ直接描き、その上に情報を重ねる。
 */
#include <pspkernel.h>
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
#include "trackinfo.h"
#include "video.h"

/*
 * 曲版とミュージックビデオ版の切り替え (YouTube Music の「曲 / 動画」と同じもの)。
 * 同じ楽曲の別バージョンは別の動画として存在する。
 * 対応する版が無い曲も多いので、取得できるまでと無い場合はトグルを出さない。
 *
 * 取得は再生が始まってから 1 曲につき 1 回だけ行う。通信の待ちで描画は止まるが、
 * 音声は別スレッドなので再生は途切れない。
 */
static const ApiTrackInfo *g_info = NULL;  /* 取得済みなら中身、まだなら NULL */
static int g_info_msg_frames = 0;  /* 切り替えできない旨の表示時間 */
static char g_video_for[24] = "";         /* 映像を受け取っている videoId */
static int g_gate_frames = 0;             /* 映像待ちで音を止めている時間 */
static int g_radio_error_frames = 0;

static void video_release(void);

/* 動画再生中の操作パネルは、しばらく触らなければ引っ込める */
#define CONTROLS_SHOW_FRAMES 180          /* 約3秒 */
static int g_controls_frames = CONTROLS_SHOW_FRAMES;

/* シークは連打をまとめてから実行する (1 回ごとに配信を開き直すため) */
static int g_seek_pending = 0;
static int g_seek_target = 0;
static int g_seek_frames = 0;
static int g_rate_error_frames = 0;   /* 評価に失敗した旨の表示時間 */

/*
 * 再生画面の「その他」メニュー。
 * 本家もプレイヤーの主要な操作は直接置き、
 * ラジオやスリープタイマーはメニューの中に入れている。
 */
#define MENU_ITEMS 3
static int g_menu_open = 0;
static int g_menu_sel = 0;

/*
 * 十字キーの上下で出す操作パネル (テレビ版の再生画面と同じ構成)。
 *
 * 出している間はアートワークが上に詰まり、題名とアーティストは画面の一番上へ移る。
 * 下は上から順に シークバー → ボタン → キュー で、この 3 つを上下で行き来する。
 *
 * ボタンの並びはテレビ版から「チャンネル情報・コメント・Gemini」を抜いたもの。
 * 左に再生の操作、右に曲そのものへの操作を寄せる。
 */
typedef enum {
    BTN_PREV = 0, BTN_PLAY, BTN_NEXT,          /* 左寄せ */
    BTN_LIKE, BTN_DISLIKE, BTN_SAVE, BTN_VIEW, /* 右寄せ */
    BTN_COUNT
} PlayerBtn;
#define BTN_RIGHT_FIRST BTN_LIKE

typedef enum { ZONE_SEEK = 0, ZONE_BUTTONS, ZONE_QUEUE } PanelZone;

/* 開発時の確認用: エミュレータへは入力を送れないので、
   パネルやシートを出した状態で起動できるようにしてある
   (make DEMO_PANEL=1 / DEMO_SHEET=1 (表示) / DEMO_SHEET=2 (設定)) */
#ifdef DEMO_PANEL
static int g_panel_open = 1;
#else
static int g_panel_open = 0;
#endif
static int g_panel_zone = ZONE_BUTTONS;
static int g_panel_btn = BTN_PLAY;
static int g_queue_sel = 0;

/* パネルから開くシート (0=出していない) */
typedef enum { SHEET_NONE = 0, SHEET_VIEW } PlayerSheet;
#ifdef DEMO_SHEET
static int g_sheet = DEMO_SHEET;
#else
static int g_sheet = SHEET_NONE;
#endif
static int g_sheet_sel = 0;
static int g_save_error_frames = 0;   /* 保存に失敗した旨の表示時間 */

/*
 * 再生できない曲は自動で次へ送る (本家も 1 曲で止まらない)。
 * Premium 限定として配信されている音源は Cookie が無いと取得できず、
 * そのままだとキューがそこで止まってしまう。
 * ただし全部失敗するときに延々と送り続けないよう、連続失敗に上限を設ける。
 *
 * カウンタのリセットは tick 側で行う: 曲が変わると状態が BUFFERING に
 * なるので、ERROR 以外を見たフレームで 0 に戻る (start_track では触らない)。
 */
#define FAIL_SKIP_LIMIT 3
static int g_fail_frames = 0;    /* エラー表示を続けた時間。-1 = この曲は送り済み */
static int g_fail_streak = 0;    /* 連続で再生できなかった数 */

/*
 * パネルの出入りのアニメーション。
 * 0=閉じている 1=開ききっている。アートワークはこの値で
 * 大きさと位置を補間し、上へ滑らかに移動する。
 */
static float g_panel_t = 0.0f;

static int g_lyrics_scroll = 0;   /* 歌詞画面のスクロール (入るたびに先頭へ) */

/* 動画版を再生しているときだけ映像も受け取る。
   曲版に戻ったり別の画面へ移ったら止める (通信と Media Engine を無駄に使わない) */
static void video_sync(const ApiTrack *t)
{
    /* 対応版の有無とは無関係に「いま再生しているのが動画か」で決める。
       ミュージックビデオそのものの曲は対応版を持たないため */
    /*
     * 音が鳴らせなくなったら映像も止める。
     * 音無しの映像だけを流し続けても意味が無いうえ、
     * 音を基準にした歩調合わせが効かなくなって早送りに見える。
     */
    int want = (t && g_info && g_info->current_is_video && g_net_ok &&
                player_state() != PLAYER_ERROR);
    if (want && strcmp(g_video_for, t->video_id) != 0) {
        snprintf(g_video_for, sizeof(g_video_for), "%s", t->video_id);
        video_start(t->video_id, t->duration_sec, player_elapsed_sec());
        /*
         * 映像の用意は音より時間がかかる (サーバーが変換を始めるまでの間がある)。
         * 先に音だけ進めると、映像が追いつくまで早送りに見えるので、
         * 最初のフレームが出るまで音を止めておく。
         */
        player_gate(1);
        g_gate_frames = 0;
    } else if (!want && g_video_for[0]) {
        video_stop();
        g_video_for[0] = '\0';
    }

    if (!g_video_for[0]) {
        player_gate(0);
        return;
    }

    VideoState vs = video_state();
    g_gate_frames++;
    /* 映像が出た / 出ないと分かった / 待ちすぎた のいずれかで音を解禁する */
    if (vs == VIDEO_PLAYING || vs == VIDEO_ERROR || vs == VIDEO_FINISHED ||
        g_gate_frames > 60 * 20)
        player_gate(0);
}

/*
 * シーク。押すたびに配信を開き直すことになるので、
 * 連打が止まってからまとめて 1 回だけ実行する。
 */
static void seek_by(int delta_sec)
{
    if (g_playing_index < 0 || g_playing_index >= g_track_count)
        return;
    const ApiTrack *t = &g_tracks[g_playing_index];
    int base = g_seek_pending ? g_seek_target : player_elapsed_sec();
    int pos = base + delta_sec;
    if (t->duration_sec > 0 && pos > t->duration_sec - 3)
        pos = t->duration_sec - 3;
    if (pos < 0)
        pos = 0;
    g_seek_target = pos;
    g_seek_pending = 1;
    g_seek_frames = 30;          /* 0.5 秒 追加の入力が無ければ実行 */
}

static void seek_tick(void)
{
    if (!g_seek_pending)
        return;
    if (--g_seek_frames > 0)
        return;
    g_seek_pending = 0;
    if (g_playing_index < 0 || g_playing_index >= g_track_count)
        return;
    const ApiTrack *t = &g_tracks[g_playing_index];
    video_release();             /* 映像も同じ位置から取り直す */
    player_start(t->video_id, t->duration_sec, g_seek_target);
}

/* 画面に出す再生位置。シーク操作中は移動先を見せる */
static int display_elapsed_sec(void)
{
    return g_seek_pending ? g_seek_target : player_elapsed_sec();
}

/* 音を基準にできないときの歩調合わせの起点 (0 = まだ決めていない) */
static unsigned int g_vpace_base_us = 0;
static int g_vpace_base_ms = 0;

static void video_release(void)
{
    g_vpace_base_us = 0;
    if (g_video_for[0]) {
        video_stop();
        g_video_for[0] = '\0';
    }
    player_gate(0);   /* 映像待ちで止めていた音を必ず開ける */
}

/* --- 対応バージョン (曲 / 動画) --------------------------------------- */

/*
 * 再生中の曲の対応バージョンを、ワーカーに取りに行かせて結果を拾う。
 * 問い合わせ自体は trackinfo.c の別スレッドで走るので、ここでは待たない
 * (描画スレッドで通信を待つと、その間ボタンも絵も止まってしまう)。
 */
static void trackinfo_refresh(const char *video_id)
{
    g_info = NULL;
    if (!video_id || !video_id[0] || !g_net_ok)
        return;   /* オフライン起動時は問い合わせない */
    trackinfo_request(video_id);
    g_info = trackinfo_result(video_id);
}

/* 高評価・低評価。同じ側をもう一度押すと解除する (本家と同じ) */
static void rate_current(ApiRating want)
{
    if (g_playing_index < 0 || g_playing_index >= g_track_count || !g_info)
        return;
    ApiRating next = (g_info->rating == want) ? RATE_NONE : want;
    if (api_rate(g_tracks[g_playing_index].video_id, next) < 0) {
        g_rate_error_frames = 150;
        return;
    }
    trackinfo_set_rating(g_tracks[g_playing_index].video_id, next);
}

/*
 * 別バージョンに入れ替えて再生し直す。
 * キューの並びと位置は変えない (純正も切り替えで曲順は動かない)。
 * 0=切り替えた / <0=対応する版が無い
 */
static int version_switch(void)
{
    if (!g_info || !g_info->has_alt)
        return -1;
    if (g_playing_index < 0 || g_playing_index >= g_track_count)
        return -1;
    g_tracks[g_playing_index] = g_info->alt;
    g_info = NULL;   /* 切り替え先の対応情報は取り直す */
    video_release();        /* 映像も作り直す */
    start_track(g_playing_index);
    return 0;
}

/* 動画で流す / 曲で流す を指定して切り替える。0=その状態になった */
static int set_video_mode(int want_video)
{
    if (!g_info)
        return -1;
    if ((g_info->current_is_video != 0) == (want_video != 0))
        return 0;               /* すでにその状態 */
    return version_switch();
}

/* ライブラリへの保存 / 解除。もう一度押すと外れる (本家と同じ) */
static void save_current(void)
{
    if (g_playing_index < 0 || g_playing_index >= g_track_count ||
        !g_info || !g_info->can_save) {
        g_save_error_frames = 150;
        return;
    }
    int want = !g_info->in_library;
    if (api_library(g_tracks[g_playing_index].video_id, want) < 0) {
        g_save_error_frames = 150;
        return;
    }
    trackinfo_set_library(g_tracks[g_playing_index].video_id, want);
}

/* 純正と同じ「曲 / 動画」のトグル。いま再生している側を白く塗って示す */
static void draw_song_video_toggle(float x, float y)
{
    if (!g_info || !g_info->has_alt)
        return;
    static const char *label[2] = { "曲", "動画" };
    const float size = 0.58f;
    float cx = x;
    int i;
    for (i = 0; i < 2; i++) {
        /* i=0 が曲、i=1 が動画。いま再生中の側が選択状態 */
        int selected = ((i == 1) == (g_info->current_is_video != 0));
        float w = gfx_text_width(size, label[i]) + 18.0f;
        draw_rect((int)cx, (int)y - 12, (int)w, 17, selected ? C_TEXT : C_LINE);
        text(cx + 9.0f, y, selected ? C_BG : C_DIM, size, label[i]);
        cx += w + 4.0f;
    }
}

/* 高評価・低評価。本家のプレイヤーと同じく、押されている側だけ白くする */
static void draw_rating(float x, float y)
{
    if (!g_info)
        return;
    /* 押されている側は塗りつぶしにする (本家と同じ) */
    int liked = (g_info->rating == RATE_LIKE);
    int disliked = (g_info->rating == RATE_DISLIKE);
    gfx_icon(liked ? ICON_THUMB_UP_FILL : ICON_THUMB_UP,
             x, y, 18.0f, liked ? C_TEXT : C_DIM);
    gfx_icon(disliked ? ICON_THUMB_DOWN_FILL : ICON_THUMB_DOWN,
             x + 26.0f, y, 18.0f, disliked ? C_TEXT : C_DIM);
}

/*
 * 再生画面の題名・アーティストを 1 行で描く。
 *
 * 箱 (x から幅 w) に収まるなら普通に置く (center が真なら箱の中央)。
 * 収まらないなら箱の中を流す (テレビ版と同じ)。
 * 端で少し止めてから一定の速さで流し、反対の端でも止めて戻る。
 */
static void marquee_line(float x, int baseline, int w, unsigned int color,
                         float size, const char *s, int bold, int center,
                         unsigned int frames)
{
    if (!s || !s[0])
        return;

    float tw = gfx_text_width(size, s);
    if (tw <= (float)w) {
        float tx = center ? x + ((float)w - tw) / 2.0f : x;
        if (bold)
            text_bold(tx, baseline, color, size, s);
        else
            text(tx, baseline, color, size, s);
        return;
    }

    const float speed = 0.4f;       /* 1 フレームあたりの移動量 (px) */
    const unsigned int hold = 90;   /* 端で止める時間 (約 1.5 秒) */
    float over = tw - (float)w;
    unsigned int run = (unsigned int)(over / speed) + 1;
    unsigned int p = frames % (hold + run + hold + run);
    float dx;
    if (p < hold)                 dx = 0.0f;
    else if (p < hold + run)      dx = (float)(p - hold) * speed;
    else if (p < hold + run + hold) dx = over;
    else                          dx = over - (float)(p - hold - run - hold) * speed;
    if (dx < 0.0f)  dx = 0.0f;
    if (dx > over)  dx = over;

    text_scroll(x, baseline, w, dx, color, size, s, bold);
}

/* 空でないものだけを中黒でつなぐ (アーティスト・アルバム・再生回数) */
static void append_meta(char *dst, int size, const char *s)
{
    if (!s || !s[0])
        return;
    int n = (int)strlen(dst);
    snprintf(dst + n, (size_t)(size - n), "%s%s", n ? "・" : "", s);
}

/* 「その他」メニュー。画面下から出るシート状の見た目にする */
static void draw_player_menu(void)
{
    if (!g_menu_open)
        return;
    const int h = 96;
    const int top = SCR_H - h;
    draw_rect(0, 0, SCR_W, SCR_H, 0xA0000000);   /* 後ろを暗くする */
    draw_rect(0, top, SCR_W, h, 0xFF181818);
    draw_rect(0, top, SCR_W, 1, C_LINE);

    /* 動いている間は残り時間を出す (画面に常駐させる代わりにここで見せる) */
    char timer[48];
    int minutes = sleep_timer_minutes();
    unsigned long long remaining_us = sleep_timer_remaining_us();
    if (minutes > 0 && remaining_us > 0) {
        unsigned int sec = (unsigned int)
            ((remaining_us + 999999ULL) / 1000000ULL);
        snprintf(timer, sizeof(timer), "スリープタイマー: 残り %u:%02u",
                 sec / 60, sec % 60);
    } else if (minutes > 0) {
        snprintf(timer, sizeof(timer), "スリープタイマー: %d分", minutes);
    } else {
        snprintf(timer, sizeof(timer), "%s", "スリープタイマー: オフ");
    }

    char mode[48];
    const char *ml = play_mode_label();
    snprintf(mode, sizeof(mode), "再生モード: %s", ml[0] ? ml : "通常");

    const char *items[MENU_ITEMS] = { mode, "この曲からラジオ", timer };
    IconId icons[MENU_ITEMS] = {
        (g_play_mode == PLAY_MODE_SHUFFLE)    ? ICON_SHUFFLE :
        (g_play_mode == PLAY_MODE_REPEAT_ONE) ? ICON_REPEAT_ONE : ICON_REPEAT,
        ICON_RADIO,
        ICON_BEDTIME,
    };
    for (int i = 0; i < MENU_ITEMS; i++) {
        int y = top + 22 + i * 24;
        unsigned int col = (i == g_menu_sel) ? C_TEXT : C_DIM;
        if (i == g_menu_sel) {
            draw_rect(16, y - 14, SCR_W - 32, 22, C_SEL_BG);
            draw_rect(16, y - 14, 3, 22, C_ACCENT);
        }
        gfx_icon(icons[i], 28, y - 13, 18.0f, col);
        text(54, y, col, 0.68f, items[i]);
    }
}

/* --- 操作パネル (十字キーの上下で出す) ----------------------------------- */

/*
 * パネルの出入りに合わせて色を薄くする。
 * 座標だけ動かして色をそのままにすると、閉じ際に文字が居座って見える。
 */
static unsigned int panel_fade(unsigned int color)
{
    unsigned int a = (unsigned int)((float)((color >> 24) & 0xFF) * g_panel_t);
    return (color & 0x00FFFFFF) | (a << 24);
}

/* 下からせり上がる量。開ききると 0 になる */
static float panel_rise(void)
{
    return (1.0f - g_panel_t) * 24.0f;
}

/* ボタン 1 つ。選んでいるものは白い丸で塗る (テレビ版と同じ) */
static void panel_button(PlayerBtn id, float x, float y, IconId icon)
{
    int focused = (g_panel_zone == ZONE_BUTTONS && g_panel_btn == (int)id &&
                   g_sheet == SHEET_NONE);
    gfx_circle_fill(x, y, PANEL_BTN,
                    panel_fade(focused ? 0xFFFFFFFF : 0x66000000));
    gfx_icon(icon, x + 3.0f, y + 3.0f, PANEL_BTN - 6.0f,
             panel_fade(focused ? C_BG : C_TEXT));
}

/*
 * ボタンの並び。テレビ版から「チャンネル情報・コメント・Gemini」を抜き、
 * 再生の操作を左端、曲そのものへの操作を右端に寄せる。
 */
static void draw_panel_buttons(PlayerState st)
{
    int liked    = (g_info && g_info->rating == RATE_LIKE);
    int disliked = (g_info && g_info->rating == RATE_DISLIKE);
    int saved    = (g_info && g_info->in_library);
    const IconId icons[BTN_COUNT] = {
        ICON_SKIP_PREVIOUS,
        (st == PLAYER_PLAYING) ? ICON_PAUSE : ICON_PLAY_ARROW,
        ICON_SKIP_NEXT,
        liked    ? ICON_THUMB_UP_FILL   : ICON_THUMB_UP,
        disliked ? ICON_THUMB_DOWN_FILL : ICON_THUMB_DOWN,
        saved    ? ICON_BOOKMARK_FILL   : ICON_BOOKMARK,
        ICON_SMART_DISPLAY,
    };

    float y = (float)PANEL_BTN_Y + panel_rise();
    for (int i = 0; i < BTN_COUNT; i++) {
        float x = (i < BTN_RIGHT_FIRST)
            ? (float)(MARGIN + i * PANEL_BTN_STEP)
            : (float)(SCR_W - MARGIN - PANEL_BTN -
                      (BTN_COUNT - 1 - i) * PANEL_BTN_STEP);
        panel_button((PlayerBtn)i, x, y, icons[i]);
    }
}

/* シークバー。経過を左、全体を右に置く (テレビ版はバーの上に出す) */
static void draw_panel_seek(const ApiTrack *t)
{
    int elapsed = display_elapsed_sec();
    int rise = (int)panel_rise();
    char buf[16];
    snprintf(buf, sizeof(buf), "%d:%02d", elapsed / 60, elapsed % 60);
    text(MARGIN, PANEL_TIME_Y + rise, panel_fade(C_DIM), 0.5f, buf);
    if (t->duration_sec > 0) {
        snprintf(buf, sizeof(buf), "%d:%02d",
                 t->duration_sec / 60, t->duration_sec % 60);
        text((float)(SCR_W - MARGIN) - gfx_text_width(0.5f, buf),
             PANEL_TIME_Y + rise, panel_fade(C_DIM), 0.5f, buf);
    }

    int w = SCR_W - MARGIN * 2;
    int y = PANEL_BAR_Y + rise;
    draw_rect(MARGIN, y, w, 2, panel_fade(0x66FFFFFF));
    if (t->duration_sec <= 0)
        return;
    int done = w * elapsed / t->duration_sec;
    if (done > w) done = w;
    draw_rect(MARGIN, y, done, 2, panel_fade(C_TEXT));
    if (g_panel_zone == ZONE_SEEK && g_sheet == SHEET_NONE)
        gfx_circle_fill((float)(MARGIN + done) - 5.0f, (float)y - 4.0f,
                        10.0f, panel_fade(0xFFFFFFFF));
}

/* キュー。再生中の曲を中央付近に置き、左右で選ぶと そこへ飛ぶ */
static void draw_panel_queue(void)
{
    if (g_track_count <= 0)
        return;
    int visible = (SCR_W - MARGIN * 2 + PANEL_Q_STEP - PANEL_Q) / PANEL_Q_STEP;
    if (visible < 1) visible = 1;
    int first = g_queue_sel - visible / 2;
    if (first > g_track_count - visible) first = g_track_count - visible;
    if (first < 0) first = 0;

    float y = (float)PANEL_Q_Y + panel_rise();
    for (int i = 0; i < visible && first + i < g_track_count; i++) {
        int idx = first + i;
        float x = (float)(MARGIN + i * PANEL_Q_STEP);
        if (g_panel_zone == ZONE_QUEUE && g_sheet == SHEET_NONE &&
            idx == g_queue_sel)
            gfx_glow(x, y, PANEL_Q, PANEL_Q, (int)(150.0f * g_panel_t));
        art_draw_ex(g_tracks[idx].video_id, x, y, PANEL_Q,
                    panel_fade((idx == g_playing_index) ? 0xFFFFFFFF
                                                        : C_CARD_DIM));
    }
}

/* 題名とアーティスト。パネルを出している間は画面の一番上へ移す */
static unsigned int g_mq_frames = 0;   /* 流れる題名の位置 (曲ごとに 0 から) */

static void draw_panel_header(const ApiTrack *t, unsigned int frames,
                              const char *note, unsigned int note_color)
{
    const int w = SCR_W - MARGIN * 2;
    marquee_line(MARGIN, PANEL_TITLE_Y, w, panel_fade(C_TEXT), 0.78f,
                 t->title, 1, 0, frames);

    /* 2 行目はアーティスト・アルバム・再生回数。
       一時的なお知らせがあるときは、この行を明け渡す */
    if (note && note[0]) {
        text_clipped(MARGIN, PANEL_SUB_Y, w, panel_fade(note_color), 0.55f,
                     note);
        return;
    }
    char sub[224];
    sub[0] = '\0';
    append_meta(sub, sizeof(sub), t->artist);
    if (g_info) {
        append_meta(sub, sizeof(sub), g_info->album);
        append_meta(sub, sizeof(sub), g_info->views);
    }
    marquee_line(MARGIN, PANEL_SUB_Y, w, panel_fade(C_DIM), 0.55f, sub, 0, 0,
                 frames);
}

/*
 * パネル一式 (アートワーク以外)。
 * アートワークは開閉に合わせて位置と大きさを補間するので、呼び出し元で描く。
 * over_video が真なら映像の上に重ねる。
 */
static void draw_panel(const ApiTrack *t, PlayerState st,
                       const char *note, unsigned int note_color,
                       int over_video)
{
    if (over_video) {
        /* 映像の上では文字が沈むので、上下だけ暗くする */
        draw_vgrad(0, 0, SCR_W, 64, panel_fade(0xC8000000), 0x00000000);
        draw_vgrad(0, SCR_H - 130, SCR_W, 130, 0x00000000,
                   panel_fade(0xD8000000));
    }
    draw_panel_header(t, g_mq_frames, note, note_color);
    draw_panel_seek(t);
    draw_panel_buttons(st);
    draw_panel_queue();
}

/*
 * パネルから開くシート (表示)。
 * テレビ版は一覧を左に出して再生中の画面を右に縮める。
 * 480x272 では縮めると何も見えなくなるので、暗くした上に一覧だけ重ねる。
 */
#define SHEET_ITEMS 3

static void draw_sheet(void)
{
    if (g_sheet == SHEET_NONE)
        return;

    /* 題名の行と重ならないよう、下地はしっかり暗くする */
    draw_rect(0, 0, SCR_W, SCR_H, 0xE6000000);

    static const char *labels[SHEET_ITEMS] = { "動画", "歌詞", "音声" };
    const int x = MARGIN, w = 260;

    text_bold(x, 74, C_TEXT, 0.8f, "表示");
    text(x, 92, C_DIM, 0.52f, "音楽の再生中に画面に表示するものを選びます");

    for (int i = 0; i < SHEET_ITEMS; i++) {
        int y = 102 + i * 26;
        int sel = (i == g_sheet_sel);
        draw_rect(x, y, w, 22, sel ? C_TEXT : 0x40FFFFFF);
        text(x + 12, y + 16, sel ? C_BG : C_TEXT, 0.62f, labels[i]);
    }
}

/*
 * 映像の上に重ねる情報。画面を覆いすぎないよう上下の帯だけにする。
 * 本家も動画再生中は情報を最小限にするので、同じ考え方に揃えた。
 */
static void video_overlay(const ApiTrack *t, PlayerState st)
{
    /* 触っていなければ引っ込める。映像を隠さないため */
    if (g_controls_frames <= 0)
        return;
    g_controls_frames--;

    draw_rect(0, 0, SCR_W, 24, 0xB0000000);
    draw_rect(0, SCR_H - 52, SCR_W, 52, 0xB0000000);

    if (t) {
        text_clipped(MARGIN, 17, SCR_W - MARGIN * 2, C_TEXT, 0.6f, t->title);

        int elapsed = display_elapsed_sec();
        char tm[32];
        if (t->duration_sec > 0) {
            snprintf(tm, sizeof(tm), "%d:%02d / %d:%02d",
                     elapsed / 60, elapsed % 60,
                     t->duration_sec / 60, t->duration_sec % 60);
            int w = (SCR_W - 48) * elapsed / t->duration_sec;
            if (w > SCR_W - 48) w = SCR_W - 48;
            draw_rect(24, SCR_H - 40, SCR_W - 48, 3, C_LINE);
            draw_rect(24, SCR_H - 40, w, 3, C_ACCENT);
            gfx_card_fill(24 + w - 4, SCR_H - 43, 9, 0xFFFFFFFF);
        } else {
            snprintf(tm, sizeof(tm), "%d:%02d", elapsed / 60, elapsed % 60);
        }
        text(24, SCR_H - 24, C_DIM, 0.55f, tm);
    }
    if (st == PLAYER_BUFFERING)
        text(SCR_W - 120, SCR_H - 24, C_DIM, 0.55f, "バッファリング中...");

    draw_song_video_toggle(150, SCR_H - 22);
    draw_rating(SCR_W - 70, SCR_H - 32);
}

/*
 * 音を基準に映像の歩調を合わせる。
 *
 * デコードは音より速く回せてしまうので、そのままだと映像だけ先に進む。
 * 直前に描いたフレームの表示時刻が音の再生位置より先なら、その差だけ待つ。
 * 遅れているときは待たずに次へ進めば自然に追いつく。
 *
 * 待っている間は入力を拾えないので上限を設ける。
 */
static void video_pace(void)
{
    int vms = video_pts_ms();
    if (vms < 0)
        return;

    int ref;
    if (player_state() == PLAYER_PLAYING) {
        ref = player_elapsed_ms();
        g_vpace_base_us = 0;      /* 音が基準に戻った */
    } else {
        /*
         * 音がまだ始まっていない間 (読み込み中など)。
         * 基準が無いまま描くと、届いたぶんを全速力で描いて早送りになる。
         * 最初に描いたフレームの時刻を起点に、実時間で歩調を取る。
         */
        unsigned int now = (unsigned int)sceKernelGetSystemTimeLow();
        if (g_vpace_base_us == 0) {
            g_vpace_base_us = now;
            g_vpace_base_ms = vms;
        }
        ref = g_vpace_base_ms + (int)((now - g_vpace_base_us) / 1000);
    }

    int ahead = vms - ref;
    if (ahead <= 8)
        return;
    if (ahead > 120)
        ahead = 120;
    sceKernelDelayThread((unsigned int)ahead * 1000);
}

Screen screen_player_tick(void)
{
    PlayerState st = player_state();

    /* 対応バージョンの取得結果を先に拾う (○ の判定で使うため) */
    trackinfo_refresh((g_playing_index >= 0 && g_playing_index < g_track_count)
                        ? g_tracks[g_playing_index].video_id : NULL);

    /* 何か操作したら操作パネルを出し直す (動画再生中は放っておくと消える) */
    if (g_pressed)
        g_controls_frames = CONTROLS_SHOW_FRAMES;

    if (g_menu_open) {
        if (g_pressed & PSP_CTRL_UP) {
            snd_play(SND_MOVE);
            g_menu_sel = (g_menu_sel + MENU_ITEMS - 1) % MENU_ITEMS;
        }
        if (g_pressed & PSP_CTRL_DOWN) {
            snd_play(SND_MOVE);
            g_menu_sel = (g_menu_sel + 1) % MENU_ITEMS;
        }
        if (g_pressed & PSP_CTRL_CIRCLE) {
            snd_play(SND_OK);
            if (g_menu_sel == 0) {
                cycle_play_mode();
            } else if (g_menu_sel == 1) {
                if (replace_queue_with_radio() != 0)
                    g_radio_error_frames = 150;
                g_menu_open = 0;
            } else {
                cycle_sleep_timer();
            }
        }
        if (g_pressed & (PSP_CTRL_CROSS | PSP_CTRL_SELECT)) {
            snd_play(SND_CANCEL);
            g_menu_open = 0;
        }
    } else if (g_sheet != SHEET_NONE) {
        /* --- 「表示」のシート --- */
        if (g_pressed & PSP_CTRL_UP) {
            snd_play(SND_MOVE);
            g_sheet_sel = (g_sheet_sel + SHEET_ITEMS - 1) % SHEET_ITEMS;
        }
        if (g_pressed & PSP_CTRL_DOWN) {
            snd_play(SND_MOVE);
            g_sheet_sel = (g_sheet_sel + 1) % SHEET_ITEMS;
        }
        if (g_pressed & PSP_CTRL_CIRCLE) {
            snd_play(SND_OK);
            if (g_sheet_sel == 1) {
                g_sheet = SHEET_NONE;
                g_lyrics_scroll = 0;
                return SCR_LYRICS;
            } else {
                if (set_video_mode(g_sheet_sel == 0) != 0)
                    g_info_msg_frames = 120;
                g_sheet = SHEET_NONE;
                st = player_state();
            }
        }
        if (g_pressed & PSP_CTRL_CROSS) {
            snd_play(SND_CANCEL);
            g_sheet = SHEET_NONE;
        }
    } else if (g_panel_open) {
        /* --- 操作パネル。上下で シークバー / ボタン / キュー を行き来する --- */
        if (g_pressed & PSP_CTRL_CROSS) {
            snd_play(SND_CANCEL);
            g_panel_open = 0;
        }
        if (g_pressed_edge & PSP_CTRL_UP) {
            snd_play(SND_MOVE);
            if (g_panel_zone == ZONE_QUEUE)        g_panel_zone = ZONE_BUTTONS;
            else if (g_panel_zone == ZONE_BUTTONS) g_panel_zone = ZONE_SEEK;
            else                                   g_panel_open = 0;
        }
        if (g_pressed_edge & PSP_CTRL_DOWN) {
            snd_play(SND_MOVE);
            if (g_panel_zone == ZONE_SEEK)         g_panel_zone = ZONE_BUTTONS;
            else if (g_panel_zone == ZONE_BUTTONS) g_panel_zone = ZONE_QUEUE;
        }
        if (g_pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) {
            int d = (g_pressed & PSP_CTRL_RIGHT) ? 1 : -1;
            if (g_panel_zone == ZONE_SEEK) {
                snd_play(SND_MOVE);
                seek_by(d * 10);
            } else if (g_panel_zone == ZONE_BUTTONS) {
                snd_play(SND_MOVE);
                g_panel_btn = (g_panel_btn + BTN_COUNT + d) % BTN_COUNT;
            } else if (g_queue_sel + d >= 0 && g_queue_sel + d < g_track_count) {
                snd_play(SND_MOVE);
                g_queue_sel += d;
            }
        }
        if (g_pressed & PSP_CTRL_CIRCLE) {
            if (g_panel_zone == ZONE_QUEUE) {
                snd_play(SND_OK);
                start_track(g_queue_sel);
            } else if (g_panel_zone == ZONE_SEEK) {
                snd_play(SND_OK);
                player_toggle_pause();
            } else {
                snd_play(SND_OK);
                switch (g_panel_btn) {
                case BTN_LIKE:     rate_current(RATE_LIKE); break;
                case BTN_DISLIKE:  rate_current(RATE_DISLIKE); break;
                case BTN_SAVE:     save_current(); break;
                case BTN_VIEW:
                    g_sheet = SHEET_VIEW;
                    /* いまの状態に合わせて開く (どちらで鳴っているかが分かる) */
                    g_sheet_sel = (g_info && g_info->current_is_video) ? 0 : 2;
                    break;
                case BTN_PREV:     skip_track(0); break;
                case BTN_NEXT:     skip_track(1); break;
                default:           player_toggle_pause(); break;
                }
            }
        }
        if (g_pressed & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER))
            skip_track((g_pressed & PSP_CTRL_RTRIGGER) != 0);
        if (g_pressed & PSP_CTRL_SELECT) {
            snd_play(SND_OK);
            g_menu_open = 1;
            g_menu_sel = 0;
        }
    } else {

    if (g_pressed & PSP_CTRL_CROSS) {
        snd_play(SND_CANCEL);
        video_release();
        return SCR_PLAYLIST;
    }
    if (g_pressed & PSP_CTRL_CIRCLE) {
        snd_play(SND_OK);
        player_toggle_pause();
    }
    if (g_pressed & PSP_CTRL_TRIANGLE) {
        if (version_switch() == 0) {
            snd_play(SND_OK);
            st = player_state();
        } else {
            g_info_msg_frames = 120;
        }
    }
    if ((g_pressed & PSP_CTRL_SQUARE) &&
        g_playing_index >= 0 && g_playing_index < g_track_count) {
        snd_play(SND_OK);
        g_lyrics_scroll = 0;
        return SCR_LYRICS;
    }
    if (g_pressed & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT)) {
        snd_play(SND_MOVE);
        seek_by((g_pressed & PSP_CTRL_RIGHT) ? 10 : -10);
    }
    /* 上下で操作パネルを出す (テレビ版と同じ) */
    if (g_pressed_edge & (PSP_CTRL_UP | PSP_CTRL_DOWN)) {
        snd_play(SND_OK);
        g_panel_open = 1;
        g_panel_zone = ZONE_BUTTONS;
        g_panel_btn = BTN_PLAY;
        g_queue_sel = (g_playing_index >= 0) ? g_playing_index : 0;
    }
    if (g_pressed & PSP_CTRL_SELECT) {
        snd_play(SND_OK);
        g_menu_open = 1;
        g_menu_sel = 0;
    }
    if (g_pressed & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER))
        skip_track((g_pressed & PSP_CTRL_RTRIGGER) != 0);
    }   /* メニュー表示中は再生操作を受け付けない */

    seek_tick();

    /* 再生できない曲は 2 秒ほど理由を出してから次へ送る */
    if (st == PLAYER_ERROR) {
        if (g_fail_frames >= 0 && ++g_fail_frames > 120) {
            g_fail_frames = -1;          /* この曲では二度と送らない */
            if (g_fail_streak < FAIL_SKIP_LIMIT) {
                g_fail_streak++;
                skip_track(1);
            }
        }
    } else {
        g_fail_frames = 0;
        if (st == PLAYER_PLAYING)
            g_fail_streak = 0;
    }

    ApiTrack *t = (g_playing_index >= 0) ? &g_tracks[g_playing_index] : NULL;

    /* パネルの出入り。閉じたまま何もしないフレームでも進める */
    {
        float want = g_panel_open ? 1.0f : 0.0f;
        g_panel_t += (want - g_panel_t) * 0.22f;
        if (g_panel_t > 0.999f) g_panel_t = 1.0f;
        if (g_panel_t < 0.001f) g_panel_t = 0.0f;
    }

    /* 流れる題名の位置。曲が変わったら先頭に戻す */
    {
        static char mq_id[24] = "";
        if (!t) {
            mq_id[0] = '\0';
        } else if (strcmp(mq_id, t->video_id) != 0) {
            snprintf(mq_id, sizeof(mq_id), "%s", t->video_id);
            g_mq_frames = 0;
        } else {
            g_mq_frames++;
        }
    }

    /*
     * 動画版を再生しているなら映像も流す。
     * デコードは画面を消す前に行うこと。Media Engine がフレームバッファへ
     * 直接書くので、後から消すと書いた絵ごと消える。
     */
    video_sync(t);
    /* 一時停止中は映像を止めて通常の画面に戻す。
       描かないフレームがあると、表と裏で違う絵が交互に出てちらつくため */
    if (g_video_for[0] && st != PLAYER_PAUSED &&
        video_decode(gfx_draw_buffer()) == 1) {
        gfx_frame_begin_keep();
        /* パネルを出しているなら映像の上に重ねる (本家も映像を消さない) */
        if (g_panel_t > 0.0f && t)
            draw_panel(t, st, NULL, C_DIM, 1);
        else
            video_overlay(t, st);
        draw_sheet();
        draw_player_menu();
        dl_status_line();
        gfx_frame_end();
        video_pace();
        return SCR_PLAYER;
    }

    /*
     * テレビ版と同じ中央 1 カラム。
     * 背景はアートワークをぼかしたもの、その上に アートワーク → 題名 →
     * アーティスト の順で縦に積む。シークバー・再生制御・評価・キューは
     * ここには出さず、十字キーの上下で呼び出すパネルに入れる。
     */
    gfx_frame_begin();
    if (t)
        art_draw_blur_bg(t->video_id, 0xFF787878);
    /* 文字が沈まないよう下へ行くほど暗くする */
    draw_vgrad(0, 0, SCR_W, SCR_H, 0x30000000, 0xA8000000);

    if (t) {
        /*
         * 一時的なお知らせ。1 行を取り合うので優先順位を付けて 1 つだけ描く。
         * ここに何も出ないのが通常の状態。
         */
        char note[96] = "";
        unsigned int note_color = C_DIM;
        if (g_radio_error_frames > 0) {
            snprintf(note, sizeof(note), "%s", "ラジオを取得できませんでした");
            note_color = C_ACCENT;
            g_radio_error_frames--;
        } else if (g_rate_error_frames > 0) {
            snprintf(note, sizeof(note), "%s", "評価を送信できませんでした");
            note_color = C_ACCENT;
            g_rate_error_frames--;
        } else if (g_save_error_frames > 0) {
            snprintf(note, sizeof(note), "%s", "この曲は保存できませんでした");
            note_color = C_ACCENT;
            g_save_error_frames--;
        } else if (g_info_msg_frames > 0) {
            snprintf(note, sizeof(note), "%s",
                     "この曲に対応する動画はありません");
            g_info_msg_frames--;
        } else if (st == PLAYER_BUFFERING) {
            snprintf(note, sizeof(note), "%s", "読み込み中");
        } else if (st == PLAYER_ERROR) {
            note_color = C_ACCENT;
            if (player_last_error() == PLAYER_ERR_NO_DATA)
                snprintf(note, sizeof(note), "%s",
                         "この曲を再生できませんでした");
            else
                snprintf(note, sizeof(note), "再生できませんでした (0x%08X)",
                         player_last_error());
        }

        /*
         * アートワークは開閉に合わせて上へ滑り、同時に小さくなる。
         * 題名とアーティストは中央 (閉) と左上 (開) で入れ替わるので、
         * 濃さで交差させる。
         */
        const float tt = g_panel_t;
        float size = PLAY_ART + (PANEL_ART - PLAY_ART) * tt;
        float ay = PLAY_ART_Y + (PANEL_ART_Y - PLAY_ART_Y) * tt;
        float ax = ((float)SCR_W - size) / 2.0f;
        gfx_shadow(ax, ay, size, size, (int)(0x90 * (1.0f - tt * 0.2f)));
        art_draw_ex(t->video_id, ax, ay, size, 0xFFFFFFFF);

        if (tt < 1.0f) {
            unsigned int a = (unsigned int)(255.0f * (1.0f - tt)) << 24;
            marquee_line(((float)SCR_W - PLAY_TEXT_W) / 2.0f, PLAY_TITLE_Y,
                         PLAY_TEXT_W, a | (C_TEXT & 0x00FFFFFF), 0.85f,
                         t->title, 1, 1, g_mq_frames);
            marquee_line(((float)SCR_W - PLAY_TEXT_W) / 2.0f, PLAY_ARTIST_Y,
                         PLAY_TEXT_W, a | (C_DIM & 0x00FFFFFF), 0.62f,
                         t->artist, 0, 1, g_mq_frames);
            if (note[0])
                text(((float)SCR_W - gfx_text_width(0.58f, note)) / 2.0f,
                     PLAY_NOTE_Y, a | (note_color & 0x00FFFFFF), 0.58f, note);
        }
        if (tt > 0.0f)
            draw_panel(t, st, note, note_color, 0);
    } else {
        const char *msg = "再生していません";
        text(((float)SCR_W - gfx_text_width(0.7f, msg)) / 2.0f, 140,
             C_DIM, 0.7f, msg);
    }
    draw_sheet();
    draw_player_menu();
    dl_status_line();
    gfx_frame_end();
    return SCR_PLAYER;
}

/* --- 歌詞 --------------------------------------------------------------- */

/*
 * 歌詞は曲 (videoId) 単位でこの画面が持つ。
 * 表示中の曲と取得済みの曲が食い違ったら取り直すだけなので、
 * 曲を切り替える側 (start_track) は何も知らなくてよい。
 */
#define MAX_LYRIC_LINES 200
#define LYRICS_BUF_SIZE (32 * 1024)
static char g_lyrics_buf[LYRICS_BUF_SIZE];
static char *g_lyrics_lines[MAX_LYRIC_LINES];
static int g_lyrics_count = 0;
static char g_lyrics_video_id[24] = "";

static void load_lyrics(const ApiTrack *track)
{
    g_lyrics_count = 0;
    g_lyrics_scroll = 0;
    snprintf(g_lyrics_video_id, sizeof(g_lyrics_video_id), "%s",
             track->video_id);

    if (api_lyrics(track->video_id, g_lyrics_buf,
                   sizeof(g_lyrics_buf)) < 0) {
        g_lyrics_buf[0] = '\0';
        return;
    }

    char *line = g_lyrics_buf;
    while (*line && g_lyrics_count < MAX_LYRIC_LINES) {
        char *next = strchr(line, '\n');
        if (next)
            *next = '\0';
        {
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\r')
                line[len - 1] = '\0';
        }
        if (strncmp(line, "line\t", 5) == 0)
            g_lyrics_lines[g_lyrics_count++] = line + 5;
        if (!next)
            break;
        line = next + 1;
    }
}

Screen screen_lyrics_tick(void)
{
    ApiTrack *track =
        (g_playing_index >= 0 && g_playing_index < g_track_count)
            ? &g_tracks[g_playing_index] : NULL;

    if (!track)
        return SCR_PLAYER;

    if (g_pressed & PSP_CTRL_CROSS) {
        snd_play(SND_CANCEL);
        return SCR_PLAYER;
    }
    if (g_pressed & PSP_CTRL_UP) {
        if (g_lyrics_scroll > 0) {
            g_lyrics_scroll--;
            snd_play(SND_MOVE);
        } else {
            snd_play(SND_CANCEL);
            return SCR_PLAYER;
        }
    }
    if ((g_pressed & PSP_CTRL_DOWN) &&
        g_lyrics_scroll + LIST_ROWS < g_lyrics_count) {
        g_lyrics_scroll++;
        snd_play(SND_MOVE);
    }

    ui_bg_ambient(art_avg_color(track->video_id));
    ui_frame_begin();
    ui_chrome("歌詞", "上下: スクロール    ↑(先頭)/×: 戻る",
              g_auth, g_account);
    text_bold(MARGIN, 44, C_TEXT, 0.75f, track->title);
    gu_state_2d();
    draw_rect(MARGIN, 52, SCR_W - MARGIN * 2, 1, C_LINE);

    if (strcmp(g_lyrics_video_id, track->video_id) != 0) {
        const char *loading = "歌詞を取得しています...";
        float w = gfx_text_width(0.72f, loading);
        text((SCR_W - w) / 2.0f, 142, C_DIM, 0.72f, loading);
        gfx_frame_end();
        load_lyrics(track);
        return SCR_LYRICS;
    }

    if (g_lyrics_count == 0) {
        const char *none = "歌詞はありません";
        float w = gfx_text_width(0.78f, none);
        text((SCR_W - w) / 2.0f, 142, C_DIM, 0.78f, none);
    } else {
        int y = 70;
        for (int i = g_lyrics_scroll;
             i < g_lyrics_count && i < g_lyrics_scroll + LIST_ROWS; i++) {
            text_clipped(MARGIN, y, SCR_W - MARGIN * 2,
                         C_TEXT, 0.62f, g_lyrics_lines[i]);
            y += 18;
        }
    }

    gfx_frame_end();
    return SCR_LYRICS;
}
