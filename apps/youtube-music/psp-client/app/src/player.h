#ifndef PLAYER_H
#define PLAYER_H

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

/* video_id のストリーム再生を開始 (既存の再生は停止される)。0=成功 */
int player_start(const char *video_id);

void player_stop(void);
void player_toggle_pause(void);

PlayerState player_state(void);
int player_elapsed_sec(void);
int player_last_error(void);

#endif
