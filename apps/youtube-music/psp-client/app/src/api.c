/*
 * サーバーの TSV API クライアント。
 * JSON パーサを持たないための設計 (行 = レコード、タブ = フィールド)。
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "api.h"
#include "net.h"
#include "common.h"

static char g_buf[192 * 1024]; /* API レスポンス受信バッファ */

static void copy_field(char *dst, int dstsize, const char *src)
{
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dstsize, "%s", src);
}

/* buf を改行で分割し、各行をタブで最大 nfields に分割して cb 相当の処理へ渡す。
   分割はインプレース (buf を書き換える)。 */
typedef int (*line_fn)(char **fields, int nf, void *ctx);

static int for_each_line(char *buf, int nfields, line_fn fn, void *ctx)
{
    char *save_line = NULL;
    char *line = strtok_r(buf, "\n", &save_line);
    while (line) {
        char *fields[8];
        int nf = 0;
        char *p = line;
        while (nf < nfields && p) {
            fields[nf++] = p;
            char *tab = strchr(p, '\t');
            if (tab) {
                *tab = '\0';
                p = tab + 1;
            } else {
                p = NULL;
            }
        }
        if (nf > 0 && fields[0][0] != '\0') {
            int rc = fn(fields, nf, ctx);
            if (rc < 0)
                return rc;
        }
        line = strtok_r(NULL, "\n", &save_line);
    }
    return 0;
}

/* --- /api/status -------------------------------------------------------- */

struct status_ctx { int auth; int can_login; char *name; int name_size; };

static int status_line(char **f, int nf, void *vctx)
{
    struct status_ctx *ctx = vctx;
    if (strcmp(f[0], "auth") == 0 && nf >= 2)
        ctx->auth = atoi(f[1]);
    else if (strcmp(f[0], "can_login") == 0 && nf >= 2)
        ctx->can_login = atoi(f[1]);
    else if (strcmp(f[0], "name") == 0 && nf >= 2)
        copy_field(ctx->name, ctx->name_size, f[1]);
    return 0;
}

int api_status(char *name_out, int name_size, int *can_login_out)
{
    int len = http_get(SERVER_HOST, SERVER_PORT, "/api/status", g_buf, sizeof(g_buf));
    if (len < 0)
        return len;
    struct status_ctx ctx = { 0, 0, name_out, name_size };
    name_out[0] = '\0';
    for_each_line(g_buf, 2, status_line, &ctx);
    if (can_login_out)
        *can_login_out = ctx.can_login;
    return ctx.auth;
}

int api_logout(void)
{
    int len = http_get(SERVER_HOST, SERVER_PORT, "/api/logout", g_buf, sizeof(g_buf));
    return (len < 0) ? len : 0;
}

/* --- /api/home ---------------------------------------------------------- */

struct home_ctx { ApiItem *items; int max; int count; };

static int home_line(char **f, int nf, void *vctx)
{
    struct home_ctx *ctx = vctx;
    if (ctx->count >= ctx->max)
        return 0;
    ApiItem *it = &ctx->items[ctx->count];
    if (strcmp(f[0], "section") == 0 && nf >= 2) {
        it->kind = 'S';
        it->id[0] = '\0';
        copy_field(it->title, sizeof(it->title), f[1]);
        it->subtitle[0] = '\0';
        ctx->count++;
    } else if (strcmp(f[0], "playlist") == 0 && nf >= 3) {
        it->kind = 'P';
        copy_field(it->id, sizeof(it->id), f[1]);
        copy_field(it->title, sizeof(it->title), f[2]);
        copy_field(it->subtitle, sizeof(it->subtitle), nf >= 4 ? f[3] : "");
        ctx->count++;
    } else if (strcmp(f[0], "video") == 0 && nf >= 3) {
        it->kind = 'V';
        copy_field(it->id, sizeof(it->id), f[1]);
        copy_field(it->title, sizeof(it->title), f[2]);
        copy_field(it->subtitle, sizeof(it->subtitle), nf >= 4 ? f[3] : "");
        ctx->count++;
    }
    return 0;
}

int api_home(ApiItem *items, int max)
{
    int len = http_get(SERVER_HOST, SERVER_PORT, "/api/home", g_buf, sizeof(g_buf));
    if (len < 0)
        return len;
    struct home_ctx ctx = { items, max, 0 };
    for_each_line(g_buf, 4, home_line, &ctx);
    return ctx.count;
}

/* --- /api/playlist ------------------------------------------------------ */

struct pl_ctx {
    char *title; int title_size;
    ApiTrack *tracks; int max; int count;
};

static int pl_line(char **f, int nf, void *vctx)
{
    struct pl_ctx *ctx = vctx;
    if (strcmp(f[0], "meta") == 0 && nf >= 2) {
        copy_field(ctx->title, ctx->title_size, f[1]);
    } else if (strcmp(f[0], "track") == 0 && nf >= 4 && ctx->count < ctx->max) {
        ApiTrack *t = &ctx->tracks[ctx->count++];
        copy_field(t->video_id, sizeof(t->video_id), f[1]);
        copy_field(t->title, sizeof(t->title), f[2]);
        copy_field(t->artist, sizeof(t->artist), f[3]);
        t->duration_sec = (nf >= 5) ? atoi(f[4]) : 0;
    }
    return 0;
}

int api_playlist(const char *id, char *title_out, int title_size,
                 ApiTrack *tracks, int max)
{
    char path[160];
    snprintf(path, sizeof(path), "/api/playlist?id=%s", id);
    int len = http_get(SERVER_HOST, SERVER_PORT, path, g_buf, sizeof(g_buf));
    if (len < 0)
        return len;
    title_out[0] = '\0';
    struct pl_ctx ctx = { title_out, title_size, tracks, max, 0 };
    for_each_line(g_buf, 5, pl_line, &ctx);
    return ctx.count;
}
