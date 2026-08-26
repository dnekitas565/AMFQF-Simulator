#include "scheduler_rr.h"
#include "ready_queue.h"
#include <stdio.h>

/* Local, private debug-print helper. Deliberately NOT added to group.h —
   Phase 1/2 files are not being modified for this. A ~10-line duplicate
   switch statement here is cheaper than touching shared headers for a
   debug-only convenience used in exactly one file. */
static const char *group_short_name(group_id_t g) {
    switch (g) {
        case GROUP_SYSTEM:      return "SYSTEM";
        case GROUP_INTERACTIVE: return "INTERACTIVE";
        case GROUP_BATCH:       return "BATCH";
        case GROUP_IDLE:        return "IDLE";
        default:                return "UNKNOWN";
    }
}

rr_run_result_t scheduler_rr_run(workload_t *workload, int debug) {
    rr_run_result_t result;
    result.total_ticks = 0;
    result.idle_ticks = 0;

    if (workload->count == 0) {
        return result; /* empty trace: nothing to simulate, not an error */
    }

    ready_queue_t rq;
    rq_init(&rq);

    int next_unadmitted = 0;   /* index into workload->items: everything
                                   before this index has already been
                                   enqueued at least once */
    int completed_count = 0;
    process_t *current = NULL; /* process presently occupying the CPU,
                                   or NULL if the CPU is idle/needs a new
                                   dispatch decision */
    int quantum_used = 0;      /* ticks `current` has run since its last
                                   dispatch (reset to 0 every time a NEW
                                   process is dispatched, NOT reset when
                                   the same process merely continues) */
    int tick = 0;

    if (debug) {
        printf("Tick | Running PID | Group       | Remaining | QuantumUsed\n");
    }

    while (completed_count < workload->count) {

        /* --- Step: admission ---
           Everything whose arrival_tick <= tick, in the array's sorted
           (arrival_tick, pid) order, is enqueued now. Because the array
           is pre-sorted, this is a simple forward pointer sweep: each
           process is examined and admitted exactly once across the
           entire run (amortized O(1) per tick, O(n) total). */
        while (next_unadmitted < workload->count &&
               workload->items[next_unadmitted]->arrival_tick <= tick) {
            rq_enqueue(&rq, workload->items[next_unadmitted]);
            next_unadmitted++;
        }

        /* --- Step: dispatch (only when nobody is currently running) ---
           If `current` is already set (it survived from the previous
           tick because its quantum hadn't expired and it hadn't
           finished), this is skipped entirely — it just keeps running. */
        if (current == NULL) {
            current = rq_dequeue(&rq);
            quantum_used = 0;
        }

        if (current == NULL) {
            /* Still nothing to run after attempting dispatch. This is
               only ever legitimate while some process hasn't arrived
               yet. If everything has already been admitted and we still
               have nothing to run despite not everyone being complete,
               that is a scheduler bug, not a real idle period — fail
               loudly instead of spinning forever. */
            if (next_unadmitted >= workload->count) {
                fprintf(stderr,
                        "scheduler_rr: INTERNAL ERROR at tick %d — no process "
                        "running, ready queue empty, all %d processes already "
                        "admitted, but only %d/%d completed. Aborting to avoid "
                        "an infinite loop.\n",
                        tick, workload->count, completed_count, workload->count);
                result.total_ticks = tick;
                return result;
            }

            result.idle_ticks++;
            if (debug) {
                printf("%4d | %-11s | %-11s | %-9s | %-11s\n", tick, "-", "-", "-", "-");
            }
            tick++;
            result.total_ticks = tick;
            continue;
        }

        /* --- Step: wait-time accounting ---
           `current` was already removed from `rq` (either just now, or
           on a prior tick), so this loop can structurally never include
           it — no double counting, and no risk of crediting wait time to
           the very process that's about to run this tick. Every OTHER
           process presently linked into the queue is, by construction,
           admitted (arrival_tick <= tick already true) and not yet
           complete (completed processes are never re-enqueued), so this
           is exactly "runnable/ready but not dispatched this tick." */
        for (process_t *p = rq.head; p != NULL; p = p->next) {
            p->wait_time++;
        }

        if (debug) {
            printf("%4d | %-11d | %-11s | %-9d | %-11d\n",
                   tick, current->pid, group_short_name(current->group),
                   current->remaining_burst, quantum_used);
        }

        /* --- Step: execute exactly one tick --- */
        current->remaining_burst--;
        quantum_used++;

        /* --- Step: completion / quantum-expiry, in that priority order ---
           Completion is checked FIRST. If a process's last unit of burst
           and its quantum expiry land on the exact same tick (burst
           length is an exact multiple of RR_QUANTUM), it must be retired
           — not rotated back into the queue, which would incorrectly
           schedule a finished process again. */
        if (current->remaining_burst == 0) {
            current->completion_tick = tick; /* convention: the tick
                during which the process's FINAL unit of burst executed —
                i.e. completion_tick is inclusive of the tick it finished
                on, not "one past" it */
            completed_count++;
            current = NULL;
        } else if (quantum_used == RR_QUANTUM) {
            rq_enqueue(&rq, current); /* rotate to the back of the line */
            current = NULL;
        }
        /* else: mid-quantum, work remaining — `current` stays set, and
           next tick's dispatch step is skipped, so it runs uninterrupted */

        tick++;
        result.total_ticks = tick;
    }

    return result;
}
