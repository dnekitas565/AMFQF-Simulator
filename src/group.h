#ifndef AMFQF_GROUP_H
#define AMFQF_GROUP_H

/*
 * DEPENDENCY NOTE (why this header does NOT #include "process.h"):
 *
 * process.h needs group_id_t (every process is tagged with a group), and
 * group_t needs a pointer to a process_t (each group's ready queue head).
 * If both headers #included each other directly, whichever one is
 * processed first would hit an unresolved type on its very first pass,
 * and the standard #ifndef include-guard pattern does not fix a genuine
 * two-way #include cycle by itself.
 *
 * The break: group.h only ever needs a *pointer* to a process, never the
 * full definition of process_t (it doesn't dereference process fields
 * anywhere in this file). A pointer to an incomplete type is legal in C,
 * so a forward declaration ("struct process;") is sufficient here. That
 * makes the dependency one-directional: process.h -> group.h, and group.h
 * depends on nothing. This mirrors the guide's Phase 1 instruction to
 * "use a forward declaration where necessary so process.h and group.h do
 * not create unnecessary circular dependencies."
 */
struct process;

/*
 * group_id_t — the four AMFQF fairness groups.
 *
 * NUM_GROUPS is deliberately the last enumerator (not a separately
 * maintained #define) so that it always equals the true count of groups
 * even if a group is ever added or removed — every later phase that
 * loops "for g in 0..NUM_GROUPS" or sizes a "group_t groups[NUM_GROUPS]"
 * array stays correct automatically.
 */
typedef enum {
    GROUP_SYSTEM = 0,
    GROUP_INTERACTIVE,
    GROUP_BATCH,
    GROUP_IDLE,
    NUM_GROUPS
} group_id_t;

/*
 * AMFQF_BASE_SHARE — the fixed base CPU share for each group, as
 * specified by the project: system 10%, interactive 40%, batch 40%,
 * idle 10%. Indexed by group_id_t.
 *
 * WHY a single named array here (rather than each scheduler hard-coding
 * its own 0.10/0.40/0.40/0.10 literals): Phase 5's budget calculation
 * (`budget[g] = base_share[g] * N`) and Phase 4's baseline weighting both
 * need these numbers, and if they were each declared separately anywhere
 * a value gets changed later, the two schedulers could silently drift
 * out of agreement about what "the base shares" actually are. Declaring
 * it `static const` in a header gives every .c file that includes
 * group.h its own private copy of the same four numbers, with no linkage
 * conflicts across translation units.
 */
static const double AMFQF_BASE_SHARE[NUM_GROUPS] = {
    [GROUP_SYSTEM]      = 0.10,
    [GROUP_INTERACTIVE] = 0.40,
    [GROUP_BATCH]       = 0.40,
    [GROUP_IDLE]        = 0.10
};

/*
 * group_t — per-group scheduling state.
 *
 * Field-by-field purpose:
 *
 *   id           Which group this is. Stored on the struct itself (not
 *                just implied by array index) so that a group_t can be
 *                passed around by pointer and still self-identify in
 *                debug output/metrics without the caller also having to
 *                thread the index through separately.
 *
 *   base_share   This group's fixed fraction of CPU time (0.10 / 0.40 /
 *                0.40 / 0.10). Copied from AMFQF_BASE_SHARE at group-
 *                array initialization time rather than looked up fresh
 *                every tick, so Phase 5's per-tick budget-ratio
 *                computation (remaining budget / base share) is a single
 *                struct field read, not an array-indexed lookup, on the
 *                hot path.
 *
 *   budget       This group's remaining CPU-time budget within the
 *                CURRENT accounting window, in quanta. Reset to
 *                base_share * N at the start of every window (Phase 5.1)
 *                and decremented by 1 each time a process from this
 *                group is dispatched for a tick. Can be adjusted by the
 *                idle-redistribution (5.2) and debt-borrowing (5.3)
 *                logic — those are Phase 5 concerns, not Phase 1, but
 *                the field exists now because the guide's dependency
 *                order has trace-gen/baselines built before AMFQF touches
 *                this field.
 *
 *   idle_ticks   Consecutive ticks (within the current window) that this
 *                group's ready queue has been empty. Reset to 0 the
 *                instant any process is in the group's ready queue;
 *                incremented by 1 every tick it stays empty. Phase 5.2
 *                redistributes a group's unused budget once this counter
 *                reaches threshold K.
 *
 *   ready_head   Head pointer of this group's ready queue, implemented
 *                as a circular singly-linked list of process_t nodes
 *                (see process.h's `next` field). A circular list (rather
 *                than a plain NULL-terminated list) is used specifically
 *                so round-robin advancement is a single pointer follow
 *                with no special-case wraparound check at the tail.
 *
 *   rr_cursor    "Whose turn is it next" pointer within this group's
 *                ready queue. Kept separate from ready_head so that
 *                round-robin position survives across ticks even as
 *                ready_head potentially changes identity (e.g. if the
 *                head process completes and is removed, ready_head
 *                moves on, but rr_cursor should still point at the
 *                *next* process in turn order rather than resetting to
 *                the new head every time).
 */
typedef struct group {
    group_id_t id;
    double base_share;
    double budget;
    int idle_ticks;
    struct process *ready_head;
    struct process *rr_cursor;
} group_t;

#endif /* AMFQF_GROUP_H */
