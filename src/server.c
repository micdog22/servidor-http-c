/*
 * server.c: o ciclo de vida de uma conexão.
 *
 * ler o cabeçalho (com prazo) -> analisar -> responder -> registrar no log
 * -> se for keep-alive, voltar ao começo com o que sobrou no buffer.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE

#include "server.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "http.h"
#include "mime.h"
#include "path.h"
#include "strbuf.h"

#define SERVER_NAME "servidor-http/1.0"
#define FILE_CHUNK  65536

typedef struct {
    server_config *cfg;
    int            fd;
    char           client[64];
    bool           head;        /* HEAD: só o cabeçalho, sem corpo */
    bool           keep_alive;
    int            status;
    uint64_t       sent;        /* bytes de corpo enviados (para o log) */
    bool           broken;      /* erro ao escrever: a conexão acabou */
} conn;

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void format_peer(const struct sockaddr_storage *peer, char *out, size_t cap)
{
    if (peer->ss_family == AF_INET) {
        struct sockaddr_in a;
        memcpy(&a, peer, sizeof a);
        inet_ntop(AF_INET, &a.sin_addr, out, (socklen_t)cap);
    } else if (peer->ss_family == AF_INET6) {
        struct sockaddr_in6 a;
        memcpy(&a, peer, sizeof a);
        if (IN6_IS_ADDR_V4MAPPED(&a.sin6_addr)) {
            /* Cliente IPv4 num socket de pilha dupla: ::ffff:127.0.0.1 vira 127.0.0.1. */
            inet_ntop(AF_INET, &a.sin6_addr.s6_addr[12], out, (socklen_t)cap);
        } else {
            inet_ntop(AF_INET6, &a.sin6_addr, out, (socklen_t)cap);
        }
    } else {
        snprintf(out, cap, "-");
    }
}

static bool send_all(conn *c, const void *data, size_t len)
{
    const char *p = data;
    while (len > 0 && !c->broken) {
        ssize_t n = write(c->fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            c->broken = true; /* EPIPE, ECONNRESET ou o prazo de SO_SNDTIMEO */
            break;
        }
        p += n;
        len -= (size_t)n;
    }
    return !c->broken;
}

/* Linha de status e cabeçalhos. length < 0: sem Content-Length (304). */
static void send_head(conn *c, int status, const char *ctype, long long length, const char *extra)
{
    char date[32];
    http_format_date(time(NULL), date);
    strbuf b;
    sb_init(&b);
    sb_printf(&b, "HTTP/1.1 %d %s\r\nDate: %s\r\nServer: %s\r\n", status, http_reason(status), date, SERVER_NAME);
    if (ctype) sb_printf(&b, "Content-Type: %s\r\n", ctype);
    if (length >= 0) sb_printf(&b, "Content-Length: %lld\r\n", length);
    if (extra) sb_puts(&b, extra);
    if (c->keep_alive) {
        sb_printf(&b, "Connection: keep-alive\r\nKeep-Alive: timeout=%d, max=%d\r\n", KEEPALIVE_TIMEOUT_MS / 1000,
                  MAX_REQUESTS);
    } else {
        sb_puts(&b, "Connection: close\r\n");
    }
    sb_puts(&b, "\r\n");
    c->status = status;
    if (b.failed) {
        c->broken = true;
    } else {
        send_all(c, b.data, b.len);
    }
    sb_free(&b);
}

static const char *detail_pt(int status)
{
    switch (status) {
    case 301: return "Este endereço mudou: o certo termina com barra.";
    case 400: return "O servidor não entendeu a requisição.";
    case 403: return "Você não tem permissão para acessar este endereço.";
    case 404: return "Não existe nada neste endereço.";
    case 405: return "Este servidor só aceita os métodos GET e HEAD.";
    case 408: return "A requisição demorou demais para chegar.";
    case 414: return "O endereço pedido é longo demais.";
    case 416: return "O intervalo de bytes pedido não existe neste arquivo.";
    case 431: return "Os cabeçalhos da requisição passam do limite de 16 KB.";
    case 503: return "Muitas conexões ao mesmo tempo. Tente de novo em instantes.";
    case 505: return "Este servidor fala HTTP/1.0 e HTTP/1.1.";
    default: return "Algo deu errado ao atender a requisição.";
    }
}

/* Página de erro pequena em HTML. Não repete nada que veio do cliente, para
 * não abrir espaço para injeção de HTML. */
static void send_error(conn *c, int status, const char *extra)
{
    strbuf body;
    sb_init(&body);
    sb_printf(&body,
              "<!doctype html>\n<html lang=\"pt-BR\">\n<head><meta charset=\"utf-8\"><title>%d %s</title></head>\n"
              "<body>\n<h1>%d %s</h1>\n<p>%s</p>\n</body>\n</html>\n",
              status, http_title_pt(status), status, http_title_pt(status), detail_pt(status));
    if (body.failed) {
        c->broken = true;
        sb_free(&body);
        return;
    }
    send_head(c, status, "text/html; charset=utf-8", (long long)body.len, extra);
    if (!c->head && send_all(c, body.data, body.len)) c->sent += body.len;
    sb_free(&body);
}

/* Escapa aspas, barra invertida e bytes não imprimíveis, para que ninguém
 * consiga forjar linhas no log mandando um caminho esquisito. */
static void append_log_text(strbuf *b, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            sb_printf(b, "\\%c", *p);
        } else if (*p < 0x20 || *p >= 0x7F) {
            sb_printf(b, "\\x%02X", *p);
        } else {
            sb_append(b, (const char *)p, 1);
        }
    }
}

/* Common Log Format: host ident authuser [data] "requisição" status bytes */
static void log_request(conn *c, const http_request *req)
{
    if (!c->cfg->log) return;
    char date[32];
    http_format_log_date(time(NULL), date);
    strbuf line;
    sb_init(&line);
    sb_printf(&line, "%s - - [%s] \"", c->client, date);
    if (req && req->method) {
        append_log_text(&line, req->method);
        sb_puts(&line, " ");
        append_log_text(&line, req->target);
        sb_printf(&line, " HTTP/1.%d", req->minor);
    } else {
        sb_puts(&line, "-");
    }
    if (c->sent) {
        sb_printf(&line, "\" %d %llu\n", c->status, (unsigned long long)c->sent);
    } else {
        sb_printf(&line, "\" %d -\n", c->status);
    }
    if (!line.failed) {
        pthread_mutex_lock(&c->cfg->log_lock);
        fputs(line.data, c->cfg->log);
        fflush(c->cfg->log);
        pthread_mutex_unlock(&c->cfg->log_lock);
    }
    sb_free(&line);
}

/* ---------------------------------------------------------------------------
 * Arquivos
 * ------------------------------------------------------------------------ */

static void send_file_range(conn *c, int f, uint64_t offset, uint64_t length)
{
    char *chunk = malloc(FILE_CHUNK);
    if (!chunk) {
        c->broken = true;
        return;
    }
    while (length > 0 && !c->broken) {
        size_t want = length < FILE_CHUNK ? (size_t)length : FILE_CHUNK;
        ssize_t n = pread(f, chunk, want, (off_t)offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            /* O arquivo encolheu no meio do envio: o Content-Length prometido não
             * será cumprido, então a única saída honesta é fechar a conexão. */
            c->broken = true;
            break;
        }
        if (!send_all(c, chunk, (size_t)n)) break;
        c->sent += (uint64_t)n;
        offset += (uint64_t)n;
        length -= (uint64_t)n;
    }
    free(chunk);
}

static void serve_file(conn *c, const http_request *req, const char *real)
{
    int f = open(real, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (f < 0) {
        send_error(c, errno == EACCES ? 403 : 404, NULL);
        return;
    }
    struct stat st;
    if (fstat(f, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(f);
        send_error(c, 403, NULL);
        return;
    }
    uint64_t size = (uint64_t)st.st_size;

    /* Validadores: a ETag muda quando muda o inode, o tamanho ou a data. */
    char etag[80], lastmod[32], validators[256];
    snprintf(etag, sizeof etag, "\"%llx-%llx-%llx\"", (unsigned long long)st.st_ino, (unsigned long long)size,
             (unsigned long long)st.st_mtime);
    http_format_date(st.st_mtime, lastmod);
    snprintf(validators, sizeof validators, "ETag: %s\r\nLast-Modified: %s\r\nCache-Control: no-cache\r\n", etag,
             lastmod);

    /* Requisição condicional (RFC 9110, 13.2.2): If-None-Match vence If-Modified-Since. */
    const char *inm = http_header_get(req, "If-None-Match");
    const char *ims = http_header_get(req, "If-Modified-Since");
    bool not_modified = false;
    if (inm) {
        not_modified = http_etag_matches(inm, etag);
    } else if (ims) {
        time_t t;
        not_modified = http_parse_date(ims, &t) == 0 && st.st_mtime <= t;
    }
    if (not_modified) {
        close(f);
        send_head(c, 304, NULL, -1, validators);
        return;
    }

    uint64_t first = 0, last = size ? size - 1 : 0;
    range_result rr = RANGE_NONE;
    const char *range = http_header_get(req, "Range");
    if (range) {
        /* If-Range: só vale o intervalo se o arquivo ainda for o mesmo. */
        const char *if_range = http_header_get(req, "If-Range");
        bool honor = true;
        if (if_range) {
            if (if_range[0] == '"') honor = strcmp(if_range, etag) == 0;
            else if (strncmp(if_range, "W/", 2) == 0) honor = false; /* ETag fraca não serve aqui */
            else honor = strcmp(if_range, lastmod) == 0;
        }
        if (honor) rr = http_parse_range(range, size, &first, &last);
    }

    char extra[512];
    if (rr == RANGE_UNSATISFIABLE) {
        close(f);
        snprintf(extra, sizeof extra, "Content-Range: bytes */%llu\r\n", (unsigned long long)size);
        send_error(c, 416, extra);
        return;
    }
    int status = 200;
    uint64_t length = size;
    if (rr == RANGE_OK) {
        status = 206;
        length = last - first + 1;
        snprintf(extra, sizeof extra,
                 "%sAccept-Ranges: bytes\r\nContent-Range: bytes %llu-%llu/%llu\r\nX-Content-Type-Options: nosniff\r\n",
                 validators, (unsigned long long)first, (unsigned long long)last, (unsigned long long)size);
    } else {
        snprintf(extra, sizeof extra, "%sAccept-Ranges: bytes\r\nX-Content-Type-Options: nosniff\r\n", validators);
    }
    send_head(c, status, mime_type(real), (long long)length, extra);
    if (!c->head && !c->broken) send_file_range(c, f, first, length);
    close(f);
}

/* ---------------------------------------------------------------------------
 * Listagem de pastas
 * ------------------------------------------------------------------------ */

typedef struct {
    char    *name;
    bool     dir;
    uint64_t size;
    time_t   mtime;
} entry;

static int cmp_entry(const void *a, const void *b)
{
    const entry *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1; /* pastas primeiro */
    return strcmp(x->name, y->name);
}

static void fmt_size(uint64_t n, char *out, size_t cap)
{
    static const char *const UNITS[] = { "KB", "MB", "GB", "TB" };
    if (n < 1024) {
        snprintf(out, cap, "%llu B", (unsigned long long)n);
        return;
    }
    double v = (double)n;
    int u = -1;
    while (v >= 1024.0 && u < 3) {
        v /= 1024.0;
        u++;
    }
    snprintf(out, cap, "%.1f %s", v, UNITS[u]);
    char *dot = strchr(out, '.');
    if (dot) *dot = ','; /* vírgula decimal, como se escreve no Brasil */
}

static void fmt_listing_date(time_t t, char *out, size_t cap)
{
    long long s = (long long)t, days = s / 86400, rem = s % 86400, y;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    snprintf(out, cap, "%02u/%02u/%04lld %02lld:%02lld UTC", d, m, y, rem / 3600, rem / 60 % 60);
}

static void serve_listing(conn *c, const char *real, const char *rel)
{
    DIR *d = opendir(real);
    if (!d) {
        send_error(c, 403, NULL);
        return;
    }
    entry *list = NULL;
    size_t n = 0, cap = 0;
    bool oom = false;
    struct dirent *de;
    int dfd = dirfd(d);
    while (!oom && (de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue; /* ocultos e as entradas . e .. */
        struct stat st;
        if (fstatat(dfd, de->d_name, &st, 0) != 0) continue;
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 64;
            entry *grown = realloc(list, nc * sizeof *grown);
            if (!grown) {
                oom = true;
                break;
            }
            list = grown;
            cap = nc;
        }
        list[n].name = strdup(de->d_name);
        if (!list[n].name) {
            oom = true;
            break;
        }
        list[n].dir = S_ISDIR(st.st_mode);
        list[n].size = (uint64_t)st.st_size;
        list[n].mtime = st.st_mtime;
        n++;
    }
    closedir(d);

    strbuf b;
    sb_init(&b);
    if (!oom) {
        qsort(list, n, sizeof *list, cmp_entry);
        sb_puts(&b, "<!doctype html>\n<html lang=\"pt-BR\">\n<head>\n<meta charset=\"utf-8\">\n"
                    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>Índice de /");
        sb_html(&b, rel);
        sb_puts(&b, rel[0] ? "/" : "");
        sb_puts(&b, "</title>\n<style>body{font-family:system-ui,sans-serif;margin:2rem;color:#1b1f24}"
                    "table{border-collapse:collapse}td,th{padding:.25rem 1rem;text-align:left}"
                    "td:nth-child(2){text-align:right}a{color:#2563eb}</style>\n</head>\n<body>\n<h1>Índice de /");
        sb_html(&b, rel);
        sb_puts(&b, rel[0] ? "/" : "");
        sb_puts(&b, "</h1>\n<table>\n<tr><th>Nome</th><th>Tamanho</th><th>Modificado</th></tr>\n");
        if (rel[0]) sb_puts(&b, "<tr><td><a href=\"../\">../</a></td><td></td><td></td></tr>\n");
        for (size_t i = 0; i < n; i++) {
            char size[32], date[40];
            fmt_listing_date(list[i].mtime, date, sizeof date);
            if (list[i].dir) snprintf(size, sizeof size, "-");
            else fmt_size(list[i].size, size, sizeof size);
            /* O nome entra duas vezes: codificado em %XX no href e escapado em HTML no texto. */
            sb_puts(&b, "<tr><td><a href=\"");
            url_encode_append(&b, list[i].name);
            sb_puts(&b, list[i].dir ? "/\">" : "\">");
            sb_html(&b, list[i].name);
            sb_puts(&b, list[i].dir ? "/" : "");
            sb_printf(&b, "</a></td><td>%s</td><td>%s</td></tr>\n", size, date);
        }
        sb_puts(&b, "</table>\n</body>\n</html>\n");
    }
    for (size_t i = 0; i < n; i++) free(list[i].name);
    free(list);

    if (oom || b.failed) {
        send_error(c, 500, NULL);
    } else {
        send_head(c, 200, "text/html; charset=utf-8", (long long)b.len, "Cache-Control: no-cache\r\n");
        if (!c->head && send_all(c, b.data, b.len)) c->sent += b.len;
    }
    sb_free(&b);
}

/* ---------------------------------------------------------------------------
 * Roteamento
 * ------------------------------------------------------------------------ */

/* Algum trecho do caminho real (depois da raiz) começa com ponto? */
static bool real_is_hidden(const char *root, const char *real)
{
    const char *p = real + strlen(root);
    while (*p) {
        if (p[0] == '/' && p[1] == '.') return true;
        p++;
    }
    return false;
}

static void handle_request(conn *c, const http_request *req)
{
    const char *root = c->cfg->root;
    bool get = strcmp(req->method, "GET") == 0;
    c->head = strcmp(req->method, "HEAD") == 0;
    if (!get && !c->head) {
        send_error(c, 405, "Allow: GET, HEAD\r\n");
        return;
    }

    const char *path;
    long plen = http_target_path(req->target, &path);
    if (plen < 0) {
        send_error(c, 400, NULL);
        return;
    }
    char decoded[HTTP_MAX_TARGET + 1];
    if (url_decode(path, (size_t)plen, decoded) < 0) {
        send_error(c, 400, NULL);
        return;
    }
    char rel[PATH_MAX];
    bool trailing;
    switch (path_normalize(decoded, rel, sizeof rel, &trailing)) {
    case PATH_TRAVERSAL:
        send_error(c, 403, NULL);
        return;
    case PATH_HIDDEN:
        send_error(c, 404, NULL);
        return;
    case PATH_TOO_LONG:
        send_error(c, 414, NULL);
        return;
    case PATH_OK:
        break;
    }

    char full[PATH_MAX * 2], real[PATH_MAX];
    snprintf(full, sizeof full, "%s/%s", root, rel);
    if (!realpath(full, real)) {
        int status = (errno == EACCES || errno == ELOOP) ? 403 : errno == ENAMETOOLONG ? 414 : 404;
        send_error(c, status, NULL);
        return;
    }
    /* realpath resolveu todos os links simbólicos: se o resultado saiu da
     * raiz, alguém criou um link para fora. */
    if (!path_inside(root, real)) {
        send_error(c, 403, NULL);
        return;
    }
    if (real_is_hidden(root, real)) {
        send_error(c, 404, NULL);
        return;
    }
    struct stat st;
    if (stat(real, &st) != 0) {
        send_error(c, 404, NULL);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        if (!trailing) {
            /* /pasta vira /pasta/: assim os links relativos da página funcionam. */
            strbuf loc;
            sb_init(&loc);
            sb_puts(&loc, "Location: ");
            sb_append(&loc, path, (size_t)plen);
            sb_puts(&loc, "/\r\n");
            if (loc.failed) c->broken = true;
            else send_error(c, 301, loc.data);
            sb_free(&loc);
            return;
        }
        char index[PATH_MAX + 16], index_real[PATH_MAX];
        struct stat ist;
        snprintf(index, sizeof index, "%s/index.html", real);
        if (realpath(index, index_real) && path_inside(root, index_real) && stat(index_real, &ist) == 0 &&
            S_ISREG(ist.st_mode)) {
            serve_file(c, req, index_real);
        } else {
            serve_listing(c, real, rel);
        }
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        send_error(c, 403, NULL); /* soquete, FIFO, dispositivo */
        return;
    }
    serve_file(c, req, real);
}

/* Fechamento gradual. Depois de uma resposta com "Connection: close", o
 * cliente ainda pode estar mandando dados (um corpo que não foi lido, o
 * resto de um cabeçalho grande demais). Fechar um socket com dados não lidos
 * faz o sistema mandar um RST, que pode apagar a resposta antes de o cliente
 * lê-la. Então: avisa que não vai escrever mais (shutdown) e descarta o que
 * ainda chegar por até 1 s, como fazem o Apache e o nginx. */
static void linger_close(int fd)
{
    shutdown(fd, SHUT_WR);
    char junk[4096];
    long long end = now_ms() + 1000;
    size_t total = 0;
    while (total < (1u << 20)) {
        long long left = end - now_ms();
        if (left <= 0) break;
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, (int)left) <= 0) break;
        ssize_t n = read(fd, junk, sizeof junk);
        if (n <= 0) break;
        total += (size_t)n;
    }
}

void server_handle(server_config *cfg, int fd, const struct sockaddr_storage *peer)
{
    conn c;
    memset(&c, 0, sizeof c);
    c.cfg = cfg;
    c.fd = fd;
    format_peer(peer, c.client, sizeof c.client);

    /* Cliente que para de ler a resposta não pode prender a thread para sempre. */
    struct timeval tv = { SEND_TIMEOUT_S, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    char *buf = malloc(HTTP_MAX_HEADER_BYTES + 1);
    if (!buf) return;
    size_t have = 0;
    int served = 0;

    while (served < MAX_REQUESTS) {
        http_request req;
        http_parse_result pr = HTTP_PARSE_INCOMPLETE;
        long long start = now_ms();
        for (;;) {
            if (have > 0) {
                pr = http_parse_request(buf, have, &req);
                if (pr != HTTP_PARSE_INCOMPLETE) break;
            }
            /* Ociosa entre requisições: 5 s. Requisição começada: 10 s para terminar. */
            long long limit = have == 0 ? KEEPALIVE_TIMEOUT_MS : HEADER_TIMEOUT_MS;
            long long left = limit - (now_ms() - start);
            if (left <= 0) {
                if (have > 0) {
                    c.keep_alive = false;
                    c.head = false;
                    c.sent = 0;
                    send_error(&c, 408, NULL);
                    log_request(&c, NULL);
                    if (!c.broken) linger_close(fd);
                }
                goto out;
            }
            if (have == 0 && atomic_load(cfg->stop)) goto out; /* encerrando: larga a conexão ociosa */
            struct pollfd p = { fd, POLLIN, 0 };
            int r = poll(&p, 1, left < 250 ? (int)left : 250);
            if (r < 0 && errno != EINTR) goto out;
            if (r <= 0) continue;
            ssize_t n = read(fd, buf + have, HTTP_MAX_HEADER_BYTES - have);
            if (n == 0) goto out; /* o cliente fechou */
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                goto out;
            }
            have += (size_t)n;
        }

        c.sent = 0;
        c.status = 0;
        c.head = false;
        if (pr != HTTP_PARSE_OK) {
            int status = pr == HTTP_PARSE_TOO_LARGE      ? 431
                         : pr == HTTP_PARSE_URI_TOO_LONG ? 414
                         : pr == HTTP_PARSE_VERSION      ? 505
                                                         : 400;
            c.keep_alive = false;
            send_error(&c, status, NULL);
            log_request(&c, NULL);
            if (!c.broken) linger_close(fd);
            goto out;
        }
        served++;
        /* Com corpo (que este servidor não lê), a conexão fecha depois da
         * resposta: o corpo não lido seria confundido com a próxima requisição. */
        c.keep_alive = http_keep_alive(&req) && !http_has_body(&req) && served < MAX_REQUESTS &&
                       !atomic_load(cfg->stop);
        handle_request(&c, &req);
        log_request(&c, &req);
        if (c.broken) break;
        if (!c.keep_alive) {
            linger_close(fd);
            break;
        }
        /* O que sobrou no buffer é o começo da próxima requisição (pipelining). */
        memmove(buf, buf + req.length, have - req.length);
        have -= req.length;
    }
out:
    free(buf);
}
