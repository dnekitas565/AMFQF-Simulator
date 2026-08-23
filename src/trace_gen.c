/*
 * trace_gen.c — AMFQF synthetic workload trace generator (Phase 2)
 *
 * Produces three CSV trace files under traces/:
 *   balanced.csv       — equal process counts across all 4 groups
 *   batch_heavy.csv     — 10:1 batch-to-interactive ratio
 *   bursty_system.csv   — periodic clusters of extra system-group arrivals
 *
 * CSV columns: pid,arrival_tick,burst,group
 *
 * This file is compiled standalone (its own main()) because Phase 7's
 * experiment harness does not exist yet, and the guide's Phase 2
 * checkpoint requires running the generator twice and diffing output —
 * that needs a runnable executable now, not something wired into a
 * harness three phases away.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "group.h"

/* ------------------------------------------------------------------ */
/* PARAMETERS — documented explicitly per the guide's requirement not  */
/* to choose numbers silently.                                         */
/* ------------------------------------------------------------------ */

#define RNG_SEED 42

/*
 * Burst-length ranges, in quanta, per group. Chosen as follows:
 *   SYSTEM      1-3   : guide's explicit example ("short, 1-3 quanta")
 *   INTERACTIVE 1-5   : guide's explicit example ("short bursts, 1-5")
 *   BATCH       10-50 : guide's explicit example ("long bursts, 10-50")
 *   IDLE        20-60 : NOT given an explicit numeric range by the
 *                        guide (it only says "long/low-frequency
 *                        workload" qualitatively). Chosen here as
 *                        longer than BATCH's upper bound, to make idle
 *                        tasks unambiguously the longest-running class
 *                        when they do run — consistent with "idle"
 *                        meaning background maintenance-style work
 *                        (e.g. a nightly reindex), not "trivially
 *                        short." "Low frequency" is implemented
 *                        separately, via a larger mean inter-arrival
 *                        time for the idle group (see
 *                        MEAN_INTERARRIVAL_IDLE below) rather than via
 *                        burst length — frequency and duration are
 *                        different axes and conflating them would make
 *                        idle both rare AND short, which isn't what
 *                        "background batch-like work" usually looks
 *                        like.
 */
#define SYSTEM_BURST_MIN 1
#define SYSTEM_BURST_MAX 3
#define INTERACTIVE_BURST_MIN 1
#define INTERACTIVE_BURST_MAX 5
#define BATCH_BURST_MIN 10
#define BATCH_BURST_MAX 50
#define IDLE_BURST_MIN 20
#define IDLE_BURST_MAX 60

/*
 * Mean inter-arrival time (in ticks) per group, used as the exponential
 * distribution's mean. Smaller = more frequent arrivals.
 *   SYSTEM: 15      — system activity (daemons waking, timers) is
 *                      fairly frequent but not constant
 *   INTERACTIVE: 8   — interactive processes arrive relatively often
 *                      (this is the "user is actively doing things"
 *                      class)
 *   BATCH: 20        — batch jobs are submitted less often than
 *                      interactive activity, but each one runs long
 *                      (see burst range above)
 *   IDLE: 60         — deliberately the least frequent, implementing
 *                      "low-frequency" per the guide's description
 */
#define MEAN_INTERARRIVAL_SYSTEM 15.0
#define MEAN_INTERARRIVAL_INTERACTIVE 8.0
#define MEAN_INTERARRIVAL_BATCH 20.0
#define MEAN_INTERARRIVAL_IDLE 60.0

/* Balanced workload: equal process count per group. */
#define BALANCED_COUNT_PER_GROUP 25

/*
 * Batch-heavy workload: exact 10:1 batch:interactive ratio as specified.
 * System and idle counts are held at the same level as "balanced" so
 * the only thing that changes between workloads is the batch/interactive
 * imbalance being tested — changing every group's count at once would
 * make it harder to attribute any observed effect specifically to the
 * batch:interactive ratio.
 */
#define BATCH_HEAVY_INTERACTIVE_COUNT 10
#define BATCH_HEAVY_BATCH_COUNT 100 /* exactly 10x interactive count */
#define BATCH_HEAVY_SYSTEM_COUNT 25
#define BATCH_HEAVY_IDLE_COUNT 25

/*
 * Bursty-system workload: starts from the same base counts as
 * "balanced," then injects periodic clusters of EXTRA system processes
 * on top. Every BURST_PERIOD_TICKS ticks, BURST_CLUSTER_SIZE system
 * processes arrive within a tight window (BURST_CLUSTER_SPAN_TICKS) of
 * each other, simulating e.g. a periodic cron/timer firing multiple
 * system tasks at once. BURST_PERIOD_TICKS=200 matches the guide's own
 * suggested example value directly, so it is used as given rather than
 * re-derived.
 */
#define BURSTY_BASE_COUNT_PER_GROUP 25
#define BURST_PERIOD_TICKS 200
#define BURST_CLUSTER_SIZE 5
#define BURST_CLUSTER_SPAN_TICKS 3   /* the 5 processes land within a 3-tick window */
#define BURSTY_NUM_PERIODS 4         /* how many periodic clusters to inject */

#define MAX_TRACE_RECORDS 2048

typedef struct {
    int pid;
    int arrival_tick;
    int burst;
    group_id_t group;
} trace_record_t;

/* ------------------------------------------------------------------ */
/* Random number generation                                            */
/* ------------------------------------------------------------------ */

/*
 * uniform_open01 — draw a uniform random double strictly in (0, 1).
 *
 * WHY strictly open on both ends: exponential_interarrival() below
 * computes log(U). log(0) is -infinity (undefined behavior territory /
 * a domain error), so U must never be exactly 0. We also exclude 1 for
 * symmetry, though U==1 would not itself break log(); excluding both
 * ends is the standard, safe form of this transform.
 *
 * Implementation: rand() returns an int in [0, RAND_MAX]. Dividing by
 * (RAND_MAX + 1.0) maps that to a half-open interval [0, 1). Adding a
 * tiny epsilon and re-checking guards the one edge case where rand()
 * returns exactly 0.
 */
static double uniform_open01(void) {
    double u;
    do {
        u = (double)rand() / ((double)RAND_MAX + 1.0);
    } while (u <= 0.0);
    return u;
}

/*
 * exponential_interarrival — inverse transform sampling for an
 * exponential distribution.
 *
 * WHY exponential inter-arrival times: this is the standard way to
 * simulate "independent arrivals happening at some average rate over
 * time" (a Poisson arrival process) — the same model used for things
 * like customers arriving at a queue or requests hitting a server. It
 * produces the right qualitative shape (arrivals bunch up sometimes,
 * spread out other times) instead of the unrealistically regular
 * spacing you'd get from fixed inter-arrival times.
 *
 * WHY inverse transform sampling specifically: the exponential
 * distribution's CDF, F(x) = 1 - e^(-x/mean), is analytically
 * invertible. Solving F(x) = U for x gives x = -mean * ln(1 - U), which
 * simplifies to x = -mean * ln(U) because (1 - U) is itself uniform on
 * (0,1) when U is. This turns "sample from an exponential" into "take
 * one uniform sample and apply a closed-form formula" — no rejection
 * sampling or lookup tables needed, and it is exact (not an
 * approximation).
 */
static double exponential_interarrival(double mean_interarrival) {
    double u = uniform_open01();
    return -mean_interarrival * log(u);
}

/*
 * random_int_range — uniform integer in [min_val, max_val] inclusive.
 * Used for burst-length sampling.
 */
static int random_int_range(int min_val, int max_val) {
    return min_val + (rand() % (max_val - min_val + 1));
}

/* ------------------------------------------------------------------ */
/* Generation helpers                                                   */
/* ------------------------------------------------------------------ */

static int burst_min_for_group(group_id_t g) {
    switch (g) {
        case GROUP_SYSTEM:      return SYSTEM_BURST_MIN;
        case GROUP_INTERACTIVE: return INTERACTIVE_BURST_MIN;
        case GROUP_BATCH:       return BATCH_BURST_MIN;
        case GROUP_IDLE:        return IDLE_BURST_MIN;
        default:                return 1;
    }
}
static int burst_max_for_group(group_id_t g) {
    switch (g) {
        case GROUP_SYSTEM:      return SYSTEM_BURST_MAX;
        case GROUP_INTERACTIVE: return INTERACTIVE_BURST_MAX;
        case GROUP_BATCH:       return BATCH_BURST_MAX;
        case GROUP_IDLE:        return IDLE_BURST_MAX;
        default:                return 1;
    }
}

static int generate_burst(group_id_t g) {
    return random_int_range(burst_min_for_group(g), burst_max_for_group(g));
}

static double mean_interarrival_for_group(group_id_t g) {
    switch (g) {
        case GROUP_SYSTEM:      return MEAN_INTERARRIVAL_SYSTEM;
        case GROUP_INTERACTIVE: return MEAN_INTERARRIVAL_INTERACTIVE;
        case GROUP_BATCH:       return MEAN_INTERARRIVAL_BATCH;
        case GROUP_IDLE:        return MEAN_INTERARRIVAL_IDLE;
        default:                return 10.0;
    }
}

/*
 * append_group_arrivals — generate `count` processes for group `g`
 * using exponential inter-arrival times, appending them into `records`
 * starting at *inout_count. Arrival ticks are cumulative (each is the
 * previous arrival + a fresh exponential sample), so within one group's
 * own stream they are naturally non-decreasing; different groups'
 * streams are independent of each other and get merged/sorted later.
 *
 * *inout_pid is the shared PID counter across the whole trace, threaded
 * through by pointer so PIDs stay unique across all four groups within
 * one trace file.
 */
static void append_group_arrivals(trace_record_t *records, int *inout_count,
                                   int *inout_pid, group_id_t g, int count) {
    double mean_ia = mean_interarrival_for_group(g);
    double cursor = 0.0;
    for (int i = 0; i < count; i++) {
        cursor += exponential_interarrival(mean_ia);
        trace_record_t *r = &records[*inout_count];
        r->pid = (*inout_pid)++;
        r->arrival_tick = (int)(cursor + 0.5); /* round to nearest tick */
        r->burst = generate_burst(g);
        r->group = g;
        (*inout_count)++;
    }
}

static const char *group_csv_name(group_id_t g) {
    switch (g) {
        case GROUP_SYSTEM:      return "SYSTEM";
        case GROUP_INTERACTIVE: return "INTERACTIVE";
        case GROUP_BATCH:       return "BATCH";
        case GROUP_IDLE:        return "IDLE";
        default:                return "UNKNOWN";
    }
}

static int compare_by_arrival(const void *a, const void *b) {
    const trace_record_t *ra = (const trace_record_t *)a;
    const trace_record_t *rb = (const trace_record_t *)b;
    if (ra->arrival_tick != rb->arrival_tick) {
        return ra->arrival_tick - rb->arrival_tick;
    }
    return ra->pid - rb->pid; /* stable tie-break, keeps output deterministic */
}

/*
 * write_trace_csv — write records (already sorted by arrival_tick) to
 * `path` as pid,arrival_tick,burst,group.
 *
 * Sorting by arrival_tick before writing is a readability/debuggability
 * choice, not a correctness requirement of the CSV format itself — it
 * makes "eyeball the file, does it look right" (the guide's own Phase 2
 * checkpoint instruction) actually feasible, since the four groups'
 * streams were generated independently and would otherwise be
 * interleaved in generation order, not time order.
 */
static void write_trace_csv(const char *path, trace_record_t *records, int count) {
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "FAIL: could not open %s for writing\n", path);
        exit(1);
    }
    fprintf(f, "pid,arrival_tick,burst,group\n");
    for (int i = 0; i < count; i++) {
        fprintf(f, "%d,%d,%d,%s\n",
                records[i].pid, records[i].arrival_tick,
                records[i].burst, group_csv_name(records[i].group));
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* Workload 1: Balanced                                                */
/* ------------------------------------------------------------------ */
static void generate_balanced(const char *path) {
    trace_record_t records[MAX_TRACE_RECORDS];
    int count = 0;
    int pid = 1;
    append_group_arrivals(records, &count, &pid, GROUP_SYSTEM, BALANCED_COUNT_PER_GROUP);
    append_group_arrivals(records, &count, &pid, GROUP_INTERACTIVE, BALANCED_COUNT_PER_GROUP);
    append_group_arrivals(records, &count, &pid, GROUP_BATCH, BALANCED_COUNT_PER_GROUP);
    append_group_arrivals(records, &count, &pid, GROUP_IDLE, BALANCED_COUNT_PER_GROUP);
    qsort(records, count, sizeof(trace_record_t), compare_by_arrival);
    write_trace_csv(path, records, count);
    printf("Wrote %s: %d processes (balanced, %d per group)\n", path, count, BALANCED_COUNT_PER_GROUP);
}

/* ------------------------------------------------------------------ */
/* Workload 2: Batch-heavy                                             */
/* ------------------------------------------------------------------ */
static void generate_batch_heavy(const char *path) {
    trace_record_t records[MAX_TRACE_RECORDS];
    int count = 0;
    int pid = 1;
    append_group_arrivals(records, &count, &pid, GROUP_SYSTEM, BATCH_HEAVY_SYSTEM_COUNT);
    append_group_arrivals(records, &count, &pid, GROUP_INTERACTIVE, BATCH_HEAVY_INTERACTIVE_COUNT);
    append_group_arrivals(records, &count, &pid, GROUP_BATCH, BATCH_HEAVY_BATCH_COUNT);
    append_group_arrivals(records, &count, &pid, GROUP_IDLE, BATCH_HEAVY_IDLE_COUNT);
    qsort(records, count, sizeof(trace_record_t), compare_by_arrival);
    write_trace_csv(path, records, count);
    printf("Wrote %s: %d processes (batch=%d, interactive=%d, ratio=%.1f:1)\n",
           path, count, BATCH_HEAVY_BATCH_COUNT, BATCH_HEAVY_INTERACTIVE_COUNT,
           (double)BATCH_HEAVY_BATCH_COUNT / (double)BATCH_HEAVY_INTERACTIVE_COUNT);
}

/* ------------------------------------------------------------------ */
/* Workload 3: Bursty-system                                           */
/* ------------------------------------------------------------------ */
static void generate_bursty_system(const char *path) {
    trace_record_t records[MAX_TRACE_RECORDS];
    int count = 0;
    int pid = 1;

    /* Base load: same shape as "balanced," so the periodic system burst
       is the ONLY variable being tested against a normal mixed load. */
    append_group_arrivals(records, &count, &pid, GROUP_SYSTEM, BURSTY_BASE_COUNT_PER_GROUP);
    append_group_arrivals(records, &count, &pid, GROUP_INTERACTIVE, BURSTY_BASE_COUNT_PER_GROUP);
    append_group_arrivals(records, &count, &pid, GROUP_BATCH, BURSTY_BASE_COUNT_PER_GROUP);
    append_group_arrivals(records, &count, &pid, GROUP_IDLE, BURSTY_BASE_COUNT_PER_GROUP);

    /* Inject periodic clusters: at ticks 200, 400, 600, 800 (per
       BURST_PERIOD_TICKS/BURSTY_NUM_PERIODS), add BURST_CLUSTER_SIZE
       extra system processes landing within a BURST_CLUSTER_SPAN_TICKS
       window of that tick. */
    for (int period = 1; period <= BURSTY_NUM_PERIODS; period++) {
        int period_tick = period * BURST_PERIOD_TICKS;
        for (int c = 0; c < BURST_CLUSTER_SIZE; c++) {
            trace_record_t *r = &records[count];
            r->pid = pid++;
            r->arrival_tick = period_tick + random_int_range(0, BURST_CLUSTER_SPAN_TICKS);
            r->burst = generate_burst(GROUP_SYSTEM);
            r->group = GROUP_SYSTEM;
            count++;
        }
    }

    qsort(records, count, sizeof(trace_record_t), compare_by_arrival);
    write_trace_csv(path, records, count);
    printf("Wrote %s: %d processes (base=%d/group + %d clusters of %d system procs every %d ticks)\n",
           path, count, BURSTY_BASE_COUNT_PER_GROUP, BURSTY_NUM_PERIODS,
           BURST_CLUSTER_SIZE, BURST_PERIOD_TICKS);
}

/* ------------------------------------------------------------------ */

int main(void) {
    srand(RNG_SEED);

    generate_balanced("traces/balanced.csv");
    generate_batch_heavy("traces/batch_heavy.csv");
    generate_bursty_system("traces/bursty_system.csv");

    printf("\nAll traces written with fixed seed %d.\n", RNG_SEED);
    return 0;
}
