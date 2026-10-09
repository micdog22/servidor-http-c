/*
 * path.h: do caminho da URL para um caminho seguro dentro da pasta servida.
 */
#ifndef PATH_H
#define PATH_H

#include <stdbool.h>
#include <stddef.h>

#include "strbuf.h"

/* Decodifica %XX (RFC 3986). out precisa de len + 1 bytes. Devolve o
 * tamanho, ou -1 se houver um %XX malformado ou um byte nulo (%00). */
long url_decode(const char *in, size_t len, char *out);

typedef enum {
    PATH_OK,
    PATH_TRAVERSAL,  /* tem ".." depois de decodificado: 403 */
    PATH_HIDDEN,     /* algum trecho começa com ponto (.git, .env): 404 */
    PATH_TOO_LONG
} path_status;

/* Normaliza um caminho já decodificado: junta barras repetidas, descarta
 * "." e recusa "..". O resultado é relativo à raiz, sem barra inicial
 * ("" é a própria raiz). *trailing indica se o pedido terminava em barra. */
path_status path_normalize(const char *in, char *out, size_t cap, bool *trailing);

/* real está dentro de root (que é um caminho canônico, sem barra no fim)? */
bool path_inside(const char *root, const char *real);

/* Codifica um nome de arquivo para usar num href: tudo que não é letra,
 * dígito ou um de "-._~" vira %XX. */
void url_encode_append(strbuf *b, const char *name);

#endif
