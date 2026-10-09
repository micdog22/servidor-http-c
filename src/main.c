/*
 * main.c: opções, socket de escuta, sinais e o laço que aceita conexões.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "pool.h"
#include "server.h"

#define VERSION "1.0.0"

static atomic_int stop_flag;
static int wake_pipe[2] = { -1, -1 };

/* Só funções seguras em sinais aqui: guardar a flag e escrever um byte no
 * pipe, o que acorda o poll() da thread principal (o truque do self-pipe). */
static void on_signal(int sig)
{
    (void)sig;
    int saved = errno;
    atomic_store(&stop_flag, 1);
    if (wake_pipe[1] >= 0) {
        ssize_t r = write(wake_pipe[1], "x", 1);
        (void)r;
    }
    errno = saved;
}

static void usage(FILE *f)
{
    fputs("uso: servidor-http [opções]\n"
          "\n"
          "Servidor HTTP/1.1 de arquivos estáticos.\n"
          "\n"
          "Opções:\n"
          "  -p, --porta N        porta TCP (padrão: 8080; 0 escolhe uma porta livre)\n"
          "  -d, --pasta DIR      pasta a servir (padrão: a pasta atual)\n"
          "  -b, --endereco IP    endereço de escuta, como 127.0.0.1 ou ::1\n"
          "                       (padrão: todos, IPv6 e IPv4 no mesmo socket)\n"
          "  -t, --threads N      threads de trabalho (padrão: 8)\n"
          "      --log ARQ        grava o log de acesso no arquivo (padrão: saída padrão)\n"
          "      --silencioso     não grava log de acesso\n"
          "  -h, --help           mostra esta ajuda\n"
          "  -v, --versao         mostra a versão\n",
          f);
}

typedef struct {
    int         port;
    const char *dir;
    const char *bind;
    int         threads;
    const char *log_path;
    bool        quiet;
} options;

static const char *opt_value(int argc, char **argv, int *i, const char *name, const char *alias)
{
    const char *a = argv[*i];
    size_t n = strlen(name);
    if (strncmp(a, name, n) == 0 && a[n] == '=') return a + n + 1;
    if (strcmp(a, name) != 0 && (!alias || strcmp(a, alias) != 0)) return NULL;
    if (*i + 1 >= argc) {
        fprintf(stderr, "servidor-http: a opção %s precisa de um valor\n", name);
        exit(2);
    }
    return argv[++*i];
}

static int parse_int(const char *s, int lo, int hi, const char *what)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (!*s || *end || v < lo || v > hi) {
        fprintf(stderr, "servidor-http: %s deve ser um número de %d a %d\n", what, lo, hi);
        exit(2);
    }
    return (int)v;
}

static void parse_args(int argc, char **argv, options *o)
{
    o->port = 8080;
    o->dir = ".";
    o->bind = NULL;
    o->threads = 8;
    o->log_path = NULL;
    o->quiet = false;
    for (int i = 1; i < argc; i++) {
        const char *v;
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            exit(0);
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--versao") == 0) {
            printf("servidor-http %s\n", VERSION);
            exit(0);
        } else if ((v = opt_value(argc, argv, &i, "--porta", "-p"))) {
            o->port = parse_int(v, 0, 65535, "a porta");
        } else if ((v = opt_value(argc, argv, &i, "--pasta", "-d"))) {
            o->dir = v;
        } else if ((v = opt_value(argc, argv, &i, "--endereco", "-b"))) {
            o->bind = v;
        } else if ((v = opt_value(argc, argv, &i, "--threads", "-t"))) {
            o->threads = parse_int(v, 1, 256, "o número de threads");
        } else if ((v = opt_value(argc, argv, &i, "--log", NULL))) {
            o->log_path = v;
        } else if (strcmp(argv[i], "--silencioso") == 0) {
            o->quiet = true;
        } else {
            fprintf(stderr, "servidor-http: opção desconhecida \"%s\"\n\n", argv[i]);
            usage(stderr);
            exit(2);
        }
    }
}

static void set_flags(int fd, bool nonblocking)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) fcntl(fd, F_SETFL, nonblocking ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

/* Cria o socket de escuta. Sem endereço, tenta a pilha dupla (IPv6 que também
 * aceita IPv4) e cai para IPv4 se o sistema não tiver IPv6. */
static int open_listener(const options *o, char *url, size_t urlcap, const char **families)
{
    struct sockaddr_in6 a6;
    struct sockaddr_in a4;
    memset(&a6, 0, sizeof a6);
    memset(&a4, 0, sizeof a4);
    a6.sin6_family = AF_INET6;
    a6.sin6_port = htons((uint16_t)o->port);
    a4.sin_family = AF_INET;
    a4.sin_port = htons((uint16_t)o->port);

    int family;
    bool dual = false;
    if (!o->bind || strcmp(o->bind, "::") == 0) {
        family = AF_INET6;
        a6.sin6_addr = in6addr_any;
        dual = true;
    } else if (inet_pton(AF_INET6, o->bind, &a6.sin6_addr) == 1) {
        family = AF_INET6;
    } else if (inet_pton(AF_INET, o->bind, &a4.sin_addr) == 1) {
        family = AF_INET;
    } else {
        fprintf(stderr, "servidor-http: endereço inválido \"%s\"\n", o->bind);
        return -1;
    }

    int fd = socket(family, SOCK_STREAM, 0);
    if (fd < 0 && dual && !o->bind) {
        /* Sem IPv6 no sistema: escuta só em IPv4. */
        family = AF_INET;
        a4.sin_addr.s_addr = htonl(INADDR_ANY);
        dual = false;
        fd = socket(family, SOCK_STREAM, 0);
    }
    if (fd < 0) {
        perror("servidor-http: socket");
        return -1;
    }
    int one = 1, zero = 0;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); /* religar logo depois de reiniciar */
    if (family == AF_INET6) {
        if (dual && setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero) != 0) dual = false;
        if (!dual) setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);
    }
    int rc = family == AF_INET6 ? bind(fd, (struct sockaddr *)&a6, sizeof a6)
                                : bind(fd, (struct sockaddr *)&a4, sizeof a4);
    if (rc != 0 || listen(fd, 128) != 0) {
        fprintf(stderr, "servidor-http: não foi possível escutar na porta %d: %s\n", o->port,
                errno == EADDRINUSE ? "a porta já está em uso" : errno == EACCES ? "sem permissão" : "erro de rede");
        close(fd);
        return -1;
    }
    set_flags(fd, true);

    struct sockaddr_storage bound;
    socklen_t blen = sizeof bound;
    int port = o->port;
    if (getsockname(fd, (struct sockaddr *)&bound, &blen) == 0) {
        if (bound.ss_family == AF_INET6) {
            struct sockaddr_in6 b;
            memcpy(&b, &bound, sizeof b);
            port = ntohs(b.sin6_port);
        } else {
            struct sockaddr_in b;
            memcpy(&b, &bound, sizeof b);
            port = ntohs(b.sin_port);
        }
    }
    const char *host = !o->bind || strcmp(o->bind, "::") == 0 || strcmp(o->bind, "0.0.0.0") == 0 ? "localhost" : o->bind;
    if (strchr(host, ':')) snprintf(url, urlcap, "http://[%s]:%d/", host, port);
    else snprintf(url, urlcap, "http://%s:%d/", host, port);
    *families = dual ? "IPv6 e IPv4" : family == AF_INET6 ? "IPv6" : "IPv4";
    return fd;
}

int main(int argc, char **argv)
{
    options o;
    parse_args(argc, argv, &o);

    static server_config cfg;
    struct stat st;
    if (!realpath(o.dir, cfg.root) || stat(cfg.root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "servidor-http: \"%s\" não é uma pasta\n", o.dir);
        return 1;
    }
    cfg.stop = &stop_flag;
    cfg.log = o.quiet ? NULL : stdout;
    if (o.log_path && !o.quiet) {
        cfg.log = fopen(o.log_path, "a");
        if (!cfg.log) {
            perror(o.log_path);
            return 1;
        }
    }
    pthread_mutex_init(&cfg.log_lock, NULL);

    /* Escrever num socket que o cliente fechou gera SIGPIPE, que mataria o
     * processo. Ignorando o sinal, write() devolve o erro EPIPE. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
    if (pipe(wake_pipe) != 0) {
        perror("servidor-http: pipe");
        return 1;
    }
    set_flags(wake_pipe[0], true);
    set_flags(wake_pipe[1], true);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    char url[128];
    const char *families;
    int lfd = open_listener(&o, url, sizeof url, &families);
    if (lfd < 0) return 1;

    /* As threads de trabalho nascem com SIGINT e SIGTERM bloqueados: o sinal
     * sempre cai na thread principal, que é quem precisa acordar. */
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    pool workers;
    int queue = o.threads * 16 > 64 ? o.threads * 16 : 64;
    int rc = pool_start(&workers, o.threads, queue, &cfg);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (rc != 0) {
        fprintf(stderr, "servidor-http: não foi possível criar as threads\n");
        return 1;
    }

    fprintf(stderr, "Servindo %s em %s (%s, %d threads). Ctrl+C para encerrar.\n", cfg.root, url, families,
            o.threads);

    while (!atomic_load(&stop_flag)) {
        struct pollfd pfd[2] = { { lfd, POLLIN, 0 }, { wake_pipe[0], POLLIN, 0 } };
        int r = poll(pfd, 2, -1);
        if (atomic_load(&stop_flag)) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("servidor-http: poll");
            break;
        }
        if (!(pfd[0].revents & POLLIN)) continue;
        for (;;) { /* aceita tudo o que estiver esperando */
            struct sockaddr_storage peer;
            socklen_t plen = sizeof peer;
            int c = accept(lfd, (struct sockaddr *)&peer, &plen);
            if (c < 0) {
                if (errno == EINTR) continue;
                if (errno == EMFILE || errno == ENFILE) {
                    struct timespec pause = { 0, 100000000 }; /* sem descritores livres: respira */
                    nanosleep(&pause, NULL);
                }
                break;
            }
            /* No BSD e no macOS o socket aceito herda O_NONBLOCK; aqui ele é bloqueante. */
            set_flags(c, false);
            if (!pool_submit(&workers, c, &peer)) {
                static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
                                           "Retry-After: 1\r\nConnection: close\r\n\r\n";
                ssize_t w = write(c, busy, sizeof busy - 1);
                (void)w;
                close(c);
            }
        }
    }

    fprintf(stderr, "\nEncerrando: terminando as conexões em andamento...\n");
    close(lfd);
    pool_stop(&workers);
    if (cfg.log && cfg.log != stdout) fclose(cfg.log);
    pthread_mutex_destroy(&cfg.log_lock);
    close(wake_pipe[0]);
    close(wake_pipe[1]);
    fprintf(stderr, "Encerrado.\n");
    return 0;
}
