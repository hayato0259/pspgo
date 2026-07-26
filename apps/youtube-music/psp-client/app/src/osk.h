#ifndef OSK_H
#define OSK_H

enum {
    OSK_RUNNING = 0,
    OSK_ACCEPTED = 1,
    OSK_CANCELLED = -1,
    OSK_ERROR = -2
};

/* PSP 標準 OSK を空の検索語で開く。 */
int osk_begin(void);

/*
 * GU の描画・同期と OSK 更新を1フレーム進める。
 * 確定時は UTF-8 を out に格納して OSK_ACCEPTED を返す。
 */
int osk_update(char *out, int out_size);

#endif
