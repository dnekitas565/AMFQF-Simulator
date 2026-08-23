# AMFQF — Adaptive Multi-Level Feedback Queue with Fairness

Simulator implementing the AMFQF scheduling algorithm, built phase-by-phase
per the AMFQF Implementation Guide.

## Status

- [x] Phase 1 — Core data structures (`process.h`, `group.h`)
- [x] Phase 2 — Synthetic trace generator (`trace_gen.c`)
- [ ] Phase 3 — Round Robin baseline
- [ ] Phase 4 — Simplified CFS/nice-value baseline
- [ ] Phase 5 — AMFQF core algorithm
- [ ] Phase 6 — Metrics collection
- [ ] Phase 7 — Experiment harness
- [ ] Phase 9 — Analysis & plotting
- [ ] Phase 10 — Validation experiments
- [ ] Phase 11 — Report writing

## Layout

```
amfqf/
├── src/        # C source (schedulers, trace gen, metrics, structs)
├── traces/     # generated workload CSVs
├── results/    # experiment output CSVs
├── analysis/   # Python plotting scripts + generated figures
└── README.md
```

## Build (Phase 1 validation only, for now)

```bash
gcc -Wall -Wextra -std=c11 -g -o phase1_validate src/phase1_validate.c
./phase1_validate
```
