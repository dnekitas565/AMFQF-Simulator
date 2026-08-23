/*
 * phase1_validate.c
 *
 * WHY this file exists (and why it is not called main.c):
 * The guide reserves main.c for the Phase 7 experiment harness, which
 * does not exist yet and depends on schedulers/metrics that haven't been
 * built. Naming this file main.c now would mean either leaving a stub
 * that has to be entirely rewritten in Phase 7, or accidentally building
 * Phase 7 logic on top of validation code. Keeping it as a separate,
 * clearly-temporary file avoids that collision; it is expected to be
 * deleted (or left as a standalone `make phase1_validate` target) once
 * Phase 7's real main.c exists.
 *
 * This program is the Phase 1 checkpoint's validation:
 *   1. Create several processes, assigned to different groups.
 *   2. Print PID / Group / Arrival / Burst / RemainingBurst / WaitTime /
 *      CompletionTick for each.
 *   3. Verify the four group identifiers are distinct and NUM_GROUPS==4.
 *   4. Verify the 10/40/40/10 base shares.
 *   5. Free everything.
 */

#include <stdio.h>
#include <assert.h>
#include "process.h"
#include "group.h"

static const char *group_name(group_id_t g) {
    switch (g) {
        case GROUP_SYSTEM:      return "SYSTEM";
        case GROUP_INTERACTIVE: return "INTERACTIVE";
        case GROUP_BATCH:       return "BATCH";
        case GROUP_IDLE:        return "IDLE";
        default:                return "UNKNOWN";
    }
}

static void print_process(const process_t *p) {
    printf("%-6d %-12s %-10d %-8d %-16d %-11d %-16d\n",
           p->pid,
           group_name(p->group),
           p->arrival_tick,
           p->total_burst,
           p->remaining_burst,
           p->wait_time,
           p->completion_tick);
}

int main(void) {
    printf("=== PHASE 1 VALIDATION ===\n\n");

    /* --- Step 1/2: create processes across all four groups, print --- */
    process_t *procs[6];
    procs[0] = process_create(1, 0, 2,  GROUP_SYSTEM);
    procs[1] = process_create(2, 0, 3,  GROUP_INTERACTIVE);
    procs[2] = process_create(3, 1, 3,  GROUP_INTERACTIVE);
    procs[3] = process_create(4, 2, 25, GROUP_BATCH);
    procs[4] = process_create(5, 3, 40, GROUP_IDLE);
    procs[5] = process_create(6, 5, 1,  GROUP_SYSTEM);

    for (int i = 0; i < 6; i++) {
        if (procs[i] == NULL) {
            fprintf(stderr, "FAIL: process_create returned NULL at index %d\n", i);
            return 1;
        }
    }

    printf("%-6s %-12s %-10s %-8s %-16s %-11s %-16s\n",
           "PID", "Group", "Arrival", "Burst", "RemainingBurst", "WaitTime", "CompletionTick");
    for (int i = 0; i < 6; i++) {
        print_process(procs[i]);
    }
    printf("\n");

    /* --- Sentinel-value checks (documented in process.h) --- */
    int sentinel_ok = 1;
    for (int i = 0; i < 6; i++) {
        if (procs[i]->completion_tick != -1) sentinel_ok = 0;
        if (procs[i]->wait_time != 0) sentinel_ok = 0;
        if (procs[i]->next != NULL) sentinel_ok = 0;
        if (procs[i]->remaining_burst != procs[i]->total_burst) sentinel_ok = 0;
    }
    printf("Sentinel values (completion_tick=-1, wait_time=0, next=NULL, "
           "remaining_burst==total_burst): %s\n", sentinel_ok ? "PASS" : "FAIL");

    /* --- Step 3: verify the four group identifiers --- */
    int groups_ok = 1;
    groups_ok &= (GROUP_SYSTEM == 0);
    groups_ok &= (GROUP_INTERACTIVE == 1);
    groups_ok &= (GROUP_BATCH == 2);
    groups_ok &= (GROUP_IDLE == 3);
    groups_ok &= (NUM_GROUPS == 4);
    printf("Four group identifiers distinct, NUM_GROUPS==4: %s\n",
           groups_ok ? "PASS" : "FAIL");

    /* --- Step 4: verify 10/40/40/10 base shares --- */
    int shares_ok = 1;
    shares_ok &= (AMFQF_BASE_SHARE[GROUP_SYSTEM]      == 0.10);
    shares_ok &= (AMFQF_BASE_SHARE[GROUP_INTERACTIVE] == 0.40);
    shares_ok &= (AMFQF_BASE_SHARE[GROUP_BATCH]       == 0.40);
    shares_ok &= (AMFQF_BASE_SHARE[GROUP_IDLE]        == 0.10);
    double sum = AMFQF_BASE_SHARE[GROUP_SYSTEM] + AMFQF_BASE_SHARE[GROUP_INTERACTIVE] +
                 AMFQF_BASE_SHARE[GROUP_BATCH] + AMFQF_BASE_SHARE[GROUP_IDLE];
    shares_ok &= (sum > 0.999 && sum < 1.001); /* base shares must sum to 1.0 */
    printf("Base shares 10/40/40/10 and sum to 1.0: %s (sum=%.4f)\n",
           shares_ok ? "PASS" : "FAIL", sum);

    /* --- group_t sanity: construct one group_t per group, check wiring --- */
    group_t groups[NUM_GROUPS];
    int group_struct_ok = 1;
    for (int g = 0; g < NUM_GROUPS; g++) {
        groups[g].id = (group_id_t)g;
        groups[g].base_share = AMFQF_BASE_SHARE[g];
        groups[g].budget = 0.0;
        groups[g].idle_ticks = 0;
        groups[g].ready_head = NULL;
        groups[g].rr_cursor = NULL;
        if (groups[g].id != (group_id_t)g) group_struct_ok = 0;
        if (groups[g].base_share != AMFQF_BASE_SHARE[g]) group_struct_ok = 0;
    }
    printf("group_t array constructs correctly for all NUM_GROUPS: %s\n",
           group_struct_ok ? "PASS" : "FAIL");

    /* --- manual linked-list wiring test: link procs[0..2] into GROUP mix --- */
    /* not a full ready-queue implementation (that's Phase 3+) — this only
       confirms process_t->next is usable as a link field */
    procs[0]->next = procs[1];
    procs[1]->next = procs[2];
    procs[2]->next = NULL;
    int link_count = 0;
    for (process_t *cur = procs[0]; cur != NULL; cur = cur->next) {
        link_count++;
    }
    printf("Manual linked-list traversal via ->next (expect 3): %s (got %d)\n",
           link_count == 3 ? "PASS" : "FAIL", link_count);
    procs[0]->next = NULL; /* undo before freeing independently below */
    procs[1]->next = NULL;

    /* --- Step 6: free everything --- */
    for (int i = 0; i < 6; i++) {
        process_destroy(procs[i]);
    }
    printf("\nAll 6 processes freed.\n");

    int all_pass = sentinel_ok && groups_ok && shares_ok && group_struct_ok && (link_count == 3);
    printf("\n=== PHASE 1 VALIDATION: %s ===\n", all_pass ? "ALL CHECKS PASSED" : "FAILURES PRESENT");
    return all_pass ? 0 : 1;
}
