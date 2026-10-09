/*
 * pool.h: um conjunto fixo de threads que atende conexões de uma fila limitada.
 */
#ifndef POOL_H
#define POOL_H

#include <pthread.h>
#include <stdbool.h>
#include <sys/socket.h>

#include "server.h"

typedef struct {
    int                     fd;
    struct sockaddr_storage peer;
} pool_job;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pool_job       *jobs;       /* fila circular */
    int             cap, head, count;
    bool            closing;
    pthread_t      *threads;
    int             nthreads;
    server_config  *cfg;
} pool;

/* Cria nthreads threads e uma fila para cap conexões. 0 ou -1. */
int  pool_start(pool *p, int nthreads, int cap, server_config *cfg);

/* Põe uma conexão na fila. false se a fila estiver cheia (o chamador responde 503). */
bool pool_submit(pool *p, int fd, const struct sockaddr_storage *peer);

/* Fecha a fila, espera as threads terminarem o que estão fazendo e libera tudo. */
void pool_stop(pool *p);

#endif
