/*
 * server.h: atende uma conexão HTTP do começo ao fim.
 */
#ifndef SERVER_H
#define SERVER_H

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/socket.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define KEEPALIVE_TIMEOUT_MS 5000   /* espera pela próxima requisição */
#define HEADER_TIMEOUT_MS    10000  /* para o cabeçalho chegar inteiro */
#define SEND_TIMEOUT_S       10     /* cliente que não lê a resposta */
#define MAX_REQUESTS         100    /* requisições por conexão */

typedef struct {
    char            root[PATH_MAX];  /* caminho canônico da pasta servida */
    FILE           *log;             /* NULL: sem log de acesso */
    pthread_mutex_t log_lock;
    atomic_int     *stop;            /* 1 quando o servidor está encerrando */
} server_config;

/* Atende requisições na conexão fd até ela fechar, expirar ou o servidor
 * parar. Não fecha o fd. */
void server_handle(server_config *cfg, int fd, const struct sockaddr_storage *peer);

/* Escreve o endereço do cliente (IPv4 mapeado em IPv6 aparece como IPv4). */
void format_peer(const struct sockaddr_storage *peer, char *out, size_t cap);

#endif
