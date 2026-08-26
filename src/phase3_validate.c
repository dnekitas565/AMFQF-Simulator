/*
 * phase3_validate.c — Phase 3 checkpoint validation.
 *
 * Same role as phase1_validate.c: a scoped, deletable validation program,
 * not main.c (Phase 7's real harness doesn't exist yet).
 *
 * Every test builds a tiny CSV trace in /tmp (NOT under traces/ — these
 * are throwaway test fixtures, not real experiment workload data, so
 * they don't belong in the project's version-controlled traces/
 * directory), loads it through the real trace_loader, runs the real
 * scheduler_rr_run, and checks the resulting process_t fields against
 * hand-computed expected values documented in each test's comment block.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "process.h"
#include "group.h"
#include "trace_loader.h"
#include "scheduler_rr.h"

static int g_pass_count = 0;
static int g_fail_count = 0;

typedef struct {
    int pid;
    int arrival;
    int burst;
    const char *group;
} row_t;

static void write_temp_csv(const char *path, const row_t *rows, int n) {
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "test harness: could not create fixture %s\n", path);
        exit(1);
    }
    fprintf(f, "pid,arrival_tick,burst,group\n");
    for (int i = 0; i < n; i++) {
        fprintf(f, "%d,%d,%d,%s\n", rows[i].pid, rows[i].arrival, rows[i].burst, rows[i].group);
    }
    fclose(f);
}

static process_t *find_pid(workload_t *w, int pid) {
    for (int i = 0; i < w->count; i++) {
        if (w->items[i]->pid == pid) return w->items[i];
    }
    return NULL;
}

#define CHECK(cond, desc) do { \
    if (cond) { \
        printf("  [PASS] %s\n", desc); \
        g_pass_count++; \
    } else { \
        printf("  [FAIL] %s\n", desc); \
        g_fail_count++; \
    } \
} while (0)

/* ================================================================
 * TEST 1 — Single process, arrival=0, burst=3
 * Expected: runs ticks 0,1,2 (3 ticks total), completion_tick=2,
 * wait_time=0 (nothing else ever competes for the CPU), total_ticks=3,
 * idle_ticks=0.
 * ================================================================ */
static void test1(void) {
    printf("\n=== TEST 1: single process, burst=3 ===\n");
    row_t rows[] = { {1, 0, 3, "SYSTEM"} };
    write_temp_csv("test_tmp/amfqf_t1.csv", rows, 1);

    workload_t w;
    int rc = trace_loader_load("test_tmp/amfqf_t1.csv", &w);
    CHECK(rc == 0, "trace loads successfully");

    rr_run_result_t r = scheduler_rr_run(&w, 1 /* debug on for this one */);

    process_t *p1 = find_pid(&w, 1);
    CHECK(p1 != NULL, "P1 present in workload");
    CHECK(p1->completion_tick == 2, "P1 completion_tick == 2 (ran ticks 0,1,2)");
    CHECK(p1->wait_time == 0, "P1 wait_time == 0 (never contended for CPU)");
    CHECK(r.total_ticks == 3, "total_ticks == 3");
    CHECK(r.idle_ticks == 0, "idle_ticks == 0");

    trace_loader_free(&w);
}

/* ================================================================
 * TEST 2 — Two processes at tick 0, quantum=4
 * P1(pid1) burst=6, P2(pid2) burst=3
 * Hand trace (see chat explanation for full walk-through):
 *   t0-3: P1 runs (4 ticks, quantum expires, remaining=2, requeued)
 *   t4-6: P2 runs (3 ticks, completes exactly at t6)
 *   t7-8: P1 resumes (2 more ticks, completes at t8)
 * Expected: P1 completion_tick=8, wait_time=3 (waited during t4,5,6)
 *           P2 completion_tick=6, wait_time=4 (waited during t0,1,2,3)
 *           total_ticks=9, idle_ticks=0
 * ================================================================ */
static void test2(void) {
    printf("\n=== TEST 2: two processes at tick 0, quantum rotation ===\n");
    row_t rows[] = {
        {1, 0, 6, "BATCH"},
        {2, 0, 3, "INTERACTIVE"},
    };
    write_temp_csv("test_tmp/amfqf_t2.csv", rows, 2);

    workload_t w;
    int rc = trace_loader_load("test_tmp/amfqf_t2.csv", &w);
    CHECK(rc == 0, "trace loads successfully");

    rr_run_result_t r = scheduler_rr_run(&w, 1);

    process_t *p1 = find_pid(&w, 1);
    process_t *p2 = find_pid(&w, 2);
    CHECK(p1->completion_tick == 8, "P1 completion_tick == 8");
    CHECK(p1->wait_time == 3, "P1 wait_time == 3");
    CHECK(p2->completion_tick == 6, "P2 completion_tick == 6");
    CHECK(p2->wait_time == 4, "P2 wait_time == 4");
    CHECK(r.total_ticks == 9, "total_ticks == 9");
    CHECK(r.idle_ticks == 0, "idle_ticks == 0");

    trace_loader_free(&w);
}

/* ================================================================
 * TEST 3 — Process arriving after simulation starts
 * P1 arrives at tick 5, burst=3.
 * Expected: idle ticks 0-4 (5 idle ticks), runs ticks 5,6,7,
 * completion_tick=7, wait_time=0, total_ticks=8, idle_ticks=5.
 * ================================================================ */
static void test3(void) {
    printf("\n=== TEST 3: process arrives after simulation start ===\n");
    row_t rows[] = { {1, 5, 3, "SYSTEM"} };
    write_temp_csv("test_tmp/amfqf_t3.csv", rows, 1);

    workload_t w;
    trace_loader_load("test_tmp/amfqf_t3.csv", &w);
    rr_run_result_t r = scheduler_rr_run(&w, 1);

    process_t *p1 = find_pid(&w, 1);
    CHECK(p1->completion_tick == 7, "P1 completion_tick == 7 (not scheduled before arrival)");
    CHECK(p1->wait_time == 0, "P1 wait_time == 0");
    CHECK(r.idle_ticks == 5, "idle_ticks == 5 (ticks 0-4 before arrival)");
    CHECK(r.total_ticks == 8, "total_ticks == 8");

    trace_loader_free(&w);
}

/* ================================================================
 * TEST 4 — Three processes, different bursts, quantum=4
 * P1(pid1,burst2) P2(pid2,burst5) P3(pid3,burst4), all arrive at tick 0.
 * Full hand trace (see chat explanation):
 *   P1: completion_tick=1,  wait=0
 *   P2: completion_tick=10, wait=6
 *   P3: completion_tick=9,  wait=6
 *   total_ticks=11, idle_ticks=0
 * ================================================================ */
static void test4(void) {
    printf("\n=== TEST 4: three processes, mixed bursts, rotation + re-rotation ===\n");
    row_t rows[] = {
        {1, 0, 2, "SYSTEM"},
        {2, 0, 5, "BATCH"},
        {3, 0, 4, "INTERACTIVE"},
    };
    write_temp_csv("test_tmp/amfqf_t4.csv", rows, 3);

    workload_t w;
    trace_loader_load("test_tmp/amfqf_t4.csv", &w);
    rr_run_result_t r = scheduler_rr_run(&w, 1);

    process_t *p1 = find_pid(&w, 1);
    process_t *p2 = find_pid(&w, 2);
    process_t *p3 = find_pid(&w, 3);

    CHECK(p1->completion_tick == 1, "P1 completion_tick == 1");
    CHECK(p1->wait_time == 0, "P1 wait_time == 0");
    CHECK(p3->completion_tick == 9, "P3 completion_tick == 9");
    CHECK(p3->wait_time == 6, "P3 wait_time == 6");
    CHECK(p2->completion_tick == 10, "P2 completion_tick == 10");
    CHECK(p2->wait_time == 6, "P2 wait_time == 6");
    CHECK(r.total_ticks == 11, "total_ticks == 11");
    CHECK(r.idle_ticks == 0, "idle_ticks == 0");

    trace_loader_free(&w);
}

/* ================================================================
 * TEST 5 — CPU idle period before first arrival
 * Single process arrives at tick 7, burst=3.
 * Expected: idle_ticks == 7 exactly, runs ticks 7,8,9, completion_tick=9,
 * total_ticks=10.
 * (Distinct from TEST 3: this specifically checks idle_ticks count
 * matches arrival_tick exactly, i.e. no off-by-one in the idle counter.)
 * ================================================================ */
static void test5(void) {
    printf("\n=== TEST 5: CPU idle period before first arrival ===\n");
    row_t rows[] = { {1, 7, 3, "IDLE"} };
    write_temp_csv("test_tmp/amfqf_t5.csv", rows, 1);

    workload_t w;
    trace_loader_load("test_tmp/amfqf_t5.csv", &w);
    rr_run_result_t r = scheduler_rr_run(&w, 0);

    process_t *p1 = find_pid(&w, 1);
    CHECK(r.idle_ticks == 7, "idle_ticks == 7 (exactly matches arrival_tick)");
    CHECK(p1->completion_tick == 9, "P1 completion_tick == 9");
    CHECK(r.total_ticks == 10, "total_ticks == 10");

    trace_loader_free(&w);
}

/* ================================================================
 * TEST 6 — Multiple processes arriving at the same tick, deterministic order
 * CSV rows deliberately written in pid 3,1,2 order (NOT sorted) to prove
 * trace_loader's (arrival_tick, pid) sort — not file order — determines
 * admission order. All arrive at tick 0, burst=2 each, quantum=4 (no
 * preemption since burst < quantum).
 * Expected admission/execution order: pid1, pid2, pid3 (ascending pid).
 *   P1: ticks 0,1   -> completion_tick=1, wait=0
 *   P2: ticks 2,3   -> completion_tick=3, wait=2 (waited during P1's run)
 *   P3: ticks 4,5   -> completion_tick=5, wait=4 (waited during P1+P2)
 * ================================================================ */
static void test6(void) {
    printf("\n=== TEST 6: same-tick arrivals, deterministic ordering ===\n");
    row_t rows[] = {
        {3, 0, 2, "BATCH"},        /* deliberately out of pid order in the file */
        {1, 0, 2, "SYSTEM"},
        {2, 0, 2, "INTERACTIVE"},
    };
    write_temp_csv("test_tmp/amfqf_t6.csv", rows, 3);

    workload_t w;
    trace_loader_load("test_tmp/amfqf_t6.csv", &w);
    rr_run_result_t r = scheduler_rr_run(&w, 1);

    process_t *p1 = find_pid(&w, 1);
    process_t *p2 = find_pid(&w, 2);
    process_t *p3 = find_pid(&w, 3);

    CHECK(p1->completion_tick == 1, "P1 (lowest pid) completion_tick == 1 (ran first)");
    CHECK(p1->wait_time == 0, "P1 wait_time == 0");
    CHECK(p2->completion_tick == 3, "P2 completion_tick == 3 (ran second)");
    CHECK(p2->wait_time == 2, "P2 wait_time == 2");
    CHECK(p3->completion_tick == 5, "P3 completion_tick == 5 (ran third, despite being first in the file)");
    CHECK(p3->wait_time == 4, "P3 wait_time == 4");
    CHECK(r.total_ticks == 6, "total_ticks == 6");

    trace_loader_free(&w);
}

/* ================================================================
 * TEST 7 (robustness) — empty trace (header only, no data rows)
 * Expected: loads successfully with count==0; scheduler returns
 * immediately with total_ticks==0, idle_ticks==0, no crash.
 * ================================================================ */
static void test7_empty_trace(void) {
    printf("\n=== TEST 7 (robustness): empty trace ===\n");
    FILE *f = fopen("test_tmp/amfqf_t7.csv", "w");
    fprintf(f, "pid,arrival_tick,burst,group\n");
    fclose(f);

    workload_t w;
    int rc = trace_loader_load("test_tmp/amfqf_t7.csv", &w);
    CHECK(rc == 0, "empty trace (header only) loads successfully");
    CHECK(w.count == 0, "workload count == 0");

    rr_run_result_t r = scheduler_rr_run(&w, 0);
    CHECK(r.total_ticks == 0, "total_ticks == 0 for empty workload");
    CHECK(r.idle_ticks == 0, "idle_ticks == 0 for empty workload");

    trace_loader_free(&w);
}

/* ================================================================
 * TEST 8 (robustness) — malformed CSV rows
 * Several distinct malformed rows, each in its own file, each expected
 * to make trace_loader_load return -1 and leave the workload empty.
 * ================================================================ */
static void test8_malformed_csv(void) {
    printf("\n=== TEST 8 (robustness): malformed CSV rows ===\n");

    struct { const char *content; const char *desc; } cases[] = {
        {"pid,arrival_tick,burst,group\nnotanumber,0,3,SYSTEM\n", "non-numeric pid"},
        {"pid,arrival_tick,burst,group\n1,0,3\n",                  "missing group field"},
        {"pid,arrival_tick,burst,group\n1,0,0,SYSTEM\n",           "burst == 0 (must be > 0)"},
        {"pid,arrival_tick,burst,group\n1,-1,3,SYSTEM\n",          "negative arrival_tick"},
        {"pid,arrival_tick,burst,group\n1,0,3,NOTAGROUP\n",        "unrecognized group name"},
        {"pid,arrival_tick,burst,group\n0,0,3,SYSTEM\n",           "pid == 0 (must be > 0)"},
    };
    int n = (int)(sizeof(cases) / sizeof(cases[0]));

    for (int i = 0; i < n; i++) {
        char path[64];
        snprintf(path, sizeof(path), "test_tmp/amfqf_t8_%d.csv", i);
        FILE *f = fopen(path, "w");
        fputs(cases[i].content, f);
        fclose(f);

        workload_t w;
        int rc = trace_loader_load(path, &w);
        char desc[128];
        snprintf(desc, sizeof(desc), "rejects malformed row: %s", cases[i].desc);
        CHECK(rc == -1, desc);
        CHECK(w.items == NULL && w.count == 0, "workload left empty after rejected load");
        /* trace_loader_free is safe to call even though load failed
           (w.items is NULL, w.count is 0) — verifies no double-free risk */
        trace_loader_free(&w);
    }
}

/* ================================================================
 * TEST 9 (edge case) — burst exactly equal to quantum
 * Single process, burst=4, quantum=4: completion and quantum-expiry
 * land on the exact same tick. Must retire, not rotate.
 * Expected: completion_tick=3, wait_time=0, total_ticks=4, idle_ticks=0.
 * ================================================================ */
static void test9_burst_equals_quantum(void) {
    printf("\n=== TEST 9 (edge case): burst exactly equals quantum ===\n");
    row_t rows[] = { {1, 0, 4, "BATCH"} };
    write_temp_csv("test_tmp/amfqf_t9.csv", rows, 1);

    workload_t w;
    trace_loader_load("test_tmp/amfqf_t9.csv", &w);
    rr_run_result_t r = scheduler_rr_run(&w, 1);

    process_t *p1 = find_pid(&w, 1);
    CHECK(p1->completion_tick == 3, "completion_tick == 3 (retired, not rotated)");
    CHECK(p1->wait_time == 0, "wait_time == 0");
    CHECK(r.total_ticks == 4, "total_ticks == 4");
    CHECK(r.idle_ticks == 0, "idle_ticks == 0");

    trace_loader_free(&w);
}

/* ================================================================
 * SMOKE TEST — run RR against the three real Phase 2 traces
 * Not a hand-verified correctness test (traces are too large / random
 * for manual computation), but confirms: loads without error, every
 * process reaches remaining_burst==0, completed_count matches trace
 * size, no crash under the real generated workloads.
 * ================================================================ */
static void smoke_test_real_traces(void) {
    const char *paths[] = {
        "traces/balanced.csv",
        "traces/batch_heavy.csv",
        "traces/bursty_system.csv",
    };
    printf("\n=== SMOKE TEST: real Phase 2 traces ===\n");
    for (int i = 0; i < 3; i++) {
        workload_t w;
        int rc = trace_loader_load(paths[i], &w);
        char desc[128];
        snprintf(desc, sizeof(desc), "%s loads successfully", paths[i]);
        CHECK(rc == 0, desc);
        if (rc != 0) continue;

        rr_run_result_t r = scheduler_rr_run(&w, 0);

        int all_complete = 1;
        long total_wait = 0;
        for (int j = 0; j < w.count; j++) {
            if (w.items[j]->remaining_burst != 0) all_complete = 0;
            if (w.items[j]->completion_tick < 0) all_complete = 0;
            total_wait += w.items[j]->wait_time;
        }
        printf("  %s: %d processes, total_ticks=%d, idle_ticks=%d, sum(wait_time)=%ld\n",
               paths[i], w.count, r.total_ticks, r.idle_ticks, total_wait);

        snprintf(desc, sizeof(desc), "%s: every process completed (remaining_burst==0, completion_tick set)", paths[i]);
        CHECK(all_complete, desc);

        trace_loader_free(&w);
    }
}

int main(void) {
    printf("=== PHASE 3 VALIDATION ===\n");

    test1();
    test2();
    test3();
    test4();
    test5();
    test6();
    test7_empty_trace();
    test8_malformed_csv();
    test9_burst_equals_quantum();
    smoke_test_real_traces();

    printf("\n=== PHASE 3 VALIDATION SUMMARY ===\n");
    printf("PASS: %d\n", g_pass_count);
    printf("FAIL: %d\n", g_fail_count);
    printf("=== %s ===\n", g_fail_count == 0 ? "ALL CHECKS PASSED" : "FAILURES PRESENT");

    return g_fail_count == 0 ? 0 : 1;
}
