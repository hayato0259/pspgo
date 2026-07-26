#ifndef STORE_H
#define STORE_H

#include "api.h"

/*
 * オフライン ライブラリ: EBOOT と同じフォルダの offline/ 配下に
 *   <videoId>.mp3   変換済み音声 (サーバーが配信するものと同一)
 *   <videoId>.art   64x64 RGBA 生ピクセル (アートワーク)
 *   index.tsv       曲の一覧 (videoId \t 曲名 \t アーティスト \t 秒)
 * を置き、ネットワーク無しでも再生できるようにする。
 *
 * UI スレッドとダウンロードスレッドの両方から呼ばれるため内部でロックする。
 */

#define STORE_MAX 500

int store_init(void);          /* index.tsv を読み込む。0=成功 */

int store_count(void);
/* i 番目の曲をコピーして返す。0=成功 (ロック中のポインタを外へ出さない) */
int store_get(int i, ApiTrack *out);
int store_has(const char *video_id);

/* ファイルを書き終えた曲を索引に追加し index.tsv を更新する。0=成功 */
int store_add(const ApiTrack *t);
/* i 番目の曲をファイルごと削除する。0=成功 */
int store_remove(int i);

void store_mp3_path(const char *video_id, char *buf, int size);
void store_art_path(const char *id, char *buf, int size);

#endif
