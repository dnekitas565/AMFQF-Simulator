/*
 * AMFQF scheduler runtime skeleton.
 *
 * This translation unit intentionally has no public scheduling entry point
 * in this milestone.  It fixes the private state layout before policy is
 * added incrementally: process admission and MLFQ transitions are the next
 * component, followed by accounting, redistribution, borrowing, and
 * starvation protection.
 */

#include "scheduler_amfqf.h"
#include "ready_queue.h"
#include "trace_loader.h"

typedef enum {
    AMFQF_PROCESS_NEW,
    AMFQF_PROCESS_READY,
    AMFQF_PROCESS_RUNNING,
    AMFQF_PROCESS_COMPLETE
} amfqf_process_status_t;

typedef struct {
    int queue_level;
    int quantum_left;
    int ready_wait;
    int first_dispatch_tick; /* -1 until first service */
    int protected_service;
    amfqf_process_status_t status;
} amfqf_process_state_t;

typedef struct {
    double target_budget;
    double actual_usage;
    double debt;
} amfqf_group_state_t;

typedef struct {
    ready_queue_t ready[AMFQF_QUEUE_LEVELS][AMFQF_RUNNABLE_GROUPS];
    amfqf_group_state_t group[AMFQF_RUNNABLE_GROUPS];
    amfqf_process_state_t *process;
    process_t *running;
    int tick;
    int next_unadmitted;
    int completed_count;
    int idle_ticks;
    int protected_ticks_left;
} amfqf_runtime_state_t;

/* The structures above are deliberately private.  They isolate AMFQF's
 * queue level, quantum, ready-wait, protection, and budget state from the
 * shared process_t used unchanged by the RR and CFS baselines. */
