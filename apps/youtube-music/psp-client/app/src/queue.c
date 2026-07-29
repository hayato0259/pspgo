/*
 * 再生キューと再生モード、スリープタイマー (queue.h 参照)。
 * 画面から独立した「何をどの順で鳴らすか」だけを持ち、描画は行わない
 * (例外は画面下の再生バー。全画面が同じものを出すのでここに置く)。
 */
#include <pspkernel.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "queue.h"
#include "player.h"
#include "snd.h"
#include "ui.h"

ApiTrack g_tracks[API_MAX_TRACKS];
int g_track_count = 0;
int g_track_sel = 0, g_track_scroll = 0;
int g_playing_index = -1;
char g_pl_title[128];
PlayMode g_play_mode = PLAY_MODE_NORMAL;

#define SHUFFLE_HISTORY_WORDS ((API_MAX_TRACKS + 31) / 32)
static unsigned int g_shuffle_history[SHUFFLE_HISTORY_WORDS];

static int shuffle_track_played(int index)
{
    return (g_shuffle_history[index / 32] & (1U << (index % 32))) != 0;
}

static void shuffle_mark_played(int index)
{
    if (index >= 0 && index < g_track_count)
        g_shuffle_history[index / 32] |= 1U << (index % 32);
}

void shuffle_history_reset(void)
{
    memset(g_shuffle_history, 0, sizeof(g_shuffle_history));
    shuffle_mark_played(g_playing_index);
}

int next_track_index(void)
{
    if (g_playing_index < 0 || g_playing_index >= g_track_count ||
        g_track_count <= 0)
        return -1;

    switch (g_play_mode) {
    case PLAY_MODE_NORMAL:
        return (g_playing_index + 1 < g_track_count)
                   ? g_playing_index + 1 : -1;
    case PLAY_MODE_SHUFFLE: {
        int remaining = 0;
        for (int i = 0; i < g_track_count; i++)
            if (!shuffle_track_played(i))
                remaining++;
        if (remaining == 0)
            return -1;

        int pick = rand() % remaining;
        for (int i = 0; i < g_track_count; i++) {
            if (shuffle_track_played(i))
                continue;
            if (pick-- == 0) {
                shuffle_mark_played(i);
                return i;
            }
        }
        return -1;
    }
    case PLAY_MODE_REPEAT_ALL:
        return (g_playing_index + 1) % g_track_count;
    case PLAY_MODE_REPEAT_ONE:
        return g_playing_index;
    case PLAY_MODE_COUNT:
        break;
    }
    return -1;
}

void cycle_play_mode(void)
{
    g_play_mode = (PlayMode)((g_play_mode + 1) % PLAY_MODE_COUNT);
    if (g_play_mode == PLAY_MODE_SHUFFLE)
        shuffle_history_reset();
}

const char *play_mode_label(void)
{
    switch (g_play_mode) {
    case PLAY_MODE_SHUFFLE:    return "シャッフル";
    case PLAY_MODE_REPEAT_ALL: return "リピート";
    case PLAY_MODE_REPEAT_ONE: return "1曲リピート";
    case PLAY_MODE_NORMAL:
    case PLAY_MODE_COUNT:
        return "";
    }
    return "";
}

void start_track(int index)
{
    if (index < 0 || index >= g_track_count)
        return;
    g_playing_index = index;
    if (g_play_mode == PLAY_MODE_SHUFFLE)
        shuffle_mark_played(index);
    player_start(g_tracks[index].video_id, g_tracks[index].duration_sec, 0);
}

void skip_track(int forward)
{
    int index = -1;
    if (g_play_mode == PLAY_MODE_SHUFFLE)
        index = next_track_index();
    else if (!forward && g_playing_index > 0)
        index = g_playing_index - 1;
    else if (forward && g_playing_index + 1 < g_track_count)
        index = g_playing_index + 1;

    if (index >= 0) {
        snd_play(SND_MOVE);
        start_track(index);
    } else if (g_play_mode == PLAY_MODE_SHUFFLE) {
        player_stop();
    }
}

/*
 * 単曲を選んだときのキュー。
 *
 * 本家は 1 曲を選ぶと、その曲から続くミックスがそのまま流れる。
 * 選んだ 1 曲だけをキューにすると、終わった時点で止まり
 * 画面下の「次の曲」も出ない。ラジオを取ってキューにする。
 * 取れなかったときはその 1 曲だけで再生する (再生自体は妨げない)。
 */
void queue_from_single(const ApiItem *item)
{
    ApiTrack first;
    memset(&first, 0, sizeof(first));
    /* videoId は 11 文字。ApiItem の id 枠 (64) より狭いので明示的に切る */
    snprintf(first.video_id, sizeof(first.video_id), "%.23s", item->id);
    snprintf(first.title, sizeof(first.title), "%s", item->title);
    snprintf(first.artist, sizeof(first.artist), "%s", item->subtitle);

    int n = api_radio(item->id, g_pl_title, sizeof(g_pl_title),
                      g_tracks, API_MAX_TRACKS);
    if (n <= 0) {
        g_tracks[0] = first;
        g_track_count = 1;
        snprintf(g_pl_title, sizeof(g_pl_title), "%s", item->title);
    } else {
        g_track_count = n;
        /* 先頭が選んだ曲でなければ差し込む (ラジオは別の曲から始まることがある) */
        if (strcmp(g_tracks[0].video_id, item->id) != 0) {
            if (g_track_count > API_MAX_TRACKS - 1)
                g_track_count = API_MAX_TRACKS - 1;
            memmove(&g_tracks[1], &g_tracks[0],
                    (size_t)g_track_count * sizeof(ApiTrack));
            g_tracks[0] = first;
            g_track_count++;
        }
    }
    g_track_sel = 0;
    g_track_scroll = 0;
    g_playing_index = -1;
    shuffle_history_reset();
    start_track(0);
}

/*
 * 再生中の音声には触れず、ラジオ取得に成功したときだけキューを置き換える。
 * 再生モードは維持し、インデックス依存のシャッフル履歴だけ作り直す。
 */
int replace_queue_with_radio(void)
{
    if (g_playing_index < 0 || g_playing_index >= g_track_count)
        return -1;

    ApiTrack current = g_tracks[g_playing_index];
    static ApiTrack radio_tracks[API_MAX_TRACKS];
    char radio_title[128];
    int n = api_radio(current.video_id, radio_title, sizeof(radio_title),
                      radio_tracks, API_MAX_TRACKS);
    if (n <= 0)
        return -1;

    int insert_current = strcmp(radio_tracks[0].video_id,
                                current.video_id) != 0;
    int radio_count = n;
    if (insert_current && radio_count >= API_MAX_TRACKS)
        radio_count = API_MAX_TRACKS - 1;

    if (insert_current)
        g_tracks[0] = current;
    memcpy(&g_tracks[insert_current], radio_tracks,
           (size_t)radio_count * sizeof(ApiTrack));
    g_track_count = radio_count + insert_current;
    g_playing_index = 0;
    g_track_sel = 0;
    g_track_scroll = 0;
    snprintf(g_pl_title, sizeof(g_pl_title), "ラジオ: %.88s", current.title);
    shuffle_history_reset();
    return 0;
}

/* --- スリープタイマー ----------------------------------------------------- */

static const int g_sleep_timer_minutes[] = { 0, 15, 30, 60, 90 };
static int g_sleep_timer_option = 0;
static unsigned long long g_sleep_timer_remaining_us = 0;
static unsigned int g_sleep_timer_last_us = 0;

void cycle_sleep_timer(void)
{
    g_sleep_timer_option =
        (g_sleep_timer_option + 1) %
        (int)(sizeof(g_sleep_timer_minutes) / sizeof(g_sleep_timer_minutes[0]));

    int minutes = g_sleep_timer_minutes[g_sleep_timer_option];
    if (minutes == 0) {
        g_sleep_timer_remaining_us = 0;
        return;
    }

    g_sleep_timer_remaining_us =
        (unsigned long long)minutes * 60ULL * 1000000ULL;
    g_sleep_timer_last_us = (unsigned int)sceKernelGetSystemTimeLow();
}

/*
 * sceKernelGetSystemTimeLow() は約71分で周回するため、期限の絶対値ではなく
 * 毎ループの unsigned 差分を64-bitの残り時間から引く。
 */
int sleep_timer_tick(void)
{
    if (g_sleep_timer_option == 0)
        return 0;

    unsigned int now = (unsigned int)sceKernelGetSystemTimeLow();
    unsigned int elapsed = now - g_sleep_timer_last_us;
    g_sleep_timer_last_us = now;

    if ((unsigned long long)elapsed >= g_sleep_timer_remaining_us) {
        g_sleep_timer_option = 0;
        g_sleep_timer_remaining_us = 0;
        return 1;
    }

    g_sleep_timer_remaining_us -= (unsigned long long)elapsed;
    return 0;
}

int sleep_timer_minutes(void)
{
    return g_sleep_timer_minutes[g_sleep_timer_option];
}

unsigned long long sleep_timer_remaining_us(void)
{
    return g_sleep_timer_remaining_us;
}

/* --- 再生バー ------------------------------------------------------------- */

void now_playing_bar(void)
{
    PlayerState st = player_state();
    if (st == PLAYER_STOPPED || g_playing_index < 0)
        return;
    ApiTrack *t = &g_tracks[g_playing_index];
    ui_now_playing(t->video_id, t->title, t->artist, st,
                   player_elapsed_sec(), t->duration_sec);
}
