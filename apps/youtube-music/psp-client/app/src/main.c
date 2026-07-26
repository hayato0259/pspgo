/*
 * PSP Go YouTube Music クライアント
 *
 * 画面遷移: 接続 → ホーム (プレイリスト一覧) → プレイリスト (曲一覧) → 再生
 * 操作: 上下=移動 / ○=決定 / ×=戻る / △=一時停止 / L,R=前後の曲 / START=終了
 *
 * 「ログイン」はサーバー側の browser.json で行われ、
 * この画面ではその認証状態とアカウント名を表示する (接続画面)。
 */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <intraFont.h>
#include <string.h>
#include <stdio.h>
#include "common.h"
#include "net.h"
#include "api.h"
#include "player.h"

PSP_MODULE_INFO("ytmusic", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(14 * 1024);

/* --- 色 --- */
#define C_BG      0xFF201510   /* ABGR: 暗い青灰 */
#define C_TEXT    0xFFEEEEEE
#define C_DIM     0xFF999999
#define C_ACCENT  0xFF4040FF   /* 赤 (ABGR) */
#define C_SEL_BG  0xFF483020
#define C_HEADER  0xFF66CCFF   /* 黄 */

typedef enum {
    SCR_CONNECT,
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

/* --- 描画基盤 --- */
static unsigned int __attribute__((aligned(16))) g_gu_list[262144];
static intraFont *g_font = NULL, *g_font_jpn = NULL;

static void gu_init(void)
{
    sceGuInit();
    sceGuStart(GU_DIRECT, g_gu_list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, 512);
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
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

/*
 * 実機は flash0:/font/ に PGF フォントがある。
 * PPSSPP は flash0 が仮想FSで intraFont から開けないことがあるため、
 * アプリ同梱パス (相対) もフォールバックとして試す。
 * ※同梱フォントはリポジトリにコミットしない (著作物)。README 参照。
 */
static const char *FONT_LTN[] = { "flash0:/font/ltn8.pgf", "font/ltn8.pgf", NULL };
static const char *FONT_JPN[] = { "flash0:/font/jpn0.pgf", "font/jpn0.pgf", NULL };

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
    g_font = load_first(FONT_LTN);
    g_font_jpn = load_first(FONT_JPN);
    if (!g_font)
        return -2;
    if (g_font_jpn) {
        intraFontSetAltFont(g_font, g_font_jpn); /* 日本語グリフはこちらへフォールバック */
    }
    return 0;
}

static void frame_begin(void)
{
    sceGuStart(GU_DIRECT, g_gu_list);
    sceGuClearColor(C_BG);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
}

static void frame_end(void)
{
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

typedef struct { unsigned int color; short x, y, z; } RectVtx;

static void draw_rect(int x, int y, int w, int h, unsigned int color)
{
    RectVtx *v = sceGuGetMemory(2 * sizeof(RectVtx));
    v[0].color = color; v[0].x = x;     v[0].y = y;     v[0].z = 0;
    v[1].color = color; v[1].x = x + w; v[1].y = y + h; v[1].z = 0;
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_SPRITES,
                   GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
    sceGuEnable(GU_TEXTURE_2D);
}

static void text(float x, float y, unsigned int color, float size, const char *s)
{
    intraFontSetStyle(g_font, size, color, 0, 0.0f, 0);
    intraFontPrint(g_font, x, y, s);
}

/* --- 入力 (エッジ検出) --- */
static unsigned int g_prev_buttons = 0;
static unsigned int g_pressed = 0;
static int g_repeat_timer = 0;

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
    /* E2E 自動検証用: ホーム表示後に自動でプレイリストを開いて再生する */
    {
        static int demo_frame = 0;
        demo_frame++;
        if (demo_frame == 300 || demo_frame == 600)
            g_pressed |= PSP_CTRL_CIRCLE;
    }
#endif
}

/* --- アプリ状態 --- */
static ApiItem g_home_items[API_MAX_ITEMS];
static int g_home_count = 0;
static int g_home_sel = 0, g_home_scroll = 0;

static ApiTrack g_tracks[API_MAX_TRACKS];
static int g_track_count = 0;
static int g_track_sel = 0, g_track_scroll = 0;
static char g_pl_title[128];

static int g_playing_index = -1; /* g_tracks 内の再生中インデックス */
static int g_auth = 0;
static char g_account[64] = "-";
static char g_error[128] = "";

/* --- リスト描画共通 --- */
#define LIST_TOP 42
#define LIST_ROWS 12
#define ROW_H 18

static void scroll_to(int sel, int *scroll)
{
    if (sel < *scroll)
        *scroll = sel;
    if (sel >= *scroll + LIST_ROWS)
        *scroll = sel - LIST_ROWS + 1;
}

static void draw_chrome(const char *title, const char *hint)
{
    draw_rect(0, 0, SCR_W, 24, 0xFF2A1A10);
    draw_rect(0, 24, SCR_W, 1, C_ACCENT);
    text(8, 17, C_TEXT, 0.9f, title);
    if (g_auth)
        text(SCR_W - 8 - intraFontMeasureText(g_font, g_account) * 0.7f, 17,
             C_DIM, 0.7f, g_account);
    draw_rect(0, SCR_H - 18, SCR_W, 18, 0xFF2A1A10);
    text(8, SCR_H - 5, C_DIM, 0.65f, hint);
}

/* 再生中ミニ表示 (下部バーの上) */
static void draw_now_playing_bar(void)
{
    PlayerState st = player_state();
    if (st == PLAYER_STOPPED || st == PLAYER_ERROR || g_playing_index < 0)
        return;
    ApiTrack *t = &g_tracks[g_playing_index];
    char line[160];
    const char *mark = (st == PLAYER_PAUSED) ? "||" :
                       (st == PLAYER_BUFFERING) ? ".." : ">";
    snprintf(line, sizeof(line), "%s %s - %s  [%d:%02d]",
             mark, t->title, t->artist,
             player_elapsed_sec() / 60, player_elapsed_sec() % 60);
    draw_rect(0, SCR_H - 36, SCR_W, 18, 0xFF351F12);
    text(8, SCR_H - 23, C_HEADER, 0.65f, line);
}

/* --- 各画面 --- */

static Screen screen_connect_tick(void)
{
    frame_begin();
    draw_chrome("YouTube Music for PSP", "setsuzoku-chuu...");
    text(SCR_W / 2 - 60, 120, C_TEXT, 0.9f, "Connecting...");
    text(SCR_W / 2 - 110, 145, C_DIM, 0.7f, "server: " SERVER_HOST);
    frame_end();

    /* 初回のみ実処理 (画面を1度描いてから) */
    static int step = 0;
    if (step == 0) {
        step = 1;
        return SCR_CONNECT; /* 1フレーム描画してから接続処理へ */
    }
    if (step == 1) {
        int rc = net_init();
        if (rc < 0) {
            snprintf(g_error, sizeof(g_error), "Wi-Fi error: 0x%08X", rc);
            step = 99;
            return SCR_CONNECT;
        }
        rc = api_status(g_account, sizeof(g_account));
        if (rc < 0) {
            snprintf(g_error, sizeof(g_error),
                     "server error: %d (server kidou zumi?)", rc);
            step = 99;
            return SCR_CONNECT;
        }
        g_auth = (rc > 0);
        g_home_count = api_home(g_home_items, API_MAX_ITEMS);
        if (g_home_count < 0) {
            snprintf(g_error, sizeof(g_error), "home error: %d", g_home_count);
            step = 99;
            return SCR_CONNECT;
        }
        /* 最初の選択可能行へ */
        g_home_sel = 0;
        while (g_home_sel < g_home_count && g_home_items[g_home_sel].kind == 'S')
            g_home_sel++;
        return SCR_HOME;
    }
    /* step 99: エラー表示で停止 */
    frame_begin();
    draw_chrome("YouTube Music for PSP", "START = exit");
    text(30, 120, C_ACCENT, 0.8f, g_error);
    frame_end();
    return SCR_CONNECT;
}

static Screen screen_home_tick(void)
{
    if (g_pressed & PSP_CTRL_UP) {
        int i = g_home_sel - 1;
        while (i >= 0 && g_home_items[i].kind == 'S') i--;
        if (i >= 0) g_home_sel = i;
    }
    if (g_pressed & PSP_CTRL_DOWN) {
        int i = g_home_sel + 1;
        while (i < g_home_count && g_home_items[i].kind == 'S') i++;
        if (i < g_home_count) g_home_sel = i;
    }
    scroll_to(g_home_sel, &g_home_scroll);

    if ((g_pressed & PSP_CTRL_CIRCLE) && g_home_count > 0) {
        ApiItem *it = &g_home_items[g_home_sel];
        if (it->kind == 'P') {
            g_track_count = api_playlist(it->id, g_pl_title, sizeof(g_pl_title),
                                         g_tracks, API_MAX_TRACKS);
            if (g_track_count >= 0) {
                g_track_sel = 0;
                g_track_scroll = 0;
                g_playing_index = -1;
                return SCR_PLAYLIST;
            }
        } else if (it->kind == 'V') {
            /* 単曲: 1曲だけの擬似プレイリストにする */
            snprintf(g_pl_title, sizeof(g_pl_title), "%s", it->title);
            snprintf(g_tracks[0].video_id, sizeof(g_tracks[0].video_id), "%s", it->id);
            snprintf(g_tracks[0].title, sizeof(g_tracks[0].title), "%s", it->title);
            snprintf(g_tracks[0].artist, sizeof(g_tracks[0].artist), "%s", it->subtitle);
            g_tracks[0].duration_sec = 0;
            g_track_count = 1;
            g_track_sel = 0;
            g_playing_index = 0;
            player_start(g_tracks[0].video_id);
            return SCR_PLAYER;
        }
    }

    frame_begin();
    draw_chrome(g_auth ? "Home (login zumi)" : "Home (mi-login)",
                "^v: sentaku  O: hiraku  START: exit");
    int y = LIST_TOP;
    for (int i = g_home_scroll;
         i < g_home_count && i < g_home_scroll + LIST_ROWS; i++) {
        ApiItem *it = &g_home_items[i];
        if (it->kind == 'S') {
            text(8, y + 13, C_HEADER, 0.75f, it->title);
        } else {
            if (i == g_home_sel)
                draw_rect(0, y + 1, SCR_W, ROW_H - 1, C_SEL_BG);
            text(20, y + 13, (i == g_home_sel) ? C_TEXT : C_DIM, 0.8f, it->title);
        }
        y += ROW_H;
    }
    draw_now_playing_bar();
    frame_end();
    return SCR_HOME;
}

static void start_track(int index)
{
    if (index < 0 || index >= g_track_count)
        return;
    g_playing_index = index;
    player_start(g_tracks[index].video_id);
}

static Screen screen_playlist_tick(void)
{
    if ((g_pressed & PSP_CTRL_UP) && g_track_sel > 0)
        g_track_sel--;
    if ((g_pressed & PSP_CTRL_DOWN) && g_track_sel < g_track_count - 1)
        g_track_sel++;
    scroll_to(g_track_sel, &g_track_scroll);

    if (g_pressed & PSP_CTRL_CROSS)
        return SCR_HOME;
    if ((g_pressed & PSP_CTRL_CIRCLE) && g_track_count > 0) {
        start_track(g_track_sel);
        return SCR_PLAYER;
    }

    frame_begin();
    draw_chrome(g_pl_title, "^v: sentaku  O: saisei  X: modoru");
    int y = LIST_TOP;
    for (int i = g_track_scroll;
         i < g_track_count && i < g_track_scroll + LIST_ROWS; i++) {
        ApiTrack *t = &g_tracks[i];
        if (i == g_track_sel)
            draw_rect(0, y + 1, SCR_W, ROW_H - 1, C_SEL_BG);
        char line[200];
        snprintf(line, sizeof(line), "%s%s",
                 (i == g_playing_index) ? "> " : "", t->title);
        text(10, y + 13, (i == g_track_sel) ? C_TEXT : C_DIM, 0.8f, line);
        if (t->duration_sec > 0) {
            char dur[16];
            snprintf(dur, sizeof(dur), "%d:%02d",
                     t->duration_sec / 60, t->duration_sec % 60);
            text(SCR_W - 44, y + 13, C_DIM, 0.7f, dur);
        }
        y += ROW_H;
    }
    draw_now_playing_bar();
    frame_end();
    return SCR_PLAYLIST;
}

static Screen screen_player_tick(void)
{
    PlayerState st = player_state();

    /* 曲が終わったら次の曲へ */
    if (st == PLAYER_FINISHED && g_playing_index >= 0) {
        if (g_playing_index + 1 < g_track_count)
            start_track(g_playing_index + 1);
        else
            player_stop();
    }

    if (g_pressed & PSP_CTRL_CROSS)
        return SCR_PLAYLIST;
    if (g_pressed & PSP_CTRL_TRIANGLE)
        player_toggle_pause();
    if ((g_pressed & PSP_CTRL_LTRIGGER) && g_playing_index > 0)
        start_track(g_playing_index - 1);
    if ((g_pressed & PSP_CTRL_RTRIGGER) && g_playing_index + 1 < g_track_count)
        start_track(g_playing_index + 1);

    ApiTrack *t = (g_playing_index >= 0) ? &g_tracks[g_playing_index] : NULL;

    frame_begin();
    draw_chrome("Now Playing",
                "^: pause  L/R: mae/tsugi  X: ichiran e");
    if (t) {
        intraFontSetStyle(g_font, 1.1f, C_TEXT, 0, 0.0f, 0);
        intraFontPrintColumn(g_font, 24, 90, SCR_W - 48, t->title);
        text(24, 120, C_DIM, 0.85f, t->artist);

        const char *st_label =
            (st == PLAYER_BUFFERING) ? "Buffering..." :
            (st == PLAYER_PAUSED)    ? "Paused" :
            (st == PLAYER_ERROR)     ? "Error" :
            (st == PLAYER_PLAYING)   ? "Playing" : "";
        text(24, 150, (st == PLAYER_ERROR) ? C_ACCENT : C_HEADER, 0.8f, st_label);
        if (st == PLAYER_ERROR) {
            char e[64];
            snprintf(e, sizeof(e), "code: 0x%08X", player_last_error());
            text(24, 170, C_DIM, 0.7f, e);
        }

        /* 進捗バー */
        int elapsed = player_elapsed_sec();
        char tm[32];
        if (t->duration_sec > 0) {
            snprintf(tm, sizeof(tm), "%d:%02d / %d:%02d",
                     elapsed / 60, elapsed % 60,
                     t->duration_sec / 60, t->duration_sec % 60);
            int w = (SCR_W - 48) * elapsed / t->duration_sec;
            if (w > SCR_W - 48) w = SCR_W - 48;
            draw_rect(24, 200, SCR_W - 48, 6, 0xFF404040);
            draw_rect(24, 200, w, 6, C_ACCENT);
        } else {
            snprintf(tm, sizeof(tm), "%d:%02d", elapsed / 60, elapsed % 60);
        }
        text(24, 222, C_DIM, 0.75f, tm);

        if (g_playing_index + 1 < g_track_count) {
            char next[160];
            snprintf(next, sizeof(next), "tsugi: %s",
                     g_tracks[g_playing_index + 1].title);
            text(24, 244, C_DIM, 0.7f, next);
        }
    } else {
        text(24, 120, C_DIM, 0.9f, "(saisei shiteimasen)");
    }
    frame_end();
    return SCR_PLAYER;
}

int main(void)
{
    setup_callbacks();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

    gu_init();
    if (font_init() < 0) {
        sceKernelExitGame();
        return 0;
    }
    player_global_init();

    Screen scr = SCR_CONNECT;
    while (g_running) {
        input_update();
        if (g_pressed & PSP_CTRL_START)
            break;

        switch (scr) {
        case SCR_CONNECT:  scr = screen_connect_tick();  break;
        case SCR_HOME:     scr = screen_home_tick();     break;
        case SCR_PLAYLIST: scr = screen_playlist_tick(); break;
        case SCR_PLAYER:   scr = screen_player_tick();   break;
        }
    }

    player_stop();
    intraFontUnload(g_font);
    if (g_font_jpn)
        intraFontUnload(g_font_jpn);
    intraFontShutdown();
    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
