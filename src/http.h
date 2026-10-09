/*
 * http.h: a parte do HTTP/1.1 que não depende de rede.
 *
 * Analisador de requisições com limites, datas no formato do HTTP,
 * cabeçalho Range, comparação de ETags e textos de status. Nada aqui faz
 * entrada ou saída, então tudo é testado direto, sem sockets.
 */
#ifndef HTTP_H
#define HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define HTTP_MAX_HEADER_BYTES (16 * 1024) /* linha de requisição + cabeçalhos */
#define HTTP_MAX_HEADERS      100
#define HTTP_MAX_TARGET       8192        /* tamanho máximo do caminho pedido */

typedef struct {
    const char *name;   /* apontam para dentro do buffer da requisição */
    const char *value;
} http_header;

typedef struct {
    const char *method;
    const char *target;         /* como veio: "/pasta/arquivo%20x.txt?q=1" */
    int         minor;          /* HTTP/1.minor: 0 ou 1 */
    http_header headers[HTTP_MAX_HEADERS];
    int         nheaders;
    size_t      length;         /* bytes do cabeçalho, incluindo a linha em branco */
} http_request;

typedef enum {
    HTTP_PARSE_OK = 0,
    HTTP_PARSE_INCOMPLETE,      /* faltam bytes: ainda não chegou a linha em branco */
    HTTP_PARSE_BAD,             /* 400 */
    HTTP_PARSE_TOO_LARGE,       /* 431: cabeçalho grande demais ou cabeçalhos demais */
    HTTP_PARSE_URI_TOO_LONG,    /* 414 */
    HTTP_PARSE_VERSION          /* 505: não é HTTP/1.0 nem HTTP/1.1 */
} http_parse_result;

/* Analisa uma requisição em buf[0..len). Só modifica buf (trocando
 * separadores por '\0') quando a requisição está completa e é válida o
 * bastante para isso; com INCOMPLETE o buffer fica intacto. */
http_parse_result http_parse_request(char *buf, size_t len, http_request *req);

/* Valor de um cabeçalho (nome sem diferenciar maiúsculas), ou NULL. */
const char *http_header_get(const http_request *req, const char *name);

/* A conexão continua aberta depois da resposta? (Connection e versão) */
bool http_keep_alive(const http_request *req);

/* A requisição tem corpo? (Content-Length maior que zero ou Transfer-Encoding) */
bool http_has_body(const http_request *req);

/* Separa o caminho do alvo: tira "http://host" da forma absoluta e corta em
 * '?' ou '#'. Devolve o tamanho do caminho, ou -1 se o alvo não for válido. */
long http_target_path(const char *target, const char **path);

/* Datas no formato IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT". */
void http_format_date(time_t t, char out[32]);
int  http_parse_date(const char *s, time_t *out);   /* 0 ou -1 */
/* Data para o log no formato Common Log Format: "06/Nov/1994:08:49:37 +0000". */
void http_format_log_date(time_t t, char out[32]);

/* Conversão de datas sem depender do fuso nem de funções não padronizadas
 * (algoritmo de Howard Hinnant para o calendário gregoriano). */
long long days_from_civil(long long y, unsigned m, unsigned d);
void      civil_from_days(long long z, long long *y, unsigned *m, unsigned *d);

typedef enum {
    RANGE_NONE,           /* sem Range, ou Range que deve ser ignorado: resposta 200 completa */
    RANGE_OK,             /* um intervalo válido: resposta 206 */
    RANGE_UNSATISFIABLE   /* intervalo inválido ou fora do arquivo: resposta 416 */
} range_result;

/* Interpreta "bytes=início-fim", "bytes=início-" ou "bytes=-sufixo" para um
 * arquivo de size bytes. Em RANGE_OK, *first e *last são inclusivos. */
range_result http_parse_range(const char *value, uint64_t size, uint64_t *first, uint64_t *last);

/* If-None-Match: lista de ETags (ou "*"), com comparação fraca (ignora W/). */
bool http_etag_matches(const char *list, const char *etag);

const char *http_reason(int status);       /* "Not Found" */
const char *http_title_pt(int status);     /* "Não encontrado" */

#endif
