/******************************************************************************
 * threadpool.h — Fixed-size thread pool with a bounded work queue.
 *
 * Producer-consumer: callers submit work via dispatch(), workers dequeue
 * and execute.  Queue has a max capacity; dispatch() returns -1 (non-blocking)
 * when full.
 ******************************************************************************/

#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>

#define MAXT_IN_POOL  200
#define MAXW_IN_QUEUE 200

typedef struct work_st {
    int (*routine)(void *);
    void           *arg;
    struct work_st *next;
} work_t;

typedef int (*dispatch_fn)(void *);

typedef struct _threadpool_st {
    int              num_threads;
    int              qsize;
    int              max_qsize;
    pthread_t       *threads;
    work_t          *qhead;
    work_t          *qtail;
    pthread_mutex_t  qlock;
    pthread_cond_t   q_not_empty;
    pthread_cond_t   q_empty;
    pthread_cond_t   q_not_full;
    int              shutdown;
    int              dont_accept;
} threadpool;

threadpool *create_threadpool(int num_threads_in_pool, int max_queue_size);
int         dispatch(threadpool *from_me, dispatch_fn dispatch_to_here, void *arg);
int         dispatch_priority(threadpool *from_me, dispatch_fn dispatch_to_here, void *arg);
void       *do_work(void *p);
void        destroy_threadpool(threadpool *destroyme);

#endif /* THREADPOOL_H */
