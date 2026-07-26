#ifndef NET_H
#define NET_H

/* Wi-Fi (apctl) 接続。0=成功 */
int net_init(void);

/* EAGAIN を待ち合わせる recv。>0 受信 / 0 切断 / <0 エラー */
int net_recv_wait(int sock, void *buf, int len);

/* HTTP GET してボディ全体を buf に受け取る。戻り値: ボディ長 / <0 エラー */
int http_get(const char *host, int port, const char *path, char *buf, int bufsize);

/* バイナリ用。終端文字を付けず、受信バイト数を返す。<0 エラー */
int http_get_bin(const char *host, int port, const char *path,
                 void *buf, int bufsize);

/* HTTP GET してヘッダ読み飛ばし後のソケットを返す (ストリーミング用)。<0 エラー */
int http_open_stream(const char *host, int port, const char *path);

void net_close(int sock);

#endif
