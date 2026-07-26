/*
 * PSP Go YouTube Music クライアント — 画面と状態機械
 *
 * 画面遷移: 接続 → ログイン選択 → ログイン(QR) → ホーム → プレイリスト → 再生
 * 操作: 上下=移動 / ○=決定 / ×=戻る / △=一時停止 / L,R=前後の曲
 *       SELECT=ログイン・ログアウト / START=終了
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

PSP_MODULE_INFO("ytmusic", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(14 * 1024);

typedef enum {
    SCR_CONNECT,
    SCR_WELCOME,   /* ログインするか、しないで使うかの選択 */
    SCR_LOGIN,     /* コード表示 + 承認待ち */
    SCR_HOME,
    SCR_PLAYLIST,
    SCR_PLAYER,
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
static int g_repeat_timer = 0;
static int g_demo_screen = -1;  /* AUTODEMO / SHOTDUMP が参照する現在の画面 */

static void input_update(void)
{
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);
    unsigned int now = pad.Buttons;
    g_pressed = now & ~g_prev_buttons;

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
    }
#endif
}

/* --- アプリ状態 --- */
static ApiItem g_home_items[API_MAX_ITEMS];
static int g_home_count = 0;

/*
 * ホームは PC 版 YouTube Music と同じ構造で見せる:
 * セクション見出し + 横並びのカード (アートワーク)。
 * 左右でカード移動、上下でセクション移動。
 */
#define MAX_SECTIONS 12
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

static int g_playing_index = -1; /* g_tracks 内の再生中インデックス */
static int g_auth = 0;
static int g_can_login = 0;      /* サーバーに OAuth クライアントが設定済みか */
static char g_account[64] = "-";
static char g_error[128] = "";
static int g_welcome_sel = 0;    /* 0=ログイン 1=ログインせずに使う */

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
        float ss = is_error ? 0.7f : 0.65f;
        float sw = gfx_text_width(ss, status);
        text((SCR_W - sw) / 2.0f, 168, is_error ? C_ACCENT : C_DIM, ss, status);
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
        draw_splash(g_error, 1);
        text(SCR_W / 2.0f - 40, 230, C_DIM, 0.6f, "START: 終了");
        gfx_frame_end();
        return SCR_CONNECT;
    }

    draw_splash("サーバーに接続しています", 0);
    text(SCR_W / 2.0f - 70, 230, C_DIM, 0.55f, "接続先: " SERVER_HOST);
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
        snprintf(g_error, sizeof(g_error),
                 "サーバーに接続できません (%d)。起動していますか?", rc);
        step = 99;
        return SCR_CONNECT;
    }
    g_auth = (rc > 0);

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
            intraFontSetStyle(gfx_font(), 1.2f, C_TEXT, 0, 0.0f, 0);
            intraFontPrint(gfx_font(), tx, 172, login_user_code());
        } else {
            /* QR が使えない場合はコードを大きく見せる */
            text(40, 28, C_ACCENT, 0.6f, "(QR コードを受信できませんでした)");
            text(40, 62, C_DIM, 0.75f, "スマートフォンや PC で次の URL を開いてください");
            text(56, 86, C_TEXT, 0.9f, login_url());
            text(40, 118, C_DIM, 0.75f, "そこに、このコードを入力してください:");
            draw_rect(56, 130, SCR_W - 112, 44, C_SEL_BG);
            intraFontSetStyle(gfx_font(), 1.7f, C_TEXT, 0, 0.0f,
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
        intraFontSetStyle(gfx_font(), 0.7f, C_DIM, 0, 0.0f, 0);
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

    /*
     * SELECT: 未ログイン時のみログイン画面へ。
     * ログイン済みでのログアウトは割り当てない —
     * PPSSPP の既定キーで SELECT はスペースキーであり、
     * 「再生しようとしてスペースを押す → 即ログアウト」の誤爆が起きたため。
     * ログアウトはサーバー側 (auth/ ディレクトリの削除) で行う。
     */
    if ((g_pressed & PSP_CTRL_SELECT) && !g_auth && g_can_login) {
        g_welcome_sel = 0;
        return SCR_WELCOME;
    }

    ApiItem *cur = selected_card();
    if ((g_pressed & PSP_CTRL_CIRCLE) && cur) {
        snd_play(SND_OK);
        if (cur->kind == 'P') {
            g_track_count = api_playlist(cur->id, g_pl_title, sizeof(g_pl_title),
                                         g_tracks, API_MAX_TRACKS);
            if (g_track_count >= 0) {
                g_track_sel = 0;
                g_track_scroll = 0;
                g_playing_index = -1;
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
            g_playing_index = 0;
            player_start(g_tracks[0].video_id, 0);
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
            intraFontSetStyle(gfx_font(), 0.7f, C_DIM, 0, 0.0f, 0);
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

    now_playing_bar();
    gfx_frame_end();
    return SCR_HOME;
}

static void start_track(int index)
{
    if (index < 0 || index >= g_track_count)
        return;
    g_playing_index = index;
    player_start(g_tracks[index].video_id, g_tracks[index].duration_sec);
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
        return SCR_HOME;
    }
    if ((g_pressed & PSP_CTRL_CIRCLE) && g_track_count > 0) {
        snd_play(SND_OK);
        start_track(g_track_sel);
        return SCR_PLAYER;
    }

    if (g_track_count > 0)
        ui_bg_ambient(art_avg_color(g_tracks[g_track_sel].video_id));
    ui_frame_begin();
    ui_chrome(g_pl_title, "上下: 選択    ○: 再生    ×: 戻る", g_auth, g_account);
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
        y += ROW_H;
    }
    now_playing_bar();
    gfx_frame_end();
    return SCR_PLAYLIST;
}

/* --- プレイヤー --- */

static Screen screen_player_tick(void)
{
    PlayerState st = player_state();

    if (g_pressed & PSP_CTRL_CROSS) {
        snd_play(SND_CANCEL);
        return SCR_PLAYLIST;
    }
    if (g_pressed & PSP_CTRL_TRIANGLE) {
        snd_play(SND_OK);
        player_toggle_pause();
    }
    if ((g_pressed & PSP_CTRL_LTRIGGER) && g_playing_index > 0) {
        snd_play(SND_MOVE);
        start_track(g_playing_index - 1);
    }
    if ((g_pressed & PSP_CTRL_RTRIGGER) && g_playing_index + 1 < g_track_count) {
        snd_play(SND_MOVE);
        start_track(g_playing_index + 1);
    }

    ApiTrack *t = (g_playing_index >= 0) ? &g_tracks[g_playing_index] : NULL;

    ui_bg_ambient(t ? art_avg_color(t->video_id) : 0);
    ui_frame_begin();
    ui_chrome("再生中", "△: 一時停止    L/R: 前後の曲    ×: 一覧へ",
              g_auth, g_account);
    if (t) {
        /* 左に大きなアートワーク (柔らかい影のみ、枠なし)、右に曲情報 */
        gfx_shadow(20, 48, 128, 128, 0x90);
        art_draw(t->video_id, 20, 48, 128);

        const int tx = 168;
        intraFontSetStyle(gfx_font(), 0.88f, C_TEXT, 0, 0.0f, 0);
        intraFontPrintColumn(gfx_font(), tx, 70, SCR_W - tx - 16, t->title);
        intraFontSetStyle(gfx_font(), 0.62f, C_DIM, 0, 0.0f, 0);
        intraFontPrintColumn(gfx_font(), tx, 104, SCR_W - tx - 16, t->artist);

        const char *st_label =
            (st == PLAYER_BUFFERING) ? "バッファリング中..." :
            (st == PLAYER_PAUSED)    ? "一時停止" :
            (st == PLAYER_ERROR)     ? "エラー" :
            (st == PLAYER_PLAYING)   ? "再生中" : "";
        text(tx, 132, (st == PLAYER_ERROR) ? C_ACCENT : C_DIM, 0.62f, st_label);
        if (st == PLAYER_ERROR) {
            char e[64];
            snprintf(e, sizeof(e), "code: 0x%08X", player_last_error());
            text(tx, 152, C_DIM, 0.6f, e);
        }

        /* 進捗バー (細いトラック + 赤 + 角丸のつまみ) */
        int elapsed = player_elapsed_sec();
        char tm[32];
        if (t->duration_sec > 0) {
            snprintf(tm, sizeof(tm), "%d:%02d / %d:%02d",
                     elapsed / 60, elapsed % 60,
                     t->duration_sec / 60, t->duration_sec % 60);
            int w = (SCR_W - 48) * elapsed / t->duration_sec;
            if (w > SCR_W - 48) w = SCR_W - 48;
            draw_rect(24, 202, SCR_W - 48, 3, C_LINE);
            draw_rect(24, 202, w, 3, C_ACCENT);
            gfx_card_fill(24 + w - 4, 199, 9, 0xFFFFFFFF);   /* つまみ */
        } else {
            snprintf(tm, sizeof(tm), "%d:%02d", elapsed / 60, elapsed % 60);
        }
        text(24, 222, C_DIM, 0.65f, tm);

        if (g_playing_index + 1 < g_track_count) {
            char next[160];
            snprintf(next, sizeof(next), "次の曲: %s",
                     g_tracks[g_playing_index + 1].title);
            text_clipped(24, 243, SCR_W - 48, C_DIM, 0.6f, next);
        }
    } else {
        text(24, 120, C_DIM, 0.75f, "(再生していません)");
    }
    gfx_frame_end();
    return SCR_PLAYER;
}

int main(void)
{
    setup_callbacks();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

    if (gfx_init() < 0) {
        sceKernelExitGame();
        return 0;
    }
    player_global_init();
    art_init();
    snd_init();

    Screen scr = SCR_CONNECT;
    while (g_running) {
        g_demo_screen = (int)scr;
        input_update();
        if (g_pressed & PSP_CTRL_START)
            break;

        /* 曲が終わったら、どの画面にいても自動で次の曲へ進む */
        if (player_state() == PLAYER_FINISHED && g_playing_index >= 0) {
            if (g_playing_index + 1 < g_track_count)
                start_track(g_playing_index + 1);
            else
                player_stop();
        }

#ifdef SHOTDUMP
        /* 画面ごとに1枚だけ、内容が落ち着いたところで書き出しを予約する */
        {
            static int prev = -1, held = 0, done[8] = {0};
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
        case SCR_PLAYLIST: scr = screen_playlist_tick(); break;
        case SCR_PLAYER:   scr = screen_player_tick();   break;
        }
    }

    login_cancel();
    player_stop();
    art_shutdown();
    snd_shutdown();
    gfx_shutdown();
    sceKernelExitGame();
    return 0;
}
