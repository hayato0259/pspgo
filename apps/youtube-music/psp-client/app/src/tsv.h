#ifndef TSV_H
#define TSV_H

/*
 * サーバーが返す TSV の解析。PSP 依存を持たないので、
 * ホスト側 (tests/test_tsv.c) でそのままテストできる。
 */

/* key に一致する行の 2 番目のフィールドを out にコピーする。
   見つかれば 1、なければ 0。 */
int tsv_value(const char *buf, const char *key, char *out, int outsize);

/* "qr\t<辺>\t<0/1の羅列>" 行を out (モジュール数分) に展開する。
   戻り値: 辺のモジュール数 / 0 = QR 行なし・不正。
   max_side は out が受けられる辺の最大値。 */
int tsv_qr(const char *buf, unsigned char *out, int max_side);

#endif
