#ifndef AMFQF_READY_QUEUE_H
#define AMFQF_READY_QUEUE_H

#include "process.h"

/*
 * ready_queue_t — a plain FIFO queue of process_t nodes, linked via each
 * process's own `next` field (process.h). Head/tail pointers give O(1)
 * enqueue-at-tail and O(1) dequeue-at-head.
 *
 * WHY NOT the circular list group.h's comments describe: that design
 * exists for AMFQF's per-group queues (Phase 5), where a persistent
 * "whose turn is it" cursor needs to survive across many dispatch
 * decisions without walking off the end. Plain Round Robin doesn't need
 * that: "rotate to the back of the line" is already exactly what
 * dequeue-then-enqueue-at-tail does, with no wraparound pointer to get
 * wrong. A non-circular head/tail list is the simpler structure that
 * produces the identical externally-visible rotation behavior.
 *
 * Ownership: this queue does NOT own the process_t nodes it holds — it
 * only holds pointers into a workload_t (see trace_loader.h) that owns
 * them. Destroying a queue does not and must not destroy its members.
 */
typedef struct {
    process_t *head;
    process_t *tail;
    int count;
} ready_queue_t;

/* Initialize an empty queue. Must be called before any other operation. */
void rq_init(ready_queue_t *q);

/* Returns nonzero if the queue currently holds no processes. */
int rq_is_empty(const ready_queue_t *q);

/*
 * Append p to the tail of the queue. p->next is forcibly reset to NULL
 * first — a process being enqueued must never carry a stale link from
 * whatever list it was previously part of (e.g. the workload array
 * itself doesn't use ->next, but defensively clearing it here means this
 * function is safe to call regardless of p's prior history).
 */
void rq_enqueue(ready_queue_t *q, process_t *p);

/*
 * Remove and return the process at the head of the queue, or NULL if the
 * queue is empty. The returned process's ->next is set to NULL before
 * returning — it is fully detached from the queue's internal linkage, so
 * the caller can safely hold onto it (e.g. as "currently running") for
 * multiple ticks without any residual pointer inside this queue
 * referencing it. This is the specific guard against the
 * "use-after-free / stale pointer" class of bug: once dequeued, nothing
 * inside ready_queue_t still points at that node.
 */
process_t *rq_dequeue(ready_queue_t *q);

#endif /* AMFQF_READY_QUEUE_H */
