/*
 * OAuth デバイスコードフローのクライアント側。
 *
 * PSP は現代の TLS も JavaScript も扱えないため、Google との通信はサーバーが行う。
 * このアプリの役割は「サーバーからコードを受け取って画面に出し、
 * 承認が終わるまでポーリングする」こと。テレビやゲーム機のログインと同じ方式。
 *
 * 通信はワーカースレッドで行い、UI スレッドは volatile な状態だけを読む。
 */
#include <pspkernel.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "login.h"
#include "net.h"
#include "common.h"

static volatile LoginState g_state = LOGIN_IDLE;
static volatile int g_cancel = 0;
static volatile int g_remaining = 0;

static char g_code[32];
static char g_url[96];
static char g_msg[128];

static SceUID g_thread = -1;
static char g_buf[4096];

LoginState login_state(void) { return g_state; }
const char *login_user_code(void) { return g_code; }
const char *login_url(void) { return g_url; }
const char *login_message(void) { return g_msg; }
int login_remaining_sec(void) { return g_remaining; }

/* TSV から key の値を取り出す。見つからなければ 0 を返す */
static int tsv_value(const char *buf, const char *key, char *out, int outsize)
{
    int klen = strlen(key);
    const char *p = buf;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '\t') {
            const char *v = p + klen + 1;
            const char *end = strpbrk(v, "\r\n");
            int len = end ? (int)(end - v) : (int)strlen(v);
            if (len > outsize - 1)
                len = outsize - 1;
            memcpy(out, v, len);
            out[len] = '\0';
            return 1;
        }
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return 0;
}

static void set_failed(const char *msg)
{
    snprintf(g_msg, sizeof(g_msg), "%s", msg);
    g_state = LOGIN_FAILED;
}

static int login_thread(SceSize args, void *argp)
{
    char tmp[128];

    /* 1. デバイスコードを取得 */
    g_state = LOGIN_REQUESTING;
    int len = http_get(SERVER_HOST, SERVER_PORT, "/api/login/start",
                       g_buf, sizeof(g_buf));
    if (len < 0) {
        set_failed("server ni tsunagarimasen");
        return 0;
    }
    if (tsv_value(g_buf, "error", tmp, sizeof(tmp))) {
        set_failed(tmp);
        return 0;
    }
    if (!tsv_value(g_buf, "code", g_code, sizeof(g_code)) ||
        !tsv_value(g_buf, "url", g_url, sizeof(g_url))) {
        set_failed("code no shutoku ni shippai");
        return 0;
    }

    int interval = 5;
    if (tsv_value(g_buf, "interval", tmp, sizeof(tmp))) {
        interval = atoi(tmp);
        if (interval < 2) interval = 2;
        if (interval > 30) interval = 30;
    }
    g_remaining = 1800;
    if (tsv_value(g_buf, "expires", tmp, sizeof(tmp)))
        g_remaining = atoi(tmp);

    g_state = LOGIN_WAITING;

    /* 2. 承認されるまでポーリング */
    while (!g_cancel && g_remaining > 0) {
        /* interval 秒待つ (キャンセル反応のため小刻みに) */
        for (int i = 0; i < interval * 10 && !g_cancel; i++) {
            sceKernelDelayThread(100 * 1000);
            if ((i % 10) == 9 && g_remaining > 0)
                g_remaining--;
        }
        if (g_cancel)
            break;

        len = http_get(SERVER_HOST, SERVER_PORT, "/api/login/poll",
                       g_buf, sizeof(g_buf));
        if (len < 0)
            continue; /* 一時的な通信失敗は待って再試行 */

        if (tsv_value(g_buf, "error", tmp, sizeof(tmp))) {
            set_failed(tmp);
            return 0;
        }
        if (tsv_value(g_buf, "state", tmp, sizeof(tmp))) {
            if (strcmp(tmp, "ok") == 0) {
                g_state = LOGIN_SUCCESS;
                return 0;
            }
            /* pending: 継続 */
        }
    }

    if (g_cancel)
        g_state = LOGIN_IDLE;
    else
        set_failed("code no yuukou kigen kire");
    return 0;
}

int login_begin(void)
{
    login_cancel();

    g_cancel = 0;
    g_remaining = 0;
    g_code[0] = '\0';
    g_url[0] = '\0';
    g_msg[0] = '\0';
    g_state = LOGIN_REQUESTING;

    g_thread = sceKernelCreateThread("login", login_thread, 0x18, 0x4000, 0, 0);
    if (g_thread < 0) {
        set_failed("thread sakusei shippai");
        return g_thread;
    }
    return sceKernelStartThread(g_thread, 0, 0);
}

void login_cancel(void)
{
    if (g_thread >= 0) {
        g_cancel = 1;
        SceUInt timeout = 15 * 1000 * 1000; /* HTTP 応答待ちを考慮 */
        sceKernelWaitThreadEnd(g_thread, &timeout);
        sceKernelDeleteThread(g_thread);
        g_thread = -1;
    }
    if (g_state != LOGIN_SUCCESS)
        g_state = LOGIN_IDLE;
}
