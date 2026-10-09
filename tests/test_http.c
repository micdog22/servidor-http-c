/*
 * test_http.c: analisador de requisições, decodificação de URL,
 * normalização de caminhos, Range, MIME, datas, ETag e escapes.
 */
#include <stdlib.h>
#include <time.h>

#include "http.h"
#include "mime.h"
#include "path.h"
#include "strbuf.h"
#include "teste.h"

static char buf[HTTP_MAX_HEADER_BYTES * 2];
static http_request req;

static http_parse_result parse(const char *text)
{
    size_t n = strlen(text);
    memcpy(buf, text, n);
    return http_parse_request(buf, n, &req);
}

static void test_parser_ok(void)
{
    CHECK_EQ(parse("GET /a/b?x=1 HTTP/1.1\r\nHost: exemplo.com\r\nUser-Agent:   curl/8   \r\nAccept: */*\r\n\r\n"),
             HTTP_PARSE_OK);
    CHECK_STR(req.method, "GET");
    CHECK_STR(req.target, "/a/b?x=1");
    CHECK_EQ(req.minor, 1);
    CHECK_EQ(req.nheaders, 3);
    CHECK_STR(http_header_get(&req, "host"), "exemplo.com");        /* nome sem diferenciar maiúsculas */
    CHECK_STR(http_header_get(&req, "USER-AGENT"), "curl/8");      /* espaços em volta somem */
    CHECK(http_header_get(&req, "Cookie") == NULL);
    CHECK(http_keep_alive(&req));
    CHECK(!http_has_body(&req));

    /* Só LF, linhas em branco antes, HTTP/1.0 sem Host. */
    CHECK_EQ(parse("\r\n\nHEAD / HTTP/1.0\nX-A: 1\n\n"), HTTP_PARSE_OK);
    CHECK_STR(req.method, "HEAD");
    CHECK_EQ(req.minor, 0);
    CHECK(!http_keep_alive(&req));                                 /* 1.0: fecha por padrão */

    /* Incompleta: o buffer não pode ser alterado. */
    const char *part = "GET / HTTP/1.1\r\nHost: x\r\n";
    memcpy(buf, part, strlen(part));
    char copy[64];
    memcpy(copy, buf, strlen(part));
    CHECK_EQ(http_parse_request(buf, strlen(part), &req), HTTP_PARSE_INCOMPLETE);
    CHECK(memcmp(copy, buf, strlen(part)) == 0);
    CHECK_EQ(parse(""), HTTP_PARSE_INCOMPLETE);

    /* Duas requisições juntas (pipelining): length aponta o fim da primeira. */
    const char *two = "GET /1 HTTP/1.1\r\nHost: x\r\n\r\nGET /2 HTTP/1.1\r\nHost: x\r\n\r\n";
    size_t n = strlen(two);
    memcpy(buf, two, n);
    CHECK_EQ(http_parse_request(buf, n, &req), HTTP_PARSE_OK);
    CHECK_EQ(req.length, n / 2);
    CHECK_STR(req.target, "/1");
    CHECK_EQ(http_parse_request(buf + req.length, n - n / 2, &req), HTTP_PARSE_OK);
    CHECK_STR(req.target, "/2");
}

static void test_parser_errors(void)
{
    CHECK_EQ(parse("GET / HTTP/1.1\r\n\r\n"), HTTP_PARSE_BAD);                  /* 1.1 sem Host */
    CHECK_EQ(parse("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n"), HTTP_PARSE_BAD); /* dois Host */
    CHECK_EQ(parse("GET / HTTP/1.1\r\nHost: a\r\n dobrado\r\n\r\n"), HTTP_PARSE_BAD); /* continuação obsoleta */
    CHECK_EQ(parse("GET / HTTP/1.1\r\nHost : a\r\n\r\n"), HTTP_PARSE_BAD);       /* espaço antes do ':' */
    CHECK_EQ(parse("GET / HTTP/1.1\r\nHost a\r\n\r\n"), HTTP_PARSE_BAD);         /* sem ':' */
    CHECK_EQ(parse("GET / HTTP/1.1\r\nHost: a\x01\r\n\r\n"), HTTP_PARSE_BAD);    /* controle no valor */
    CHECK_EQ(parse("G(T / HTTP/1.1\r\nHost: a\r\n\r\n"), HTTP_PARSE_BAD);        /* método com caractere inválido */
    CHECK_EQ(parse("ABCDEFGHIJKLMNOPQ / HTTP/1.1\r\nHost: a\r\n\r\n"), HTTP_PARSE_BAD);
    CHECK_EQ(parse("GET  / HTTP/1.1\r\nHost: a\r\n\r\n"), HTTP_PARSE_BAD);       /* dois espaços */
    CHECK_EQ(parse("GET /\r\nHost: a\r\n\r\n"), HTTP_PARSE_BAD);                 /* sem versão */
    CHECK_EQ(parse("GET / http/1.1\r\nHost: a\r\n\r\n"), HTTP_PARSE_BAD);
    CHECK_EQ(parse("GET /\xC3\xA9 HTTP/1.1\r\nHost: a\r\n\r\n"), HTTP_PARSE_BAD); /* não ASCII sem %XX */
    CHECK_EQ(parse("GET / HTTP/2.0\r\nHost: a\r\n\r\n"), HTTP_PARSE_VERSION);
    CHECK_EQ(parse("GET / HTTP/1.2\r\nHost: a\r\n\r\n"), HTTP_PARSE_VERSION);
    CHECK_EQ(parse("GET / HTTP/0.9\r\nHost: a\r\n\r\n"), HTTP_PARSE_VERSION);

    /* Alvo de 8193 bytes: 414. */
    static char big[HTTP_MAX_HEADER_BYTES * 2];
    strcpy(big, "GET /");
    memset(big + 5, 'a', HTTP_MAX_TARGET);
    strcpy(big + 5 + HTTP_MAX_TARGET, " HTTP/1.1\r\nHost: a\r\n\r\n");
    CHECK_EQ(parse(big), HTTP_PARSE_URI_TOO_LONG);
    /* Primeira linha gigante que nem terminou: 414 sem esperar o resto. */
    memset(big, 'a', HTTP_MAX_TARGET + 100);
    memcpy(big, "GET /", 5);
    big[HTTP_MAX_TARGET + 100] = '\0';
    CHECK_EQ(parse(big), HTTP_PARSE_URI_TOO_LONG);

    /* 101 cabeçalhos: 431. */
    strbuf b;
    sb_init(&b);
    sb_puts(&b, "GET / HTTP/1.1\r\nHost: a\r\n");
    for (int i = 0; i < HTTP_MAX_HEADERS; i++) sb_printf(&b, "X-%d: %d\r\n", i, i);
    sb_puts(&b, "\r\n");
    CHECK_EQ(parse(b.data), HTTP_PARSE_TOO_LARGE);
    sb_free(&b);

    /* Mais de 16 KB sem a linha em branco: 431. */
    sb_init(&b);
    sb_puts(&b, "GET / HTTP/1.1\r\nHost: a\r\n");
    while (b.len < HTTP_MAX_HEADER_BYTES) sb_puts(&b, "X-Grande: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n");
    CHECK_EQ(parse(b.data), HTTP_PARSE_TOO_LARGE);
    sb_free(&b);
}

static void test_connection(void)
{
    CHECK_EQ(parse("GET / HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n"), HTTP_PARSE_OK);
    CHECK(!http_keep_alive(&req));
    CHECK_EQ(parse("GET / HTTP/1.1\r\nHost: a\r\nConnection: Upgrade, CLOSE\r\n\r\n"), HTTP_PARSE_OK);
    CHECK(!http_keep_alive(&req));
    CHECK_EQ(parse("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n"), HTTP_PARSE_OK);
    CHECK(http_keep_alive(&req));
    CHECK_EQ(parse("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\n"), HTTP_PARSE_OK);
    CHECK(http_has_body(&req));
    CHECK_EQ(parse("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\n\r\n"), HTTP_PARSE_OK);
    CHECK(!http_has_body(&req));
    CHECK_EQ(parse("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n"), HTTP_PARSE_OK);
    CHECK(http_has_body(&req));
}

static void test_target_and_decoding(void)
{
    const char *p;
    CHECK_EQ(http_target_path("/a/b?x=1", &p), 4);
    CHECK(strncmp(p, "/a/b", 4) == 0);
    CHECK_EQ(http_target_path("/a#frag", &p), 2);
    CHECK_EQ(http_target_path("http://exemplo.com:8080/x/y?z", &p), 4);
    CHECK(strncmp(p, "/x/y", 4) == 0);
    CHECK_EQ(http_target_path("HTTP://exemplo.com", &p), 1);
    CHECK_STR(p, "/");
    CHECK_EQ(http_target_path("*", &p), -1);
    CHECK_EQ(http_target_path("exemplo.com:443", &p), -1);

    char out[64];
    CHECK_EQ(url_decode("/a%20b", 6, out), 4);
    CHECK_STR(out, "/a b");
    CHECK_EQ(url_decode("%2e%2E%2f", 9, out), 3);
    CHECK_STR(out, "../");                       /* o ataque disfarçado aparece depois de decodificar */
    CHECK_EQ(url_decode("a+b", 3, out), 3);
    CHECK_STR(out, "a+b");                       /* + só vira espaço em formulários, não em caminhos */
    CHECK_EQ(url_decode("caf%C3%A9", 9, out), 5);
    CHECK_STR(out, "café");
    CHECK_EQ(url_decode("%zz", 3, out), -1);
    CHECK_EQ(url_decode("%2", 2, out), -1);
    CHECK_EQ(url_decode("%", 1, out), -1);
    CHECK_EQ(url_decode("a%00b", 5, out), -1);   /* byte nulo cortaria o caminho */
}

static void test_normalize(void)
{
    char out[256];
    bool slash;
    CHECK_EQ(path_normalize("/", out, sizeof out, &slash), PATH_OK);
    CHECK_STR(out, "");
    CHECK(slash);
    CHECK_EQ(path_normalize("/a/./b//c/", out, sizeof out, &slash), PATH_OK);
    CHECK_STR(out, "a/b/c");
    CHECK(slash);
    CHECK_EQ(path_normalize("/a/b.", out, sizeof out, &slash), PATH_OK);
    CHECK_STR(out, "a/b.");
    CHECK(!slash);
    CHECK_EQ(path_normalize("/../etc/passwd", out, sizeof out, &slash), PATH_TRAVERSAL);
    CHECK_EQ(path_normalize("/a/../b", out, sizeof out, &slash), PATH_TRAVERSAL);
    CHECK_EQ(path_normalize("/..", out, sizeof out, &slash), PATH_TRAVERSAL);
    CHECK_EQ(path_normalize("/a/b/..", out, sizeof out, &slash), PATH_TRAVERSAL);
    CHECK_EQ(path_normalize("/.git/config", out, sizeof out, &slash), PATH_HIDDEN);
    CHECK_EQ(path_normalize("/a/.env", out, sizeof out, &slash), PATH_HIDDEN);
    CHECK_EQ(path_normalize("/a/..b", out, sizeof out, &slash), PATH_HIDDEN);
    CHECK_EQ(path_normalize("/abcdef", out, 4, &slash), PATH_TOO_LONG);

    CHECK(path_inside("/srv/site", "/srv/site"));
    CHECK(path_inside("/srv/site", "/srv/site/a/b"));
    CHECK(!path_inside("/srv/site", "/srv/site2/a"));  /* prefixo de texto não basta */
    CHECK(!path_inside("/srv/site", "/srv"));
    CHECK(!path_inside("/srv/site", "/etc/passwd"));
    CHECK(path_inside("/", "/etc"));
}

static void test_ranges(void)
{
    uint64_t a = 0, b = 0;
    CHECK_EQ(http_parse_range("bytes=0-99", 1000, &a, &b), RANGE_OK);
    CHECK(a == 0 && b == 99);
    CHECK_EQ(http_parse_range("bytes=500-", 1000, &a, &b), RANGE_OK);
    CHECK(a == 500 && b == 999);
    CHECK_EQ(http_parse_range("bytes=-100", 1000, &a, &b), RANGE_OK);  /* os últimos 100 */
    CHECK(a == 900 && b == 999);
    CHECK_EQ(http_parse_range("bytes=-5000", 1000, &a, &b), RANGE_OK);
    CHECK(a == 0 && b == 999);
    CHECK_EQ(http_parse_range("bytes=0-5000", 1000, &a, &b), RANGE_OK); /* fim além do arquivo: corta */
    CHECK(a == 0 && b == 999);
    CHECK_EQ(http_parse_range("Bytes=999-999", 1000, &a, &b), RANGE_OK);
    CHECK(a == 999 && b == 999);
    CHECK_EQ(http_parse_range("bytes=1000-", 1000, &a, &b), RANGE_UNSATISFIABLE);
    CHECK_EQ(http_parse_range("bytes=-0", 1000, &a, &b), RANGE_UNSATISFIABLE);
    CHECK_EQ(http_parse_range("bytes=5-2", 1000, &a, &b), RANGE_UNSATISFIABLE);
    CHECK_EQ(http_parse_range("bytes=abc", 1000, &a, &b), RANGE_UNSATISFIABLE);
    CHECK_EQ(http_parse_range("bytes=-", 1000, &a, &b), RANGE_UNSATISFIABLE);
    CHECK_EQ(http_parse_range("bytes=0-0", 0, &a, &b), RANGE_UNSATISFIABLE); /* arquivo vazio */
    CHECK_EQ(http_parse_range("bytes=99999999999999999999-", 1000, &a, &b), RANGE_UNSATISFIABLE);
    CHECK_EQ(http_parse_range("bytes=0-1,5-6", 1000, &a, &b), RANGE_NONE);   /* vários: arquivo inteiro */
    CHECK_EQ(http_parse_range("items=0-5", 1000, &a, &b), RANGE_NONE);      /* unidade desconhecida */
    CHECK_EQ(http_parse_range(NULL, 1000, &a, &b), RANGE_NONE);
}

static void test_mime_and_escape(void)
{
    CHECK_STR(mime_type("/site/index.html"), "text/html; charset=utf-8");
    CHECK_STR(mime_type("FOTO.PNG"), "image/png");
    CHECK_STR(mime_type("a/b/video.mp4"), "video/mp4");
    CHECK_STR(mime_type("pacote.tar.gz"), "application/gzip");
    CHECK_STR(mime_type("estilo.css"), "text/css; charset=utf-8");
    CHECK_STR(mime_type("app.mjs"), "text/javascript; charset=utf-8");
    CHECK_STR(mime_type("LEIAME"), "application/octet-stream");
    CHECK_STR(mime_type(".bashrc"), "application/octet-stream");
    CHECK_STR(mime_type("pasta.d/arquivo"), "application/octet-stream");

    strbuf b;
    sb_init(&b);
    sb_html(&b, "<a href=\"x\">'&'</a>");
    CHECK_STR(b.data, "&lt;a href=&quot;x&quot;&gt;&#39;&amp;&#39;&lt;/a&gt;");
    sb_free(&b);
    sb_init(&b);
    url_encode_append(&b, "a b#c?.txt");
    CHECK_STR(b.data, "a%20b%23c%3F.txt");
    sb_free(&b);
    sb_init(&b);
    url_encode_append(&b, "café-~_.x");
    CHECK_STR(b.data, "caf%C3%A9-~_.x");
    sb_free(&b);
}

static void test_dates(void)
{
    char out[32];
    http_format_date(0, out);
    CHECK_STR(out, "Thu, 01 Jan 1970 00:00:00 GMT");
    http_format_date(784111777, out);
    CHECK_STR(out, "Sun, 06 Nov 1994 08:49:37 GMT");   /* o exemplo da RFC 9110 */
    http_format_log_date(784111777, out);
    CHECK_STR(out, "06/Nov/1994:08:49:37 +0000");
    http_format_date(951782400, out);
    CHECK_STR(out, "Tue, 29 Feb 2000 00:00:00 GMT");   /* 2000 é bissexto (divisível por 400) */

    time_t t = 0;
    CHECK_EQ(http_parse_date("Sun, 06 Nov 1994 08:49:37 GMT", &t), 0);
    CHECK_EQ((long long)t, 784111777);
    t = 0;
    CHECK_EQ(http_parse_date("Sunday, 06-Nov-94 08:49:37 GMT", &t), 0);
    CHECK_EQ((long long)t, 784111777);
    t = 0;
    CHECK_EQ(http_parse_date("Sun Nov  6 08:49:37 1994", &t), 0);
    CHECK_EQ((long long)t, 784111777);
    CHECK_EQ(http_parse_date("Sun, 32 Nov 1994 08:49:37 GMT", &t), -1);
    CHECK_EQ(http_parse_date("Sun, 06 Foo 1994 08:49:37 GMT", &t), -1);
    CHECK_EQ(http_parse_date("Sun, 06 Nov 1994 08:49:37", &t), -1);
    CHECK_EQ(http_parse_date("Sun, 06 Nov 1994 08:49:37 GMT lixo", &t), -1);
    CHECK_EQ(http_parse_date("ontem", &t), -1);

    /* Conversão de calendário: ida e volta em 1,6 milhão de dias (de -2400 a 6300). */
    bool round = true;
    for (long long z = -800000; z <= 800000; z++) {
        long long y;
        unsigned m, d;
        civil_from_days(z, &y, &m, &d);
        if (days_from_civil(y, m, d) != z) round = false;
    }
    CHECK(round);
    CHECK_EQ(days_from_civil(2000, 3, 1), 11017);
    CHECK_EQ(days_from_civil(1969, 12, 31), -1);

    /* Formato e leitura contra a biblioteca padrão (gmtime + strftime). */
    bool same = true, parsed = true;
    for (long long s = 0; s < 4102444800LL; s += 86413 * 37 + 7) {   /* 1970 a 2100 */
        time_t tt = (time_t)s;
        char mine[32], ref[64];
        http_format_date(tt, mine);
        strftime(ref, sizeof ref, "%a, %d %b %Y %H:%M:%S GMT", gmtime(&tt));
        if (strcmp(mine, ref) != 0) same = false;
        time_t back;
        if (http_parse_date(mine, &back) != 0 || back != tt) parsed = false;
    }
    CHECK(same);
    CHECK(parsed);
}

static void test_etags_and_status(void)
{
    CHECK(http_etag_matches("\"a\", \"b\"", "\"b\""));
    CHECK(http_etag_matches("W/\"a\"", "\"a\""));       /* comparação fraca */
    CHECK(http_etag_matches("\"a\"", "W/\"a\""));
    CHECK(http_etag_matches("*", "\"qualquer\""));
    CHECK(http_etag_matches("lixo, \"a\"", "\"a\""));
    CHECK(!http_etag_matches("\"c\"", "\"a\""));
    CHECK(!http_etag_matches("\"ab\"", "\"a\""));
    CHECK(!http_etag_matches("", "\"a\""));

    CHECK_STR(http_reason(404), "Not Found");
    CHECK_STR(http_title_pt(404), "Não encontrado");
    CHECK_STR(http_reason(416), "Range Not Satisfiable");
    CHECK_STR(http_reason(999), "Unknown");
}

int main(void)
{
    test_parser_ok();
    test_parser_errors();
    test_connection();
    test_target_and_decoding();
    test_normalize();
    test_ranges();
    test_mime_and_escape();
    test_dates();
    test_etags_and_status();
    return teste_resumo("test_http");
}
