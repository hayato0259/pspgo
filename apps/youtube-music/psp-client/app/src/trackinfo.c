/*
 * 曲版 / ミュージックビデオ版の対応を背後で取ってくるワーカー。
 *
 * 構成は dl.c と同じで、依頼を 1 件だけ保持する箱と、それを処理する
 * スレッドからなる。描画スレッドは依頼を置くだけで待たない。
 */
#include <pspkernel.h>
#include <string.h>
#include <stdio.h>
#include "trackinfo.h"
#include "api.h"

static SceUID g_thread = -1;

/* 依頼: 描画スレッドが書き、ワーカーが読む */
static char g_want[24];
static volatile int g_have_request = 0;

/* 結果: ワーカーが書き、描画スレッドが読む */
static char g_done_for[24];
static ApiTrackInfo g_done;
static volatile int g_done_ready = 0;

/* いま取得中の曲。同じ依頼を二重に投げないための目印 */
static char g_inflight[24];

static int worker(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        if (!g_have_request) {
            sceKernelDelayThread(100 * 1000);
            continue;
        }

        char id[24];
        snprintf(id, sizeof(id), "%s", g_want);
        g_have_request = 0;

        ApiTrackInfo got;
        if (api_trackinfo(id, &got) < 0)
            memset(&got, 0, sizeof(got));   /* 取れなければ「対応版なし」扱い */

        /* 先に中身を書いてから、最後に「この曲の結果です」と宣言する。
           逆順だと描画スレッドが古い中身を新しい結果と誤認する */
        g_done = got;
        snprintf(g_done_for, sizeof(g_done_for), "%s", id);
        g_done_ready = 1;
        g_inflight[0] = '\0';
    }
    return 0;
}

void trackinfo_init(void)
{
    if (g_thread >= 0)
        return;
    g_thread = sceKernelCreateThread("trackinfo", worker, 0x14, 0x4000, 0, 0);
    if (g_thread >= 0)
        sceKernelStartThread(g_thread, 0, 0);
}

void trackinfo_request(const char *video_id)
{
    if (!video_id || !video_id[0])
        return;
    if (g_done_ready && strcmp(g_done_for, video_id) == 0)
        return;                                   /* 取得済み */
    if (strcmp(g_inflight, video_id) == 0)
        return;                                   /* 取得中 */

    snprintf(g_inflight, sizeof(g_inflight), "%s", video_id);
    snprintf(g_want, sizeof(g_want), "%s", video_id);
    g_done_ready = 0;
    g_have_request = 1;
}

void trackinfo_set_rating(const char *video_id, ApiRating rating)
{
    if (video_id && g_done_ready && strcmp(g_done_for, video_id) == 0)
        g_done.rating = rating;
}

const ApiTrackInfo *trackinfo_result(const char *video_id)
{
    if (!video_id || !g_done_ready)
        return NULL;
    if (strcmp(g_done_for, video_id) != 0)
        return NULL;
    return &g_done;
}
