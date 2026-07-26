/*
 * PSP Go YouTube Music クライアント
 *
 * 画面遷移: 接続 → ログイン選択 → ログイン(QR) → ホーム → プレイリスト → 再生
 * 操作: 上下=移動 / ○=決定 / ×=戻る / △=一時停止 / L,R=前後の曲
 *       SELECT=ログイン・ログアウト / START=終了
 *
 * ログインは OAuth デバイスコードフロー。Google との通信はサーバーが行い、
 * このアプリは QR コードと入力コードを表示して承認完了を待つだけ。
 */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <pspge.h>
#include <intraFont.h>
#include <string.h>
#include <stdio.h>
#ifdef SHOTDUMP
#include <pspdisplay.h>
#endif
#include "common.h"
#include "net.h"
#include "api.h"
#include "player.h"
#include "login.h"

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

/* --- 描画基盤 --- */
static unsigned int __attribute__((aligned(16))) g_gu_list[262144];
static intraFont *g_font = NULL, *g_font_jpn = NULL;

#ifdef SHOTDUMP
/* 目視確認用のオフスクリーン描画先 (メインメモリ) */
static unsigned int __attribute__((aligned(64))) g_sysfb[512 * SCR_H];
#endif

static void gu_init(void)
{
    sceGuInit();
    sceGuStart(GU_DIRECT, g_gu_list);
#ifdef SHOTDUMP
    /*
     * ダンプ用ビルドでは描画先をメインメモリ上の配列にする。
     * VRAM は GPU 側の都合で CPU から読んだ内容が最新とは限らないが、
     * メインメモリなら描画結果をそのまま読み出せる。
     */
    sceGuDrawBuffer(GU_PSM_8888, g_sysfb, 512);
#else
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, 512);
#endif
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
 *
 * 重要: 日本語フォント (jpn0.pgf) を「主フォント」にする。
 * ラテンフォントを主にして altFont で日本語へ逃がす構成だと、
 * 連続する日本語文字が 1 文字目以降描画されない
 * (intraFont の代替フォント切り替えの問題)。
 * jpn0.pgf は ASCII も含むため、これ 1 本で日本語と英数字を両方描ける。
 */
static const char *FONT_JPN[] = { "flash0:/font/jpn0.pgf", "font/jpn0.pgf", NULL };
static const char *FONT_LTN[] = { "flash0:/font/ltn8.pgf", "font/ltn8.pgf", NULL };

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

    g_font = load_first(FONT_JPN);       /* 日本語フォントを主に使う */
    if (!g_font) {
        g_font = load_first(FONT_LTN);   /* 日本語フォントが無い環境向けの保険 */
        return g_font ? 0 : -2;
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

#ifdef SHOTDUMP
/*
 * 開発時の目視確認用。
 * ダブルバッファのどちらに描いたかを自分で追跡し、スワップ前に
 * 「今描き終えたバッファ」を raw で書き出す。表示中バッファを
 * 後から読む方法では、どちらを掴んだか確定できず古いフレームが出てしまう。
 */
static char g_dump_req[32] = "";      /* 次の frame_end で書き出すファイル名 */

static void dump_drawn_buffer(void)
{
    /* GPU が書いた内容を読むため、CPU キャッシュを捨ててから読む */
    sceKernelDcacheInvalidateRange(g_sysfb, sizeof(g_sysfb));
    FILE *fp = fopen(g_dump_req, "wb");
    if (!fp)
        return;
    fwrite(g_sysfb, 4, (size_t)512 * SCR_H, fp);
    fclose(fp);
}
#endif

static void frame_end(void)
{
    sceGuFinish();
    sceGuSync(0, 0);
#ifdef SHOTDUMP
    if (g_dump_req[0]) {
        dump_drawn_buffer();
        g_dump_req[0] = '\0';
    }
#endif
    sceDisplayWaitVblankStart();
#ifndef SHOTDUMP
    sceGuSwapBuffers();   /* ダンプ用ビルドは常に同じオフスクリーンへ描く */
#endif
}

typedef struct { unsigned int color; short x, y, z; } RectVtx;

static void draw_rect(int x, int y, int w, int h, unsigned int color);

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

/*
 * QR コードを描画する。サーバーから受け取ったモジュール配列 (1バイト1マス) を、
 * 黒マスだけスプライトとして一括描画する。画像デコーダは不要。
 */
static void draw_qr(const unsigned char *qr, int size, int x, int y, int scale)
{
    int quiet = 2 * scale;                 /* QR に必要な余白 */
    int side = size * scale;

    /* 余白を含めた白地 */
    draw_rect(x - quiet, y - quiet, side + quiet * 2, side + quiet * 2, 0xFFFFFFFF);

    /* 黒マスを数える (頂点バッファのサイズ決定用) */
    int dark = 0;
    for (int i = 0; i < size * size; i++)
        if (qr[i])
            dark++;
    if (dark == 0)
        return;

    RectVtx *v = sceGuGetMemory(2 * dark * sizeof(RectVtx));
    int n = 0;
    for (int row = 0; row < size; row++) {
        for (int col = 0; col < size; col++) {
            if (!qr[row * size + col])
                continue;
            short px = (short)(x + col * scale);
            short py = (short)(y + row * scale);
            v[n].color = 0xFF000000; v[n].x = px;         v[n].y = py;         v[n].z = 0; n++;
            v[n].color = 0xFF000000; v[n].x = px + scale; v[n].y = py + scale; v[n].z = 0; n++;
        }
    }
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_SPRITES,
                   GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, n, 0, v);
    sceGuEnable(GU_TEXTURE_2D);
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
static int g_home_sel = 0, g_home_scroll = 0;

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
    g_home_sel = 0;
    g_home_scroll = 0;
    while (g_home_sel < g_home_count && g_home_items[g_home_sel].kind == 'S')
        g_home_sel++;
    return 0;
}

static Screen screen_connect_tick(void)
{
    static int step = 0;

    if (step == 99) { /* エラー表示で停止 */
        frame_begin();
        draw_chrome("YouTube Music for PSP", "START: 終了");
        text(30, 120, C_ACCENT, 0.8f, g_error);
        frame_end();
        return SCR_CONNECT;
    }

    frame_begin();
    draw_chrome("YouTube Music for PSP", "接続中...");
    text(SCR_W / 2 - 60, 120, C_TEXT, 0.9f, "サーバーに接続しています...");
    text(SCR_W / 2 - 110, 145, C_DIM, 0.7f, "接続先: " SERVER_HOST);
    frame_end();

    if (step == 0) {
        step = 1;
        return SCR_CONNECT; /* 1フレーム描画してから接続処理へ */
    }

    int rc = net_init();
    if (rc < 0) {
        snprintf(g_error, sizeof(g_error), "Wi-Fi error: 0x%08X", rc);
        step = 99;
        return SCR_CONNECT;
    }
    rc = api_status(g_account, sizeof(g_account), &g_can_login);
    if (rc < 0) {
        snprintf(g_error, sizeof(g_error),
                 "server error: %d (server kidou zumi?)", rc);
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
    if (g_pressed & (PSP_CTRL_UP | PSP_CTRL_DOWN))
        g_welcome_sel ^= 1;

    if (g_pressed & PSP_CTRL_CIRCLE) {
        if (g_welcome_sel == 0) {
            login_begin();
            return SCR_LOGIN;
        }
        if (load_home() < 0)
            return SCR_WELCOME;
        return SCR_HOME;
    }

    frame_begin();
    draw_chrome("YouTube Music for PSP", "上下: 選択    ○: 決定");
    text(40, 70, C_TEXT, 0.9f, "Google アカウントでログインしますか?");
    text(40, 92, C_DIM, 0.7f,
         "ログインすると、マイミックスなどが表示されます。");

    const char *opts[2] = { "ログインする", "ログインせずに使う" };
    for (int i = 0; i < 2; i++) {
        int y = 130 + i * 30;
        if (i == g_welcome_sel)
            draw_rect(30, y - 14, SCR_W - 60, 24, C_SEL_BG);
        text(44, y + 2, (i == g_welcome_sel) ? C_TEXT : C_DIM, 0.85f, opts[i]);
    }
    text(40, 215, C_DIM, 0.65f,
         "テレビの YouTube アプリと同じ方式です。");
    text(40, 233, C_DIM, 0.65f,
         "画面の QR コードをスマートフォンで読み取ります。");
    frame_end();
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

    frame_begin();
    draw_chrome("ログイン", (st == LOGIN_FAILED) ? "○: やり直す    ×: 戻る"
                                              : "×: 中止");

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
            text(tx, 118, C_HEADER, 0.7f, login_url());
            text(tx, 142, C_DIM, 0.65f, "に、このコードを入力");
            intraFontSetStyle(g_font, 1.2f, C_TEXT, 0, 0.0f, 0);
            intraFontPrint(g_font, tx, 172, login_user_code());
        } else {
            /* QR が使えない場合はコードを大きく見せる */
            text(40, 28, C_ACCENT, 0.6f, "(QR コードを受信できませんでした)");
            text(40, 62, C_DIM, 0.75f, "スマートフォンや PC で次の URL を開いてください");
            text(56, 86, C_HEADER, 0.9f, login_url());
            text(40, 118, C_DIM, 0.75f, "そこに、このコードを入力してください:");
            draw_rect(56, 130, SCR_W - 112, 44, 0xFF3A2418);
            intraFontSetStyle(g_font, 1.7f, C_TEXT, 0, 0.0f, INTRAFONT_ALIGN_CENTER);
            intraFontPrint(g_font, SCR_W / 2, 162, login_user_code());
        }

        text(40, 216, C_DIM, 0.7f, "承認すると自動で次に進みます");
        char rem[96];
        int r = login_remaining_sec();
        snprintf(rem, sizeof(rem), "承認をお待ちしています  (コード有効: %d:%02d)",
                 r / 60, r % 60);
        text(40, 238, C_HEADER, 0.7f, rem);
    } else if (st == LOGIN_FAILED) {
        text(40, 110, C_ACCENT, 0.85f, "ログインに失敗しました");
        intraFontSetStyle(g_font, 0.7f, C_DIM, 0, 0.0f, 0);
        intraFontPrintColumn(g_font, 40, 138, SCR_W - 80, login_message());
    }
    frame_end();
    return SCR_LOGIN;
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

    /* SELECT: ログイン / ログアウトの切り替え */
    if (g_pressed & PSP_CTRL_SELECT) {
        if (g_auth) {
            player_stop();
            api_logout();
            g_auth = 0;
            snprintf(g_account, sizeof(g_account), "-");
            if (g_can_login) {
                g_welcome_sel = 0;
                return SCR_WELCOME;
            }
            load_home();
        } else if (g_can_login) {
            g_welcome_sel = 0;
            return SCR_WELCOME;
        }
    }

    /* ログイン不可 (サーバーに OAuth クライアント未設定) なら案内を出さない */
    const char *hint = g_auth      ? "○: 開く    SELECT: ログアウト    START: 終了"
                     : g_can_login ? "○: 開く    SELECT: ログイン    START: 終了"
                                   : "○: 開く    START: 終了";
    frame_begin();
    draw_chrome(g_auth ? "ホーム" : "ホーム (未ログイン)", hint);

    if (g_home_count == 0) {
        /* 空表示のまま放置せず、理由と次の操作を示す */
        text(24, 80, C_ACCENT, 0.85f, "表示できる項目がありませんでした");
        if (g_error[0]) {
            intraFontSetStyle(g_font, 0.7f, C_DIM, 0, 0.0f, 0);
            intraFontPrintColumn(g_font, 24, 108, SCR_W - 48, g_error);
        }
        text(24, 170, C_DIM, 0.7f, "サーバーのログを確認してください");
        if (g_auth)
            text(24, 192, C_DIM, 0.7f, "SELECT でログアウトすると一般向け表示に戻ります");
        draw_now_playing_bar();
        frame_end();
        return SCR_HOME;
    }

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
    draw_chrome(g_pl_title, "上下: 選択    ○: 再生    ×: 戻る");
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
    draw_chrome("再生中",
                "△: 一時停止    L/R: 前後の曲    ×: 一覧へ");
    if (t) {
        intraFontSetStyle(g_font, 1.1f, C_TEXT, 0, 0.0f, 0);
        intraFontPrintColumn(g_font, 24, 90, SCR_W - 48, t->title);
        text(24, 120, C_DIM, 0.85f, t->artist);

        const char *st_label =
            (st == PLAYER_BUFFERING) ? "バッファリング中..." :
            (st == PLAYER_PAUSED)    ? "一時停止" :
            (st == PLAYER_ERROR)     ? "エラー" :
            (st == PLAYER_PLAYING)   ? "再生中" : "";
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
            snprintf(next, sizeof(next), "次の曲: %s",
                     g_tracks[g_playing_index + 1].title);
            text(24, 244, C_DIM, 0.7f, next);
        }
    } else {
        text(24, 120, C_DIM, 0.9f, "(再生していません)");
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
        g_demo_screen = (int)scr;
        input_update();
        if (g_pressed & PSP_CTRL_START)
            break;

        Screen before = scr;

#ifdef SHOTDUMP
        /* 画面ごとに1枚だけ、内容が落ち着いたところで書き出しを予約する */
        {
            static int prev = -1, held = 0, done[8] = {0};
            if ((int)scr != prev) { prev = (int)scr; held = 0; }
            held++;
            if (held > 180 && !done[(int)scr]) {
                done[(int)scr] = 1;
                snprintf(g_dump_req, sizeof(g_dump_req), "shot_%d.raw", (int)scr);
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

        (void)before;
    }

    login_cancel();
    player_stop();
    intraFontUnload(g_font);
    if (g_font_jpn)
        intraFontUnload(g_font_jpn);
    intraFontShutdown();
    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
