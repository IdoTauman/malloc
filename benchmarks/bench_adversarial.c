/*
 * bench_adversarial.c
 *
 * Workloads chosen to stress the weak spots of a size-class, non-coalescing,
 * thread-cached allocator: memory footprint, cross-class reuse, large
 * allocations, realloc growth and thread churn.
 *
 * Each scenario runs in a forked child so peak RSS (ru_maxrss) is measured
 * per scenario, relative to the child's starting RSS.
 *
 * Build against my allocator:
 *   gcc -std=c11 -O2 -pthread -D_DEFAULT_SOURCE -Iinclude benchmarks/bench_adversarial.c \
 *       -Lbuild -lallocator -Wl,-rpath,$PWD/build -o build/bench_adv_mine
 * Build against glibc (no -lallocator):
 *   gcc -std=c11 -O2 -pthread -D_DEFAULT_SOURCE benchmarks/bench_adversarial.c \
 *       -o build/bench_adv_glibc
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_THREADS 16

static volatile unsigned char sink;
static uint64_t g_requested; /* bytes the scenario asked for (live at peak), 0 if n/a */

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static uint32_t rnd(uint32_t *s) {
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

static long rss_kb(void) {
    struct rusage r;
    getrusage(RUSAGE_SELF, &r);
    return r.ru_maxrss; /* KiB on Linux */
}

static void must(void *p) {
    if (!p) {
        fputs("allocation failed\n", stderr);
        _exit(2);
    }
}

static void run_threads(size_t n, void *(*fn)(void *)) {
    pthread_t th[MAX_THREADS];
    size_t ids[MAX_THREADS];
    for (size_t i = 0; i < n; i++) {
        ids[i] = i;
        if (pthread_create(&th[i], NULL, fn, &ids[i]) != 0) {
            perror("pthread_create");
            _exit(2);
        }
    }
    for (size_t i = 0; i < n; i++) {
        pthread_join(th[i], NULL);
    }
}

/* ------------------------------------------------------------------ */
/* 1. Large allocations: 128 KiB .. 1 MiB, alloc + touch + free.       */
/*    A mmap/munmap per op for a design that maps large blocks directly */
/* ------------------------------------------------------------------ */
#define LARGE_ITERS 20000

static void *large_worker(void *arg) {
    size_t id = *(size_t *)arg;
    uint32_t s = 0x9E3779B9u * (uint32_t)(id + 1);
    for (size_t i = 0; i < LARGE_ITERS; i++) {
        size_t size = 128 * 1024 + (size_t)(rnd(&s) % (896 * 1024));
        unsigned char *p = malloc(size);
        must(p);
        p[0] = 1;
        p[size - 1] = 1;
        sink = p[size / 2];
        free(p);
    }
    return NULL;
}

static uint64_t scen_large(size_t threads) {
    run_threads(threads, large_worker);
    return (uint64_t)threads * LARGE_ITERS * 2;
}

/* ------------------------------------------------------------------ */
/* 2. Producer/consumer: memory is always freed by another thread.     */
/*    Batches are handed over so the queue lock is not the bottleneck. */
/* ------------------------------------------------------------------ */
#define PC_BATCH 512
#define PC_ROUNDS 2000
#define PC_DEPTH 4

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t cv;
    void *slot[PC_DEPTH][PC_BATCH];
    size_t head, tail, count;
} PCQueue;

static PCQueue pcq[MAX_THREADS / 2];

static void *pc_worker(void *arg) {
    size_t id = *(size_t *)arg;
    PCQueue *q = &pcq[id / 2];
    uint32_t s = 0x9E3779B9u * (uint32_t)(id + 1);

    if (id % 2 == 0) { /* producer */
        for (size_t r = 0; r < PC_ROUNDS; r++) {
            pthread_mutex_lock(&q->m);
            while (q->count == PC_DEPTH) pthread_cond_wait(&q->cv, &q->m);
            size_t idx = q->tail;
            pthread_mutex_unlock(&q->m);

            for (size_t i = 0; i < PC_BATCH; i++) {
                size_t size = 16 + (size_t)(rnd(&s) % 497);
                unsigned char *p = malloc(size);
                must(p);
                p[0] = 1;
                q->slot[idx][i] = p;
            }

            pthread_mutex_lock(&q->m);
            q->tail = (q->tail + 1) % PC_DEPTH;
            q->count++;
            pthread_cond_broadcast(&q->cv);
            pthread_mutex_unlock(&q->m);
        }
    } else { /* consumer */
        for (size_t r = 0; r < PC_ROUNDS; r++) {
            pthread_mutex_lock(&q->m);
            while (q->count == 0) pthread_cond_wait(&q->cv, &q->m);
            size_t idx = q->head;
            pthread_mutex_unlock(&q->m);

            for (size_t i = 0; i < PC_BATCH; i++) {
                unsigned char *p = q->slot[idx][i];
                sink = p[0];
                free(p);
            }

            pthread_mutex_lock(&q->m);
            q->head = (q->head + 1) % PC_DEPTH;
            q->count--;
            pthread_cond_broadcast(&q->cv);
            pthread_mutex_unlock(&q->m);
        }
    }
    return NULL;
}

static uint64_t scen_prodcons(size_t threads) {
    size_t pairs = threads / 2;
    if (pairs == 0) pairs = 1;
    for (size_t i = 0; i < pairs; i++) {
        memset(&pcq[i], 0, sizeof(pcq[i]));
        pthread_mutex_init(&pcq[i].m, NULL);
        pthread_cond_init(&pcq[i].cv, NULL);
    }
    run_threads(pairs * 2, pc_worker);
    return (uint64_t)pairs * PC_ROUNDS * PC_BATCH * 2;
}

/* ------------------------------------------------------------------ */
/* 3. Shifting size distribution: fill 32 MiB with one size, free it   */
/*    all, move to the next size. A coalescing allocator reuses the    */
/*    same pages; per-class free lists cannot.                         */
/* ------------------------------------------------------------------ */
#define PHASE_BYTES (32u << 20)
static void *phase_ptrs[PHASE_BYTES / 32];

static uint64_t scen_phase_shift(size_t threads) {
    (void)threads;
    uint64_t ops = 0;
    for (size_t size = 32; size <= 8192; size *= 2) {
        size_t count = PHASE_BYTES / size;
        for (size_t i = 0; i < count; i++) {
            unsigned char *p = malloc(size);
            must(p);
            memset(p, 1, size);
            phase_ptrs[i] = p;
        }
        for (size_t i = 0; i < count; i++) {
            free(phase_ptrs[i]);
        }
        ops += (uint64_t)count * 2;
    }
    g_requested = PHASE_BYTES; /* live data never exceeds this */
    return ops;
}

/* ------------------------------------------------------------------ */
/* 4. Footprint: hold many objects live, compare RSS to bytes asked.   */
/*    16 B objects expose per-block header overhead; 4100 B objects    */
/*    land just above a power-of-two class boundary (~2x internal waste) */
/* ------------------------------------------------------------------ */
typedef struct Node {
    struct Node *next;
} Node;

static uint64_t footprint(size_t count, size_t size) {
    Node *head = NULL;
    for (size_t i = 0; i < count; i++) {
        Node *n = malloc(size);
        must(n);
        memset(n, 1, size);
        n->next = head;
        head = n;
    }
    g_requested = (uint64_t)count * size;
    uint64_t ops = count;
    /* RSS peak already captured; keep the list live until here. */
    while (head) {
        Node *n = head->next;
        free(head);
        head = n;
        ops++;
    }
    return ops;
}

static uint64_t scen_footprint_16(size_t threads) {
    (void)threads;
    return footprint(2000000, 16);
}

static uint64_t scen_footprint_4100(size_t threads) {
    (void)threads;
    return footprint(20000, 4100);
}

/* ------------------------------------------------------------------ */
/* 5. realloc growth in 4 KiB steps up to 4 MiB. Allocators that can   */
/*    mremap large blocks avoid the copy; copy-always designs go O(n^2) */
/* ------------------------------------------------------------------ */
#define GROW_STEP 4096u
#define GROW_MAX (4u << 20)
#define GROW_REPEATS 5

static uint64_t scen_realloc_growth(size_t threads) {
    (void)threads;
    uint64_t ops = 0;
    for (int rep = 0; rep < GROW_REPEATS; rep++) {
        size_t size = GROW_STEP;
        unsigned char *p = malloc(size);
        must(p);
        memset(p, 1, size);
        while (size < GROW_MAX) {
            size += GROW_STEP;
            p = realloc(p, size);
            must(p);
            memset(p + size - GROW_STEP, 1, GROW_STEP);
            ops++;
        }
        free(p);
    }
    return ops;
}

/* ------------------------------------------------------------------ */
/* 6. Thread churn: many short-lived threads, each with a few allocs.  */
/*    Exercises per-thread cache setup and teardown.                   */
/* ------------------------------------------------------------------ */
#define CHURN_ROUNDS 2500
#define CHURN_OPS 100

static void *churn_worker(void *arg) {
    size_t id = *(size_t *)arg;
    uint32_t s = 0x85EBCA6Bu * (uint32_t)(id + 1);
    void *live[32] = {0};
    for (size_t i = 0; i < CHURN_OPS; i++) {
        size_t slot = i % 32;
        free(live[slot]);
        size_t size = 1 + (size_t)(rnd(&s) % 2048);
        unsigned char *p = malloc(size);
        must(p);
        p[0] = 1;
        live[slot] = p;
    }
    for (size_t i = 0; i < 32; i++) free(live[i]);
    return NULL;
}

static uint64_t scen_churn(size_t threads) {
    for (size_t r = 0; r < CHURN_ROUNDS; r++) {
        run_threads(threads, churn_worker);
    }
    return (uint64_t)CHURN_ROUNDS * threads * CHURN_OPS * 2;
}

/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    uint64_t (*fn)(size_t);
    size_t threads[4]; /* 0-terminated */
} Scenario;

static const Scenario scenarios[] = {
    {"large_alloc",      scen_large,           {1, 4, 8, 0}},
    {"producer_consumer", scen_prodcons,       {2, 4, 8, 0}},
    {"phase_shift",      scen_phase_shift,     {1, 0}},
    {"footprint_16B",    scen_footprint_16,    {1, 0}},
    {"footprint_4100B",  scen_footprint_4100,  {1, 0}},
    {"realloc_growth",   scen_realloc_growth,  {1, 0}},
    {"thread_churn",     scen_churn,           {1, 8, 0}},
};

int main(void) {
    printf("%-18s %7s %9s %10s %10s  %s\n", "scenario", "threads", "time(s)",
           "Mops/s", "peakRSS(MB)", "RSS / requested");
    puts("--------------------------------------------------------------------------------");
    fflush(stdout);

    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
        for (size_t t = 0; scenarios[i].threads[t] != 0; t++) {
            size_t threads = scenarios[i].threads[t];
            pid_t pid = fork();
            if (pid < 0) {
                perror("fork");
                return 1;
            }
            if (pid == 0) {
                long base = rss_kb();
                double t0 = now();
                uint64_t ops = scenarios[i].fn(threads);
                double secs = now() - t0;
                double rss_mb = (double)(rss_kb() - base) / 1024.0;
                char ratio[32] = "-";
                if (g_requested) {
                    snprintf(ratio, sizeof ratio, "%.2fx",
                             rss_mb * 1048576.0 / (double)g_requested);
                }
                printf("%-18s %7zu %9.3f %10.2f %10.1f  %s\n", scenarios[i].name,
                       threads, secs, (double)ops / secs / 1e6, rss_mb, ratio);
                fflush(stdout);
                _exit(0);
            }
            int status = 0;
            waitpid(pid, &status, 0);
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                printf("%-18s %7zu  FAILED\n", scenarios[i].name, threads);
            }
        }
    }
    return 0;
}