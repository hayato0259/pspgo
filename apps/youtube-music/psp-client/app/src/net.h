#ifndef NET_H
#define NET_H

/*
 * 接続先サーバーの実行時解決。
 * EBOOT と同じフォルダの server.txt
 * (例: "192.168.0.5:8080" / "myhome.example.net:8080") を読み、
 * 無ければコンパイル時既定 (SERVER_HOST / SERVER_PORT) を使う。
 * ビルド済み EBOOT を配布しても、利用者が server.txt を書くだけで使える。
 */
void net_load_server_config(void);
const char *net_server_host(void);
int net_server_port(void);
/* path に共有トークンを付与する。0=成功 / <0=バッファ不足または引数不正 */
int net_build_path(char *out, int size, const char *path);
/* server.txt を読めた場合 1 (接続失敗時の案内文の出し分け用) */
int net_server_config_loaded(void);

/* Wi-Fi (apctl) 接続。0=成功 */
int net_init(void);

/* EAGAIN を待ち合わせる recv。>0 受信 / 0 切断 / <0 エラー */
int net_recv_wait(int sock, void *buf, int len);

/* 受信待ちを途中で打ち切れる版。abort_flag が 0 以外になったら諦めて負値を返す。
   再生停止のように「待っている側を早く終わらせたい」場面で使う */
int net_recv_wait_abortable(int sock, void *buf, int len,
                            const volatile int *abort_flag);

/* HTTP GET してボディ全体を buf に受け取る。戻り値: ボディ長 / <0 エラー */
int http_get(const char *host, int port, const char *path, char *buf, int bufsize);

/* バイナリ用。終端文字を付けず、受信バイト数を返す。<0 エラー */
int http_get_bin(const char *host, int port, const char *path,
                 void *buf, int bufsize);

/* HTTP GET してヘッダ読み飛ばし後のソケットを返す (ストリーミング用)。<0 エラー */
int http_open_stream(const char *host, int port, const char *path);

void net_close(int sock);

#endif
