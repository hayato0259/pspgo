/*
 * PSP Go YouTube Music クライアント — 入力とメインループ。
 *
 * 画面遷移: 接続 → ログイン選択 → ログイン(QR) → ホーム → プレイリスト → 再生
 *           ホーム/接続失敗 → オフライン ライブラリ (ダウンロード済みの曲)
 * 操作: 上下=移動 / ○=決定 / ×=戻る / △=一時停止 / L,R=前後の曲
 *       □=ダウンロード (オフライン画面では削除) / SELECT=検索
 *       終了は HOME (本体のシステム画面が出る)
 *
 * 各画面は screen_*.c (app.h 参照)、再生キューは queue.c、
 * 描画プリミティブは gfx.c、共通 UI 部品は ui.c、色とレイアウトは theme.h。
 */
#include <pspkernel.h>
#include <pspctrl.h>
#include <stdlib.h>
#include <stdio.h>
#include "app.h"
#include "queue.h"
#include "common.h"
#include "gfx.h"
#include "net.h"
#include "player.h"
#include "login.h"
#include "art.h"
#include "snd.h"
#include "store.h"
#include "dl.h"
#include "trackinfo.h"
#include "prefetch.h"

PSP_MODULE_INFO("ytmusic", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(14 * 1024);

/* --- 画面間で共有するアプリ状態 (app.h) --- */
unsigned int g_pressed = 0;
unsigned int g_pressed_edge = 0;
int g_auth = 0;
int g_can_login = 0;
int g_net_ok = 0;
char g_account[64] = "-";
char g_error[192] = "";
int g_playlist_from_search = 0;

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
    prefetch_init();

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

        /* 次に流れる曲をサーバーに先読みさせる (曲間の待ちを消す) */
        queue_prefetch_tick();

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
