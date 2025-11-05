#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <errno.h>
#include <semaphore.h>
#include <time.h>

#define ORDER_Q_CAP  256
#define DONE_Q_CAP   256

// Timestamp helper for logs
static void now_str(char *buf, size_t n) {
    struct timeval tv; gettimeofday(&tv, NULL);
    struct tm tm; localtime_r(&tv.tv_sec, &tm);
    snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d.%06ld",
             tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, (long)tv.tv_usec);
}

// Single-line, flushed logging
static void log_event(const char *role, unsigned long tid, const char *fmt, ...) {
    char ts[64]; now_str(ts, sizeof ts);
    fprintf(stdout, "%s | %s %lu | ", ts, role, tid);
    va_list ap; va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap);
    fputc('\n', stdout); fflush(stdout);
}

typedef struct customer customer_t;

typedef struct {
    int id;
    customer_t *cust;
} order_t;

// Per-customer state + condvars
struct customer {
    int cid;
    pthread_mutex_t mu;
    pthread_cond_t  cv_seated;
    pthread_cond_t  cv_meal;
    bool seated;
    bool meal_ready;
};

// Bounded MPMC queue (circular buffer)
typedef struct {
    customer_t **buf; int cap; int head; int tail; int count;
    pthread_mutex_t mu; pthread_cond_t not_empty; pthread_cond_t not_full;
} cust_queue_t;

static void cq_init(cust_queue_t *q, int cap) {
    q->buf = calloc(cap, sizeof(customer_t*)); q->cap = cap; q->head = q->tail = q->count = 0;
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}
static void cq_destroy(cust_queue_t *q) {
    free(q->buf); pthread_mutex_destroy(&q->mu); pthread_cond_destroy(&q->not_empty); pthread_cond_destroy(&q->not_full);
}
static void cq_push(cust_queue_t *q, customer_t *c) {
    pthread_mutex_lock(&q->mu);
    while (q->count == q->cap) pthread_cond_wait(&q->not_full, &q->mu);
    q->buf[q->tail] = c; q->tail = (q->tail + 1) % q->cap; q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
}
static customer_t* cq_pop(cust_queue_t *q) {
    pthread_mutex_lock(&q->mu);
    while (q->count == 0) pthread_cond_wait(&q->not_empty, &q->mu);
    customer_t *c = q->buf[q->head]; q->head = (q->head + 1) % q->cap; q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mu);
    return c;
}
static int cq_try_pop(cust_queue_t *q, customer_t **out) {
    int ok = 0; pthread_mutex_lock(&q->mu);
    if (q->count > 0) { *out = q->buf[q->head]; q->head = (q->head + 1) % q->cap; q->count--; pthread_cond_signal(&q->not_full); ok = 1; }
    pthread_mutex_unlock(&q->mu); return ok;
}

// Same queue pattern for orders
typedef struct {
    order_t *buf; int cap; int head; int tail; int count;
    pthread_mutex_t mu; pthread_cond_t not_empty; pthread_cond_t not_full;
} order_queue_t;

static void oq_init(order_queue_t *q, int cap) {
    q->buf = calloc(cap, sizeof(order_t)); q->cap = cap; q->head = q->tail = q->count = 0;
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}
static void oq_destroy(order_queue_t *q) {
    free(q->buf); pthread_mutex_destroy(&q->mu); pthread_cond_destroy(&q->not_empty); pthread_cond_destroy(&q->not_full);
}
static void oq_push(order_queue_t *q, order_t it) {
    pthread_mutex_lock(&q->mu);
    while (q->count == q->cap) pthread_cond_wait(&q->not_full, &q->mu);
    q->buf[q->tail] = it; q->tail = (q->tail + 1) % q->cap; q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
}

static order_t oq_pop(order_queue_t *q) {
    pthread_mutex_lock(&q->mu);
    while (q->count == 0) pthread_cond_wait(&q->not_empty, &q->mu);
    order_t it = q->buf[q->head]; q->head = (q->head + 1) % q->cap; q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mu);
    return it;
}
static int oq_try_pop(order_queue_t *q, order_t *out) {
    int ok = 0; pthread_mutex_lock(&q->mu);
    if (q->count > 0) { *out = q->buf[q->head]; q->head = (q->head + 1) % q->cap; q->count--; pthread_cond_signal(&q->not_full); ok = 1; }
    pthread_mutex_unlock(&q->mu); return ok;
}

typedef order_queue_t done_queue_t;

static int TOTAL_CUSTOMERS, NUM_TABLES, NUM_WAITERS, NUM_CHEFS, WAITING_CAPACITY;
static volatile int customers_created = 0;
static volatile int customers_served  = 0;

static pthread_mutex_t g_count_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_order_id_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_next_order_id = 1; // shared order id to avoid duplicates across waiters
static sem_t tables_sem;

static cust_queue_t waiting_q;
static order_queue_t order_q;
static done_queue_t  done_q;

static void rnd_sleep_ms(int lo, int hi) {
    int span = hi - lo + 1; if (span < 1) span = 1; int ms = lo + (rand() % span); usleep(ms * 1000);
}

static bool queues_all_empty(void) {
    bool empty;
    pthread_mutex_lock(&waiting_q.mu);
    pthread_mutex_lock(&order_q.mu);
    pthread_mutex_lock(&done_q.mu);
    empty = (waiting_q.count == 0 && order_q.count == 0 && done_q.count == 0);
    pthread_mutex_unlock(&done_q.mu);
    pthread_mutex_unlock(&order_q.mu);
    pthread_mutex_unlock(&waiting_q.mu);
    return empty;
}

// Customer: arrive → wait → meal → eat → leave
static void *customer_thread(void *arg) {
    customer_t *c = (customer_t *)arg;
    unsigned long tid = (unsigned long)pthread_self();
    log_event("Customer", tid, "arrived (cid=%d)", c->cid);

    cq_push(&waiting_q, c);
    log_event("Customer", tid, "queued for seating (cid=%d)", c->cid);

    pthread_mutex_lock(&c->mu);
    while (!c->seated) pthread_cond_wait(&c->cv_seated, &c->mu);
    pthread_mutex_unlock(&c->mu);
    log_event("Customer", tid, "seated (cid=%d)", c->cid);

    pthread_mutex_lock(&c->mu);
    while (!c->meal_ready) pthread_cond_wait(&c->cv_meal, &c->mu);
    pthread_mutex_unlock(&c->mu);
    log_event("Customer", tid, "meal received (cid=%d)", c->cid);

    rnd_sleep_ms(1500, 3000);

    sem_post(&tables_sem);
    log_event("Customer", tid, "leaving (cid=%d)", c->cid);

    pthread_mutex_lock(&g_count_mu); customers_served++; pthread_mutex_unlock(&g_count_mu);

    pthread_mutex_destroy(&c->mu); pthread_cond_destroy(&c->cv_seated); pthread_cond_destroy(&c->cv_meal);
    free(c);
    return NULL;
}


