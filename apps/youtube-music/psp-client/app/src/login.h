#ifndef LOGIN_H
#define LOGIN_H

typedef enum {
    LOGIN_IDLE = 0,
    LOGIN_REQUESTING,  /* デバイスコードの取得中 */
    LOGIN_WAITING,     /* コード表示中。ユーザーの承認を待っている */
    LOGIN_SUCCESS,
    LOGIN_FAILED,
} LoginState;

/* ログインを開始 (別スレッドで進行するので UI は固まらない)。0=成功 */
int login_begin(void);

/* 進行中のログインを打ち切る */
void login_cancel(void);

LoginState login_state(void);
const char *login_user_code(void);   /* ユーザーが入力するコード */
const char *login_url(void);         /* コードを入力するURL */
const char *login_message(void);     /* 失敗理由 */
int login_remaining_sec(void);       /* コードの残り有効時間 */

/* QR コードのモジュール配列 (1バイト1モジュール, 行優先)。
   QR が無ければ NULL。size には辺のモジュール数が入る。 */
const unsigned char *login_qr(int *size);

#endif
