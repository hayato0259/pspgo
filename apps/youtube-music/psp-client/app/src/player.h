#ifndef PLAYER_H
#define PLAYER_H

/* サーバーが 1 バイトも返さなかった (その曲を取得できなかった) 場合の印。
   生のデコーダのエラー番号と区別して、画面に理由を出すために使う */
#define PLAYER_ERR_NO_DATA (-2000)

typedef enum {
    PLAYER_STOPPED = 0,
    PLAYER_BUFFERING,
    PLAYER_PLAYING,
    PLAYER_PAUSED,
    PLAYER_FINISHED,   /* 曲の終端まで再生した (次曲へ進んでよい) */
    PLAYER_ERROR,
} PlayerState;

/* 起動時に1回。AVモジュールのロードと sceMp3InitResource。0=成功 */
int player_global_init(void);

/* video_id のストリーム再生を開始 (既存の再生は停止される)。0=成功
   duration_hint_sec は終端検知の補助 (不明なら 0) */
/* start_sec から再生する (0 = 先頭)。シークはこの引数で行う */
int player_start(const char *video_id, int duration_hint_sec, int start_sec);

/*
 * 1 を渡すと、受信は続けたまま音を鳴らし始めない。
 * 映像の用意が整うまで音を進めないために使う
 * (先に音だけ進むと、映像が追いつくまで早送りに見えるため)。
 */
void player_gate(int hold);

void player_stop(void);
void player_toggle_pause(void);

PlayerState player_state(void);
int player_elapsed_sec(void);
/* 再生位置をミリ秒で返す。映像を音に合わせるときは秒では粗すぎるため */
int player_elapsed_ms(void);
int player_last_error(void);

#endif
