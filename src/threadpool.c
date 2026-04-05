/******************************************************************************
 * threadpool.c — Fixed-size thread pool with a bounded work queue.
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include "threadpool.h"

#ifdef DEBUG
#define DEBUG_PRINT(fmt, args...) fprintf(stderr, "[DEBUG] " fmt "\n", ##args)
#else
#define DEBUG_PRINT(fmt, args...)
#endif

threadpool *create_threadpool(int num_threads_in_pool, int max_queue_size) {
    if (num_threads_in_pool <= 0 || num_threads_in_pool > MAXT_IN_POOL ||
        max_queue_size <= 0      || max_queue_size > MAXW_IN_QUEUE) {
        DEBUG_PRINT("Error: invalid pool parameters");
        return NULL;
    }

    threadpool *pool = (threadpool *)malloc(sizeof(threadpool));
    if (!pool) { perror("malloc"); return NULL; }

    pool->num_threads = num_threads_in_pool;
    pool->qsize       = 0;
    pool->max_qsize   = max_queue_size;
    pool->threads     = (pthread_t *)malloc(num_threads_in_pool * sizeof(pthread_t));
    pool->qhead       = NULL;
    pool->qtail       = NULL;
    pool->shutdown    = 0;
    pool->dont_accept = 0;

    if (!pool->threads) {
        perror("malloc");
        free(pool);
        return NULL;
    }

    if (pthread_mutex_init(&pool->qlock, NULL) != 0 ||
        pthread_cond_init(&pool->q_not_empty, NULL) != 0 ||
        pthread_cond_init(&pool->q_empty, NULL) != 0 ||
        pthread_cond_init(&pool->q_not_full, NULL) != 0) {
        perror("pthread init");
        free(pool->threads);
        free(pool);
        return NULL;
    }

    for (int i = 0; i < num_threads_in_pool; i++) {
        if (pthread_create(&pool->threads[i], NULL, do_work, pool) != 0) {
            perror("pthread_create");
            pool->num_threads = i;
            destroy_threadpool(pool);
            return NULL;
        }
    }

    DEBUG_PRINT("Threadpool created: %d threads, max queue=%d",
                num_threads_in_pool, max_queue_size);
    return pool;
}

int dispatch(threadpool *from_me, dispatch_fn dispatch_to_here, void *arg) {
    if (!from_me) return -1;

    pthread_mutex_lock(&from_me->qlock);

    if (from_me->dont_accept || from_me->shutdown) {
        pthread_mutex_unlock(&from_me->qlock);
        return -1;
    }

    if (from_me->qsize >= from_me->max_qsize) {
        pthread_mutex_unlock(&from_me->qlock);
        return -1;
    }

    work_t *work = (work_t *)malloc(sizeof(work_t));
    if (!work) {
        perror("malloc");
        pthread_mutex_unlock(&from_me->qlock);
        return -1;
    }
    work->routine = dispatch_to_here;
    work->arg     = arg;
    work->next    = NULL;

    if (from_me->qsize == 0) {
        from_me->qhead = work;
        from_me->qtail = work;
        pthread_cond_signal(&from_me->q_not_empty);
    } else {
        from_me->qtail->next = work;
        from_me->qtail       = work;
    }
    from_me->qsize++;

    pthread_mutex_unlock(&from_me->qlock);
    DEBUG_PRINT("Job dispatched, qsize=%d", from_me->qsize);
    return 0;
}

/*
 * dispatch_priority — enqueue at the HEAD of the work queue.
 *
 * High-priority requests are serviced before normal (tail-enqueued) work,
 * giving configurable traffic-shaping without a separate queue or scheduler.
 */
int dispatch_priority(threadpool *from_me, dispatch_fn dispatch_to_here, void *arg) {
    if (!from_me) return -1;

    pthread_mutex_lock(&from_me->qlock);

    if (from_me->dont_accept || from_me->shutdown) {
        pthread_mutex_unlock(&from_me->qlock);
        return -1;
    }

    if (from_me->qsize >= from_me->max_qsize) {
        pthread_mutex_unlock(&from_me->qlock);
        return -1;
    }

    work_t *work = (work_t *)malloc(sizeof(work_t));
    if (!work) {
        perror("malloc");
        pthread_mutex_unlock(&from_me->qlock);
        return -1;
    }
    work->routine = dispatch_to_here;
    work->arg     = arg;

    /* Insert at HEAD instead of tail. */
    work->next    = from_me->qhead;
    from_me->qhead = work;
    if (from_me->qsize == 0) {
        from_me->qtail = work;
        pthread_cond_signal(&from_me->q_not_empty);
    }
    from_me->qsize++;

    pthread_mutex_unlock(&from_me->qlock);
    DEBUG_PRINT("HIGH-PRIORITY job dispatched at HEAD, qsize=%d", from_me->qsize);
    return 0;
}

void *do_work(void *p) {
    threadpool *pool = (threadpool *)p;

    while (1) {
        pthread_mutex_lock(&pool->qlock);

        while (pool->qsize == 0 && !pool->shutdown)
            pthread_cond_wait(&pool->q_not_empty, &pool->qlock);

        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->qlock);
            DEBUG_PRINT("Thread %ld exiting (shutdown)", (long)pthread_self());
            pthread_exit(NULL);
        }

        work_t *work = pool->qhead;
        if (work) {
            pool->qhead = work->next;
            pool->qsize--;
            if (pool->qsize == 0) {
                pool->qtail = NULL;
                pthread_cond_broadcast(&pool->q_empty);
            }
            pthread_cond_signal(&pool->q_not_full);
        }
        pthread_mutex_unlock(&pool->qlock);

        if (work) {
            DEBUG_PRINT("Thread %ld executing job", (long)pthread_self());
            work->routine(work->arg);
            free(work);
        }
    }
    return NULL;
}

void destroy_threadpool(threadpool *destroyme) {
    if (!destroyme) return;

    pthread_mutex_lock(&destroyme->qlock);
    destroyme->dont_accept = 1;
    pthread_cond_broadcast(&destroyme->q_not_full);

    while (destroyme->qsize != 0)
        pthread_cond_wait(&destroyme->q_empty, &destroyme->qlock);

    destroyme->shutdown = 1;
    pthread_cond_broadcast(&destroyme->q_not_empty);
    pthread_mutex_unlock(&destroyme->qlock);

    for (int i = 0; i < destroyme->num_threads; i++)
        pthread_join(destroyme->threads[i], NULL);

    /* Free any remaining queue nodes (defensive). */
    work_t *cur = destroyme->qhead;
    while (cur) {
        work_t *next = cur->next;
        free(cur);
        cur = next;
    }

    pthread_mutex_destroy(&destroyme->qlock);
    pthread_cond_destroy(&destroyme->q_not_empty);
    pthread_cond_destroy(&destroyme->q_empty);
    pthread_cond_destroy(&destroyme->q_not_full);
    free(destroyme->threads);
    free(destroyme);
}
