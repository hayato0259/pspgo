#ifndef DL_H
#define DL_H

#include "api.h"

/*
 * オフライン用ダウンローダ。
 * UI スレッドが曲を積み、ワーカースレッドが 1 曲ずつ
 * 音声 (MP3) とアートワークを offline/ に保存して store に登録する。
 * 再生と並行して動く (別ソケット・別スレッド)。
 */

int dl_init(void);
void dl_shutdown(void);

/* 1 曲積む。0=積んだ / 1=保存済みか重複でスキップ / <0=行列が満杯 */
int dl_enqueue(const ApiTrack *t);

int dl_pending(void);                /* 処理中を含む残り曲数 */
const char *dl_current_title(void);  /* 処理中の曲名 ("" = 待機中) */

#endif
