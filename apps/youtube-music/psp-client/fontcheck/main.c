/*
 * フォント検証ツール (開発用)
 *
 * intraFont が日本語文字列を「最後まで」扱えているかを、
 * intraFontMeasureText の返す描画幅で定量的に確認する。
 * 文字が脱落していれば、文字数を増やしても幅がほとんど伸びない。
 *
 * 2 つの構成を比較する:
 *   A) ltn8.pgf を主フォント + jpn0.pgf を代替フォント
 *   B) jpn0.pgf を主フォント (アプリが採用している構成)
 *
 * 結果は fontcheck.txt に書き出す (PPSSPP では
 * ~/.config/ppsspp/PSP/GAME/<app>/fontcheck.txt に出る)。
 */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <intraFont.h>
#include <stdio.h>

PSP_MODULE_INFO("fontcheck", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(12 * 1024);

static const char *SAMPLES[] = {
    "A",
    "ABCDEFGH",
    "\xE3\x82\xB5",                                  /* サ (1文字) */
    "\xE3\x82\xB5\xE3\x83\xBC\xE3\x83\x90\xE3\x83\xBC", /* サーバー (4文字) */
    "\xE6\x8E\xA5\xE7\xB6\x9A\xE4\xB8\xAD",          /* 接続中 (3文字) */
    "\xE3\x83\xAD\xE3\x82\xB0\xE3\x82\xA4\xE3\x83\xB3\xE3\x81\x99\xE3\x82\x8B", /* ログインする (6文字) */
    NULL,
};
static const char *LABELS[] = {
    "ASCII 1文字", "ASCII 8文字", "日本語 1文字", "日本語 4文字",
    "日本語 3文字", "日本語 6文字", NULL,
};

static intraFont *load_first(const char *a, const char *b)
{
    intraFont *f = intraFontLoad(a, INTRAFONT_STRING_UTF8);
    if (!f)
        f = intraFontLoad(b, INTRAFONT_STRING_UTF8);
    return f;
}

static void report(FILE *fp, const char *title, intraFont *font)
{
    fprintf(fp, "[%s]\n", title);
    if (!font) {
        fprintf(fp, "  フォント読み込み失敗\n\n");
        return;
    }
    intraFontSetStyle(font, 1.0f, 0xFFFFFFFF, 0, 0.0f, 0);
    for (int i = 0; SAMPLES[i]; i++) {
        float w = intraFontMeasureText(font, SAMPLES[i]);
        fprintf(fp, "  %-14s width=%7.2f\n", LABELS[i], w);
    }
    fprintf(fp, "\n");
}

int main(void)
{
    intraFontInit();

    FILE *fp = fopen("fontcheck.txt", "w");
    if (!fp) {
        sceKernelExitGame();
        return 0;
    }

    /* 構成A: ラテンを主、日本語を代替に */
    intraFont *ltn = load_first("flash0:/font/ltn8.pgf", "font/ltn8.pgf");
    intraFont *jpn = load_first("flash0:/font/jpn0.pgf", "font/jpn0.pgf");
    if (ltn && jpn)
        intraFontSetAltFont(ltn, jpn);
    report(fp, "A: ltn8 主 + jpn0 代替", ltn);

    /* 構成B: 日本語フォントを主に (アプリの採用構成) */
    intraFont *jpn2 = load_first("flash0:/font/jpn0.pgf", "font/jpn0.pgf");
    report(fp, "B: jpn0 を主フォント", jpn2);

    fclose(fp);
    sceKernelExitGame();
    return 0;
}
