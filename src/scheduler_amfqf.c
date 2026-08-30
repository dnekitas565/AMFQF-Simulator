#include "scheduler_amfqf.h"
#include "ready_queue.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

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
    int first_dispatch_tick;
    int protected_service; /* Reserved for a later milestone. */
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
    int ordinary_promotions;
    int preemptions;
    int invariant_checks;
} amfqf_runtime_state_t;

static int is_schedulable_group(group_id_t group) {
    return group == GROUP_SYSTEM ||
           group == GROUP_INTERACTIVE ||
           group == GROUP_BATCH;
}

static const char *group_name(group_id_t group) {
    switch (group) {
        case GROUP_SYSTEM:
            return "SYSTEM";
        case GROUP_INTERACTIVE:
            return "INTERACTIVE";
        case GROUP_BATCH:
            return "BATCH";
        case GROUP_IDLE:
            return "IDLE";
        default:
            return "INVALID";
    }
}

static int process_index(const workload_t *workload,
                         const process_t *process) {
    for (int i = 0; i < workload->count; i++) {
        if (workload->items[i] == process) {
            return i;
        }
    }

    return -1;
}

static amfqf_process_state_t *state_for(
    amfqf_runtime_state_t *state,
    const workload_t *workload,
    const process_t *process) {

    int index = process_index(workload, process);

    assert(index >= 0);

    return &state->process[index];
}

static void state_init(amfqf_runtime_state_t *state,
                       const workload_t *workload) {

    state->process = calloc(
        (size_t)workload->count,
        sizeof(*state->process)
    );

    for (int level = 0; level < AMFQF_QUEUE_LEVELS; level++) {
        for (int group = 0;
             group < AMFQF_RUNNABLE_GROUPS;
             group++) {

            rq_init(&state->ready[level][group]);
        }
    }

    for (int group = 0;
         group < AMFQF_RUNNABLE_GROUPS;
         group++) {

        state->group[group].target_budget =
            AMFQF_TARGET_SHARE[group] * AMFQF_WINDOW_TICKS;
    }

    for (int i = 0; i < workload->count; i++) {
        state->process[i].queue_level = -1;
        state->process[i].first_dispatch_tick = -1;
        state->process[i].status = AMFQF_PROCESS_NEW;
    }
}

static void enqueue_tail(amfqf_runtime_state_t *state,
                         const workload_t *workload,
                         process_t *process,
                         int level) {

    amfqf_process_state_t *pstate =
        state_for(state, workload, process);

    assert(level >= 0 &&
           level < AMFQF_QUEUE_LEVELS);

    assert(is_schedulable_group(process->group));

    assert(pstate->status != AMFQF_PROCESS_READY &&
           pstate->status != AMFQF_PROCESS_COMPLETE);

    pstate->queue_level = level;
    pstate->status = AMFQF_PROCESS_READY;

    rq_enqueue(
        &state->ready[level][process->group],
        process
    );
}

static void enqueue_front(amfqf_runtime_state_t *state,
                          const workload_t *workload,
                          process_t *process) {

    amfqf_process_state_t *pstate =
        state_for(state, workload, process);

    assert(pstate->status == AMFQF_PROCESS_RUNNING);

    pstate->status = AMFQF_PROCESS_READY;

    rq_enqueue_front(
        &state->ready[pstate->queue_level][process->group],
        process
    );
}

/*
 * Moving an arbitrary aged process in the existing singly linked FIFO
 * costs O(queue length). Normal enqueue, dequeue, and preemption remain O(1).
 */
static void unlink_ready_process(amfqf_runtime_state_t *state,
                                  const workload_t *workload,
                                  process_t *process) {

    amfqf_process_state_t *pstate =
        state_for(state, workload, process);

    ready_queue_t *queue =
        &state->ready[pstate->queue_level][process->group];

    process_t *previous = NULL;
    process_t *current;

    assert(pstate->status == AMFQF_PROCESS_READY);

    for (current = queue->head;
         current != NULL;
         current = current->next) {

        if (current == process) {
            break;
        }

        previous = current;
    }

    assert(current == process);

    if (previous == NULL) {
        queue->head = current->next;
    } else {
        previous->next = current->next;
    }

    if (queue->tail == current) {
        queue->tail = previous;
    }

    current->next = NULL;
    queue->count--;

    assert(queue->count >= 0);
}

static void admit_arrivals(amfqf_runtime_state_t *state,
                           workload_t *workload) {

    while (state->next_unadmitted < workload->count &&
           workload->items[state->next_unadmitted]->arrival_tick
               <= state->tick) {

        process_t *process =
            workload->items[state->next_unadmitted++];

        int level =
            process->group == GROUP_BATCH ? 1 : 0;

        assert(is_schedulable_group(process->group));

        state_for(state, workload, process)->quantum_left =
            AMFQF_QUANTUM[level];

        enqueue_tail(
            state,
            workload,
            process,
            level
        );
    }
}

static void apply_ordinary_promotions(amfqf_runtime_state_t *state,
                                       workload_t *workload) {

    for (int i = 0; i < workload->count; i++) {

        process_t *process = workload->items[i];

        amfqf_process_state_t *pstate =
            &state->process[i];

        if (pstate->status == AMFQF_PROCESS_READY &&
            pstate->ready_wait >= AMFQF_PROMOTION_TICKS &&
            pstate->queue_level > 0) {

            int new_level =
                pstate->queue_level - 1;

            unlink_ready_process(
                state,
                workload,
                process
            );

            /*
             * Detached transitional state.
             */
            pstate->status = AMFQF_PROCESS_RUNNING;

            /*
             * Reset aging counter after ordinary promotion.
             */
            pstate->ready_wait = 0;

            pstate->quantum_left =
                AMFQF_QUANTUM[new_level];

            enqueue_tail(
                state,
                workload,
                process,
                new_level
            );

            state->ordinary_promotions++;
        }
    }
}

static int highest_nonempty_level(
    const amfqf_runtime_state_t *state) {

    for (int level = 0;
         level < AMFQF_QUEUE_LEVELS;
         level++) {

        for (int group = 0;
             group < AMFQF_RUNNABLE_GROUPS;
             group++) {

            if (!rq_is_empty(
                    &state->ready[level][group])) {

                return level;
            }
        }
    }

    return -1;
}

/*
 * Temporary Milestone-2 rule:
 * lowest group ID wins inside one MLFQ level.
 *
 * Later budget-ratio selection replaces only this helper.
 */
static int temporary_group_winner(
    const amfqf_runtime_state_t *state,
    int level) {

    for (int group = 0;
         group < AMFQF_RUNNABLE_GROUPS;
         group++) {

        if (!rq_is_empty(
                &state->ready[level][group])) {

            return group;
        }
    }

    return -1;
}

static void preempt_if_higher_ready(
    amfqf_runtime_state_t *state,
    workload_t *workload) {

    int highest;
    amfqf_process_state_t *running_state;

    if (state->running == NULL) {
        return;
    }

    running_state =
        state_for(state, workload, state->running);

    highest = highest_nonempty_level(state);

    if (highest >= 0 &&
        highest < running_state->queue_level) {

        process_t *preempted = state->running;

        state->running = NULL;

        enqueue_front(
            state,
            workload,
            preempted
        );

        state->preemptions++;
    }
}

static process_t *dispatch_if_needed(
    amfqf_runtime_state_t *state,
    workload_t *workload) {

    int level;
    int group;
    process_t *process;
    amfqf_process_state_t *pstate;

    if (state->running != NULL) {
        return state->running;
    }

    level = highest_nonempty_level(state);

    if (level < 0) {
        return NULL;
    }

    group = temporary_group_winner(
        state,
        level
    );

    process =
        rq_dequeue(&state->ready[level][group]);

    pstate =
        state_for(state, workload, process);

    assert(
        pstate->status == AMFQF_PROCESS_READY &&
        pstate->queue_level == level
    );

    pstate->status =
        AMFQF_PROCESS_RUNNING;

    if (pstate->first_dispatch_tick < 0) {
        pstate->first_dispatch_tick =
            state->tick;
    }

    state->running = process;

    return process;
}

static void increment_ready_waits(
    amfqf_runtime_state_t *state,
    workload_t *workload) {

    for (int i = 0;
         i < workload->count;
         i++) {

        if (state->process[i].status ==
            AMFQF_PROCESS_READY) {

            workload->items[i]->wait_time++;

            state->process[i].ready_wait++;
        }
    }
}

static void finish_or_requeue(
    amfqf_runtime_state_t *state,
    workload_t *workload,
    process_t *process) {

    amfqf_process_state_t *pstate =
        state_for(state, workload, process);

    assert(
        pstate->status ==
            AMFQF_PROCESS_RUNNING &&
        process->remaining_burst >= 0
    );

    if (process->remaining_burst == 0) {

        /*
         * completion_tick represents the time immediately
         * after the final execution tick.
         */
        process->completion_tick =
            state->tick + 1;

        pstate->status =
            AMFQF_PROCESS_COMPLETE;

        state->completed_count++;
        state->running = NULL;

    } else if (pstate->quantum_left == 0) {

        int new_level =
            pstate->queue_level <
                AMFQF_QUEUE_LEVELS - 1
                ? pstate->queue_level + 1
                : pstate->queue_level;

        pstate->quantum_left =
            AMFQF_QUANTUM[new_level];

        state->running = NULL;

        enqueue_tail(
            state,
            workload,
            process,
            new_level
        );
    }
}

static void debug_tick(
    const amfqf_runtime_state_t *state,
    const workload_t *workload,
    const process_t *process) {

    int index =
        process_index(workload, process);

    const amfqf_process_state_t *pstate =
        &state->process[index];

    printf(
        "%4d | PID=%-4d | %-11s | Q%d | "
        "remaining=%-4d | quantum_left=%-2d\n",
        state->tick,
        process->pid,
        group_name(process->group),
        pstate->queue_level,
        process->remaining_burst,
        pstate->quantum_left
    );
}

/*
 * Verify the ownership invariant after every executed tick:
 *
 * - a ready process is linked exactly once
 * - a running process is linked nowhere
 * - a completed process is linked nowhere
 */
static void assert_queue_invariants(
    amfqf_runtime_state_t *state,
    const workload_t *workload) {

    int *membership =
        calloc(
            (size_t)workload->count,
            sizeof(*membership)
        );

    assert(membership != NULL);

    for (int level = 0;
         level < AMFQF_QUEUE_LEVELS;
         level++) {

        for (int group = 0;
             group < AMFQF_RUNNABLE_GROUPS;
             group++) {

            int observed = 0;

            for (process_t *process =
                     state->ready[level][group].head;
                 process != NULL;
                 process = process->next) {

                int index =
                    process_index(workload, process);

                assert(
                    index >= 0 &&
                    state->process[index].status ==
                        AMFQF_PROCESS_READY
                );

                assert(
                    state->process[index].queue_level ==
                        level &&
                    process->group ==
                        (group_id_t)group
                );

                assert(
                    ++membership[index] == 1
                );

                observed++;
            }

            assert(
                observed ==
                    state->ready[level][group].count
            );
        }
    }

    for (int i = 0;
         i < workload->count;
         i++) {

        if (state->process[i].status ==
            AMFQF_PROCESS_READY) {

            assert(membership[i] == 1);

        } else {

            assert(membership[i] == 0);
        }

        if (state->process[i].status ==
            AMFQF_PROCESS_RUNNING) {

            assert(
                workload->items[i] ==
                    state->running
            );
        }

        if (state->process[i].status ==
            AMFQF_PROCESS_COMPLETE) {

            assert(
                workload->items[i]->remaining_burst == 0
            );
        }
    }

    free(membership);

    state->invariant_checks++;
}

amfqf_run_result_t scheduler_amfqf_run(
    workload_t *workload,
    int debug) {

    amfqf_run_result_t result =
        {0, 0, 0, 0, 0, 0, 0};

    amfqf_runtime_state_t state = {0};

    if (workload == NULL) {
        result.rejected_input = 1;
        return result;
    }

    /*
     * AMFQF supports only:
     * SYSTEM, INTERACTIVE, BATCH.
     *
     * Legacy GROUP_IDLE traces are rejected.
     */
    for (int i = 0;
         i < workload->count;
         i++) {

        if (!is_schedulable_group(
                workload->items[i]->group)) {

            fprintf(
                stderr,
                "scheduler_amfqf: rejects legacy IDLE "
                "process PID %d\n",
                workload->items[i]->pid
            );

            result.rejected_input = 1;
            return result;
        }
    }

    if (workload->count == 0) {
        return result;
    }

    state_init(&state, workload);

    if (state.process == NULL) {
        result.rejected_input = 1;
        return result;
    }

    if (debug) {
        printf(
            "Tick | Running PID | Group       | "
            "Queue | Remaining | Quantum Left\n"
        );
    }

    while (
        state.completed_count <
        workload->count) {

        process_t *process;

        admit_arrivals(
            &state,
            workload
        );

        apply_ordinary_promotions(
            &state,
            workload
        );

        preempt_if_higher_ready(
            &state,
            workload
        );

        process =
            dispatch_if_needed(
                &state,
                workload
            );

        if (process == NULL) {

            state.idle_ticks++;
            state.tick++;

            continue;
        }

        if (debug) {
            debug_tick(
                &state,
                workload,
                process
            );
        }

        /*
         * The running process is no longer waiting.
         */
        state_for(
            &state,
            workload,
            process
        )->ready_wait = 0;

        /*
         * Execute exactly one simulation tick.
         */
        process->remaining_burst--;

        state_for(
            &state,
            workload,
            process
        )->quantum_left--;

        /*
         * All other ready processes waited during this tick.
         */
        increment_ready_waits(
            &state,
            workload
        );

        finish_or_requeue(
            &state,
            workload,
            process
        );

        assert_queue_invariants(
            &state,
            workload
        );

        state.tick++;
    }

    result.total_ticks =
        state.tick;

    result.idle_ticks =
        state.idle_ticks;

    result.completed_count =
        state.completed_count;

    result.ordinary_promotions =
        state.ordinary_promotions;

    result.preemptions =
        state.preemptions;

    result.invariant_checks =
        state.invariant_checks;

    free(state.process);

    return result;
}