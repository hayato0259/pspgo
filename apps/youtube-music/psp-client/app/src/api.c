/*
 * サーバーの TSV API クライアント。
 * JSON パーサを持たないための設計 (行 = レコード、タブ = フィールド)。
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "api.h"
#include "net.h"
#include "tsv.h"
#include "common.h"

static char g_buf[192 * 1024]; /* API レスポンス受信バッファ */
static char g_error[160];      /* サーバーが返した error 行 */

#define API_ERR_SERVER (-1000)  /* サーバーが error 行を返した */

const char *api_last_error(void) { return g_error; }

static int api_get(const char *path, char *buf, int bufsize)
{
    char request_path[896];
    if (net_build_path(request_path, sizeof(request_path), path) < 0)
        return -1;
    return http_get(net_server_host(), net_server_port(), request_path,
                    buf, bufsize);
}

/*
 * 受信本文にサーバーからの error 行が含まれていないか確認する。
 * PSP 側は HTTP ステータス行を読み飛ばしているため、
 * これを見ないと「エラー応答」を「空のリスト」と誤認してしまう。
 */
static int has_server_error(void)
{
    g_error[0] = '\0';
    if (tsv_value(g_buf, "error", g_error, sizeof(g_error)))
        return 1;
    return 0;
}

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
    int len = api_get("/api/status", g_buf, sizeof(g_buf));
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
    int len = api_get("/api/logout", g_buf, sizeof(g_buf));
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
    int len = api_get("/api/home", g_buf, sizeof(g_buf));
    if (len < 0)
        return len;
    if (has_server_error())
        return API_ERR_SERVER;
    struct home_ctx ctx = { items, max, 0 };
    for_each_line(g_buf, 4, home_line, &ctx);
    return ctx.count;
}

/* UTF-8 はバイト列のまま、URL の非予約文字以外を %XX にする。 */
static int url_encode(const char *src, char *dst, int dstsize)
{
    static const char hex[] = "0123456789ABCDEF";
    int used = 0;

    while (*src) {
        unsigned char c = (unsigned char)*src++;
        int unreserved = (c >= 'A' && c <= 'Z') ||
                         (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') ||
                         c == '-' || c == '_' || c == '.' || c == '~';
        int need = unreserved ? 1 : 3;
        if (used + need >= dstsize)
            return -1;
        if (unreserved) {
            dst[used++] = (char)c;
        } else {
            dst[used++] = '%';
            dst[used++] = hex[c >> 4];
            dst[used++] = hex[c & 0x0F];
        }
    }
    dst[used] = '\0';
    return used;
}

int api_search(const char *query_utf8, ApiItem *items, int max)
{
    char encoded[768];
    char path[800];
    if (!query_utf8 || !items || max <= 0)
        return -1;
    if (url_encode(query_utf8, encoded, sizeof(encoded)) < 0)
        return -1;
    snprintf(path, sizeof(path), "/api/search?q=%s", encoded);

    int len = api_get(path, g_buf, sizeof(g_buf));
    if (len < 0)
        return len;
    if (has_server_error())
        return API_ERR_SERVER;
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
    int len = api_get(path, g_buf, sizeof(g_buf));
    if (len < 0)
        return len;
    if (has_server_error())
        return API_ERR_SERVER;
    title_out[0] = '\0';
    struct pl_ctx ctx = { title_out, title_size, tracks, max, 0 };
    for_each_line(g_buf, 5, pl_line, &ctx);
    return ctx.count;
}

/* --- /api/radio --------------------------------------------------------- */

int api_radio(const char *video_id, char *title_out, int title_size,
              ApiTrack *tracks, int max)
{
    char path[160];
    snprintf(path, sizeof(path), "/api/radio?yt=%s", video_id);
    int len = api_get(path, g_buf, sizeof(g_buf));
    if (len < 0)
        return len;
    if (has_server_error())
        return API_ERR_SERVER;
    title_out[0] = '\0';
    struct pl_ctx ctx = { title_out, title_size, tracks, max, 0 };
    for_each_line(g_buf, 5, pl_line, &ctx);
    return ctx.count;
}

/* --- /api/lyrics -------------------------------------------------------- */

int api_lyrics(const char *video_id, char *buf, int bufsize)
{
    char path[160];
    if (!video_id || !buf || bufsize <= 0)
        return -1;
    buf[0] = '\0';
    snprintf(path, sizeof(path), "/api/lyrics?yt=%s", video_id);
    return api_get(path, buf, bufsize);
}

/* --- /api/trackinfo --------------------------------------------------- */

static int cp_line(char **f, int nf, void *vctx)
{
    ApiTrackInfo *out = vctx;
    if (strcmp(f[0], "cur") == 0 && nf >= 2) {
        out->current_is_video = (strcmp(f[1], "video") == 0);
    } else if (strcmp(f[0], "like") == 0 && nf >= 2) {
        out->rating = (strcmp(f[1], "like") == 0)    ? RATE_LIKE :
                      (strcmp(f[1], "dislike") == 0) ? RATE_DISLIKE : RATE_NONE;
    } else if (strcmp(f[0], "meta") == 0 && nf >= 3) {
        copy_field(out->album, sizeof(out->album), f[1]);
        copy_field(out->views, sizeof(out->views), f[2]);
    } else if (strcmp(f[0], "lib") == 0 && nf >= 2) {
        out->can_save = 1;
        out->in_library = (atoi(f[1]) != 0);
    } else if (strcmp(f[0], "alt") == 0 && nf >= 6) {
        out->has_alt = 1;
        copy_field(out->alt.video_id, sizeof(out->alt.video_id), f[1]);
        out->alt_is_video = (strcmp(f[2], "video") == 0);
        copy_field(out->alt.title, sizeof(out->alt.title), f[3]);
        copy_field(out->alt.artist, sizeof(out->alt.artist), f[4]);
        out->alt.duration_sec = atoi(f[5]);
    }
    return 0;
}

/*
 * 応答が数百バイトと小さいので専用の小さなバッファを持つ。
 * 共有の g_buf / g_error を触らないので、この関数だけは
 * UI とは別のスレッドから呼んでも他の API 呼び出しと衝突しない
 * (対応バージョンの問い合わせは背後で走らせるため、この性質が要る)。
 */
static char g_cp_buf[1024];

int api_trackinfo(const char *video_id, ApiTrackInfo *out)
{
    char path[160];
    if (!video_id || !out)
        return -1;
    memset(out, 0, sizeof(*out));
    snprintf(path, sizeof(path), "/api/trackinfo?yt=%s", video_id);
    int len = api_get(path, g_cp_buf, sizeof(g_cp_buf));
    if (len < 0)
        return len;
    char err[8];
    if (tsv_value(g_cp_buf, "error", err, sizeof(err)))
        return API_ERR_SERVER;
    for_each_line(g_cp_buf, 6, cp_line, out);
    return 0;
}

int api_rate(const char *video_id, ApiRating rating)
{
    char path[160];
    if (!video_id)
        return -1;
    const char *r = (rating == RATE_LIKE)    ? "like" :
                    (rating == RATE_DISLIKE) ? "dislike" : "none";
    snprintf(path, sizeof(path), "/api/rate?yt=%s&r=%s", video_id, r);
    int len = api_get(path, g_cp_buf, sizeof(g_cp_buf));
    if (len < 0)
        return len;
    char err[8];
    if (tsv_value(g_cp_buf, "error", err, sizeof(err)))
        return API_ERR_SERVER;
    return 0;
}

int api_library(const char *video_id, int save)
{
    char path[160];
    if (!video_id)
        return -1;
    snprintf(path, sizeof(path), "/api/library?yt=%s&s=%d", video_id, save ? 1 : 0);
    int len = api_get(path, g_cp_buf, sizeof(g_cp_buf));
    if (len < 0)
        return len;
    char err[8];
    if (tsv_value(g_cp_buf, "error", err, sizeof(err)))
        return API_ERR_SERVER;
    return 0;
}
