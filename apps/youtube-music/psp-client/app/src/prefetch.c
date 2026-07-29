/*
 * 次に再生する曲の先読み依頼 (prefetch.h 参照)。
 * net.c の HTTP は resolver をロックで守っているので、
 * 再生・ダウンロード・trackinfo の各スレッドと並んで動いて問題ない。
 */
#include <pspkernel.h>
#include <string.h>
#include <stdio.h>
#include "prefetch.h"
#include "net.h"

static char g_pending[24] = "";
static char g_last[24] = "";
static volatile int g_kick = 0;
static SceUID g_thread = -1;

static int worker(SceSize args, void *argp)
{
    char id[24], base[64], path[224], resp[128];
    for (;;) {
        if (!g_kick) {
            sceKernelDelayThread(100 * 1000);
            continue;
        }
        g_kick = 0;
        snprintf(id, sizeof(id), "%s", g_pending);
        if (!id[0])
            continue;
        snprintf(base, sizeof(base), "/api/prefetch?yt=%s", id);
        if (net_build_path(path, sizeof(path), base) == 0)
            http_get(net_server_host(), net_server_port(), path,
                     resp, sizeof(resp));
    }
    return 0;
}

void prefetch_init(void)
{
    g_thread = sceKernelCreateThread("prefetch", worker, 0x1B, 0x4000, 0, 0);
    if (g_thread >= 0)
        sceKernelStartThread(g_thread, 0, 0);
}

void prefetch_request(const char *video_id)
{
    if (!video_id || !video_id[0])
        return;
    if (strcmp(g_last, video_id) == 0)
        return;   /* 同じ曲を二度頼まない */
    snprintf(g_last, sizeof(g_last), "%s", video_id);
    snprintf(g_pending, sizeof(g_pending), "%s", video_id);
    g_kick = 1;
}
