#include "my_malloc.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAIRS 4
#define ITEMS 20000
#define QCAP  256

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t not_empty, not_full;
    unsigned char *buf[QCAP];
    size_t size[QCAP], head, tail, count;
} Queue;

static Queue queues[PAIRS];

static void *producer(void *arg) {
    Queue *q = arg;
    unsigned s = 12345;
    for (int i = 0; i < ITEMS; i++) {
        s = s * 1103515245u + 12345u;
        size_t size = (s >> 8) % 8192 + 1;
        unsigned char *p = malloc(size);
        if (!p) abort();
        memset(p, (unsigned char)size, size);
        pthread_mutex_lock(&q->m);
        while (q->count == QCAP) pthread_cond_wait(&q->not_full, &q->m);
        q->buf[q->tail] = p; q->size[q->tail] = size;
        q->tail = (q->tail + 1) % QCAP; q->count++;
        pthread_cond_signal(&q->not_empty);
        pthread_mutex_unlock(&q->m);
    }
    return NULL;
}

static void *consumer(void *arg) {
    Queue *q = arg;
    for (int i = 0; i < ITEMS; i++) {
        pthread_mutex_lock(&q->m);
        while (q->count == 0) pthread_cond_wait(&q->not_empty, &q->m);
        unsigned char *p = q->buf[q->head]; size_t size = q->size[q->head];
        q->head = (q->head + 1) % QCAP; q->count--;
        pthread_cond_signal(&q->not_full);
        pthread_mutex_unlock(&q->m);
        for (size_t j = 0; j < size; j++)
            if (p[j] != (unsigned char)size) { fprintf(stderr, "corrupt\n"); abort(); }
        free(p);                      /* freed by a different thread */
    }
    return NULL;
}

static void *short_lived(void *arg) {
    (void)arg;
    void *p[40];
    for (int i = 0; i < 40; i++) p[i] = malloc(16 + i * 100);
    for (int i = 0; i < 40; i++) free(p[i]);   /* leaves blocks in the cache at exit */
    return NULL;
}

int main(void) {
    pthread_t pt[PAIRS], ct[PAIRS];
    for (int i = 0; i < PAIRS; i++) {
        pthread_mutex_init(&queues[i].m, NULL);
        pthread_cond_init(&queues[i].not_empty, NULL);
        pthread_cond_init(&queues[i].not_full, NULL);
        pthread_create(&pt[i], NULL, producer, &queues[i]);
        pthread_create(&ct[i], NULL, consumer, &queues[i]);
    }
    for (int i = 0; i < PAIRS; i++) { pthread_join(pt[i], NULL); pthread_join(ct[i], NULL); }

    for (int round = 0; round < 50; round++) {      /* thread churn */
        pthread_t t[20];
        for (int i = 0; i < 20; i++) pthread_create(&t[i], NULL, short_lived, NULL);
        for (int i = 0; i < 20; i++) pthread_join(t[i], NULL);
    }
    puts("test_cross: all tests passed");
    return 0;
}