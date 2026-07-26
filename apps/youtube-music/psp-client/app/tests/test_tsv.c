/*
 * TSV パーサのホスト側テスト。
 * PSP 実機やエミュレータを使わずに、サーバー応答の解析を検証する。
 *
 * 実行:
 *   cc -o /tmp/test_tsv tests/test_tsv.c src/tsv.c -I src && /tmp/test_tsv [応答ファイル]
 * 引数に実サーバーの応答ファイルを渡すと、それも解析して結果を出す。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "tsv.h"

static int g_fail = 0;

static void check(int cond, const char *what)
{
    printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond)
        g_fail = 1;
}

static void test_value(void)
{
    const char *buf =
        "code\tABC-DEF-GHJ\n"
        "url\thttps://www.google.com/device\n"
        "interval\t5\n";
    char out[64];

    check(tsv_value(buf, "code", out, sizeof(out)) && strcmp(out, "ABC-DEF-GHJ") == 0,
          "code を取得できる");
    check(tsv_value(buf, "url", out, sizeof(out)) &&
          strcmp(out, "https://www.google.com/device") == 0,
          "url を取得できる");
    check(!tsv_value(buf, "error", out, sizeof(out)), "無い key は 0 を返す");

    /* 部分一致で誤ヒットしないこと (code と codex) */
    const char *b2 = "codex\twrong\ncode\tright\n";
    check(tsv_value(b2, "code", out, sizeof(out)) && strcmp(out, "right") == 0,
          "前方一致で誤ヒットしない");

    /* 値が長すぎる場合は切り詰めて終端する */
    char small[5];
    check(tsv_value(buf, "url", small, sizeof(small)) && strlen(small) == 4,
          "バッファに収まらない値は切り詰める");
}

/* size x size のチェッカーパターンから qr 行を組み立てる */
static char *make_qr_line(int size, int trim)
{
    int need = size * size;
    char *s = malloc(need + 64);
    int n = sprintf(s, "code\tX\nqr\t%d\t", size);
    for (int i = 0; i < need - trim; i++)
        s[n++] = (i % 2) ? '1' : '0';
    s[n++] = '\n';
    s[n] = '\0';
    return s;
}

static void test_qr(void)
{
    unsigned char mod[64 * 64];

    char *ok29 = make_qr_line(29, 0);
    int side = tsv_qr(ok29, mod, 64);
    check(side == 29, "29x29 の QR を解析できる");
    if (side == 29) {
        int dark = 0;
        for (int i = 0; i < 29 * 29; i++)
            dark += mod[i];
        check(dark == (29 * 29) / 2, "黒マスの数が一致する");
    }
    free(ok29);

    /* 途中で切れている場合は 0 (QR なし扱い) */
    char *cut = make_qr_line(29, 10);
    check(tsv_qr(cut, mod, 64) == 0, "データが途中で切れていたら 0");
    free(cut);

    /* 上限を超える辺は拒否する */
    char *big = make_qr_line(40, 0);
    check(tsv_qr(big, mod, 32) == 0, "max_side を超えたら 0");
    check(tsv_qr(big, mod, 64) == 40, "max_side 以内なら受け入れる");
    free(big);

    check(tsv_qr("code\tX\nurl\tY\n", mod, 64) == 0, "qr 行が無ければ 0");
}

/* 実サーバーの応答ファイルを解析してみる */
static void test_real_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        printf("skip  実応答ファイルなし (%s)\n", path);
        return;
    }
    static char buf[64 * 1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);

    printf("\n--- 実サーバー応答 (%zu バイト) ---\n", n);
    char v[128];
    if (tsv_value(buf, "code", v, sizeof(v)))
        printf("  code     = %s\n", v);
    if (tsv_value(buf, "url", v, sizeof(v)))
        printf("  url      = %s\n", v);
    if (tsv_value(buf, "interval", v, sizeof(v)))
        printf("  interval = %s\n", v);

    unsigned char mod[64 * 64];
    int side = tsv_qr(buf, mod, 64);
    printf("  qr       = %d\n", side);
    check(side > 0, "実応答から QR を解析できる");

    if (side > 0) {
        /* 目で見て QR らしいかを確認する (上部 12 行) */
        printf("\n");
        for (int r = 0; r < 12 && r < side; r++) {
            printf("    ");
            for (int c = 0; c < side; c++)
                printf("%s", mod[r * side + c] ? "##" : "  ");
            printf("\n");
        }
    }
}

int main(int argc, char **argv)
{
    test_value();
    test_qr();
    test_real_file(argc > 1 ? argv[1] : "/dev/null");
    printf("\n%s\n", g_fail ? "失敗あり" : "すべて成功");
    return g_fail;
}
