#ifndef APP_H
#define APP_H

/*
 * 画面の識別子と、画面間で共有するアプリ状態。
 *
 * 各画面は screen_*.c に 1 ファイルずつ置き、tick 関数が
 * 「次に表示する画面」を返す。画面固有の状態 (カーソル位置など) は
 * 各ファイルの static に閉じ、ここには画面をまたぐものだけを置く。
 * 再生キューと再生モードは queue.h。
 */

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

/* --- 入力 (main.c が毎フレーム更新する) --- */
extern unsigned int g_pressed;       /* エッジ + 上下キーのリピート */
extern unsigned int g_pressed_edge;  /* エッジのみ (リピートさせたくない操作用) */

/* --- 接続・認証の状態 --- */
extern int g_auth;           /* ログイン済みか */
extern int g_can_login;      /* サーバーに OAuth クライアントが設定済みか */
extern int g_net_ok;         /* サーバーと疎通できたか (オフライン起動の判定) */
extern char g_account[64];   /* アカウントの表示名 */
extern char g_error[192];    /* 画面に見せるエラー文 */

/* 検索から開いたプレイリストか (× で戻る先の判別) */
extern int g_playlist_from_search;

/* --- 各画面の tick (screen_*.c) --- */
Screen screen_connect_tick(void);   /* screen_startup.c */
Screen screen_welcome_tick(void);
Screen screen_login_tick(void);
Screen screen_home_tick(void);      /* screen_home.c */
Screen screen_search_tick(void);    /* screen_search.c */
Screen screen_playlist_tick(void);  /* screen_lists.c */
Screen screen_offline_tick(void);
Screen screen_player_tick(void);    /* screen_player.c */
Screen screen_lyrics_tick(void);

/* ホームの取得を裏で始める (取得中のホーム画面はスケルトンを描く)。
   接続・ログイン画面から、SCR_HOME へ移る直前に呼ぶ */
void home_load_begin(void);

/* 検索入力 (OSK) を開く。first_prompt: 検索画面へ入った直後の入力なら真
   (キャンセル時にホームへ戻すため)。0=成功 */
int begin_search_input(int first_prompt);

/* オフライン画面のカーソルを先頭に戻す (他画面から遷移するときに呼ぶ) */
void offline_reset_cursor(void);

#endif
