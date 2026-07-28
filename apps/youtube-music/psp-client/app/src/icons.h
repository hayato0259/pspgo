#ifndef ICONS_H
#define ICONS_H

/*
 * Material Symbols から起こしたアイコン (make_icons.py が生成)。
 * 直接編集しない。絵柄を足すときは tools/make_icons.py の ICONS に加える。
 *
 * 不透明度だけを 1 バイトで持つ。色は描画時に指定する。
 */

#define ICON_SIDE 32

typedef enum {
    ICON_PLAY_ARROW,
    ICON_PAUSE,
    ICON_SKIP_NEXT,
    ICON_SKIP_PREVIOUS,
    ICON_THUMB_UP,
    ICON_THUMB_DOWN,
    ICON_THUMB_UP_FILL,
    ICON_THUMB_DOWN_FILL,
    ICON_LYRICS,
    ICON_MORE_VERT,
    ICON_SEARCH,
    ICON_DOWNLOAD,
    ICON_RADIO,
    ICON_BEDTIME,
    ICON_REPEAT,
    ICON_REPEAT_ONE,
    ICON_SHUFFLE,
    ICON_MUSIC_VIDEO,
    ICON_LIBRARY_MUSIC,
    ICON_WIFI_OFF,
    ICON_BOOKMARK,
    ICON_BOOKMARK_FILL,
    ICON_SMART_DISPLAY,
    ICON_COUNT
} IconId;

/* 辺 ICON_SIDE の不透明度マップ。ICON_COUNT 個ぶん並ぶ */
extern const unsigned char icon_alpha[ICON_COUNT][ICON_SIDE * ICON_SIDE];

#endif
