#ifndef COUNTERPART_H
#define COUNTERPART_H

#include "api.h"

/*
 * 曲版とミュージックビデオ版の対応を、UI を止めずに取ってくる。
 *
 * 問い合わせは 1 曲あたり数百ミリ秒から数秒かかる。これを描画スレッドで
 * 直接呼ぶと、その間ボタンも絵も止まって「固まった」ように見えるため、
 * 専用のスレッドに逃がしている (ダウンロード処理と同じ考え方)。
 */

/* ワーカースレッドを起こす。起動時に 1 回だけ呼ぶ */
void counterpart_init(void);

/*
 * この曲の対応バージョンを背後で取りに行かせる。
 * 同じ videoId を続けて渡しても問い合わせは 1 回だけ。毎フレーム呼んでよい。
 */
void counterpart_request(const char *video_id);

/*
 * video_id について取得が終わっていれば結果を返す。
 * まだ取得中、または別の曲の結果しか無ければ NULL。
 * 返る領域は次の取得完了まで有効。
 */
const ApiCounterpart *counterpart_result(const char *video_id);

#endif
