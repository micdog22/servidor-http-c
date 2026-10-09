/*
 * strbuf.c: texto que cresce conforme a necessidade.
 */
#include "strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sb_init(strbuf *b)
{
    b->data = NULL;
    b->len = b->cap = 0;
    b->failed = false;
}

void sb_free(strbuf *b)
{
    free(b->data);
    sb_init(b);
}

static bool reserve(strbuf *b, size_t extra)
{
    if (b->failed) return false;
    if (b->len + extra + 1 <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra + 1) {
        if (cap > ((size_t)-1) / 2) {
            b->failed = true;
            return false;
        }
        cap *= 2;
    }
    char *p = realloc(b->data, cap);
    if (!p) {
        b->failed = true;
        return false;
    }
    b->data = p;
    b->cap = cap;
    return true;
}

void sb_append(strbuf *b, const char *s, size_t n)
{
    if (!reserve(b, n)) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void sb_puts(strbuf *b, const char *s)
{
    sb_append(b, s, strlen(s));
}

void sb_printf(strbuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0 || !reserve(b, (size_t)need)) {
        b->failed = true;
        return;
    }
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    b->len += (size_t)need;
}

void sb_html(strbuf *b, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&': sb_puts(b, "&amp;"); break;
        case '<': sb_puts(b, "&lt;"); break;
        case '>': sb_puts(b, "&gt;"); break;
        case '"': sb_puts(b, "&quot;"); break;
        case '\'': sb_puts(b, "&#39;"); break;
        default: sb_append(b, s, 1); break;
        }
    }
}
