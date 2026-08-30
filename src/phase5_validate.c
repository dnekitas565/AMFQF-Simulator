#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scheduler_amfqf.h"

static int passed;
static int failed;

typedef struct { int pid, arrival, burst; const char *group; } row_t;

#define CHECK(condition, message) do { \
    if (condition) { printf("PASS: %s\n", message); passed++; } \
    else { printf("FAIL: %s\n", message); failed++; } \
} while (0)

static void write_trace(const char *name, const row_t *rows, int count) {
    FILE *file = fopen(name, "w");
    if (file == NULL) { perror(name); exit(1); }
    fprintf(file, "pid,arrival_tick,burst,group\n");
    for (int i = 0; i < count; i++)
        fprintf(file, "%d,%d,%d,%s\n", rows[i].pid, rows[i].arrival,
                rows[i].burst, rows[i].group);
    fclose(file);
}

static process_t *pid(workload_t *workload, int wanted) {
    for (int i = 0; i < workload->count; i++)
        if (workload->items[i]->pid == wanted) return workload->items[i];
    return NULL;
}

static amfqf_run_result_t run_rows(const char *name, const row_t *rows, int count,
                                   workload_t *workload) {
    write_trace(name, rows, count);
    CHECK(trace_loader_load(name, workload) == 0, "fixture trace loads");
    return scheduler_amfqf_run(workload, 0);
}

/* Completion is time immediately after the final execution tick: a three-tick
 * burst beginning at tick zero therefore completes at timestamp three. */
static void test_empty_and_single(void) {
    workload_t empty;
    FILE *file = fopen("test_tmp/amfqf_empty.csv", "w");
    fprintf(file, "pid,arrival_tick,burst,group\n"); fclose(file);
    CHECK(trace_loader_load("test_tmp/amfqf_empty.csv", &empty) == 0, "empty trace loads");
    amfqf_run_result_t result = scheduler_amfqf_run(&empty, 0);
    CHECK(result.total_ticks == 0 && result.completed_count == 0, "empty workload returns immediately");
    trace_loader_free(&empty);

    row_t rows[] = {{1, 0, 3, "SYSTEM"}}; workload_t w;
    result = run_rows("test_tmp/amfqf_single.csv", rows, 1, &w);
    CHECK(pid(&w, 1)->completion_tick == 3 && pid(&w, 1)->wait_time == 0,
          "single Q0 process executes for exactly three ticks");
    CHECK(result.total_ticks == 3 && result.idle_ticks == 0 && result.completed_count == 1,
          "single-process result accounting is correct");
    trace_loader_free(&w);
}

/* System and Interactive begin in Q0, Batch begins in Q1.  Q0 must finish
 * first; within Q0 the temporary lowest-group-ID rule selects System first. */
static void test_groups_arrivals_and_initial_placement(void) {
    row_t rows[] = {{3,0,1,"BATCH"},{2,0,1,"INTERACTIVE"},{1,0,1,"SYSTEM"}};
    workload_t w; amfqf_run_result_t result = run_rows("test_tmp/amfqf_groups.csv", rows, 3, &w);
    CHECK(pid(&w,1)->completion_tick == 1 && pid(&w,2)->completion_tick == 2 &&
          pid(&w,3)->completion_tick == 3, "three groups use Q0 priority and deterministic group order");
    CHECK(result.completed_count == 3, "all three groups receive service");
    CHECK(result.invariant_checks == result.total_ticks,
          "every executed tick preserves one-queue-or-running ownership");
    trace_loader_free(&w);

    row_t same[] = {{3,0,1,"SYSTEM"},{1,0,1,"SYSTEM"},{2,0,1,"SYSTEM"}};
    result = run_rows("test_tmp/amfqf_same_tick.csv", same, 3, &w);
    CHECK(pid(&w,1)->completion_tick == 1 && pid(&w,2)->completion_tick == 2 &&
          pid(&w,3)->completion_tick == 3, "same-tick same-group arrivals follow sorted PID FIFO order");
    trace_loader_free(&w);
}

/* A process is requeued at the tail after exhausting its level quantum. */
static void test_quantums_and_rr(void) {
    row_t q0[] = {{1,0,5,"SYSTEM"},{2,0,1,"SYSTEM"}}; workload_t w;
    run_rows("test_tmp/amfqf_q0.csv", q0, 2, &w);
    CHECK(pid(&w,2)->completion_tick == 5 && pid(&w,1)->completion_tick == 6,
          "Q0 quantum is four ticks and RR rotates to the second process");
    trace_loader_free(&w);

    row_t q1[] = {{1,0,9,"BATCH"},{2,0,1,"BATCH"}};
    run_rows("test_tmp/amfqf_q1.csv", q1, 2, &w);
    CHECK(pid(&w,2)->completion_tick == 9 && pid(&w,1)->completion_tick == 10,
          "Q1 quantum is eight ticks and demotes Q1 to Q2");
    trace_loader_free(&w);

    row_t q2[] = {{1,0,25,"BATCH"},{2,0,1,"BATCH"}};
    run_rows("test_tmp/amfqf_q2.csv", q2, 2, &w);
    CHECK(pid(&w,1)->completion_tick == 26 && pid(&w,2)->completion_tick == 9,
          "Q2 quantum is sixteen ticks and Q2 remains Q2 after expiry");
    trace_loader_free(&w);
}

/* The Batch process starts in Q1. An Interactive Q0 arrival at tick one must
 * preempt it, and the preempted Batch process must resume ahead of P2. */
static void test_preemption_and_front_insertion(void) {
    row_t rows[] = {{1,0,2,"BATCH"},{2,0,1,"BATCH"},{3,1,1,"INTERACTIVE"}};
    workload_t w; amfqf_run_result_t result = run_rows("test_tmp/amfqf_preempt.csv", rows, 3, &w);
    CHECK(result.preemptions == 1, "higher-priority arrival preempts at its tick boundary");
    CHECK(pid(&w,3)->completion_tick == 2, "higher-priority Interactive process runs immediately");
    CHECK(pid(&w,1)->completion_tick == 3 && pid(&w,2)->completion_tick == 4,
          "preempted Batch process resumes from the front with preserved quantum");
    trace_loader_free(&w);
}

/* Twenty ready ticks promote Q1 Batch work exactly once.  The long Q0 System
 * backlog ensures P1 cannot run before promotion; reset prevents a second
 * immediate promotion. */
static void test_ordinary_promotion(void) {
    row_t rows[7];
    rows[0] = (row_t){1,0,1,"BATCH"};
    for (int i = 1; i < 7; i++) rows[i] = (row_t){i + 1,0,4,"SYSTEM"};
    workload_t w; amfqf_run_result_t result = run_rows("test_tmp/amfqf_promotion.csv", rows, 7, &w);
    CHECK(result.ordinary_promotions == 1, "ordinary promotion occurs after exactly twenty ready ticks");
    CHECK(pid(&w,1)->wait_time == 24, "promoted process waited under Q0 work before receiving service");
    CHECK(result.completed_count == 7, "promotion does not lose or duplicate a process");
    trace_loader_free(&w);
}

static void test_idle_completion_and_rejection(void) {
    row_t later[] = {{1,5,1,"INTERACTIVE"}}; workload_t w;
    amfqf_run_result_t result = run_rows("test_tmp/amfqf_idle.csv", later, 1, &w);
    CHECK(result.idle_ticks == 5 && pid(&w,1)->completion_tick == 6,
          "CPU idles before arrival without changing ready-process accounting");
    CHECK(pid(&w,1)->next == NULL && pid(&w,1)->remaining_burst == 0,
          "completed process is detached and completed exactly once");
    trace_loader_free(&w);

    row_t invalid[] = {{1,0,1,"IDLE"}};
    write_trace("test_tmp/amfqf_idle_group.csv", invalid, 1);
    CHECK(trace_loader_load("test_tmp/amfqf_idle_group.csv", &w) == 0, "legacy IDLE fixture loads for rejection test");
    result = scheduler_amfqf_run(&w, 0);
    CHECK(result.rejected_input == 1 && pid(&w,1)->remaining_burst == 1,
          "AMFQF clearly rejects legacy schedulable IDLE input");
    trace_loader_free(&w);
}

int main(void) {
    printf("====================================\nAMFQF PHASE 5 VALIDATION\n====================================\n");
    test_empty_and_single();
    test_groups_arrivals_and_initial_placement();
    test_quantums_and_rr();
    test_preemption_and_front_insertion();
    test_ordinary_promotion();
    test_idle_completion_and_rejection();
    printf("====================================\nTests passed: %d\nTests failed: %d\nStatus: %s\n",
           passed, failed, failed == 0 ? "PASS" : "FAIL");
    return failed == 0 ? 0 : 1;
}