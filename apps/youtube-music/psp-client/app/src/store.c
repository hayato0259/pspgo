/*
 * オフライン ライブラリの索引管理。
 *
 * index.tsv は「1 行 = 1 曲」のタブ区切り:
 *   videoId \t 曲名 \t アーティスト \t 長さ(秒)
 * 追加は追記ではなく毎回全体を書き直す (削除と同じ経路に揃えて単純化)。
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "store.h"

#define DIR_NAME   "offline"
#define INDEX_PATH DIR_NAME "/index.tsv"

static ApiTrack g_items[STORE_MAX];
static int g_count = 0;
static SceUID g_mutex = -1;

static void lock(void)   { if (g_mutex >= 0) sceKernelWaitSema(g_mutex, 1, NULL); }
static void unlock(void) { if (g_mutex >= 0) sceKernelSignalSema(g_mutex, 1); }

void store_mp3_path(const char *video_id, char *buf, int size)
{
    snprintf(buf, size, DIR_NAME "/%s.mp3", video_id);
}

void store_art_path(const char *id, char *buf, int size)
{
    snprintf(buf, size, DIR_NAME "/%s.art", id);
}

/* 1 行をタブで分解して ApiTrack に詰める。1=有効な行だった */
static int parse_line(char *line, ApiTrack *t)
{
    char *fields[4] = { line, NULL, NULL, NULL };
    int n = 1;
    for (char *p = line; *p && n < 4; p++) {
        if (*p == '\t') {
            *p = '\0';
            fields[n++] = p + 1;
        }
    }
    if (n < 3 || !fields[0][0])
        return 0;
    memset(t, 0, sizeof(*t));
    snprintf(t->video_id, sizeof(t->video_id), "%s", fields[0]);
    snprintf(t->title, sizeof(t->title), "%s", fields[1]);
    snprintf(t->artist, sizeof(t->artist), "%s", fields[2] ? fields[2] : "");
    t->duration_sec = fields[3] ? atoi(fields[3]) : 0;
    return 1;
}

int store_init(void)
{
    g_mutex = sceKernelCreateSema("store_sema", 0, 1, 1, NULL);
    sceIoMkdir(DIR_NAME, 0777);   /* 既にあれば失敗するだけ */

    g_count = 0;
    FILE *fp = fopen(INDEX_PATH, "r");
    if (!fp)
        return 0;   /* まだ 1 曲も無い */
    char line[512];
    while (g_count < STORE_MAX && fgets(line, sizeof(line), fp)) {
        char *e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r'))
            *--e = '\0';
        if (parse_line(line, &g_items[g_count]))
            g_count++;
    }
    fclose(fp);
    return 0;
}

/* ロック中に呼ぶこと */
static int write_index(void)
{
    FILE *fp = fopen(INDEX_PATH, "w");
    if (!fp)
        return -1;
    for (int i = 0; i < g_count; i++)
        fprintf(fp, "%s\t%s\t%s\t%d\n",
                g_items[i].video_id, g_items[i].title,
                g_items[i].artist, g_items[i].duration_sec);
    fclose(fp);
    return 0;
}

int store_count(void)
{
    lock();
    int n = g_count;
    unlock();
    return n;
}

int store_get(int i, ApiTrack *out)
{
    int rc = -1;
    lock();
    if (i >= 0 && i < g_count) {
        *out = g_items[i];
        rc = 0;
    }
    unlock();
    return rc;
}

int store_has(const char *video_id)
{
    if (!video_id || !video_id[0])
        return 0;
    int found = 0;
    lock();
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_items[i].video_id, video_id) == 0) {
            found = 1;
            break;
        }
    }
    unlock();
    return found;
}

int store_add(const ApiTrack *t)
{
    int rc = -1;
    lock();
    int dup = 0;
    for (int i = 0; i < g_count; i++)
        if (strcmp(g_items[i].video_id, t->video_id) == 0)
            dup = 1;
    if (!dup && g_count < STORE_MAX) {
        g_items[g_count++] = *t;
        rc = write_index();
    } else if (dup) {
        rc = 0;
    }
    unlock();
    return rc;
}

int store_remove(int i)
{
    char mp3[128], art[128];
    int rc = -1;
    lock();
    if (i >= 0 && i < g_count) {
        store_mp3_path(g_items[i].video_id, mp3, sizeof(mp3));
        store_art_path(g_items[i].video_id, art, sizeof(art));
        for (int j = i; j + 1 < g_count; j++)
            g_items[j] = g_items[j + 1];
        g_count--;
        rc = write_index();
        sceIoRemove(mp3);
        sceIoRemove(art);
    }
    unlock();
    return rc;
}
