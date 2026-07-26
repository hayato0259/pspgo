#ifndef COMMON_H
#define COMMON_H

/* サーバー接続先。Makefile の -D で差し替える */
#ifndef SERVER_HOST
#define SERVER_HOST "127.0.0.1"
#endif
#ifndef SERVER_PORT
#define SERVER_PORT 8080
#endif

#define SCR_W 480
#define SCR_H 272

#endif
