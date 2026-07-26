#ifndef UI_H
#define UI_H

#include "player.h"

/*
 * アプリ共通の UI 部品。描画プリミティブは gfx.h、
 * 画面固有のロジックは main.c に置く。
 */

/*
 * 背景の環境色の目標値 (ABGR)。選択中アートワークの平均色を渡すと、
 * PC 版 YouTube Music のように画面上部がその色でうっすら染まる。
 * 0 を渡すと既定のニュートラルな色に戻る。切り替えは毎フレーム滑らかに補間される。
 */
void ui_bg_ambient(unsigned int abgr);

/* フレーム開始 (クリア + 背景)。各画面はこれを呼んでから描く */
void ui_frame_begin(void);

/* ホーム用トップバー: ロゴ + Music + アカウント名 */
void ui_top_bar(int auth, const char *account);

/* その他の画面用: 小ロゴ + タイトル + 下部の操作ヒント */
void ui_chrome(const char *title, const char *hint,
               int auth, const char *account);

/*
 * 画面下の再生バー (PC 版のプレイヤーバー相当)。
 * 再生していないときは何も描かない。
 */
void ui_now_playing(const char *art_id, const char *title, const char *artist,
                    PlayerState st, int elapsed_sec, int duration_sec);

/*
 * 画面下に固定表示する「選択中カードの曲情報」パネル。
 * bottom はパネル下端の Y (再生バーがあるときはその上端を渡す)。
 * slim が真ならサムネイル無しの細い帯になる (再生バーとの同時表示用)。
 */
void ui_selection_info(const char *art_id, const char *title,
                       const char *subtitle, int bottom, int slim);

#endif
