#ifndef AMFQF_SCHEDULER_RR_H
#define AMFQF_SCHEDULER_RR_H

#include "trace_loader.h"

/*
 * RR_QUANTUM — fixed Round Robin time slice, in ticks.
 *
 * No existing project constant controls this (process.h/group.h define
 * no quantum value — group_t.budget is an AMFQF Phase 5 concept, unused
 * here). Per the explicit instruction accompanying this phase, defaulting
 * to 4 ticks since no other value was specified anywhere in the existing
 * codebase.
 */
#define RR_QUANTUM 4

/*
 * rr_run_result_t — small summary of one completed simulation run.
 * Per-process results (wait_time, completion_tick) are NOT duplicated
 * here — they live directly on each process_t in the workload that was
 * passed in, mutated in place. This struct only reports whole-run
 * aggregates that aren't naturally attached to any single process:
 *
 *   total_ticks   Number of ticks simulated (the last tick's index + 1).
 *                 Equivalently: the tick at which the final process
 *                 completed, plus one.
 *   idle_ticks    Number of those ticks where the CPU had nothing
 *                 runnable (used to validate idle-period behavior, e.g.
 *                 Phase 3's TEST5).
 */
typedef struct {
    int total_ticks;
    int idle_ticks;
} rr_run_result_t;

/*
 * scheduler_rr_run — simulate plain Round Robin over `workload`.
 *
 * Mutates every process_t in workload->items in place: wait_time
 * accumulates as described in process.h, completion_tick is set once
 * each process finishes. remaining_burst reaches 0 for every process by
 * the time this function returns (guaranteed, since the loop only exits
 * when completed_count == workload->count).
 *
 * `debug`: if nonzero, prints one line per tick (Tick | Running PID |
 * Group | Remaining | QuantumUsed) to stdout. Off by default — this is
 * NOT required for normal experiment runs, only for manually verifying
 * a specific small trace's schedule.
 *
 * workload->items MUST already be sorted by (arrival_tick, pid) — this
 * is guaranteed by trace_loader_load(), and is a precondition of this
 * function; it does not re-sort defensively (that would hide a caller
 * bug rather than surface it).
 */
rr_run_result_t scheduler_rr_run(workload_t *workload, int debug);

#endif /* AMFQF_SCHEDULER_RR_H */
