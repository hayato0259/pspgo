/*
 * 起動まわりの画面: 接続 → ログイン選択 → ログイン(QR)。
 *
 * ログインは OAuth デバイスコードフロー。Google との通信はサーバーが行い、
 * このアプリは QR コードと入力コードを表示して承認完了を待つだけ。
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
#include "net.h"
#include "api.h"
#include "login.h"
#include "snd.h"
#include "store.h"

static int g_welcome_sel = 0;    /* 0=ログイン 1=ログインせずに使う */

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

Screen screen_connect_tick(void)
{
    static int step = 0;

    ui_bg_ambient(0);

    if (step == 99) { /* エラー表示で停止 */
        /* ダウンロード済みの曲があれば、接続できなくてもオフラインで使える */
        int off_n = store_count();
        if (off_n > 0 && (g_pressed & PSP_CTRL_CIRCLE)) {
            snd_play(SND_OK);
            offline_reset_cursor();
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
    /* 単曲を選んだときと同じ道を通す (キューの作り方まで確認できる) */
    {
        ApiItem it;
        memset(&it, 0, sizeof(it));
        it.kind = 'V';
        snprintf(it.id, sizeof(it.id), "%s", DEMO_TRACK);
        snprintf(it.title, sizeof(it.title), "%s", DEMO_TITLE);
        snprintf(it.subtitle, sizeof(it.subtitle), "%s", DEMO_ARTIST);
        queue_from_single(&it);
    }
    return SCR_PLAYER;
#endif

    /* 未ログインでログイン可能なら、まず選択画面を出す */
    if (!g_auth && g_can_login) {
        g_welcome_sel = 0;
        return SCR_WELCOME;
    }
    home_load_begin();
    return SCR_HOME;
}

/* --- ログイン選択画面 --- */

Screen screen_welcome_tick(void)
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
        home_load_begin();
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

Screen screen_login_tick(void)
{
    LoginState st = login_state();

    if (st == LOGIN_SUCCESS) {
        /* 認証状態とホームを取り直す */
        int rc = api_status(g_account, sizeof(g_account), &g_can_login);
        g_auth = (rc > 0);
        home_load_begin();
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
