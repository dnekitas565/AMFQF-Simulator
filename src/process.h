#ifndef AMFQF_PROCESS_H
#define AMFQF_PROCESS_H

#include <stdlib.h>
#include "group.h"

/*
 * process_t — a single simulated process moving through the AMFQF
 * scheduler's tick-based simulation.
 *
 * Field-by-field purpose (as specified in the AMFQF Implementation Guide,
 * Phase 1):
 *
 *   pid              Unique process identifier. Purely a label used for
 *                     reporting/debugging; the scheduler does not use it
 *                     for any decision logic.
 *
 *   arrival_tick      The tick at which this process becomes runnable.
 *                     Before this tick, the process must not be placed
 *                     into any group's ready queue. Set once at trace-load
 *                     time and never modified afterward.
 *
 *   total_burst       The process's total CPU requirement in quanta, fixed
 *                     at creation time. Kept alongside remaining_burst
 *                     (rather than recomputed) so that later metrics code
 *                     can report "percent of burst completed" without
 *                     needing to reconstruct the original value.
 *
 *   remaining_burst   CPU work still required, in quanta. Decremented by
 *                     exactly 1 every time this process is dispatched for
 *                     one tick. When it reaches 0, the process is
 *                     complete.
 *
 *   group             Which of the four AMFQF fairness groups (see
 *                     group.h) this process belongs to. Assigned once,
 *                     at trace-generation time, and does not change
 *                     during the simulation — AMFQF classifies once
 *                     up front rather than reclassifying processes
 *                     mid-run.
 *
 *   wait_time         Accumulated ticks spent runnable-but-not-running.
 *                     Incremented by 1, once per tick, for every process
 *                     that is in a ready queue but is NOT the one
 *                     dispatched that tick. This is the primary per-
 *                     process fairness/responsiveness metric used in
 *                     Phase 6.
 *
 *   completion_tick   The tick at which remaining_burst reaches 0. Used
 *                     to compute turnaround time (completion_tick -
 *                     arrival_tick) in the metrics phase. Sentinel value
 *                     -1 means "not yet completed" (see below).
 *
 *   next              Intrusive singly-linked-list pointer. Each group
 *                     maintains its own ready queue as a circular linked
 *                     list of process_t nodes (see group.h); this field
 *                     is that queue's link. A process is a member of at
 *                     most one group's ready queue at a time, so a single
 *                     `next` field (rather than a separate queue-node
 *                     wrapper struct) is sufficient and avoids an extra
 *                     allocation per process.
 *
 * Sentinel values (documented so Phase 3–5 schedulers agree on them):
 *   completion_tick == -1   process has not finished yet
 *   wait_time somewhere >= 0 always; starts at 0 at construction
 *   next == NULL             process is not currently linked into any
 *                             ready queue (either not yet arrived, or
 *                             already completed and removed)
 */
typedef struct process {
    int pid;
    int arrival_tick;
    int total_burst;
    int remaining_burst;
    group_id_t group;
    int wait_time;
    int completion_tick;
    struct process *next;
} process_t;

/*
 * process_create — allocate and initialize a process_t on the heap.
 *
 * WHY a constructor function rather than letting every caller fill the
 * struct in by hand: every later phase (trace generator, all three
 * schedulers) creates processes the same way, and centralizing the
 * sentinel-value initialization here (wait_time = 0, completion_tick =
 * -1, next = NULL) means those invariants can't be forgotten or
 * duplicated inconsistently at each call site.
 *
 * Returns NULL on allocation failure; caller must check.
 */
static inline process_t *process_create(int pid, int arrival_tick,
                                         int total_burst, group_id_t group) {
    process_t *p = (process_t *)malloc(sizeof(process_t));
    if (p == NULL) {
        return NULL;
    }
    p->pid = pid;
    p->arrival_tick = arrival_tick;
    p->total_burst = total_burst;
    p->remaining_burst = total_burst;
    p->group = group;
    p->wait_time = 0;
    p->completion_tick = -1; /* sentinel: not yet completed */
    p->next = NULL;
    return p;
}

/*
 * process_destroy — free a single process_t.
 *
 * WHY a paired destructor: matches process_create so every malloc has an
 * obvious, unambiguous free, which matters in a long-running tick loop
 * (Part 0.1 of the guide explicitly flags this as a place bugs creep in).
 * Does NOT touch p->next — the caller is responsible for having already
 * unlinked the process from any ready queue before destroying it, since
 * this function has no way to know which queue (if any) currently holds
 * a pointer to this node.
 */
static inline void process_destroy(process_t *p) {
    free(p);
}

#endif /* AMFQF_PROCESS_H */
