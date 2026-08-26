#ifndef AMFQF_TRACE_LOADER_H
#define AMFQF_TRACE_LOADER_H

#include "process.h"

/*
 * workload_t — the loaded, sorted contents of one trace CSV: an array of
 * process_t*, owned by this struct (freed by trace_loader_free).
 *
 * WHY an array (not a linked list) for storage: the array is the
 * scheduler's *admission source*, scanned strictly in arrival order to
 * decide "who has arrived by this tick" — a simple forward index sweep
 * over a sorted array is O(1) amortized per tick. The ready_queue_t
 * (separate structure) is what actually holds "currently admitted,
 * currently waiting" processes during simulation; workload_t is the
 * static input, ready_queue_t is the dynamic runtime state. Keeping
 * these as two distinct structures (rather than one array doing both
 * jobs) is what lets a process be simultaneously "known to exist in the
 * trace" and "currently linked into a queue via ->next" without the two
 * roles' pointers colliding.
 */
typedef struct {
    process_t **items;
    int count;
    int capacity; /* internal, for realloc growth — callers should only read count */
} workload_t;

/*
 * Load a CSV trace at `path` into `out`.
 *
 * Expected format: pid,arrival_tick,burst,group  (an optional matching
 * header line is recognized and skipped; if the first line isn't exactly
 * that header, it is parsed as a data row instead, so headerless traces
 * also work).
 *
 * On success: returns 0, *out contains all loaded processes sorted by
 * (arrival_tick, pid) ascending — this sort is what makes the sequential
 * admission sweep in scheduler_rr.c correct even if the input file's row
 * order is not itself time-ordered.
 *
 * On failure (file not found, a malformed row, or an allocation
 * failure): returns -1, prints a specific diagnostic to stderr
 * identifying the exact row and reason, and leaves *out fully freed
 * (out->items == NULL, out->count == 0) — the caller has nothing to
 * clean up either way.
 *
 * Design decision: a single malformed row aborts the ENTIRE load, rather
 * than being skipped. Silently dropping one bad row would mean the
 * experiment actually run no longer matches the trace file on disk,
 * which is a worse failure mode (a silently wrong result) than a loud
 * failure the caller must handle.
 */
int trace_loader_load(const char *path, workload_t *out);

/* Frees every process_t this workload owns, then the array itself. */
void trace_loader_free(workload_t *w);

#endif /* AMFQF_TRACE_LOADER_H */
