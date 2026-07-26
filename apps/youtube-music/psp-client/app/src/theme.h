#ifndef THEME_H
#define THEME_H

/*
 * 配色とレイアウトの唯一の定義場所。
 * 色は PC 版 YouTube Music の実測値 (ほぼ黒 + 白 + ブランド赤)。
 * すべて ABGR (0xAABBGGRR)。
 */

/* --- 色 --- */
#define C_BG        0xFF030303   /* 背景 (本家 #030303) */
#define C_TEXT      0xFFFFFFFF   /* 主テキスト */
#define C_DIM       0xFFAAAAAA   /* サブテキスト (本家 #AAAAAA) */
#define C_ACCENT    0xFF0000FF   /* ブランド赤 #FF0000 */
#define C_SEL_BG    0xFF282828   /* 選択行 (本家のホバー色) */
#define C_LINE      0xFF303030   /* 罫線 */
#define C_CARD_LOAD 0xFF1C1C1C   /* アートワーク読み込み中のプレースホルダ */
#define C_CARD_MISS 0xFF141414   /* アートワークが無い場合 */

/* --- レイアウト (480x272) --- */
#define MARGIN     16    /* 画面端の余白 */

/* ホーム (カルーセル) */
#define CARD_SIZE  62
#define CARD_PITCH 74    /* カード + 12px の間隔 */
#define ROW_TOP    44    /* 1 段目のセクション見出しのベースライン */
#define ROW_PITCH  96    /* セクション 1 段の高さ (見出し+カード+キャプション) */

/* リスト画面 */
#define LIST_TOP  42
#define LIST_ROWS 11
#define ROW_H     18

/* 画面下の再生バー */
#define BAR_H 32

#endif
