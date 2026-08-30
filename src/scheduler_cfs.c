#include "scheduler_cfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* Local, private debug-print helper — same rationale as scheduler_rr.c's
   group_short_name: Phase 1/2 files are not being touched for this. */
static const char *group_short_name(group_id_t g) {
    switch (g) {
        case GROUP_SYSTEM:      return "SYSTEM";
        case GROUP_INTERACTIVE: return "INTERACTIVE";
        case GROUP_BATCH:       return "BATCH";
        case GROUP_IDLE:        return "IDLE";
        default:                return "UNKNOWN";
    }
}

/*
 * Floating-point tie tolerance for vruntime comparison. Exact bit-for-bit
 * ties are actually the common case here (new arrivals copy the same
 * double, unexecuted processes stay at literal 0.0), so `==` would
 * mostly work — but relying on exact float equality is fragile practice
 * in general, so an epsilon comparison is used defensively. Any
 * difference smaller than this is treated as "not meaningfully less
 * than" for selection purposes, which is what lets the scan-order
 * tie-break below actually take effect instead of being defeated by
 * float noise.
 */
#define VR_EPSILON 1e-9

cfs_run_result_t scheduler_cfs_run(workload_t *workload, int debug) {
    cfs_run_result_t result;
    result.total_ticks = 0;
    result.idle_ticks = 0;

    if (workload->count == 0) {
        return result; /* empty trace: nothing to simulate, not an error */
    }

    int n = workload->count;

    /*
     * vruntime[i] corresponds to workload->items[i]. A plain parallel
     * array (not a wrapper struct) is sufficient because workload->items
     * never physically reorders during this run — unlike scheduler_rr.c,
     * which moves process_t pointers through a queue, CFS-replay never
     * needs to relink anything; it just re-scans the same fixed array
     * each tick. calloc zero-initializes every entry to 0.0, which is
     * the correct starting vruntime for whichever process(es) are
     * admitted before the very first scan has ever run.
     */
    double *vruntime = (double *)calloc((size_t)n, sizeof(double));
    if (vruntime == NULL) {
        fprintf(stderr, "scheduler_cfs: allocation failure for vruntime array (n=%d)\n", n);
        return result;
    }

    int next_unadmitted = 0;
    int completed_count = 0;
    double last_min_vruntime = 0.0; /* the floor new arrivals are placed
        at — see header comment / chat explanation. Starts at 0.0 before
        any tick's dispatch scan has run. */
    int tick = 0;

    if (debug) {
        printf("Tick | Running PID | Group       | Remaining | VRuntime(before)\n");
    }

    while (completed_count < n) {

        /* --- Step: admission ---
           Identical sweep technique to scheduler_rr.c: workload->items is
           pre-sorted by (arrival_tick, pid), so this is a simple forward
           index sweep, amortized O(1) per tick, O(n) total across the
           whole run. Each newly admitted process's vruntime is seeded to
           last_min_vruntime — the lowest vruntime observed among ready
           processes as of the most recent dispatch scan — so a new
           arrival neither unfairly jumps the queue (which vruntime=0
           always would, once other processes have run for a while) nor
           is unfairly punished by inheriting some arbitrary high value. */
        while (next_unadmitted < n &&
               workload->items[next_unadmitted]->arrival_tick <= tick) {
            vruntime[next_unadmitted] = last_min_vruntime;
            next_unadmitted++;
        }

        /* --- Step: dispatch selection ---
           Linear scan over every admitted, not-yet-completed process
           (indices [0, next_unadmitted), filtering out remaining_burst
           == 0). This is O(n) per tick, O(n) total per admitted process
           per tick it's eligible — worst case O(n) per tick regardless
           of n_ready, giving O(n * total_ticks) overall. Explicitly NOT
           a red-black tree: the guide's own Phase 4 text rules that out
           as unnecessary complexity for a userspace research simulator
           at this scale (traces here run 100-500 processes over at most
           a few thousand ticks; an O(n) scan is microseconds).

           Tie-break rule (documented per requirement): strict '<'
           comparison means a later-scanned process only replaces the
           current best if it is MEANINGFULLY smaller (beyond
           VR_EPSILON). Since workload->items is scanned in its existing
           (arrival_tick, pid) sorted order, any exact or near-tie is
           automatically resolved in favor of the earliest-arrived,
           lowest-pid candidate — the same tie-break convention already
           established by trace_loader.c and scheduler_rr.c, reused here
           for project-wide consistency rather than reinvented. */
        int best_idx = -1;
        for (int i = 0; i < next_unadmitted; i++) {
            if (workload->items[i]->remaining_burst <= 0) {
                continue; /* already completed — permanently excluded;
                             its frozen vruntime must never win a scan
                             again even if numerically low */
            }
            if (best_idx == -1 || vruntime[i] < vruntime[best_idx] - VR_EPSILON) {
                best_idx = i;
            }
        }

        if (best_idx == -1) {
            /* Nobody eligible. Legitimate only if some process hasn't
               arrived yet — otherwise this is a scheduler bug (mirrors
               the same defensive check scheduler_rr.c makes). */
            if (next_unadmitted >= n) {
                fprintf(stderr,
                        "scheduler_cfs: INTERNAL ERROR at tick %d — no process "
                        "eligible, all %d processes already admitted, but only "
                        "%d/%d completed. Aborting to avoid an infinite loop.\n",
                        tick, n, completed_count, n);
                result.total_ticks = tick;
                free(vruntime);
                return result;
            }
            result.idle_ticks++;
            if (debug) {
                printf("%4d | %-11s | %-11s | %-9s | %-16s\n", tick, "-", "-", "-", "-");
            }
            tick++;
            result.total_ticks = tick;
            continue;
        }

        process_t *selected = workload->items[best_idx];

        /* --- Step: wait-time accounting ---
           Same definition/timing as scheduler_rr.c: every OTHER
           admitted, not-yet-completed process gets +1 this tick. The
           selected process is structurally excluded since the loop
           explicitly skips i == best_idx. */
        for (int i = 0; i < next_unadmitted; i++) {
            if (i == best_idx) continue;
            if (workload->items[i]->remaining_burst <= 0) continue;
            workload->items[i]->wait_time++;
        }

        /* last_min_vruntime records the selected process's vruntime as
           it was BEFORE this tick's execution increment — i.e. the
           lowest vruntime that was actually present among ready
           processes at the start of this tick. Captured here,
           deliberately before the increment below. */
        last_min_vruntime = vruntime[best_idx];

        if (debug) {
            printf("%4d | %-11d | %-11s | %-9d | %-16.4f\n",
                   tick, selected->pid, group_short_name(selected->group),
                   selected->remaining_burst, vruntime[best_idx]);
        }

        /* --- Step: execute exactly one tick --- */
        selected->remaining_burst--;
        vruntime[best_idx] += CFS_REFERENCE_WEIGHT / CFS_GROUP_WEIGHT[selected->group];

        /* --- Step: completion check ---
           No separate "quantum expiry" branch exists in this design —
           dispatch is re-decided fresh every tick via the scan above, so
           there is no persisted "current process" state to rotate or
           requeue. Completion just means: stop counting this index in
           all future scans (enforced by the remaining_burst <= 0 filter
           at the top of the dispatch loop, not by any removal here). */
        if (selected->remaining_burst == 0) {
            selected->completion_tick = tick; /* same inclusive
                convention as scheduler_rr.c: the tick during which the
                FINAL unit of burst executed */
            completed_count++;
        }

        tick++;
        result.total_ticks = tick;
    }

    free(vruntime);
    return result;
}
