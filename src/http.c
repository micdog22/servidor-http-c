/*
 * http.c: análise de requisições HTTP/1.x (RFC 9110 e RFC 9112) e utilidades.
 */
#include "http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Pequenas utilidades de texto
 * ------------------------------------------------------------------------ */

static bool is_tchar(unsigned char c)
{
    /* Caracteres permitidos num "token" (método, nome de cabeçalho). */
    return c && (isalnum(c) || strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static bool eq_nocase(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        if (lower((unsigned char)*a) != lower((unsigned char)*b)) return false;
    }
    return *a == *b;
}

static bool prefix_nocase(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++) {
        if (lower((unsigned char)*s) != lower((unsigned char)*prefix)) return false;
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Requisição
 * ------------------------------------------------------------------------ */

/* Procura a linha em branco que termina o cabeçalho. Aceita CRLF e também LF
 * sozinho, como a RFC 9112 permite. Devolve o índice logo depois dela, ou 0. */
static size_t header_end(const char *buf, size_t start, size_t len)
{
    for (size_t i = start; i < len; i++) {
        if (buf[i] != '\n') continue;
        if (i + 1 < len && buf[i + 1] == '\n') return i + 2;
        if (i + 2 < len && buf[i + 1] == '\r' && buf[i + 2] == '\n') return i + 3;
    }
    return 0;
}

http_parse_result http_parse_request(char *buf, size_t len, http_request *req)
{
    memset(req, 0, sizeof *req);

    /* Linhas em branco antes da requisição são ignoradas (RFC 9112, 2.2). */
    size_t start = 0;
    while (start < len && (buf[start] == '\r' || buf[start] == '\n')) start++;

    char *first_nl = memchr(buf + start, '\n', len - start);
    if (!first_nl && len - start > HTTP_MAX_TARGET + 32) return HTTP_PARSE_URI_TOO_LONG;
    if (first_nl && (size_t)(first_nl - (buf + start)) > HTTP_MAX_TARGET + 32) return HTTP_PARSE_URI_TOO_LONG;

    size_t end = header_end(buf, start, len);
    if (end == 0) return len >= HTTP_MAX_HEADER_BYTES ? HTTP_PARSE_TOO_LARGE : HTTP_PARSE_INCOMPLETE;
    if (end > HTTP_MAX_HEADER_BYTES) return HTTP_PARSE_TOO_LARGE;
    req->length = end;

    /* Daqui em diante a requisição está inteira e pode ser cortada no lugar. */
    char *line = buf + start;
    char *nl = memchr(line, '\n', end - start);
    char *eol = (nl > line && nl[-1] == '\r') ? nl - 1 : nl;
    *eol = '\0';

    /* Linha de requisição: MÉTODO SP ALVO SP VERSÃO */
    char *sp1 = strchr(line, ' ');
    if (!sp1) return HTTP_PARSE_BAD;
    char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2 || strchr(sp2 + 1, ' ')) return HTTP_PARSE_BAD;
    *sp1 = '\0';
    *sp2 = '\0';
    char *method = line, *target = sp1 + 1, *version = sp2 + 1;

    size_t mlen = strlen(method);
    if (mlen == 0 || mlen > 15) return HTTP_PARSE_BAD;
    for (size_t i = 0; i < mlen; i++) {
        if (!is_tchar((unsigned char)method[i])) return HTTP_PARSE_BAD;
    }
    size_t tlen = strlen(target);
    if (tlen == 0) return HTTP_PARSE_BAD;
    if (tlen > HTTP_MAX_TARGET) return HTTP_PARSE_URI_TOO_LONG;
    for (size_t i = 0; i < tlen; i++) {
        unsigned char c = (unsigned char)target[i];
        if (c <= 0x20 || c >= 0x7F) return HTTP_PARSE_BAD; /* não ASCII tem de vir em %XX */
    }
    if (strncmp(version, "HTTP/", 5) != 0 || !isdigit((unsigned char)version[5]) || version[6] != '.' ||
        !isdigit((unsigned char)version[7]) || version[8] != '\0') {
        return HTTP_PARSE_BAD;
    }
    if (version[5] != '1' || (version[7] != '0' && version[7] != '1')) return HTTP_PARSE_VERSION;
    req->method = method;
    req->target = target;
    req->minor = version[7] - '0';

    /* Cabeçalhos: NOME ":" espaços VALOR espaços */
    int hosts = 0;
    line = nl + 1;
    while (line < buf + end) {
        nl = memchr(line, '\n', (size_t)(buf + end - line));
        eol = (nl > line && nl[-1] == '\r') ? nl - 1 : nl;
        if (eol == line) break; /* linha em branco: fim */
        *eol = '\0';
        if (line[0] == ' ' || line[0] == '\t') return HTTP_PARSE_BAD; /* continuação obsoleta */
        char *colon = strchr(line, ':');
        if (!colon || colon == line) return HTTP_PARSE_BAD;
        for (char *c = line; c < colon; c++) {
            if (!is_tchar((unsigned char)*c)) return HTTP_PARSE_BAD; /* inclui espaço antes do ':' */
        }
        *colon = '\0';
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t') value++;
        char *vend = value + strlen(value);
        while (vend > value && (vend[-1] == ' ' || vend[-1] == '\t')) *--vend = '\0';
        for (char *c = value; *c; c++) {
            unsigned char u = (unsigned char)*c;
            if ((u < 0x20 && u != '\t') || u == 0x7F) return HTTP_PARSE_BAD;
        }
        if (req->nheaders == HTTP_MAX_HEADERS) return HTTP_PARSE_TOO_LARGE;
        req->headers[req->nheaders].name = line;
        req->headers[req->nheaders].value = value;
        req->nheaders++;
        if (eq_nocase(line, "Host")) hosts++;
        line = nl + 1;
    }
    /* HTTP/1.1 exige exatamente um Host (RFC 9112, 3.2). */
    if ((req->minor == 1 && hosts != 1) || hosts > 1) return HTTP_PARSE_BAD;
    return HTTP_PARSE_OK;
}

const char *http_header_get(const http_request *req, const char *name)
{
    for (int i = 0; i < req->nheaders; i++) {
        if (eq_nocase(req->headers[i].name, name)) return req->headers[i].value;
    }
    return NULL;
}

/* Procura token numa lista separada por vírgulas (sem diferenciar maiúsculas). */
static bool list_has_token(const char *list, const char *token)
{
    size_t n = strlen(token);
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *s = p;
        while (*p && *p != ',') p++;
        const char *e = p;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
        if ((size_t)(e - s) == n) {
            bool same = true;
            for (size_t i = 0; i < n; i++) {
                if (lower((unsigned char)s[i]) != lower((unsigned char)token[i])) same = false;
            }
            if (same) return true;
        }
    }
    return false;
}

bool http_keep_alive(const http_request *req)
{
    bool close = false, keep = false;
    for (int i = 0; i < req->nheaders; i++) {
        if (!eq_nocase(req->headers[i].name, "Connection")) continue;
        if (list_has_token(req->headers[i].value, "close")) close = true;
        if (list_has_token(req->headers[i].value, "keep-alive")) keep = true;
    }
    if (close) return false;
    return req->minor == 1 || keep; /* no HTTP/1.0, só com "Connection: keep-alive" */
}

bool http_has_body(const http_request *req)
{
    if (http_header_get(req, "Transfer-Encoding")) return true;
    const char *cl = http_header_get(req, "Content-Length");
    if (!cl) return false;
    for (const char *c = cl; *c; c++) {
        if (*c != '0') return true; /* qualquer coisa além de "0" (inclusive lixo) conta como corpo */
    }
    return cl[0] == '\0';
}

long http_target_path(const char *target, const char **path)
{
    const char *p = target;
    if (prefix_nocase(p, "http://") || prefix_nocase(p, "https://")) {
        /* Forma absoluta (RFC 9112, 3.2.2): pula esquema e autoridade. */
        p = strstr(p, "//") + 2;
        while (*p && *p != '/' && *p != '?' && *p != '#') p++;
        if (*p != '/') {
            *path = "/";
            return 1;
        }
    }
    if (*p != '/') return -1;
    *path = p;
    size_t n = strcspn(p, "?#");
    return (long)n;
}

/* ---------------------------------------------------------------------------
 * Datas
 * ------------------------------------------------------------------------ */

static const char *const WDAY[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const MONTH[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* Dias desde 1970-01-01 para uma data do calendário gregoriano proléptico. */
long long days_from_civil(long long y, unsigned m, unsigned d)
{
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);                          /* [0, 399] */
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;    /* [0, 365] */
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;              /* [0, 146096] */
    return era * 146097 + (long long)doe - 719468;
}

void civil_from_days(long long z, long long *y, unsigned *m, unsigned *d)
{
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (long long)yoe + era * 400 + (*m <= 2);
}

static void split_time(time_t t, long long *days, int *hh, int *mm, int *ss)
{
    long long s = (long long)t;
    long long d = s / 86400, rem = s % 86400;
    if (rem < 0) {
        rem += 86400;
        d--;
    }
    *days = d;
    *hh = (int)(rem / 3600);
    *mm = (int)(rem / 60 % 60);
    *ss = (int)(rem % 60);
}

void http_format_date(time_t t, char out[32])
{
    long long days, y;
    unsigned m, d;
    int hh, mm, ss;
    split_time(t, &days, &hh, &mm, &ss);
    civil_from_days(days, &y, &m, &d);
    long long w = (days + 4) % 7; /* 1970-01-01 foi uma quinta-feira */
    if (w < 0) w += 7;
    snprintf(out, 32, "%s, %02u %s %04lld %02d:%02d:%02d GMT", WDAY[w], d, MONTH[m - 1], y, hh, mm, ss);
}

void http_format_log_date(time_t t, char out[32])
{
    long long days, y;
    unsigned m, d;
    int hh, mm, ss;
    split_time(t, &days, &hh, &mm, &ss);
    civil_from_days(days, &y, &m, &d);
    snprintf(out, 32, "%02u/%s/%04lld:%02d:%02d:%02d +0000", d, MONTH[m - 1], y, hh, mm, ss);
}

static int month_index(const char *s)
{
    for (int i = 0; i < 12; i++) {
        if (strncmp(s, MONTH[i], 3) == 0) return i;
    }
    return -1;
}

static bool valid_fields(long long y, int mon, int d, int hh, int mm, int ss)
{
    static const int DAYS[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return y >= 1 && y <= 9999 && mon >= 0 && mon < 12 && d >= 1 && d <= DAYS[mon] && hh >= 0 &&
           hh < 24 && mm >= 0 && mm < 60 && ss >= 0 && ss <= 60;
}

/* Aceita os três formatos que a RFC 9110 exige que um servidor entenda:
 *   IMF-fixdate: Sun, 06 Nov 1994 08:49:37 GMT
 *   RFC 850:     Sunday, 06-Nov-94 08:49:37 GMT
 *   asctime:     Sun Nov  6 08:49:37 1994 */
int http_parse_date(const char *s, time_t *out)
{
    char wday[16], mon[4], tz[4];
    int d, hh, mm, ss, n = 0;
    long long y;
    int mi;
    if (sscanf(s, "%3[A-Za-z], %2d %3[A-Za-z] %4lld %2d:%2d:%2d %3[A-Z]%n", wday, &d, mon, &y, &hh, &mm, &ss, tz,
               &n) == 8 &&
        s[n] == '\0' && strcmp(tz, "GMT") == 0) {
        /* IMF-fixdate */
    } else if (sscanf(s, "%15[A-Za-z], %2d-%3[A-Za-z]-%2lld %2d:%2d:%2d %3[A-Z]%n", wday, &d, mon, &y, &hh, &mm,
                      &ss, tz, &n) == 8 &&
               s[n] == '\0' && strcmp(tz, "GMT") == 0) {
        y += y < 70 ? 2000 : 1900; /* ano com dois dígitos */
    } else if (sscanf(s, "%3[A-Za-z] %3[A-Za-z] %2d %2d:%2d:%2d %4lld%n", wday, mon, &d, &hh, &mm, &ss, &y, &n) ==
                   7 &&
               s[n] == '\0') {
        /* asctime */
    } else {
        return -1;
    }
    mi = month_index(mon);
    if (!valid_fields(y, mi, d, hh, mm, ss)) return -1;
    if (ss == 60) ss = 59; /* segundo bissexto */
    long long days = days_from_civil(y, (unsigned)mi + 1, (unsigned)d);
    *out = (time_t)(days * 86400 + hh * 3600 + mm * 60 + ss);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Range
 * ------------------------------------------------------------------------ */

static bool parse_u64(const char **p, uint64_t *out)
{
    const char *s = *p;
    if (!isdigit((unsigned char)*s)) return false;
    uint64_t v = 0;
    while (isdigit((unsigned char)*s)) {
        unsigned d = (unsigned)(*s - '0');
        if (v > (UINT64_MAX - d) / 10) return false; /* estouro */
        v = v * 10 + d;
        s++;
    }
    *p = s;
    *out = v;
    return true;
}

range_result http_parse_range(const char *value, uint64_t size, uint64_t *first, uint64_t *last)
{
    if (!value) return RANGE_NONE;
    /* Unidade desconhecida: o servidor deve ignorar o Range (RFC 9110, 14.2). */
    if (!prefix_nocase(value, "bytes=")) return RANGE_NONE;
    const char *p = value + 6;
    /* Vários intervalos pediriam multipart/byteranges; aqui a resposta é o
     * arquivo inteiro, o que a RFC permite. */
    if (strchr(p, ',')) return RANGE_NONE;
    while (*p == ' ' || *p == '\t') p++;

    uint64_t a = 0, b = 0;
    bool has_a = parse_u64(&p, &a);
    if (*p != '-') return RANGE_UNSATISFIABLE;
    p++;
    bool has_b = parse_u64(&p, &b);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '\0' || (!has_a && !has_b)) return RANGE_UNSATISFIABLE;

    if (!has_a) {
        /* "-n": os últimos n bytes */
        if (b == 0 || size == 0) return RANGE_UNSATISFIABLE;
        *first = b >= size ? 0 : size - b;
        *last = size - 1;
        return RANGE_OK;
    }
    if (has_b && b < a) return RANGE_UNSATISFIABLE;
    if (a >= size) return RANGE_UNSATISFIABLE;
    *first = a;
    *last = (!has_b || b >= size) ? size - 1 : b;
    return RANGE_OK;
}

/* ---------------------------------------------------------------------------
 * ETag
 * ------------------------------------------------------------------------ */

bool http_etag_matches(const char *list, const char *etag)
{
    if (!list || !etag) return false;
    const char *e = strncmp(etag, "W/", 2) == 0 ? etag + 2 : etag;
    size_t elen = strlen(e);
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (*p == '*') return true;
        if (strncmp(p, "W/", 2) == 0) p += 2; /* comparação fraca: W/ não importa */
        if (*p != '"') {
            while (*p && *p != ',') p++; /* item inválido: pula */
            continue;
        }
        const char *q = strchr(p + 1, '"');
        if (!q) return false;
        if ((size_t)(q + 1 - p) == elen && memcmp(p, e, elen) == 0) return true;
        p = q + 1;
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Status
 * ------------------------------------------------------------------------ */

static const struct {
    int         code;
    const char *reason;
    const char *title;
} STATUS[] = {
    { 200, "OK", "OK" },
    { 206, "Partial Content", "Conteúdo parcial" },
    { 301, "Moved Permanently", "Movido" },
    { 304, "Not Modified", "Não modificado" },
    { 400, "Bad Request", "Requisição inválida" },
    { 403, "Forbidden", "Acesso negado" },
    { 404, "Not Found", "Não encontrado" },
    { 405, "Method Not Allowed", "Método não permitido" },
    { 408, "Request Timeout", "Tempo esgotado" },
    { 414, "URI Too Long", "Endereço longo demais" },
    { 416, "Range Not Satisfiable", "Intervalo impossível" },
    { 431, "Request Header Fields Too Large", "Cabeçalhos grandes demais" },
    { 500, "Internal Server Error", "Erro interno" },
    { 503, "Service Unavailable", "Servidor ocupado" },
    { 505, "HTTP Version Not Supported", "Versão do HTTP não suportada" },
};

const char *http_reason(int status)
{
    for (size_t i = 0; i < sizeof STATUS / sizeof STATUS[0]; i++) {
        if (STATUS[i].code == status) return STATUS[i].reason;
    }
    return "Unknown";
}

const char *http_title_pt(int status)
{
    for (size_t i = 0; i < sizeof STATUS / sizeof STATUS[0]; i++) {
        if (STATUS[i].code == status) return STATUS[i].title;
    }
    return "Erro";
}
