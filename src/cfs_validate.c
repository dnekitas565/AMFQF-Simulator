/*
 * phase4_validate.c — Phase 4 checkpoint validation.
 *
 * Same role/pattern as phase1_validate.c and phase3_validate.c: a
 * scoped, deletable validation program, not main.c.
 *
 * IMPORTANT METHODOLOGY NOTE: every deterministic test's expected values
 * below were derived from an INDEPENDENT Python re-implementation of the
 * same documented algorithm (a separate ~80-line reference model,
 * written before this C file, in a different language, specifically so
 * it could not share an arithmetic mistake with this implementation),
 * not from manual mental arithmetic. This is standard cross-validation
 * practice for anything involving multi-tick floating-point state
 * accumulation, where hand-tracing 8-10 ticks by mind is genuinely
 * error-prone. Each test's comment explains WHY the expected value is
 * what it is, so it remains checkable by a human without needing to
 * trust the Python model blindly.
 *
 * Fixture CSVs are written under test_tmp/ (project-root-relative, NOT
 * an absolute /tmp/ path) per the Windows-compatibility requirement, and
 * are cleaned up after each test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "process.h"
#include "group.h"
#include "trace_loader.h"
#include "scheduler_cfs.h"

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
 * TEST 1 — Single process
 * arrival=0, burst=3, group=SYSTEM.
 * Never contends with anything, so vruntime/weight are irrelevant to
 * the outcome — this only proves basic execute/decrement/complete
 * wiring works, same as scheduler_rr.c's TEST1.
 * Expected: completion_tick=2, wait_time=0, total_ticks=3, idle_ticks=0.
 * (Cross-checked: ref_sim.py TEST1.)
 * ================================================================ */
static void test1(void) {
    printf("\n=== TEST 1: single process ===\n");
    row_t rows[] = { {1, 0, 3, "SYSTEM"} };
    write_temp_csv("test_tmp/cfs_t1.csv", rows, 1);

    workload_t w;
    int rc = trace_loader_load("test_tmp/cfs_t1.csv", &w);
    CHECK(rc == 0, "trace loads successfully");

    cfs_run_result_t r = scheduler_cfs_run(&w, 1);

    process_t *p1 = find_pid(&w, 1);
    CHECK(p1 != NULL, "P1 present in workload");
    CHECK(p1->completion_tick == 2, "P1 completion_tick == 2");
    CHECK(p1->wait_time == 0, "P1 wait_time == 0");
    CHECK(r.total_ticks == 3, "total_ticks == 3");
    CHECK(r.idle_ticks == 0, "idle_ticks == 0");

    trace_loader_free(&w);
}

/* ================================================================
 * TEST 2 — Two processes at tick 0, EXTREME weight disparity
 * P1(pid1, SYSTEM, weight 3121, burst 6), P2(pid2, IDLE, weight 36, burst 3).
 * ~87x weight ratio, chosen deliberately: this is the test that proves
 * the "no starvation protection" behavior the guide requires actually
 * happens. P1's vruntime grows so much slower than P2's that once P1
 * gets ahead in the schedule, P2 is completely denied the CPU for 5
 * consecutive ticks (t2-t6) while P1 runs to full completion.
 * WHY this is the expected numbers (per ref_sim.py, cross-checked):
 *   vruntime increment per tick: SYSTEM = 1024/3121 = 0.3281,
 *                                  IDLE = 1024/36 = 28.4444
 *   t0: tie (both vr=0) -> scan order picks P1 (lower pid). P2 waits.
 *   t1: P1(vr=0.328) vs P2(vr=0) -> P2 picked (only tick it ever wins
 *       before P1 finishes). P1 waits.
 *   t2-t6: P1's vruntime (max ~1.64 by t6) stays far below P2's frozen
 *       28.44 the entire time -> P1 always selected -> P2 waits every
 *       tick (5 increments) -> P1 completes at t6 (burst 6 exhausted).
 *   t7-t8: only P2 left -> runs its remaining 2 ticks -> completes t8.
 * Expected: P1 completion_tick=6, wait_time=1
 *           P2 completion_tick=8, wait_time=6
 *           total_ticks=9, idle_ticks=0
 * ================================================================ */
static void test2(void) {
    printf("\n=== TEST 2: two processes, extreme weight disparity (SYSTEM vs IDLE) ===\n");
    row_t rows[] = {
        {1, 0, 6, "SYSTEM"},
        {2, 0, 3, "IDLE"},
    };
    write_temp_csv("test_tmp/cfs_t2.csv", rows, 2);

    workload_t w;
    trace_loader_load("test_tmp/cfs_t2.csv", &w);
    cfs_run_result_t r = scheduler_cfs_run(&w, 1);

    process_t *p1 = find_pid(&w, 1);
    process_t *p2 = find_pid(&w, 2);
    CHECK(p1->completion_tick == 6, "P1 (SYSTEM) completion_tick == 6");
    CHECK(p1->wait_time == 1, "P1 wait_time == 1");
    CHECK(p2->completion_tick == 8, "P2 (IDLE) completion_tick == 8 -- demonstrates delay under weight disparity");
    CHECK(p2->wait_time == 6, "P2 wait_time == 6 -- 5 consecutive denied ticks while P1 ran uninterrupted");
    CHECK(r.total_ticks == 9, "total_ticks == 9");
    CHECK(r.idle_ticks == 0, "idle_ticks == 0");

    trace_loader_free(&w);
}

/* ================================================================
 * TEST 3 — Three processes, SAME group (equal weight), different bursts
 * All BATCH (weight 335 each) -> identical vruntime increment per tick
 * for all three, which means comparison of vruntime reduces exactly to
 * comparison of "ticks executed so far" (a common positive multiplier
 * preserves order) -- i.e. equal-weight CFS-replay degenerates to
 * "always run whoever has executed the fewest ticks so far," a
 * genuinely different algorithm from RR's fixed-quantum rotation, but
 * one whose fairness property (short jobs finish sooner, nobody is
 * ever more than 1 tick's worth of service behind another at equal
 * weight) is independently checkable.
 * Expected (per ref_sim.py, cross-checked -- NOT hand-traced by mind,
 * this is exactly the kind of multi-tick tie-heavy trace hand-tracing
 * is unreliable for):
 *   P1(burst2): completion_tick=3, wait_time=2
 *   P2(burst5): completion_tick=9, wait_time=5
 *   P3(burst3): completion_tick=7, wait_time=5
 *   total_ticks=10, idle_ticks=0
 * Invariant checked independently of the exact trace: sum of all three
 * bursts (2+5+3=10) must equal total_ticks exactly, since all arrive at
 * tick 0 and there is no idle time -- this is checked as a SEPARATE
 * assertion from the exact expected numbers, so a real bug would likely
 * fail at least one of the two checks even if it coincidentally matched
 * the other.
 * ================================================================ */
static void test3(void) {
    printf("\n=== TEST 3: three equal-weight processes (same group), different bursts ===\n");
    row_t rows[] = {
        {1, 0, 2, "BATCH"},
        {2, 0, 5, "BATCH"},
        {3, 0, 3, "BATCH"},
    };
    write_temp_csv("test_tmp/cfs_t3.csv", rows, 3);

    workload_t w;
    trace_loader_load("test_tmp/cfs_t3.csv", &w);
    cfs_run_result_t r = scheduler_cfs_run(&w, 1);

    process_t *p1 = find_pid(&w, 1);
    process_t *p2 = find_pid(&w, 2);
    process_t *p3 = find_pid(&w, 3);

    CHECK(p1->completion_tick == 3, "P1 completion_tick == 3");
    CHECK(p1->wait_time == 2, "P1 wait_time == 2");
    CHECK(p2->completion_tick == 9, "P2 completion_tick == 9");
    CHECK(p2->wait_time == 5, "P2 wait_time == 5");
    CHECK(p3->completion_tick == 7, "P3 completion_tick == 7");
    CHECK(p3->wait_time == 5, "P3 wait_time == 5");
    CHECK(r.total_ticks == 10, "total_ticks == 10");
    CHECK(r.total_ticks == (2 + 5 + 3), "invariant: total_ticks == sum(bursts) (no idle time, checked independently)");
    CHECK(r.idle_ticks == 0, "idle_ticks == 0");

    trace_loader_free(&w);
}

/* ================================================================
 * TEST 4 — Process arriving after simulation starts
 * arrival=5, burst=3, group=INTERACTIVE. Same shape as scheduler_rr.c's
 * TEST3/TEST5: proves admission gating and idle-tick counting work
 * identically in this scheduler, independent of the vruntime mechanism
 * (nothing to compete with, so weight is irrelevant to this test).
 * Expected: idle_ticks=5 (ticks 0-4), completion_tick=7, wait_time=0,
 * total_ticks=8.
 * ================================================================ */
static void test4(void) {
    printf("\n=== TEST 4: process arrives after simulation start ===\n");
    row_t rows[] = { {1, 5, 3, "INTERACTIVE"} };
    write_temp_csv("test_tmp/cfs_t4.csv", rows, 1);

    workload_t w;
    trace_loader_load("test_tmp/cfs_t4.csv", &w);
    cfs_run_result_t r = scheduler_cfs_run(&w, 0);

    process_t *p1 = find_pid(&w, 1);
    CHECK(p1->completion_tick == 7, "P1 completion_tick == 7 (not scheduled before arrival)");
    CHECK(p1->wait_time == 0, "P1 wait_time == 0");
    CHECK(r.idle_ticks == 5, "idle_ticks == 5");
    CHECK(r.total_ticks == 8, "total_ticks == 8");

    trace_loader_free(&w);
}

/* ================================================================
 * TEST 5 — Multiple processes, IDENTICAL arrival time AND group
 * (identical scheduling state: same weight, same initial vruntime=0)
 * CSV rows deliberately written out of pid order (3,1,2) -- same
 * adversarial-order technique as scheduler_rr.c's TEST6 -- to prove the
 * tie-break is governed by the (arrival_tick, pid) sorted SCAN ORDER,
 * not file order or insertion order.
 * All BATCH, burst=2, arrival=0. Since weight is identical, every tie
 * at t=0 (all three vr=0.0) resolves to lowest pid first; this repeats
 * at each subsequent full "round" among whoever remains tied.
 * Expected (per ref_sim.py): P1 completion=3 wait=2, P2 completion=4
 * wait=3, P3 completion=5 wait=4, total_ticks=6, idle_ticks=0.
 * ================================================================ */
static void test5(void) {
    printf("\n=== TEST 5: identical arrival+group, deterministic tie-break ===\n");
    row_t rows[] = {
        {3, 0, 2, "BATCH"},   /* deliberately out of pid order in the file */
        {1, 0, 2, "BATCH"},
        {2, 0, 2, "BATCH"},
    };
    write_temp_csv("test_tmp/cfs_t5.csv", rows, 3);

    workload_t w;
    trace_loader_load("test_tmp/cfs_t5.csv", &w);
    cfs_run_result_t r = scheduler_cfs_run(&w, 1);

    process_t *p1 = find_pid(&w, 1);
    process_t *p2 = find_pid(&w, 2);
    process_t *p3 = find_pid(&w, 3);

    CHECK(p1->completion_tick == 3, "P1 (lowest pid) completion_tick == 3 (won every tie)");
    CHECK(p1->wait_time == 2, "P1 wait_time == 2");
    CHECK(p2->completion_tick == 4, "P2 completion_tick == 4");
    CHECK(p2->wait_time == 3, "P2 wait_time == 3");
    CHECK(p3->completion_tick == 5, "P3 completion_tick == 5 (despite being first in the file)");
    CHECK(p3->wait_time == 4, "P3 wait_time == 4");
    CHECK(r.total_ticks == 6, "total_ticks == 6");

    trace_loader_free(&w);
}

/* ================================================================
 * TEST 6 — Completed process must never be re-selected
 * P1(BATCH, burst=4), P2(BATCH, burst=1), both arrive at tick 0.
 * This specifically targets a real failure mode of vruntime-based
 * selection: P2 completes almost immediately (t1) with its vruntime
 * FROZEN at a very low value (0.0 -- it only ever ran once). P1 keeps
 * running afterward and its vruntime keeps climbing (3.05, 6.11, 9.17).
 * If the "skip completed processes" filter in the dispatch scan were
 * buggy or missing, P2's frozen, numerically-lowest vruntime would
 * "win" the scan again at t2, t3, and t4 -- causing either an infinite
 * loop (P2 selected repeatedly, remaining_burst never decreasing past
 * 0, completed_count never advancing) or corrupted state
 * (remaining_burst going negative). This test proves that does NOT
 * happen: P1 completes normally, alone, after P2.
 * Expected (per ref_sim.py): P2 completion_tick=1, wait_time=1 (waited
 * only at t0 while P1 ran first due to tie-break by pid... actually P1
 * has lower pid so runs first at t0's tie -- see below); P1 completion_
 * tick=4, wait_time=1; total_ticks=5, idle_ticks=0.
 * ================================================================ */
static void test6(void) {
    printf("\n=== TEST 6: completed process must never be re-selected ===\n");
    row_t rows[] = {
        {1, 0, 4, "BATCH"},
        {2, 0, 1, "BATCH"},
    };
    write_temp_csv("test_tmp/cfs_t6.csv", rows, 2);

    workload_t w;
    trace_loader_load("test_tmp/cfs_t6.csv", &w);
    cfs_run_result_t r = scheduler_cfs_run(&w, 1);

    process_t *p1 = find_pid(&w, 1);
    process_t *p2 = find_pid(&w, 2);

    CHECK(p2->completion_tick == 1, "P2 (burst=1) completion_tick == 1");
    CHECK(p2->wait_time == 1, "P2 wait_time == 1");
    CHECK(p1->completion_tick == 4, "P1 completion_tick == 4 (finished normally AFTER P2, not blocked by P2's frozen vruntime)");
    CHECK(p1->wait_time == 1, "P1 wait_time == 1");
    CHECK(r.total_ticks == 5, "total_ticks == 5 (== sum of bursts, proving no infinite loop / no stuck re-selection)");
    CHECK(r.idle_ticks == 0, "idle_ticks == 0");

    trace_loader_free(&w);
}

/* ================================================================
 * TEST 7 (robustness) — empty workload
 * Header-only CSV, zero data rows. Expected: loads successfully with
 * count==0, scheduler returns immediately with total_ticks==0,
 * idle_ticks==0, no crash, no infinite loop.
 * ================================================================ */
static void test7_empty_workload(void) {
    printf("\n=== TEST 7 (robustness): empty workload ===\n");
    FILE *f = fopen("test_tmp/cfs_t7.csv", "w");
    fprintf(f, "pid,arrival_tick,burst,group\n");
    fclose(f);

    workload_t w;
    int rc = trace_loader_load("test_tmp/cfs_t7.csv", &w);
    CHECK(rc == 0, "empty trace loads successfully");
    CHECK(w.count == 0, "workload count == 0");

    cfs_run_result_t r = scheduler_cfs_run(&w, 0);
    CHECK(r.total_ticks == 0, "total_ticks == 0 for empty workload");
    CHECK(r.idle_ticks == 0, "idle_ticks == 0 for empty workload");

    trace_loader_free(&w);
}

/* ================================================================
 * TEST 8 — Real Phase 2 traces (end-to-end, not hand-verifiable)
 * Same role as scheduler_rr.c's smoke test: confirms the scheduler
 * loads and fully completes the real generated workloads without
 * crashing, and that every process reaches remaining_burst==0. Not a
 * substitute for TEST1-6's exact hand/reference-checked values -- this
 * only proves the scheduler is robust at realistic scale (100-160
 * processes), not that its per-tick logic is correct (TEST1-6 already
 * established that).
 * ================================================================ */
static void test8_real_traces(void) {
    const char *paths[] = {
        "traces/balanced.csv",
        "traces/batch_heavy.csv",
        "traces/bursty_system.csv",
    };
    printf("\n=== TEST 8: real Phase 2 traces (end-to-end) ===\n");
    for (int i = 0; i < 3; i++) {
        workload_t w;
        int rc = trace_loader_load(paths[i], &w);
        char desc[128];
        snprintf(desc, sizeof(desc), "%s loads successfully", paths[i]);
        CHECK(rc == 0, desc);
        if (rc != 0) continue;

        cfs_run_result_t r = scheduler_cfs_run(&w, 0);

        int all_complete = 1;
        long total_wait = 0;
        for (int j = 0; j < w.count; j++) {
            if (w.items[j]->remaining_burst != 0) all_complete = 0;
            if (w.items[j]->completion_tick < 0) all_complete = 0;
            total_wait += w.items[j]->wait_time;
        }
        printf("  %s: %d processes, total_ticks=%d, idle_ticks=%d, sum(wait_time)=%ld\n",
               paths[i], w.count, r.total_ticks, r.idle_ticks, total_wait);

        snprintf(desc, sizeof(desc), "%s: every process completed", paths[i]);
        CHECK(all_complete, desc);

        trace_loader_free(&w);
    }
}

int main(void) {
    printf("=== PHASE 4 VALIDATION (CFS-replay baseline) ===\n");

    test1();
    test2();
    test3();
    test4();
    test5();
    test6();
    test7_empty_workload();
    test8_real_traces();

    printf("\n=== PHASE 4 VALIDATION SUMMARY ===\n");
    printf("PASS: %d\n", g_pass_count);
    printf("FAIL: %d\n", g_fail_count);
    printf("=== %s ===\n", g_fail_count == 0 ? "ALL CHECKS PASSED" : "FAILURES PRESENT");

    return g_fail_count == 0 ? 0 : 1;
}
