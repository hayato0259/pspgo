#ifndef VIDEO_H
#define VIDEO_H

/*
 * ミュージックビデオの映像再生。
 *
 * サーバーの /video から PSMF (H.264 Baseline 480x272) を受け取り、
 * sceMpeg (Media Engine) でデコードして画面に直接書く。
 * 音声は含まない。従来どおり player.c が /stream から MP3 で受け取って鳴らす。
 * PSMF に音声を入れないのは、PSMF の音声が Atrac3+ 固定で作れないため
 * (詳細は docs/verification-video.md)。
 *
 * 受信は専用スレッドで行い、デコードは描画スレッドから 1 フレームずつ呼ぶ。
 * デコード結果は「今から描くバッファ」に直接書かれるので、
 * 呼ぶ順番は「video_decode → 画面クリアなしで描き始める → 文字を重ねる」。
 */

typedef enum {
    VIDEO_STOPPED = 0,
    VIDEO_BUFFERING,   /* 受信待ち。まだ絵は出ない */
    VIDEO_PLAYING,
    VIDEO_FINISHED,
    VIDEO_ERROR
} VideoState;

/* 再生を始める。seconds は曲の長さ、start_sec は開始位置 (0 = 先頭)。0=成功 */
int video_start(const char *video_id, int seconds, int start_sec);
void video_stop(void);

VideoState video_state(void);
int video_last_error(void);

/*
 * 1 フレームぶんデコードして draw_buf に書く。
 * 1  = 描いた (この後クリアせずに UI を重ねる)
 * 0  = まだ絵が無い (受信待ち)
 * <0 = 終了またはエラー
 */
int video_decode(void *draw_buf);

/* デコード済みのフレーム数 (実測フレームレートの表示用) */
int video_frames(void);

/*
 * 直前に描いたフレームの表示時刻 (ミリ秒)。まだ描いていなければ -1。
 * 音の再生位置と比べて、進みすぎていれば待つ (音を基準に合わせる)。
 */
int video_pts_ms(void);

/*
 * 画質を切り替える。次に video_start() したものから効く。
 * 解像度は変えずビットレートだけ落とす (PSP 側は 480x272 固定のため)。
 */
void video_set_low_quality(int low);

#endif
