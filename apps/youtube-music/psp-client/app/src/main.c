/*
 * PSP Go YouTube Music クライアント — 画面と状態機械
 *
 * 画面遷移: 接続 → ログイン選択 → ログイン(QR) → ホーム → プレイリスト → 再生
 *           ホーム/接続失敗 → オフライン ライブラリ (ダウンロード済みの曲)
 * 操作: 上下=移動 / ○=決定 / ×=戻る / △=一時停止 / L,R=前後の曲
 *       □=ダウンロード (オフライン画面では削除) / SELECT=検索
 *       終了は HOME (本体のシステム画面が出る)
 *
 * ログインは OAuth デバイスコードフロー。Google との通信はサーバーが行い、
 * このアプリは QR コードと入力コードを表示して承認完了を待つだけ。
 *
 * 描画プリミティブは gfx.c、共通 UI 部品は ui.c、色とレイアウトは theme.h。
 */
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "common.h"
#include "theme.h"
#include "gfx.h"
#include "ui.h"
#include "net.h"
#include "api.h"
#include "player.h"
#include "login.h"
#include "art.h"
#include "snd.h"
#include "store.h"
#include "dl.h"
#include "osk.h"
#include "trackinfo.h"
#include "video.h"

PSP_MODULE_INFO("ytmusic", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(14 * 1024);

typedef enum {
    SCR_CONNECT,
    SCR_WELCOME,   /* ログインするか、しないで使うかの選択 */
    SCR_LOGIN,     /* コード表示 + 承認待ち */
    SCR_HOME,
    SCR_SEARCH,
    SCR_PLAYLIST,
    SCR_PLAYER,
    SCR_LYRICS,
    SCR_OFFLINE,   /* ダウンロード済みの曲の一覧 (ネットワーク無しでも入れる) */
} Screen;

static int g_running = 1;

/* --- 終了コールバック --- */
static int exit_callback(int arg1, int arg2, void *common)
{
    g_running = 0;
    return 0;
}
static int callback_thread(SceSize args, void *argp)
{
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}
static void setup_callbacks(void)
{
    int thid = sceKernelCreateThread("cb_thread", callback_thread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0)
        sceKernelStartThread(thid, 0, 0);
}

/* --- 入力 (エッジ検出) --- */
static unsigned int g_prev_buttons = 0;
static unsigned int g_pressed = 0;
static unsigned int g_pressed_edge = 0;
static int g_repeat_timer = 0;
static int g_demo_screen = -1;  /* AUTODEMO / SHOTDUMP が参照する現在の画面 */

static void input_update(void)
{
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);
    unsigned int now = pad.Buttons;
    g_pressed_edge = now & ~g_prev_buttons;
    g_pressed = g_pressed_edge;

    /* 上下キーはリピートさせる */
    if (now & (PSP_CTRL_UP | PSP_CTRL_DOWN)) {
        g_repeat_timer++;
        if (g_repeat_timer > 15 && (g_repeat_timer % 4) == 0)
            g_pressed |= now & (PSP_CTRL_UP | PSP_CTRL_DOWN);
    } else {
        g_repeat_timer = 0;
    }
    g_prev_buttons = now;

#ifdef AUTODEMO
    /*
     * E2E 自動検証用: 同じ画面に一定時間留まったら ○ を1回だけ自動入力し、
     * 画面遷移を自動で辿る (ログイン → ホーム → プレイリスト → 再生)。
     */
    {
        static int prev_scr = -1, held = 0, fired = 0;
        if (g_demo_screen != prev_scr) {
            prev_scr = g_demo_screen;
            held = 0;
            fired = 0;
        }
        if (++held > 260 && !fired) {
            fired = 1;
            g_pressed |= PSP_CTRL_CIRCLE;
        }
#ifndef AUTODEMO_NODL
        /* ホームとプレイリストでは ○ の前に □ を1回押し、
           ダウンロード経路 (dl.c → store.c) も自動で通す
           (ホームの最初のカードが単曲でプレイリストを経由しない日もあるため両方)

           AUTODEMO_NODL=1 で無効にできる。一括ダウンロードは回線を占有して
           ストリーミングを失敗させる (0x807F00FD) ため、
           再生まわりを見たいときは切っておく */
        if ((g_demo_screen == SCR_HOME || g_demo_screen == SCR_PLAYLIST) &&
            held == 140)
            g_pressed |= PSP_CTRL_SQUARE;
#endif
    }
#endif
}

/* --- アプリ状態 --- */
static ApiItem g_home_items[API_MAX_ITEMS];
static int g_home_count = 0;
static ApiItem g_search_items[API_MAX_ITEMS];
static int g_search_count = 0;
static int g_search_sel = -1, g_search_scroll = 0;
static int g_search_editing = 0;
static int g_search_first_prompt = 0;
static int g_playlist_from_search = 0;
static char g_search_query[192] = "";

/*
 * ホームは PC 版 YouTube Music と同じ構造で見せる:
 * セクション見出し + 横並びのカード (アートワーク)。
 * 左右でカード移動、上下でセクション移動。
 */
/* ホームのセクション数。本家のホームは 20 以上あり、
   ここで切るとミュージックビデオの段などが丸ごと出なくなる */
#define MAX_SECTIONS 24
typedef struct {
    char title[128];
    int first;      /* g_home_items 内の最初のカードの位置 */
    int count;
    int cursor;     /* このセクション内で選択中のカード */
} Section;

static Section g_sections[MAX_SECTIONS];
static int g_section_count = 0;
static int g_section_sel = 0;    /* 選択中のセクション */
static int g_section_top = 0;    /* 画面最上部に表示するセクション */

static ApiTrack g_tracks[API_MAX_TRACKS];
static int g_track_count = 0;
static int g_track_sel = 0, g_track_scroll = 0;
static char g_pl_title[128];

typedef enum {
    PLAY_MODE_NORMAL,
    PLAY_MODE_SHUFFLE,
    PLAY_MODE_REPEAT_ALL,
    PLAY_MODE_REPEAT_ONE,
    PLAY_MODE_COUNT
} PlayMode;

#define SHUFFLE_HISTORY_WORDS ((API_MAX_TRACKS + 31) / 32)

static int g_playing_index = -1; /* g_tracks 内の再生中インデックス */
static PlayMode g_play_mode = PLAY_MODE_NORMAL;
static unsigned int g_shuffle_history[SHUFFLE_HISTORY_WORDS];
static const int g_sleep_timer_minutes[] = { 0, 15, 30, 60, 90 };
static int g_sleep_timer_option = 0;
static unsigned long long g_sleep_timer_remaining_us = 0;
static unsigned int g_sleep_timer_last_us = 0;
static int g_auth = 0;
static int g_can_login = 0;      /* サーバーに OAuth クライアントが設定済みか */
static char g_account[64] = "-";
static char g_error[192] = "";
static int g_radio_error_frames = 0;

/*
 * 曲版とミュージックビデオ版の切り替え (YouTube Music の「曲 / 動画」と同じもの)。
 * 同じ楽曲の別バージョンは別の動画として存在する。
 * 対応する版が無い曲も多いので、取得できるまでと無い場合はトグルを出さない。
 *
 * 取得は再生が始まってから 1 曲につき 1 回だけ行う。通信の待ちで描画は止まるが、
 * 音声は別スレッドなので再生は途切れない。
 */
static const ApiTrackInfo *g_info = NULL;  /* 取得済みなら中身、まだなら NULL */
static int g_net_ok = 0;         /* サーバーと疎通できたか (オフライン起動の判定) */
static int g_info_msg_frames = 0;  /* 切り替えできない旨の表示時間 */
static char g_video_for[24] = "";         /* 映像を受け取っている videoId */
static int g_gate_frames = 0;             /* 映像待ちで音を止めている時間 */

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
 * PSP には物理ボタンがあるので、左に曲そのものへの操作、右に再生の操作を寄せる。
 */
typedef enum {
    BTN_LIKE = 0, BTN_DISLIKE, BTN_SAVE, BTN_VIEW, BTN_SETTINGS,  /* 左寄せ */
    BTN_PREV, BTN_PLAY, BTN_NEXT,                                 /* 右寄せ */
    BTN_COUNT
} PlayerBtn;
#define BTN_RIGHT_FIRST BTN_PREV

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
typedef enum { SHEET_NONE = 0, SHEET_VIEW, SHEET_SETTINGS } PlayerSheet;
#ifdef DEMO_SHEET
static int g_sheet = DEMO_SHEET;
#else
static int g_sheet = SHEET_NONE;
#endif
static int g_sheet_sel = 0;
static int g_low_quality = 0;         /* 画質: 0=標準 1=低 */
static int g_save_error_frames = 0;   /* 保存に失敗した旨の表示時間 */

/* 動画版を再生しているときだけ映像も受け取る。
   曲版に戻ったり別の画面へ移ったら止める (通信と Media Engine を無駄に使わない) */
static void video_sync(const ApiTrack *t)
{
    /* 対応版の有無とは無関係に「いま再生しているのが動画か」で決める。
       ミュージックビデオそのものの曲は対応版を持たないため */
    int want = (t && g_info && g_info->current_is_video && g_net_ok);
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

static void video_release(void)
{
    if (g_video_for[0]) {
        video_stop();
        g_video_for[0] = '\0';
    }
    player_gate(0);   /* 映像待ちで止めていた音を必ず開ける */
}
static int g_welcome_sel = 0;    /* 0=ログイン 1=ログインせずに使う */
static int g_off_sel = 0, g_off_scroll = 0;   /* オフライン画面のカーソル */

#define MAX_LYRIC_LINES 200
#define LYRICS_BUF_SIZE (32 * 1024)
static char g_lyrics_buf[LYRICS_BUF_SIZE];
static char *g_lyrics_lines[MAX_LYRIC_LINES];
static int g_lyrics_count = 0;
static int g_lyrics_scroll = 0;
static char g_lyrics_video_id[24] = "";

static void start_track(int index);

static int shuffle_track_played(int index)
{
    return (g_shuffle_history[index / 32] & (1U << (index % 32))) != 0;
}

static void shuffle_mark_played(int index)
{
    if (index >= 0 && index < g_track_count)
        g_shuffle_history[index / 32] |= 1U << (index % 32);
}

static void shuffle_history_reset(void)
{
    memset(g_shuffle_history, 0, sizeof(g_shuffle_history));
    shuffle_mark_played(g_playing_index);
}

static int next_track_index(void)
{
    if (g_playing_index < 0 || g_playing_index >= g_track_count ||
        g_track_count <= 0)
        return -1;

    switch (g_play_mode) {
    case PLAY_MODE_NORMAL:
        return (g_playing_index + 1 < g_track_count)
                   ? g_playing_index + 1 : -1;
    case PLAY_MODE_SHUFFLE: {
        int remaining = 0;
        for (int i = 0; i < g_track_count; i++)
            if (!shuffle_track_played(i))
                remaining++;
        if (remaining == 0)
            return -1;

        int pick = rand() % remaining;
        for (int i = 0; i < g_track_count; i++) {
            if (shuffle_track_played(i))
                continue;
            if (pick-- == 0) {
                shuffle_mark_played(i);
                return i;
            }
        }
        return -1;
    }
    case PLAY_MODE_REPEAT_ALL:
        return (g_playing_index + 1) % g_track_count;
    case PLAY_MODE_REPEAT_ONE:
        return g_playing_index;
    case PLAY_MODE_COUNT:
        break;
    }
    return -1;
}

static void cycle_play_mode(void)
{
    g_play_mode = (PlayMode)((g_play_mode + 1) % PLAY_MODE_COUNT);
    if (g_play_mode == PLAY_MODE_SHUFFLE)
        shuffle_history_reset();
}

static const char *play_mode_label(void)
{
    switch (g_play_mode) {
    case PLAY_MODE_SHUFFLE:    return "シャッフル";
    case PLAY_MODE_REPEAT_ALL: return "リピート";
    case PLAY_MODE_REPEAT_ONE: return "1曲リピート";
    case PLAY_MODE_NORMAL:
    case PLAY_MODE_COUNT:
        return "";
    }
    return "";
}

static void cycle_sleep_timer(void)
{
    g_sleep_timer_option =
        (g_sleep_timer_option + 1) %
        (int)(sizeof(g_sleep_timer_minutes) / sizeof(g_sleep_timer_minutes[0]));

    int minutes = g_sleep_timer_minutes[g_sleep_timer_option];
    if (minutes == 0) {
        g_sleep_timer_remaining_us = 0;
        return;
    }

    g_sleep_timer_remaining_us =
        (unsigned long long)minutes * 60ULL * 1000000ULL;
    g_sleep_timer_last_us = (unsigned int)sceKernelGetSystemTimeLow();
}

/*
 * sceKernelGetSystemTimeLow() は約71分で周回するため、期限の絶対値ではなく
 * 毎ループの unsigned 差分を64-bitの残り時間から引く。
 */
static int sleep_timer_tick(void)
{
    if (g_sleep_timer_option == 0)
        return 0;

    unsigned int now = (unsigned int)sceKernelGetSystemTimeLow();
    unsigned int elapsed = now - g_sleep_timer_last_us;
    g_sleep_timer_last_us = now;

    if ((unsigned long long)elapsed >= g_sleep_timer_remaining_us) {
        g_sleep_timer_option = 0;
        g_sleep_timer_remaining_us = 0;
        return 1;
    }

    g_sleep_timer_remaining_us -= (unsigned long long)elapsed;
    return 0;
}

static void scroll_to(int sel, int *scroll)
{
    if (sel < *scroll)
        *scroll = sel;
    if (sel >= *scroll + LIST_ROWS)
        *scroll = sel - LIST_ROWS + 1;
}

/* 再生中トラックの再生バーを描く (どの画面からも呼べる) */
static void now_playing_bar(void)
{
    PlayerState st = player_state();
    if (st == PLAYER_STOPPED || g_playing_index < 0)
        return;
    ApiTrack *t = &g_tracks[g_playing_index];
    ui_now_playing(t->video_id, t->title, t->artist, st,
                   player_elapsed_sec(), t->duration_sec);
}

/* ダウンロード進行中は画面右上に残数と曲名を小さく出す */
static void dl_status_line(void)
{
    int n = dl_pending();
    if (n <= 0)
        return;
    char buf[176];
    snprintf(buf, sizeof(buf), "↓%d  %s", n, dl_current_title());
    text_clipped(SCR_W - MARGIN - 150, 30, 150, C_DIM, 0.5f, buf);
}

/* --- 各画面 --- */

/* ホームを取得して最初の選択可能行へカーソルを置く。0=成功 */
static int load_home(void)
{
    g_home_count = api_home(g_home_items, API_MAX_ITEMS);
    if (g_home_count < 0) {
        /* サーバーが理由を返していればそれを見せる (単なる空表示にしない) */
        if (api_last_error()[0])
            snprintf(g_error, sizeof(g_error), "%s", api_last_error());
        else
            snprintf(g_error, sizeof(g_error),
                     "ホームを取得できません (通信エラー %d)", g_home_count);
        return -1;
    }
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
    return 0;
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

static int begin_search_input(int first_prompt)
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

/* 起動スプラッシュ: 中央にロゴ + 状態表示 (PC 版のローディングと同じ構図) */
static void draw_splash(const char *status, int is_error)
{
    ui_frame_begin();

    int lw = 36;
    float tw = gfx_text_width(0.95f, "Music");
    int x0 = (int)((SCR_W - (lw + 10 + tw)) / 2.0f);
    int ly = 96;
    gfx_logo(x0, ly, lw);
    text_bold(x0 + lw + 10, ly + lw / 2 + 8, C_TEXT, 0.95f, "Music");

    if (status && status[0]) {
        if (is_error) {
            /* エラーは長文になり得るので折り返して表示する */
            intraFontSetStyle(gfx_font(), 0.7f, C_ACCENT, GFX_TEXT_SHADOW, 0.0f, 0);
            intraFontPrintColumn(gfx_font(), 56, 168, SCR_W - 112, status);
        } else {
            float sw = gfx_text_width(0.65f, status);
            text((SCR_W - sw) / 2.0f, 168, C_DIM, 0.65f, status);
        }
    }
    if (!is_error) {
        /* 進行中を示す点滅ドット */
        int n = (gfx_frame / 20) % 4;
        char dots[8] = "";
        for (int i = 0; i < n; i++)
            strcat(dots, "・");
        text(SCR_W / 2.0f - 12, 190, C_DIM, 0.65f, dots);
    }
}

static Screen screen_connect_tick(void)
{
    static int step = 0;

    ui_bg_ambient(0);

    if (step == 99) { /* エラー表示で停止 */
        /* ダウンロード済みの曲があれば、接続できなくてもオフラインで使える */
        int off_n = store_count();
        if (off_n > 0 && (g_pressed & PSP_CTRL_CIRCLE)) {
            snd_play(SND_OK);
            g_off_sel = 0;
            g_off_scroll = 0;
            return SCR_OFFLINE;
        }
        draw_splash(g_error, 1);
        if (off_n > 0) {
            char b[64];
            snprintf(b, sizeof(b), "○: オフライン再生 (%d曲)", off_n);
            text((SCR_W - gfx_text_width(0.62f, b)) / 2.0f, 212,
                 C_TEXT, 0.62f, b);
        }
        text(SCR_W / 2.0f - 52, 230, C_DIM, 0.6f, "終了は HOME ボタン");
        gfx_frame_end();
        return SCR_CONNECT;
    }

    draw_splash("サーバーに接続しています", 0);
    {
        char dst[96];
        snprintf(dst, sizeof(dst), "接続先: %s:%d%s",
                 net_server_host(), net_server_port(),
                 net_server_config_loaded() ? " (server.txt)" : "");
        text((SCR_W - gfx_text_width(0.55f, dst)) / 2.0f, 230,
             C_DIM, 0.55f, dst);
    }
    gfx_frame_end();

    if (step == 0) {
        step = 1;
        return SCR_CONNECT; /* 1フレーム描画してから接続処理へ */
    }

    int rc = net_init();
    if (rc < 0) {
        snprintf(g_error, sizeof(g_error), "Wi-Fi エラー: 0x%08X", rc);
        step = 99;
        return SCR_CONNECT;
    }
    rc = api_status(g_account, sizeof(g_account), &g_can_login);
    if (rc < 0) {
        if (net_server_config_loaded())
            snprintf(g_error, sizeof(g_error),
                     "サーバーに接続できません (%d)。起動していますか?", rc);
        else
            snprintf(g_error, sizeof(g_error),
                     "接続失敗 (%d)。EBOOT と同じ場所に server.txt を作り、"
                     "サーバー機の IP:8080 を書いてください", rc);
        step = 99;
        return SCR_CONNECT;
    }
    g_auth = (rc > 0);
    g_net_ok = 1;

#ifdef DEMO_TRACK
    /*
     * 開発時の確認用: 接続できたら指定の曲だけのキューを作って再生画面に飛ぶ。
     * 画面を手で辿らずに再生画面の見た目を確認するために使う
     * (エミュレータへの入力送信は本体の操作を奪うため使わない方針)。
     *   make DEMO_TRACK='\"<videoId>\"'
     * 題名とアーティストは DEMO_TITLE / DEMO_ARTIST で差し替えられる
     * (長い題名を入れると、横に流れる動きを確認できる)。
     */
#ifndef DEMO_TITLE
#define DEMO_TITLE "確認用の再生"
#endif
#ifndef DEMO_ARTIST
#define DEMO_ARTIST ""
#endif
    g_track_count = 1;
    snprintf(g_tracks[0].video_id, sizeof(g_tracks[0].video_id), "%s", DEMO_TRACK);
    snprintf(g_tracks[0].title, sizeof(g_tracks[0].title), "%s", DEMO_TITLE);
    snprintf(g_tracks[0].artist, sizeof(g_tracks[0].artist), "%s", DEMO_ARTIST);
    g_tracks[0].duration_sec = 0;
    start_track(0);
    return SCR_PLAYER;
#endif

    /* 未ログインでログイン可能なら、まず選択画面を出す */
    if (!g_auth && g_can_login) {
        g_welcome_sel = 0;
        return SCR_WELCOME;
    }
    if (load_home() < 0) {
        step = 99;
        return SCR_CONNECT;
    }
    return SCR_HOME;
}

/* --- ログイン選択画面 --- */

static Screen screen_welcome_tick(void)
{
    if (g_pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN)) {
        g_welcome_sel ^= 1;
        snd_play(SND_MOVE);
    }

    if (g_pressed & PSP_CTRL_CIRCLE) {
        snd_play(SND_OK);
        if (g_welcome_sel == 0) {
            login_begin();
            return SCR_LOGIN;
        }
        if (load_home() < 0)
            return SCR_WELCOME;
        return SCR_HOME;
    }

    ui_bg_ambient(0);
    ui_frame_begin();
    ui_chrome("YouTube Music", "上下: 選択    ○: 決定", g_auth, g_account);
    text(40, 70, C_TEXT, 0.9f, "Google アカウントでログインしますか?");
    text(40, 92, C_DIM, 0.7f,
         "ログインすると、マイミックスなどが表示されます。");

    const char *opts[2] = { "ログインする", "ログインせずに使う" };
    for (int i = 0; i < 2; i++) {
        int y = 130 + i * 30;
        if (i == g_welcome_sel) {
            draw_rect(30, y - 14, SCR_W - 60, 24, C_SEL_BG);
            draw_rect(30, y - 14, 3, 24, C_ACCENT);
        }
        text(44, y + 2, (i == g_welcome_sel) ? C_TEXT : C_DIM, 0.85f, opts[i]);
    }
    text(40, 215, C_DIM, 0.65f,
         "テレビの YouTube アプリと同じ方式です。");
    text(40, 233, C_DIM, 0.65f,
         "画面の QR コードをスマートフォンで読み取ります。");
    gfx_frame_end();
    return SCR_WELCOME;
}

/* --- ログイン待ち画面 --- */

static Screen screen_login_tick(void)
{
    LoginState st = login_state();

    if (st == LOGIN_SUCCESS) {
        /* 認証状態とホームを取り直す */
        int rc = api_status(g_account, sizeof(g_account), &g_can_login);
        g_auth = (rc > 0);
        if (load_home() < 0)
            return SCR_WELCOME;
        return SCR_HOME;
    }

    if (g_pressed & PSP_CTRL_CROSS) {
        login_cancel();
        return SCR_WELCOME;
    }
    if ((st == LOGIN_FAILED) && (g_pressed & PSP_CTRL_CIRCLE)) {
        login_begin(); /* やり直す */
        return SCR_LOGIN;
    }

    ui_bg_ambient(0);
    ui_frame_begin();
    ui_chrome("ログイン", (st == LOGIN_FAILED) ? "○: やり直す    ×: 戻る"
                                              : "×: 中止",
              g_auth, g_account);

    if (st == LOGIN_REQUESTING) {
        text(40, 120, C_TEXT, 0.85f, "ログイン用のコードを取得しています...");
    } else if (st == LOGIN_WAITING) {
        int qr_size = 0;
        const unsigned char *qr = login_qr(&qr_size);

        if (qr) {
            /* 左に QR、右に手入力用の情報を並べる */
            int scale = (qr_size <= 25) ? 5 : 4;
            int side = qr_size * scale;
            int qx = 34, qy = 60 + (150 - side) / 2;
            draw_qr(qr, qr_size, qx, qy, scale);

            text(40, 48, C_TEXT, 0.8f, "スマートフォンで読み取ってください");

            int tx = qx + side + 26;
            text(tx, 96, C_DIM, 0.65f, "読み取れない場合:");
            text(tx, 118, C_TEXT, 0.7f, login_url());
            text(tx, 142, C_DIM, 0.65f, "に、このコードを入力");
            intraFontSetStyle(gfx_font(), 1.2f, C_TEXT, GFX_TEXT_SHADOW, 0.0f, 0);
            intraFontPrint(gfx_font(), tx, 172, login_user_code());
        } else {
            /* QR が使えない場合はコードを大きく見せる */
            text(40, 28, C_ACCENT, 0.6f, "(QR コードを受信できませんでした)");
            text(40, 62, C_DIM, 0.75f, "スマートフォンや PC で次の URL を開いてください");
            text(56, 86, C_TEXT, 0.9f, login_url());
            text(40, 118, C_DIM, 0.75f, "そこに、このコードを入力してください:");
            draw_rect(56, 130, SCR_W - 112, 44, C_SEL_BG);
            intraFontSetStyle(gfx_font(), 1.7f, C_TEXT, GFX_TEXT_SHADOW, 0.0f,
                              INTRAFONT_ALIGN_CENTER);
            intraFontPrint(gfx_font(), SCR_W / 2, 162, login_user_code());
        }

        text(40, 216, C_DIM, 0.7f, "承認すると自動で次に進みます");
        char rem[96];
        int r = login_remaining_sec();
        snprintf(rem, sizeof(rem), "承認をお待ちしています  (コード有効: %d:%02d)",
                 r / 60, r % 60);
        text(40, 238, C_DIM, 0.7f, rem);
    } else if (st == LOGIN_FAILED) {
        text(40, 110, C_ACCENT, 0.85f, "ログインに失敗しました");
        intraFontSetStyle(gfx_font(), 0.7f, C_DIM, GFX_TEXT_SHADOW, 0.0f, 0);
        intraFontPrintColumn(gfx_font(), 40, 138, SCR_W - 80, login_message());
    }
    gfx_frame_end();
    return SCR_LOGIN;
}

/* --- ホーム (カルーセル) --- */

static Screen screen_home_tick(void)
{
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
        g_off_sel = 0;
        g_off_scroll = 0;
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
            /* 単曲: 1 曲だけのプレイリストとして扱う */
            snprintf(g_pl_title, sizeof(g_pl_title), "%s", cur->title);
            snprintf(g_tracks[0].video_id, sizeof(g_tracks[0].video_id), "%s", cur->id);
            snprintf(g_tracks[0].title, sizeof(g_tracks[0].title), "%s", cur->title);
            snprintf(g_tracks[0].artist, sizeof(g_tracks[0].artist), "%s", cur->subtitle);
            g_tracks[0].duration_sec = 0;
            g_track_count = 1;
            g_track_sel = 0;
            g_playing_index = -1;
            shuffle_history_reset();
            start_track(0);
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

/* --- 検索 --- */

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

static Screen screen_search_tick(void)
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
            snprintf(g_pl_title, sizeof(g_pl_title), "%s", cur->title);
            snprintf(g_tracks[0].video_id, sizeof(g_tracks[0].video_id),
                     "%s", cur->id);
            snprintf(g_tracks[0].title, sizeof(g_tracks[0].title),
                     "%s", cur->title);
            snprintf(g_tracks[0].artist, sizeof(g_tracks[0].artist),
                     "%s", cur->subtitle);
            g_tracks[0].duration_sec = 0;
            g_track_count = 1;
            g_track_sel = 0;
            g_track_scroll = 0;
            g_playing_index = -1;
            g_playlist_from_search = 1;
            shuffle_history_reset();
            start_track(0);
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

static void start_track(int index)
{
    if (index < 0 || index >= g_track_count)
        return;
    if (strcmp(g_lyrics_video_id, g_tracks[index].video_id) != 0) {
        g_lyrics_video_id[0] = '\0';
        g_lyrics_buf[0] = '\0';
        g_lyrics_count = 0;
        g_lyrics_scroll = 0;
    }
    g_playing_index = index;
    if (g_play_mode == PLAY_MODE_SHUFFLE)
        shuffle_mark_played(index);
    player_start(g_tracks[index].video_id, g_tracks[index].duration_sec, 0);
}

/* 前へ / 次へ。L/R トリガーとパネルのボタンの両方から使う */
static void skip_track(int forward)
{
    int index = -1;
    if (g_play_mode == PLAY_MODE_SHUFFLE)
        index = next_track_index();
    else if (!forward && g_playing_index > 0)
        index = g_playing_index - 1;
    else if (forward && g_playing_index + 1 < g_track_count)
        index = g_playing_index + 1;

    if (index >= 0) {
        snd_play(SND_MOVE);
        start_track(index);
    } else if (g_play_mode == PLAY_MODE_SHUFFLE) {
        player_stop();
    }
}

/* --- 曲版 / ミュージックビデオ版の切り替え ------------------------------- */

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

/*
 * 別バージョンに入れ替えて再生し直す。
 * キューの並びと位置は変えない (純正も切り替えで曲順は動かない)。
 * 0=切り替えた / <0=対応する版が無い
 */
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

/* --- プレイリスト --- */

static Screen screen_playlist_tick(void)
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
        if (i == g_track_sel) {
            draw_rect(0, y, SCR_W, ROW_H, C_SEL_BG);
            draw_rect(0, y, 3, ROW_H, C_ACCENT);   /* 左端のアクセント */
        }
        art_draw(t->video_id, 8, y + 2, ROW_H - 4);
        char line[200];
        snprintf(line, sizeof(line), "%s%s",
                 (i == g_playing_index) ? "♪ " : "", t->title);
        text_clipped(30, y + 13, SCR_W - 30 - 48,
                     (i == g_track_sel) ? C_TEXT : C_DIM,
                     (i == g_track_sel) ? 0.72f : 0.65f, line);
        if (t->duration_sec > 0) {
            char dur[16];
            snprintf(dur, sizeof(dur), "%d:%02d",
                     t->duration_sec / 60, t->duration_sec % 60);
            text(SCR_W - 42, y + 13, C_DIM, 0.6f, dur);
        }
        if (store_has(t->video_id))
            text(SCR_W - 56, y + 13, C_DIM, 0.55f, "↓");   /* 保存済み */
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

static Screen screen_offline_tick(void)
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
        if (i == g_off_sel) {
            draw_rect(0, y, SCR_W, ROW_H, C_SEL_BG);
            draw_rect(0, y, 3, ROW_H, C_ACCENT);
        }
        art_draw(t.video_id, 8, y + 2, ROW_H - 4);
        char line[200];
        snprintf(line, sizeof(line), "%s%s", playing ? "♪ " : "", t.title);
        text_clipped(30, y + 13, 230,
                     (i == g_off_sel) ? C_TEXT : C_DIM,
                     (i == g_off_sel) ? 0.72f : 0.65f, line);
        text_clipped(268, y + 13, SCR_W - 268 - 48, C_DIM, 0.55f, t.artist);
        if (t.duration_sec > 0) {
            char dur[16];
            snprintf(dur, sizeof(dur), "%d:%02d",
                     t.duration_sec / 60, t.duration_sec % 60);
            text(SCR_W - 42, y + 13, C_DIM, 0.6f, dur);
        }
        y += ROW_H;
    }
    dl_status_line();
    now_playing_bar();
    gfx_frame_end();
    return SCR_OFFLINE;
}

/* --- プレイヤー --- */

/*
 * 再生中の音声には触れず、ラジオ取得に成功したときだけキューを置き換える。
 * 再生モードは維持し、インデックス依存のシャッフル履歴だけ作り直す。
 */
static int replace_queue_with_radio(void)
{
    if (g_playing_index < 0 || g_playing_index >= g_track_count)
        return -1;

    ApiTrack current = g_tracks[g_playing_index];
    static ApiTrack radio_tracks[API_MAX_TRACKS];
    char radio_title[128];
    int n = api_radio(current.video_id, radio_title, sizeof(radio_title),
                      radio_tracks, API_MAX_TRACKS);
    if (n <= 0)
        return -1;

    int insert_current = strcmp(radio_tracks[0].video_id,
                                current.video_id) != 0;
    int radio_count = n;
    if (insert_current && radio_count >= API_MAX_TRACKS)
        radio_count = API_MAX_TRACKS - 1;

    if (insert_current)
        g_tracks[0] = current;
    memcpy(&g_tracks[insert_current], radio_tracks,
           (size_t)radio_count * sizeof(ApiTrack));
    g_track_count = radio_count + insert_current;
    g_playing_index = 0;
    g_track_sel = 0;
    g_track_scroll = 0;
    snprintf(g_pl_title, sizeof(g_pl_title), "ラジオ: %.88s", current.title);
    shuffle_history_reset();
    return 0;
}

/*
 * 映像の上に重ねる情報。画面を覆いすぎないよう上下の帯だけにする。
 * 本家も動画再生中は情報を最小限にするので、同じ考え方に揃えた。
 */
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
    int minutes = g_sleep_timer_minutes[g_sleep_timer_option];
    if (minutes > 0 && g_sleep_timer_remaining_us > 0) {
        unsigned int sec = (unsigned int)
            ((g_sleep_timer_remaining_us + 999999ULL) / 1000000ULL);
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

/* ボタン 1 つ。選んでいるものは白い丸で塗る (テレビ版と同じ) */
static void panel_button(int index, float x, IconId icon)
{
    int focused = (g_panel_zone == ZONE_BUTTONS && g_panel_btn == index &&
                   g_sheet == SHEET_NONE);
    gfx_circle_fill(x, PANEL_BTN_Y, PANEL_BTN,
                    focused ? 0xFFFFFFFF : 0x66000000);
    gfx_icon(icon, x + 3.0f, PANEL_BTN_Y + 3.0f, PANEL_BTN - 6.0f,
             focused ? C_BG : C_TEXT);
}

/*
 * ボタンの並び。テレビ版から「チャンネル情報・コメント・Gemini」を抜き、
 * 曲そのものへの操作を左端、再生の操作を右端に寄せる。
 */
static void draw_panel_buttons(PlayerState st)
{
    int liked    = (g_info && g_info->rating == RATE_LIKE);
    int disliked = (g_info && g_info->rating == RATE_DISLIKE);
    int saved    = (g_info && g_info->in_library);
    const IconId icons[BTN_COUNT] = {
        liked    ? ICON_THUMB_UP_FILL   : ICON_THUMB_UP,
        disliked ? ICON_THUMB_DOWN_FILL : ICON_THUMB_DOWN,
        saved    ? ICON_BOOKMARK_FILL   : ICON_BOOKMARK,
        ICON_SMART_DISPLAY,
        ICON_SETTINGS,
        ICON_SKIP_PREVIOUS,
        (st == PLAYER_PLAYING) ? ICON_PAUSE : ICON_PLAY_ARROW,
        ICON_SKIP_NEXT,
    };
    for (int i = 0; i < BTN_COUNT; i++) {
        float x = (i < BTN_RIGHT_FIRST)
            ? (float)(MARGIN + i * PANEL_BTN_STEP)
            : (float)(SCR_W - MARGIN - PANEL_BTN -
                      (BTN_COUNT - 1 - i) * PANEL_BTN_STEP);
        panel_button(i, x, icons[i]);
    }
}

/* シークバー。経過を左、全体を右に置く (テレビ版はバーの上に出す) */
static void draw_panel_seek(const ApiTrack *t)
{
    int elapsed = display_elapsed_sec();
    char buf[16];
    snprintf(buf, sizeof(buf), "%d:%02d", elapsed / 60, elapsed % 60);
    text(MARGIN, PANEL_TIME_Y, C_DIM, 0.5f, buf);
    if (t->duration_sec > 0) {
        snprintf(buf, sizeof(buf), "%d:%02d",
                 t->duration_sec / 60, t->duration_sec % 60);
        text((float)(SCR_W - MARGIN) - gfx_text_width(0.5f, buf),
             PANEL_TIME_Y, C_DIM, 0.5f, buf);
    }

    int w = SCR_W - MARGIN * 2;
    draw_rect(MARGIN, PANEL_BAR_Y, w, 2, 0x66FFFFFF);
    if (t->duration_sec <= 0)
        return;
    int done = w * elapsed / t->duration_sec;
    if (done > w) done = w;
    draw_rect(MARGIN, PANEL_BAR_Y, done, 2, C_TEXT);
    if (g_panel_zone == ZONE_SEEK && g_sheet == SHEET_NONE)
        gfx_circle_fill((float)(MARGIN + done) - 5.0f,
                        PANEL_BAR_Y - 4.0f, 10.0f, 0xFFFFFFFF);
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

    for (int i = 0; i < visible && first + i < g_track_count; i++) {
        int idx = first + i;
        float x = (float)(MARGIN + i * PANEL_Q_STEP);
        if (g_panel_zone == ZONE_QUEUE && g_sheet == SHEET_NONE &&
            idx == g_queue_sel)
            gfx_glow(x, PANEL_Q_Y, PANEL_Q, PANEL_Q, 150);
        art_draw_ex(g_tracks[idx].video_id, x, PANEL_Q_Y, PANEL_Q,
                    (idx == g_playing_index) ? 0xFFFFFFFF : C_CARD_DIM);
    }
}

/* 題名とアーティスト。パネルを出している間は画面の一番上へ移す */
static unsigned int g_mq_frames = 0;   /* 流れる題名の位置 (曲ごとに 0 から) */

static void draw_panel_header(const ApiTrack *t, unsigned int frames,
                              const char *note, unsigned int note_color)
{
    const int w = SCR_W - MARGIN * 2;
    marquee_line(MARGIN, PANEL_TITLE_Y, w, C_TEXT, 0.78f, t->title, 1, 0,
                 frames);

    /* 2 行目はアーティスト・アルバム・再生回数。
       一時的なお知らせがあるときは、この行を明け渡す */
    if (note && note[0]) {
        text_clipped(MARGIN, PANEL_SUB_Y, w, note_color, 0.55f, note);
        return;
    }
    char sub[224];
    sub[0] = '\0';
    append_meta(sub, sizeof(sub), t->artist);
    if (g_info) {
        append_meta(sub, sizeof(sub), g_info->album);
        append_meta(sub, sizeof(sub), g_info->views);
    }
    marquee_line(MARGIN, PANEL_SUB_Y, w, C_DIM, 0.55f, sub, 0, 0, frames);
}

/*
 * パネル一式。over_video が真なら映像の上に重ねるので、
 * アートワークは描かない (映像そのものが主役になる)。
 */
static void draw_panel(const ApiTrack *t, PlayerState st,
                       const char *note, unsigned int note_color,
                       int over_video)
{
    if (over_video) {
        /* 映像の上では文字が沈むので、上下だけ暗くする */
        draw_vgrad(0, 0, SCR_W, 64, 0xC8000000, 0x00000000);
        draw_vgrad(0, SCR_H - 130, SCR_W, 130, 0x00000000, 0xD8000000);
    }
    draw_panel_header(t, g_mq_frames, note, note_color);
    if (!over_video) {
        const float ax = ((float)SCR_W - PANEL_ART) / 2.0f;
        gfx_shadow(ax, PANEL_ART_Y, PANEL_ART, PANEL_ART, 0x90);
        art_draw_ex(t->video_id, ax, PANEL_ART_Y, PANEL_ART, 0xFFFFFFFF);
    }
    draw_panel_seek(t);
    draw_panel_buttons(st);
    draw_panel_queue();
}

/*
 * パネルから開くシート (表示 / 設定)。
 * テレビ版は一覧を左に出して再生中の画面を右に縮める。
 * 480x272 では縮めると何も見えなくなるので、暗くした上に一覧だけ重ねる。
 */
#define SHEET_MAX_ITEMS 3

static void draw_sheet(void)
{
    if (g_sheet == SHEET_NONE)
        return;

    /* 題名の行と重ならないよう、下地はしっかり暗くする */
    draw_rect(0, 0, SCR_W, SCR_H, 0xE6000000);

    const char *title, *desc = NULL;
    const char *labels[SHEET_MAX_ITEMS];
    const char *values[SHEET_MAX_ITEMS] = { NULL, NULL, NULL };
    int count;

    if (g_sheet == SHEET_VIEW) {
        title = "表示";
        desc = "音楽の再生中に画面に表示するものを選びます";
        labels[0] = "動画";
        labels[1] = "歌詞";
        labels[2] = "音声";
        count = 3;
    } else {
        title = "設定";
        labels[0] = "画質";
        values[0] = g_low_quality ? "低" : "標準";
        count = 1;
    }

    const int x = MARGIN, w = 260;
    text_bold(x, 74, C_TEXT, 0.8f, title);
    if (desc)
        text(x, 92, C_DIM, 0.52f, desc);

    for (int i = 0; i < count; i++) {
        int y = 102 + i * 26;
        int sel = (i == g_sheet_sel);
        draw_rect(x, y, w, 22, sel ? C_TEXT : 0x40FFFFFF);
        unsigned int col = sel ? C_BG : C_TEXT;
        text(x + 12, y + 16, col, 0.62f, labels[i]);
        if (values[i])
            text((float)(x + w - 12) - gfx_text_width(0.62f, values[i]),
                 y + 16, col, 0.62f, values[i]);
    }
}

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
    if (player_state() != PLAYER_PLAYING)
        return;               /* 音がまだ始まっていないなら基準が無い */
    int vms = video_pts_ms();
    if (vms < 0)
        return;
    int ahead = vms - player_elapsed_ms();
    if (ahead <= 8)
        return;
    if (ahead > 120)
        ahead = 120;
    sceKernelDelayThread((unsigned int)ahead * 1000);
}

static Screen screen_player_tick(void)
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
        /* --- 表示 / 設定 のシート --- */
        int count = (g_sheet == SHEET_VIEW) ? 3 : 1;
        if (g_pressed & PSP_CTRL_UP) {
            snd_play(SND_MOVE);
            g_sheet_sel = (g_sheet_sel + count - 1) % count;
        }
        if (g_pressed & PSP_CTRL_DOWN) {
            snd_play(SND_MOVE);
            g_sheet_sel = (g_sheet_sel + 1) % count;
        }
        if (g_pressed & PSP_CTRL_CIRCLE) {
            snd_play(SND_OK);
            if (g_sheet != SHEET_VIEW) {
                g_low_quality = !g_low_quality;
                video_set_low_quality(g_low_quality);
                /* 流している映像は今の位置から取り直す (音はそのまま) */
                video_release();
            } else if (g_sheet_sel == 1) {
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
                case BTN_SETTINGS: g_sheet = SHEET_SETTINGS; g_sheet_sel = 0; break;
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

    ApiTrack *t = (g_playing_index >= 0) ? &g_tracks[g_playing_index] : NULL;

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
        if (g_panel_open && t)
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

        if (g_panel_open) {
            draw_panel(t, st, note, note_color, 0);
        } else {
            const float ax = ((float)SCR_W - PLAY_ART) / 2.0f;
            gfx_shadow(ax, PLAY_ART_Y, PLAY_ART, PLAY_ART, 0x90);
            art_draw_ex(t->video_id, ax, PLAY_ART_Y, PLAY_ART, 0xFFFFFFFF);

            marquee_line(((float)SCR_W - PLAY_TEXT_W) / 2.0f, PLAY_TITLE_Y,
                         PLAY_TEXT_W, C_TEXT, 0.85f, t->title, 1, 1,
                         g_mq_frames);
            marquee_line(((float)SCR_W - PLAY_TEXT_W) / 2.0f, PLAY_ARTIST_Y,
                         PLAY_TEXT_W, C_DIM, 0.62f, t->artist, 0, 1,
                         g_mq_frames);
            if (note[0])
                text(((float)SCR_W - gfx_text_width(0.58f, note)) / 2.0f,
                     PLAY_NOTE_Y, note_color, 0.58f, note);
        }
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

/* --- 歌詞 --- */

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

static Screen screen_lyrics_tick(void)
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

int main(void)
{
    setup_callbacks();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    srand((unsigned int)sceKernelGetSystemTimeLow());

    net_load_server_config();   /* server.txt があれば接続先を差し替える */
    if (gfx_init() < 0) {
        sceKernelExitGame();
        return 0;
    }
    store_init();               /* オフライン索引 (offline/index.tsv) */
    player_global_init();
    art_init();
    snd_init();
    dl_init();
    trackinfo_init();

    Screen scr = SCR_CONNECT;
    while (g_running) {
        g_demo_screen = (int)scr;
        input_update();
        /*
         * START では終了しない。
         * 本体の作法では終了は HOME ボタンで、システムの確認画面が出る
         * (終了コールバックを登録済みなのでそのまま動く)。
         * ゲーム以外のアプリで START が即終了なのは事故のもと。
         */

        /* スリープタイマーはどの画面にいても満了させ、自動次曲送りも止める */
        if (sleep_timer_tick()) {
            player_stop();
            g_playing_index = -1;
        }

        /* 曲が終わったら、どの画面にいても自動で次の曲へ進む */
        if (player_state() == PLAYER_FINISHED && g_playing_index >= 0) {
            int index = next_track_index();
            if (index >= 0)
                start_track(index);
            else
                player_stop();
        }

#ifdef SHOTDUMP
        /* 画面ごとに1枚だけ、内容が落ち着いたところで書き出しを予約する */
        {
            static int prev = -1, held = 0, done[9] = {0};
            if ((int)scr != prev) { prev = (int)scr; held = 0; }
            held++;
            if (held > 180 && !done[(int)scr]) {
                done[(int)scr] = 1;
                char name[32];
                snprintf(name, sizeof(name), "shot_%d.raw", (int)scr);
                gfx_request_dump(name);
            }
        }
#endif

        switch (scr) {
        case SCR_CONNECT:  scr = screen_connect_tick();  break;
        case SCR_WELCOME:  scr = screen_welcome_tick();  break;
        case SCR_LOGIN:    scr = screen_login_tick();    break;
        case SCR_HOME:     scr = screen_home_tick();     break;
        case SCR_SEARCH:   scr = screen_search_tick();   break;
        case SCR_PLAYLIST: scr = screen_playlist_tick(); break;
        case SCR_PLAYER:   scr = screen_player_tick();   break;
        case SCR_LYRICS:   scr = screen_lyrics_tick();   break;
        case SCR_OFFLINE:  scr = screen_offline_tick();  break;
        }
    }

    login_cancel();
    player_stop();
    dl_shutdown();
    art_shutdown();
    snd_shutdown();
    gfx_shutdown();
    sceKernelExitGame();
    return 0;
}
