#include "ready_queue.h"
#include <stddef.h>

void rq_init(ready_queue_t *q) {
    q->head = NULL;
    q->tail = NULL;
    q->count = 0;
}

int rq_is_empty(const ready_queue_t *q) {
    return q->count == 0;
}

void rq_enqueue(ready_queue_t *q, process_t *p) {
    p->next = NULL; /* see header comment: defend against stale links */
    if (q->tail == NULL) {
        /* queue was empty: new node is both head and tail */
        q->head = p;
        q->tail = p;
    } else {
        q->tail->next = p;
        q->tail = p;
    }
    q->count++;
}

process_t *rq_dequeue(ready_queue_t *q) {
    if (q->head == NULL) {
        return NULL; /* empty queue is a normal condition (CPU may be idle) */
    }
    process_t *p = q->head;
    q->head = p->next;
    if (q->head == NULL) {
        q->tail = NULL; /* removing the only element empties the queue */
    }
    p->next = NULL; /* fully detach — see header comment */
    q->count--;
    return p;
}
