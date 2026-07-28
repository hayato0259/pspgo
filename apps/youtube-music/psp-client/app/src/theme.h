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

/* --- レイアウト (480x272) ---
 * 本家は余白を広く取る作りなので、画面が狭くても詰め込みすぎない。
 * 1 画面に入る件数より、窮屈に見えないことを優先する。
 */
#define MARGIN     20    /* 画面端の余白 */

/* ホーム (カルーセル) */
#define CARD_SIZE  62
#define CARD_PITCH 78    /* カード + 16px の間隔 */
#define ROW_TOP    48    /* 1 段目のセクション見出しのベースライン */
#define ROW_PITCH  92    /* セクション 1 段の高さ */
#define C_CARD_DIM 0xFFA8A8A8   /* 非アクティブ段のカードの減光 tint */

/* 画面下の固定情報パネル (選択中カードの曲情報) */
#define INFO_H       42  /* 再生バーが無いときの高さ (サムネイル付き) */
#define INFO_H_SLIM  28  /* 再生バーと同時に出すときの高さ */

/* リスト画面 */
#define LIST_TOP  46
#define LIST_ROWS 10
#define ROW_H     20

/* 画面下の再生バー */
#define BAR_H 34

/* --- 再生画面 ---
 * テレビ版と同じ中央 1 カラム。上から アートワーク → 題名 → アーティスト。
 * シークバー・再生制御・評価・キューはここには置かず、
 * 十字キーの上下で呼び出すパネルに入れる。
 */
#define PLAY_ART      132  /* アートワークの辺 (横位置は画面中央) */
#define PLAY_ART_Y    40
#define PLAY_TITLE_Y  200  /* 題名のベースライン */
#define PLAY_ARTIST_Y 224
#define PLAY_TEXT_W   360  /* 文字の幅の上限。超えたぶんは横に流す */
#define PLAY_NOTE_Y   250  /* 読み込み中・エラーなどの一時的な行 */

/* --- 再生画面の操作パネル (十字キーの上下で出す) ---
 * 出すとアートワークが上に詰まり、題名とアーティストは画面の一番上へ移る。
 * 下から順に シークバー → ボタン → キュー。
 */
#define PANEL_TITLE_Y  30   /* 題名 (左揃え) */
#define PANEL_SUB_Y    48   /* アーティスト・アルバム・再生回数 */
#define PANEL_ART      88
#define PANEL_ART_Y    58
#define PANEL_TIME_Y   166  /* 経過 / 全体。本家と同じくバーの上に置く */
#define PANEL_BAR_Y    174
#define PANEL_BTN      22   /* ボタンの辺 */
#define PANEL_BTN_Y    184  /* ボタンの上端 */
#define PANEL_BTN_STEP 30   /* ボタンの間隔 */
#define PANEL_Q_Y      216  /* キュー (次に流れる曲) */
#define PANEL_Q        44
#define PANEL_Q_STEP   52

#endif
