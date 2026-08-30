#ifndef AMFQF_SCHEDULER_AMFQF_H
#define AMFQF_SCHEDULER_AMFQF_H

/* Frozen AMFQF v1.0 parameters.  This header deliberately contains no
 * scheduling policy yet; later components will expose the run API only
 * after the state transitions they require are implemented and tested. */
#define AMFQF_RUNNABLE_GROUPS 3
#define AMFQF_QUEUE_LEVELS 3
#define AMFQF_WINDOW_TICKS 100
#define AMFQF_PROMOTION_TICKS 20
#define AMFQF_STARVATION_TICKS 40

static const int AMFQF_QUANTUM[AMFQF_QUEUE_LEVELS] = { 4, 8, 16 };
static const double AMFQF_TARGET_SHARE[AMFQF_RUNNABLE_GROUPS] = {
    0.20, 0.40, 0.40
};

#endif /* AMFQF_SCHEDULER_AMFQF_H */
