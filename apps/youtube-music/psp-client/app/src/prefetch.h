#ifndef PREFETCH_H
#define PREFETCH_H

/*
 * 次に再生する曲の先読み依頼 (/api/prefetch)。
 *
 * サーバーに「次はこの曲」と伝えるだけで、変換とキャッシュはサーバーが行う。
 * 依頼自体は小さな GET 1 回だが、UI スレッドで叩くと接続待ちで描画が
 * 止まるため、専用スレッドから送る。結果は使わない (外れても損しない)。
 */

void prefetch_init(void);                     /* 起動時に 1 回 */
void prefetch_request(const char *video_id);  /* 非同期。同じ id の連投は無視 */

#endif
