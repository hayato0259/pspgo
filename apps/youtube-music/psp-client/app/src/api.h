#ifndef API_H
#define API_H

/* ホームの全行 (セクション見出し + 項目)。
   サーバー側の上限 (20 セクション x 12 項目) が収まる大きさにする */
#define API_MAX_ITEMS 288
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

/* 評価の状態 (本家プレイヤーの高評価・低評価) */
typedef enum {
    RATE_NONE = 0,
    RATE_LIKE,
    RATE_DISLIKE
} ApiRating;

/*
 * 再生中の曲について、一度の問い合わせで分かること。
 *
 * has_alt は「曲 / 動画」の切り替え先があるかどうか。
 * 同じ楽曲の別バージョンは別の動画として存在するが、対応する版が無い曲も多い。
 * current_is_video は対応版の有無と関係なく必ず入る
 * (ミュージックビデオそのものの曲は対応版を持たないが、動画として再生する)。
 */
typedef struct {
    int current_is_video;   /* いま再生しているのが動画か */
    ApiRating rating;       /* 高評価・低評価の状態 */
    char album[96];         /* アルバム名 (無ければ空) */
    char views[32];         /* 再生回数 ("165万回視聴")。取れなければ空 */
    int can_save;           /* ライブラリに保存できる曲か */
    int in_library;         /* 保存済みか */
    int has_alt;            /* 0 = 切り替え先が無い (トグルを出さない) */
    int alt_is_video;       /* 切り替え先が動画か */
    ApiTrack alt;           /* 切り替え先 */
} ApiTrackInfo;

/* /api/trackinfo?yt=。戻り値: 0=成功 / <0 エラー */
int api_trackinfo(const char *video_id, ApiTrackInfo *out);

/* /api/rate?yt=&r=。評価を付ける。戻り値: 0=成功 / <0 エラー */
int api_rate(const char *video_id, ApiRating rating);

/* /api/library?yt=&s=。ライブラリに保存 (save=1) / 解除 (0)。0=成功 / <0 エラー */
int api_library(const char *video_id, int save);

/* 直近の API 呼び出しがサーバーから返したエラー文。無ければ空文字列。
   サーバーが error 行を返した場合、上記の関数は負値を返す。 */
const char *api_last_error(void);

#endif
