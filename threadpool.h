#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>

/*
 * threadpool.h — Fixed-size thread pool with a bounded work queue.
 *
 * The pool uses a producer-consumer model: callers submit work via dispatch(),
 * and a fixed set of worker threads dequeue and execute jobs concurrently.
 * The queue has a maximum capacity; dispatch() blocks when full.
 */

#define MAXT_IN_POOL  200
#define MAXW_IN_QUEUE 200

/* A single unit of work in the queue. */
typedef struct work_st {
    int (*routine)(void *);
    void *arg;
    struct work_st *next;
} work_t;

/* Function pointer type for dispatched work. */
typedef int (*dispatch_fn)(void *);

/* The thread pool. */
typedef struct _threadpool_st {
    int num_threads;            /* number of worker threads          */
    int qsize;                  /* current number of jobs in queue   */
    int max_qsize;              /* maximum queue capacity            */
    pthread_t *threads;         /* array of worker thread handles    */
    work_t *qhead;              /* queue head (dequeue end)          */
    work_t *qtail;              /* queue tail (enqueue end)          */
    pthread_mutex_t qlock;      /* protects all pool state           */
    pthread_cond_t q_not_empty; /* signaled when work is enqueued    */
    pthread_cond_t q_empty;     /* signaled when queue drains to 0   */
    pthread_cond_t q_not_full;  /* signaled when a slot opens up     */
    int shutdown;               /* 1 = threads should exit           */
    int dont_accept;            /* 1 = reject new dispatch calls     */
} threadpool;

/*
 * create_threadpool — Allocate and start a thread pool.
 *
 * Returns a pool handle on success, or NULL on failure.
 * num_threads_in_pool: number of worker threads (1..MAXT_IN_POOL).
 * max_queue_size:      bounded queue capacity  (1..MAXW_IN_QUEUE).
 */
threadpool *create_threadpool(int num_threads_in_pool, int max_queue_size);

/*
 * dispatch — Submit a job to the pool.
 *
 * Returns 0 on success, -1 if the queue is full or the pool is shutting down.
 * dispatch_to_here: function the worker will call.
 * arg:              opaque argument passed to the function.
 */
int dispatch(threadpool *from_me, dispatch_fn dispatch_to_here, void *arg);

/*
 * do_work — Internal worker loop (passed to pthread_create).
 *
 * Dequeues jobs and executes them until shutdown is signaled.
 */
void *do_work(void *p);

/*
 * destroy_threadpool — Shut down the pool and free all resources.
 *
 * Stops accepting new work, waits for queued jobs to complete,
 * signals all workers to exit, joins every thread, then frees memory.
 */
void destroy_threadpool(threadpool *destroyme);

#endif /* THREADPOOL_H */
