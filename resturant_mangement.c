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

// Chef: cook orders, enqueue completed
static void *chef_thread(void *arg) {
    (void)arg; unsigned long tid = (unsigned long)pthread_self();
    for (;;) {
        pthread_mutex_lock(&g_count_mu);
        int done = (customers_served >= TOTAL_CUSTOMERS);
        pthread_mutex_unlock(&g_count_mu);
        order_t ord;
        if (!oq_try_pop(&order_q, &ord)) {
            if (done) break;
            ord = oq_pop(&order_q);
        }
        log_event("Chef", tid, "cooking order %d (cid=%d)", ord.id, ord.cust->cid);
        rnd_sleep_ms(1000, 3000);
        dq_push(&done_q, ord);
        log_event("Chef", tid, "finished order %d (cid=%d)", ord.id, ord.cust->cid);
    }
    log_event("Chef", tid, "exiting");
    return NULL;
}

// Waiter: deliver dishes; seat customers; place orders
static void *waiter_thread(void *arg) {
    (void)arg; unsigned long tid = (unsigned long)pthread_self();
    for (;;) {
        order_t ready;
        if (dq_try_pop(&done_q, &ready)) {
            customer_t *c = ready.cust;
            pthread_mutex_lock(&c->mu); c->meal_ready = true; pthread_cond_signal(&c->cv_meal); pthread_mutex_unlock(&c->mu);
            log_event("Waiter", tid, "delivered order %d (cid=%d)", ready.id, c->cid);
            continue;
        }

        customer_t *c = NULL;
        if (sem_trywait(&tables_sem) == 0) {
            if (cq_try_pop(&waiting_q, &c)) {
                pthread_mutex_lock(&c->mu); c->seated = true; pthread_cond_signal(&c->cv_seated); pthread_mutex_unlock(&c->mu);
                log_event("Waiter", tid, "seated customer (cid=%d)", c->cid);
                order_t ord;
                pthread_mutex_lock(&g_order_id_mu); ord.id = g_next_order_id++; pthread_mutex_unlock(&g_order_id_mu);
                ord.cust = c;
                oq_push(&order_q, ord);
                log_event("Waiter", tid, "took order %d (cid=%d)", ord.id, c->cid);
                continue;
            } else {
                sem_post(&tables_sem);
            }
        }
        struct timespec ts; struct timeval tv; gettimeofday(&tv, NULL);
        ts.tv_sec = tv.tv_sec; ts.tv_nsec = (tv.tv_usec + 200*1000) * 1000;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }

        pthread_mutex_lock(&done_q.mu);
        int rc = 0; (void)rc;
        if (done_q.count == 0) rc = pthread_cond_timedwait(&done_q.not_empty, &done_q.mu, &ts);
        pthread_mutex_unlock(&done_q.mu);

        pthread_mutex_lock(&g_count_mu);
        int done = (customers_served >= TOTAL_CUSTOMERS);
        pthread_mutex_unlock(&g_count_mu);
        if (done && queues_all_empty()) break;
    }
    log_event("Waiter", tid, "exiting");
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "Usage: %s <TOTAL_CUSTOMERS> <NUM_TABLES> <NUM_WAITERS> <NUM_CHEFS> <WAITING_CAPACITY>\n", argv[0]);
        return 1;
    }
    TOTAL_CUSTOMERS = atoi(argv[1]);
    NUM_TABLES      = atoi(argv[2]);
    NUM_WAITERS     = atoi(argv[3]);
    NUM_CHEFS       = atoi(argv[4]);
    WAITING_CAPACITY= atoi(argv[5]);
    if (TOTAL_CUSTOMERS<=0||NUM_TABLES<=0||NUM_WAITERS<=0||NUM_CHEFS<=0||WAITING_CAPACITY<=0) {
        fprintf(stderr, "All parameters must be positive.\n");
        return 1;
    }

    srand(1234567);

    if (sem_init(&tables_sem, 0, NUM_TABLES) != 0) { perror("sem_init"); return 1; }
    cq_init(&waiting_q, WAITING_CAPACITY);
    oq_init(&order_q, ORDER_Q_CAP);
    dq_init(&done_q,  DONE_Q_CAP);
    
    pthread_t *waiters = calloc(NUM_WAITERS, sizeof(pthread_t));
    for (int i = 0; i < NUM_WAITERS; ++i) pthread_create(&waiters[i], NULL, waiter_thread, NULL);

    pthread_t *chefs = calloc(NUM_CHEFS, sizeof(pthread_t));
    for (int i = 0; i < NUM_CHEFS; ++i) pthread_create(&chefs[i], NULL, chef_thread, NULL);
}
