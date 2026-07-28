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

/* /api/status。戻り値: 1=認証済み 0=未認証 <0=通信エラー
   can_login_out: サーバーに OAuth クライアントが設定されていれば 1 */
int api_status(char *name_out, int name_size, int *can_login_out);

/* /api/logout。0=成功 */
int api_logout(void);

/* /api/home。戻り値: 件数 / <0 エラー */
int api_home(ApiItem *items, int max);

/* /api/search?q=。query_utf8 は UTF-8。戻り値: 件数 / <0 エラー */
int api_search(const char *query_utf8, ApiItem *items, int max);

/* /api/playlist?id=。戻り値: トラック数 / <0 エラー */
int api_playlist(const char *id, char *title_out, int title_size,
                 ApiTrack *tracks, int max);

/* /api/radio?yt=。戻り値: トラック数 / <0 エラー */
int api_radio(const char *video_id, char *title_out, int title_size,
              ApiTrack *tracks, int max);

/* /api/lyrics?yt=。TSV を buf にそのまま格納。戻り値: バイト数 / <0 エラー */
int api_lyrics(const char *video_id, char *buf, int bufsize);

/*
 * 曲版とミュージックビデオ版の対応。
 * YouTube Music の画面にある「曲 / 動画」の切り替えと同じもので、
 * 同じ楽曲の別バージョンは別の動画として存在する。
 * 対応する版が無い曲も多いので、その場合は has_alt = 0 になる。
 */
typedef struct {
    int has_alt;            /* 0 = 対応する版が無い (トグルを出さない) */
    int current_is_video;   /* いま再生しているのがミュージックビデオ版か */
    int alt_is_video;       /* 切り替え先がミュージックビデオ版か */
    ApiTrack alt;           /* 切り替え先 */
} ApiCounterpart;

/* /api/counterpart?yt=。戻り値: 0=成功 (has_alt を見る) / <0 エラー */
int api_counterpart(const char *video_id, ApiCounterpart *out);

/* 直近の API 呼び出しがサーバーから返したエラー文。無ければ空文字列。
   サーバーが error 行を返した場合、上記の関数は負値を返す。 */
const char *api_last_error(void);

#endif
