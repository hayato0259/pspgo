#include <string.h>
#include <stdlib.h>
#include "tsv.h"

/* buf 内の各行の先頭を順に返すための補助 */
static const char *next_line(const char *p)
{
    const char *nl = strchr(p, '\n');
    return nl ? nl + 1 : NULL;
}

int tsv_value(const char *buf, const char *key, char *out, int outsize)
{
    int klen = (int)strlen(key);
    const char *p = buf;

    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '\t') {
            const char *v = p + klen + 1;
            const char *end = strpbrk(v, "\r\n");
            int len = end ? (int)(end - v) : (int)strlen(v);
            if (len > outsize - 1)
                len = outsize - 1;
            memcpy(out, v, len);
            out[len] = '\0';
            return 1;
        }
        p = next_line(p);
    }
    return 0;
}

int tsv_qr(const char *buf, unsigned char *out, int max_side)
{
    const char *line = NULL;
    const char *p = buf;

    while (p && *p) {
        if (strncmp(p, "qr\t", 3) == 0) {
            line = p + 3;
            break;
        }
        p = next_line(p);
    }
    if (!line)
        return 0;

    int side = atoi(line);
    if (side <= 0 || side > max_side)
        return 0;

    const char *bits = strchr(line, '\t');
    if (!bits)
        return 0;
    bits++;

    int need = side * side;
    for (int i = 0; i < need; i++) {
        if (bits[i] != '0' && bits[i] != '1')
            return 0;   /* 途中で切れている */
        out[i] = (unsigned char)(bits[i] == '1');
    }
    return side;
}
