/*
 * ネットワーク層: apctl 接続と HTTP/1.0 GET。
 * BSD ソケットラッパーは使わず sceNetInet* を直接呼ぶ
 * (新 NID が PPSSPP 未実装のため。実機でも本来の API はこちら)。
 */
#include <pspkernel.h>
#include <psputility.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "net.h"
#include "common.h"

/* --- 接続先サーバーの実行時解決 ------------------------------------------
 * EBOOT と同じフォルダの server.txt を読む。書式は 1 行目に
 *   192.168.0.5:8080   (ポート省略時は 8080)
 * ※ この層は DNS を引かないため、ホスト名ではなく IPv4 アドレスを書くこと。
 * ファイルが無ければコンパイル時既定 (開発時は 127.0.0.1 = PPSSPP) を使う。
 */
static char g_srv_host[64] = SERVER_HOST;
static int g_srv_port = SERVER_PORT;
static int g_srv_loaded = 0;

void net_load_server_config(void)
{
    FILE *fp = fopen("server.txt", "r");
    if (!fp)
        return;
    char line[128] = "";
    if (fgets(line, sizeof(line), fp)) {
        /* 前後の空白・改行を落とす */
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        char *e = p + strlen(p);
        while (e > p && (e[-1] == '\n' || e[-1] == '\r' ||
                         e[-1] == ' ' || e[-1] == '\t'))
            *--e = '\0';

        char *colon = strchr(p, ':');
        if (colon) {
            *colon = '\0';
            int port = atoi(colon + 1);
            if (port > 0 && port < 65536)
                g_srv_port = port;
        }
        if (p[0]) {
            snprintf(g_srv_host, sizeof(g_srv_host), "%s", p);
            g_srv_loaded = 1;
        }
    }
    fclose(fp);
}

const char *net_server_host(void) { return g_srv_host; }
int net_server_port(void) { return g_srv_port; }
int net_server_config_loaded(void) { return g_srv_loaded; }

struct in_addr_psp { unsigned int s_addr; };
struct sockaddr_in_psp {
    unsigned char  sin_len;
    unsigned char  sin_family;
    unsigned short sin_port;
    struct in_addr_psp sin_addr;
    char sin_zero[8];
};
#define PSP_AF_INET 2
#define PSP_SOCK_STREAM 1

static unsigned short psp_htons(unsigned short v)
{
    return (unsigned short)((v << 8) | (v >> 8));
}

static unsigned int ipv4_aton(const char *s)
{
    unsigned int parts[4] = {0, 0, 0, 0};
    int i = 0;
    while (*s && i < 4) {
        unsigned int v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (unsigned int)(*s - '0');
            s++; digits++;
        }
        if (!digits || v > 255)
            return 0xFFFFFFFFu;
        parts[i++] = v;
        if (*s == '.')
            s++;
        else
            break;
    }
    if (i != 4)
        return 0xFFFFFFFFu;
    return parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
}

int net_init(void)
{
    int rc;

    rc = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    if (rc < 0) return rc;
    rc = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
    if (rc < 0) return rc;

    rc = sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024);
    if (rc < 0) return rc;
    rc = sceNetInetInit();
    if (rc < 0) return rc;
    rc = sceNetApctlInit(0x8000, 48);
    if (rc < 0) return rc;

    rc = sceNetApctlConnect(1); /* 本体設定の接続 1 番 */
    if (rc < 0) return rc;

    int waited = 0;
    for (;;) {
        int state;
        rc = sceNetApctlGetState(&state);
        if (rc < 0) return rc;
        if (state == PSP_NET_APCTL_STATE_GOT_IP)
            break;
        if (waited > 30 * 1000 * 1000) /* 30秒でタイムアウト */
            return -100;
        sceKernelDelayThread(100 * 1000);
        waited += 100 * 1000;
    }
    return 0;
}

int net_recv_wait(int sock, void *buf, int len)
{
    int tries = 0;
    for (;;) {
        int got = sceNetInetRecv(sock, buf, len, 0);
        if (got >= 0)
            return got;
        if (sceNetInetGetErrno() != 11 /* EAGAIN */)
            return got;
        if (++tries > 2000) /* 約10秒でタイムアウト */
            return -1;
        sceKernelDelayThread(5 * 1000);
    }
}

void net_close(int sock)
{
    if (sock >= 0)
        sceNetInetClose(sock);
}

/* 接続して GET リクエストを送り、ヘッダを読み飛ばした状態のソケットを返す */
static int http_request(const char *host, int port, const char *path)
{
    int sock = sceNetInetSocket(PSP_AF_INET, PSP_SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    struct sockaddr_in_psp addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_len = sizeof(addr);
    addr.sin_family = PSP_AF_INET;
    addr.sin_port = psp_htons((unsigned short)port);
    addr.sin_addr.s_addr = ipv4_aton(host);
    if (addr.sin_addr.s_addr == 0xFFFFFFFFu) {
        net_close(sock);
        return -2;
    }

    if (sceNetInetConnect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        net_close(sock);
        return -3;
    }

    char req[640];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.0\r\n"
                     "Host: %s\r\n"
                     "User-Agent: pspgo-ytmusic/0.1\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     path, host);
    if (sceNetInetSend(sock, req, n, 0) != n) {
        net_close(sock);
        return -4;
    }

    char c;
    int matched = 0;
    while (matched < 4) {
        if (net_recv_wait(sock, &c, 1) != 1) {
            net_close(sock);
            return -5;
        }
        if ((matched % 2 == 0 && c == '\r') || (matched % 2 == 1 && c == '\n'))
            matched++;
        else
            matched = (c == '\r') ? 1 : 0;
    }
    return sock;
}

int http_open_stream(const char *host, int port, const char *path)
{
    return http_request(host, port, path);
}

int http_get_bin(const char *host, int port, const char *path,
                 void *buf, int bufsize)
{
    int sock = http_request(host, port, path);
    if (sock < 0)
        return sock;

    unsigned char *dst = buf;
    int total = 0;
    while (total < bufsize) {
        int got = net_recv_wait(sock, dst + total, bufsize - total);
        if (got < 0) {
            net_close(sock);
            return -6;
        }
        if (got == 0)
            break;  /* サーバーが閉じた = 本文終端 */
        total += got;
    }
    net_close(sock);
    return total;
}

int http_get(const char *host, int port, const char *path, char *buf, int bufsize)
{
    int sock = http_request(host, port, path);
    if (sock < 0)
        return sock;

    int total = 0;
    while (total < bufsize - 1) {
        int got = net_recv_wait(sock, buf + total, bufsize - 1 - total);
        if (got < 0) {
            net_close(sock);
            return -6;
        }
        if (got == 0)
            break; /* サーバーが閉じた = ボディ終端 (Connection: close 前提) */
        total += got;
    }
    buf[total] = '\0';
    net_close(sock);
    return total;
}
