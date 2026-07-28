#ifndef TRACKINFO_H
#define TRACKINFO_H

#include "api.h"

/*
 * 再生中の曲の付随情報 (対応版・評価) を、UI を止めずに取ってくる。
 *
 * 問い合わせは 1 曲あたり数百ミリ秒から数秒かかる。これを描画スレッドで
 * 直接呼ぶと、その間ボタンも絵も止まって「固まった」ように見えるため、
 * 専用のスレッドに逃がしている (ダウンロード処理と同じ考え方)。
 */

/* ワーカースレッドを起こす。起動時に 1 回だけ呼ぶ */
void trackinfo_init(void);

/*
 * この曲の対応バージョンを背後で取りに行かせる。
 * 同じ videoId を続けて渡しても問い合わせは 1 回だけ。毎フレーム呼んでよい。
 */
void trackinfo_request(const char *video_id);

/*
 * video_id について取得が終わっていれば結果を返す。
 * まだ取得中、または別の曲の結果しか無ければ NULL。
 * 返る領域は次の取得完了まで有効。
 */
const ApiTrackInfo *trackinfo_result(const char *video_id);

/*
 * 評価を付けたあと、取得済みの内容にも反映する。
 * サーバーへの反映は済んでいるので、次の取得を待たずに画面へ出すために使う。
 */
void trackinfo_set_rating(const char *video_id, ApiRating rating);

/* 保存の結果を手元の記憶に反映する (取り直しを待たずに絵を変えるため) */
void trackinfo_set_library(const char *video_id, int in_library);

#endif
