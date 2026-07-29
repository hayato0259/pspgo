#ifndef QUEUE_H
#define QUEUE_H

#include "api.h"

/*
 * 再生キューと再生モード、スリープタイマー。
 *
 * キューはアプリに 1 本だけで、どの画面から曲を選んでもここに積まれる。
 * 配列そのものを公開しているのは、プレイリスト画面 (一覧表示・カーソル) と
 * 再生画面 (パネルのキュー表示・版の差し替え) が中身を直接読むため。
 */
extern ApiTrack g_tracks[API_MAX_TRACKS];
extern int g_track_count;
extern int g_track_sel;      /* プレイリスト画面のカーソル */
extern int g_track_scroll;
extern int g_playing_index;  /* g_tracks 内の再生中インデックス (-1 = なし) */
extern char g_pl_title[128]; /* プレイリスト画面の見出し */

typedef enum {
    PLAY_MODE_NORMAL,
    PLAY_MODE_SHUFFLE,
    PLAY_MODE_REPEAT_ALL,
    PLAY_MODE_REPEAT_ONE,
    PLAY_MODE_COUNT
} PlayMode;

extern PlayMode g_play_mode;

/* index の曲を再生する (シャッフル履歴への記録もここで行う) */
void start_track(int index);

/* 前へ / 次へ。L/R トリガーとパネルのボタンの両方から使う */
void skip_track(int forward);

/* 再生モードに従った次の曲。-1 = 続きが無い */
int next_track_index(void);

/* シャッフルの一巡をやり直す (キューを作り直したとき・手動で選び直したとき) */
void shuffle_history_reset(void);

void cycle_play_mode(void);
const char *play_mode_label(void);   /* 通常モードは "" */

/*
 * 単曲からキューを作って再生を始める (本家と同じく、その曲のラジオを続ける)。
 * ラジオが取れなければその 1 曲だけで再生する。
 */
void queue_from_single(const ApiItem *item);

/* 再生中の曲のラジオでキューを置き換える。0=成功 (再生中の音声には触れない) */
int replace_queue_with_radio(void);

/* --- スリープタイマー --- */
void cycle_sleep_timer(void);                    /* オフ→15→30→60→90分 */
int sleep_timer_tick(void);                      /* 満了したフレームで 1 */
int sleep_timer_minutes(void);                   /* 設定値 (0 = オフ) */
unsigned long long sleep_timer_remaining_us(void);

/* 画面下の再生バー (再生していなければ何も描かない)。各画面の描画の最後に呼ぶ */
void now_playing_bar(void);

#endif
