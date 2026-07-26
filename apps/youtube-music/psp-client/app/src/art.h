#ifndef ART_H
#define ART_H

#define ART_SIDE 64   /* テクスチャの辺。PSP は 2 の冪が必要 */

/* 読み込みスレッドを起動する。0=成功 */
int art_init(void);
void art_shutdown(void);

/*
 * id のアートワークを画面に描く。
 * まだ取得できていない場合はプレースホルダを描き、
 * 裏で読み込みを予約する (UI は待たない)。
 */
void art_draw(const char *id, int x, int y, int size);

/*
 * 読み込み済みアートワークの平均色 (ABGR)。背景の環境光に使う。
 * まだ読み込めていない場合は 0 を返す。
 */
unsigned int art_avg_color(const char *id);

#endif
