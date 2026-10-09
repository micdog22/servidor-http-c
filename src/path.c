/*
 * path.c: decodificação de URL, normalização e confinamento na raiz.
 */
#include "path.h"

#include <string.h>

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

long url_decode(const char *in, size_t len, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (in[i] != '%') {
            out[o++] = in[i];
            continue;
        }
        if (i + 2 >= len) return -1;
        int hi = hexval((unsigned char)in[i + 1]), lo = hexval((unsigned char)in[i + 2]);
        if (hi < 0 || lo < 0) return -1;
        int c = hi * 16 + lo;
        if (c == 0) return -1; /* %00 cortaria o caminho ao meio numa função de C */
        out[o++] = (char)c;
        i += 2;
    }
    out[o] = '\0';
    return (long)o;
}

path_status path_normalize(const char *in, char *out, size_t cap, bool *trailing)
{
    size_t len = strlen(in), o = 0;
    *trailing = len > 0 && in[len - 1] == '/';
    const char *p = in;
    if (cap == 0) return PATH_TOO_LONG;
    while (*p) {
        while (*p == '/') p++;
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - s);
        if (n == 0) break;
        if (n == 1 && s[0] == '.') continue;                  /* "." não muda nada */
        if (n == 2 && s[0] == '.' && s[1] == '.') return PATH_TRAVERSAL;
        if (s[0] == '.') return PATH_HIDDEN;
        if (o + n + 2 > cap) return PATH_TOO_LONG;
        if (o) out[o++] = '/';
        memcpy(out + o, s, n);
        o += n;
    }
    out[o] = '\0';
    return PATH_OK;
}

bool path_inside(const char *root, const char *real)
{
    size_t n = strlen(root);
    /* A raiz "/" contém tudo. Fora isso, real tem de ser a raiz ou começar
     * com "raiz/" (só o prefixo não basta: /srv/site2 começa com /srv/site). */
    if (n == 1 && root[0] == '/') return real[0] == '/';
    return strncmp(real, root, n) == 0 && (real[n] == '\0' || real[n] == '/');
}

void url_encode_append(strbuf *b, const char *name)
{
    static const char HEX[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        unsigned char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
            c == '_' || c == '~') {
            sb_append(b, (const char *)p, 1);
        } else {
            char e[3] = { '%', HEX[c >> 4], HEX[c & 15] };
            sb_append(b, e, 3);
        }
    }
}
