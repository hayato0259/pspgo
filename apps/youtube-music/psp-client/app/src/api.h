#ifndef API_H
#define API_H

#define API_MAX_ITEMS 96
#define API_MAX_TRACKS 100

/* ホーム画面の1行。kind: 'S'=セクション見出し 'P'=プレイリスト 'V'=単曲 */
typedef struct {
    char kind;
    char id[64];
    char title[128];
    char subtitle[96];
} ApiItem;

typedef struct {
    char video_id[24];
    char title[128];
    char artist[96];
    int duration_sec;
} ApiTrack;

/* /api/status。戻り値: 1=認証済み 0=未認証 <0=通信エラー */
int api_status(char *name_out, int name_size);

/* /api/home。戻り値: 件数 / <0 エラー */
int api_home(ApiItem *items, int max);

/* /api/playlist?id=。戻り値: トラック数 / <0 エラー */
int api_playlist(const char *id, char *title_out, int title_size,
                 ApiTrack *tracks, int max);

#endif
