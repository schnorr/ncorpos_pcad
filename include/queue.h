/*
This file is part of "NCorpos @ PCAD".

"NCorpos @ PCAD" is free software: you can redistribute it and/or
modify it under the terms of the GNU General Public License as
published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.
*/
#ifndef __QUEUE_H_
#define __QUEUE_H_

#include <pthread.h>

/*
 * Generic, unbounded, thread-safe blocking queue.
 * Items are managed as void*.  Ownership passes to the queue on
 * enqueue and returns to the caller on dequeue.
 * A custom free_function is called by queue_clear/queue_destroy.
 */
typedef struct queue {
  void          **queue;
  size_t          buffer_size; /* capacity + 1 (distinguishes full/empty) */
  size_t          front;
  size_t          back;
  pthread_mutex_t mutex;
  pthread_cond_t  not_empty;
  void (*free_function)(void *);
} queue_t;

void  queue_init        (queue_t *q, size_t starting_capacity,
                         void (*free_function)(void *));
void  queue_enqueue     (queue_t *q, void *item);
void *queue_dequeue     (queue_t *q);      /* blocks until item available */
void *queue_try_dequeue (queue_t *q);      /* non-blocking; NULL if empty */
size_t queue_size       (queue_t *q);
void  queue_clear       (queue_t *q);
void  queue_destroy     (queue_t *q);

#endif /* __QUEUE_H_ */
