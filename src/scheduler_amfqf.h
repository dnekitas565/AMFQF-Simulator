#ifndef AMFQF_SCHEDULER_AMFQF_H
#define AMFQF_SCHEDULER_AMFQF_H

#include "trace_loader.h"

/* Frozen AMFQF v1.0 parameters. */
#define AMFQF_RUNNABLE_GROUPS 3
#define AMFQF_QUEUE_LEVELS 3
#define AMFQF_WINDOW_TICKS 100
#define AMFQF_PROMOTION_TICKS 20
#define AMFQF_STARVATION_TICKS 40

static const int AMFQF_QUANTUM[AMFQF_QUEUE_LEVELS] = { 4, 8, 16 };
static const double AMFQF_TARGET_SHARE[AMFQF_RUNNABLE_GROUPS] = {
    0.20, 0.40, 0.40
};

typedef struct {
    int total_ticks;
    int idle_ticks;
    int completed_count;
    int rejected_input;
    int ordinary_promotions;
    int preemptions;
    int invariant_checks;
} amfqf_run_result_t;

/* GROUP_IDLE is a legacy baseline-only group and is rejected. */
amfqf_run_result_t scheduler_amfqf_run(workload_t *workload, int debug);

#endif /* AMFQF_SCHEDULER_AMFQF_H */
