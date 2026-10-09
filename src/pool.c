/*
 * pool.c: produtor (a thread que aceita conexões) e consumidores (as threads
 * de trabalho), sincronizados por um mutex e uma variável de condição.
 */
#define _POSIX_C_SOURCE 200809L

#include "pool.h"

#include <stdlib.h>
#include <unistd.h>

static void *worker(void *arg)
{
    pool *p = arg;
    for (;;) {
        pthread_mutex_lock(&p->lock);
        /* while, e não if: a espera pode acordar sem motivo (spurious wakeup). */
        while (p->count == 0 && !p->closing) pthread_cond_wait(&p->not_empty, &p->lock);
        if (p->count == 0) { /* fechando e sem trabalho: fim */
            pthread_mutex_unlock(&p->lock);
            return NULL;
        }
        pool_job job = p->jobs[p->head];
        p->head = (p->head + 1) % p->cap;
        p->count--;
        pthread_mutex_unlock(&p->lock);

        server_handle(p->cfg, job.fd, &job.peer);
        close(job.fd);
    }
}

int pool_start(pool *p, int nthreads, int cap, server_config *cfg)
{
    p->jobs = calloc((size_t)cap, sizeof *p->jobs);
    p->threads = calloc((size_t)nthreads, sizeof *p->threads);
    if (!p->jobs || !p->threads) {
        free(p->jobs);
        free(p->threads);
        return -1;
    }
    p->cap = cap;
    p->head = p->count = 0;
    p->closing = false;
    p->cfg = cfg;
    p->nthreads = 0;
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->not_empty, NULL);
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&p->threads[i], NULL, worker, p) != 0) {
            pool_stop(p);
            return -1;
        }
        p->nthreads++;
    }
    return 0;
}

bool pool_submit(pool *p, int fd, const struct sockaddr_storage *peer)
{
    pthread_mutex_lock(&p->lock);
    if (p->count == p->cap || p->closing) {
        pthread_mutex_unlock(&p->lock);
        return false;
    }
    pool_job *slot = &p->jobs[(p->head + p->count) % p->cap];
    slot->fd = fd;
    slot->peer = *peer;
    p->count++;
    pthread_cond_signal(&p->not_empty);
    pthread_mutex_unlock(&p->lock);
    return true;
}

void pool_stop(pool *p)
{
    pthread_mutex_lock(&p->lock);
    p->closing = true;
    pthread_cond_broadcast(&p->not_empty);
    pthread_mutex_unlock(&p->lock);
    /* As threads ainda esvaziam a fila: conexões já aceitas são atendidas
     * (ou fechadas logo, se não mandarem nada, porque a flag de parada está ligada). */
    for (int i = 0; i < p->nthreads; i++) pthread_join(p->threads[i], NULL);
    pthread_mutex_destroy(&p->lock);
    pthread_cond_destroy(&p->not_empty);
    free(p->jobs);
    free(p->threads);
    p->jobs = NULL;
    p->threads = NULL;
}
