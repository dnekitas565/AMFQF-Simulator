#ifndef AMFQF_SCHEDULER_CFS_H
#define AMFQF_SCHEDULER_CFS_H

#include "trace_loader.h"

/*
 * ============================================================
 * DESIGN NOTE — read before touching this file (see chat explanation
 * for the full derivation). Recorded here too so it survives outside
 * the conversation.
 * ============================================================
 *
 * The AMFQF Implementation Guide's own Phase 4 text describes this
 * baseline as "weighted round robin": a flat FIFO queue where each
 * process's TURN LENGTH is scaled by weight. That mechanism is
 * starvation-free by construction (every admitted process still gets a
 * turn every pass through the queue, just a shorter one) — which
 * contradicts the same guide's stated goal for this baseline: to
 * demonstrate that a low-priority process "can be delayed arbitrarily
 * long under sustained high-weight load."
 *
 * This implementation instead tracks a genuine per-process virtual
 * runtime and always dispatches whichever admitted, incomplete process
 * currently has the MINIMUM vruntime (ties broken deterministically —
 * see scheduler_cfs.c). This is:
 *   - still a "simplified stand-in," not real CFS internals (no
 *     red-black tree; O(n) linear scan per tick, stated explicitly)
 *   - the mechanism that actually reproduces CFS's real selection
 *     invariant and its real starvation-under-flood failure mode
 *   - consistent with the vruntime-per-tick formula the guide itself
 *     cites as CFS's real mechanism ("more virtual runtime progress per
 *     real tick to higher-weight processes")
 *
 * If literal weighted-turn-length RR is preferred instead, that is a
 * different, smaller algorithm — flag it and this file will be revised.
 */

/*
 * CFS_REFERENCE_WEIGHT — the Linux kernel's nice-0 weight (1024). A
 * process at this weight accrues exactly 1.0 unit of vruntime per tick
 * it runs; anything with a higher weight accrues slower (stays "ahead"
 * — i.e. numerically behind — longer, so gets picked more), anything
 * lower accrues faster (falls "behind" faster, gets picked less).
 */
#define CFS_REFERENCE_WEIGHT 1024.0

/*
 * CFS_GROUP_WEIGHT — fixed per-group nice weight, indexed by group_id_t.
 * Values are the actual Linux kernel sched_prio_to_weight[] table
 * entries (kernel/sched/core.c) at the chosen nice level per group,
 * used here as documented historical constants rather than invented
 * numbers:
 *
 *   GROUP_SYSTEM       nice -5   weight 3121   (favored: system work)
 *   GROUP_INTERACTIVE  nice -2   weight 1586   (favored, less so)
 *   GROUP_BATCH        nice +5   weight  335   (deprioritized)
 *   GROUP_IDLE         nice +15  weight   36   (heavily deprioritized)
 *
 * These nice levels were chosen to match the ORIGINAL guide's own
 * qualitative ordering ("system=high weight, interactive=medium-high,
 * batch=medium-low, idle=low"), just realized as an actual, citable
 * table lookup rather than arbitrary numbers.
 */
static const double CFS_GROUP_WEIGHT[NUM_GROUPS] = {
    [GROUP_SYSTEM]      = 3121.0,
    [GROUP_INTERACTIVE] = 1586.0,
    [GROUP_BATCH]       =  335.0,
    [GROUP_IDLE]        =   36.0
};

/*
 * cfs_run_result_t — mirrors rr_run_result_t's design exactly (see
 * scheduler_rr.h) for the same reason: per-process results live on
 * process_t itself, this struct only reports whole-run aggregates.
 */
typedef struct {
    int total_ticks;
    int idle_ticks;
} cfs_run_result_t;

/*
 * scheduler_cfs_run — simulate the CFS-replay baseline over `workload`.
 *
 * Mutates every process_t in workload->items in place, using the exact
 * same wait_time/completion_tick conventions scheduler_rr.c already
 * established (see scheduler_cfs.c's per-function comments for the
 * precise per-tick ordering).
 *
 * `debug`: if nonzero, prints one line per tick (Tick | Running PID |
 * Group | Remaining | VRuntime) to stdout.
 *
 * Precondition (same as scheduler_rr_run): workload->items must already
 * be sorted by (arrival_tick, pid), as guaranteed by trace_loader_load().
 */
cfs_run_result_t scheduler_cfs_run(workload_t *workload, int debug);

#endif /* AMFQF_SCHEDULER_CFS_H */
