/*
 * strbuf.h: texto que cresce conforme a necessidade (respostas, listagens).
 */
#ifndef STRBUF_H
#define STRBUF_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char  *data;
    size_t len, cap;
    bool   failed;   /* faltou memória em algum momento */
} strbuf;

void sb_init(strbuf *b);
void sb_free(strbuf *b);
void sb_append(strbuf *b, const char *s, size_t n);
void sb_puts(strbuf *b, const char *s);
void sb_printf(strbuf *b, const char *fmt, ...);
/* Acrescenta s com &, <, >, " e ' trocados por entidades HTML. */
void sb_html(strbuf *b, const char *s);

#endif
