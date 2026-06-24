# SWAY Phase 1 Task A: Utility-Vector Probe

## Goal

Implement a measurement-only SWAY utility-vector probe inside
`DecoupledBPUWithBTB`. At each main phase boundary, reuse existing MBTB and
BTBTAGE way-visit snapshots to dump `sway_utility_by_phase.csv` for Task B
controller design. This task must not change prediction behavior or perform
way reallocation.

## Requirements

- Add an internal `SwayController` utility probe under `src/cpu/pred/btb/`.
- Reuse the existing `phaseSizeByInst` main-phase hook and existing
  `collectAndResetWayVisitCounts()` snapshot sources.
- Emit `sway_utility_by_phase.csv` with columns:
  `phaseID,scope,total_ways,active_ways,utility`.
- Emit exactly the SWAY-controlled scopes: `mbtb_sram0`, `mbtb_sram1`, and
  `tage_t0` through `tage_t7`. Exclude `microtage_t0`.
- Keep `sway_stranded_by_phase.csv` data unchanged as regression protection.
- Generate `figure3_utility_per_phase.pdf` from 10 workload x 10 scope utility
  time series.
- Update the SWAY paper `PLAN.md` with a Phase 1 Step A completion block.

## Acceptance Criteria

- [x] `scons build/RISCV/gem5.opt -j50` succeeds in this worktree.
- [x] Single workload smoke emits `sway_utility_by_phase.csv` with
      90 rows plus header for 500K max insts and W=50K.
- [x] Smoke `sway_stranded_by_phase.csv` row count/content shape is unchanged.
- [x] 10 workload W=100K batch completes with 0 aborts.
- [x] Batch utility CSV shape is valid for all completed workloads.
- [x] `figure3_utility_per_phase.pdf` is generated.
- [x] PRD records observations about donor/donee tendencies for Task B.
- [ ] Two commits exist: source change, then Trellis/paper closeout.

## Definition of Done

- Source change is built and smoke-tested.
- Batch run status is checked with the mytools scheduler output.
- Figure and closeout docs are updated from real data.
- No BPU behavior change is introduced beyond measurement and CSV dumping.

## Technical Approach

`SwayController` is an internal helper owned by `DecoupledBPUWithBTB`. The
existing stranded collection already computes per-scope `totalWays` and
`activeWays` at phase boundaries while resetting the visit counters. The probe
will derive utility rows from the same snapshots before the snapshots are stored
for stranded CSV dumping.

For this MVP, utility is defined as the observable phase access density:

```text
utility = active_ways / total_ways
```

This treats `active_ways` as the phase-local delta of ways that received at
least one visit after the previous reset. It is intentionally simpler than the
theoretical marginal-utility formula in the paper plan.

## Out of Scope

- No way ownership table.
- No donor/recipient decision logic.
- No invalidation or allocation mutation.
- No sampled ATD implementation.
- No MicroTAGE or fast-predictor control.

## Technical Notes

- Existing snapshot sources:
  - `MBTB::collectAndResetWayVisitCounts()`
  - `BTBTAGE::collectAndResetWayVisitCounts()`
- Existing dump pattern:
  - `DecoupledBPUWithBTB::dumpStats()` writes `sway_stranded_by_phase.csv`.
- Trace-mode contract was read because the code path sits in
  `decoupled_bpred.*`; this task does not change trace branch metadata,
  squash, redirect, or training semantics.
- Missing Trellis helper scripts in this worktree:
  `.trellis/scripts/task.py` and `.trellis/scripts/get_context.py` are absent,
  so the task directory was created manually following archived task layout.

## Observations

Batch data:

- Run root:
  `/nfs/home/goulingrui/project/papers/sway-paper/runs/utility_W100K/main_W100K`.
- Scheduler status: 10 completed, 0 running, 0 aborted, 0 missing trace, 0
  launch failed.
- Every workload emitted 49 phases x 10 scopes = 490 data rows in both
  `sway_utility_by_phase.csv` and `sway_stranded_by_phase.csv`.
- Utility rows exactly match stranded rows on `(phaseID, scope, total_ways,
  active_ways)` and all utility values are in `[0, 1]`.
- Figure outputs:
  - `figures/figure3_utility_per_phase.pdf`
  - `figures/figure3_utility_per_phase.csv`
  - `figures/figure3_utility_summary.csv`

Donor/donee tendencies by mean utility:

- MBTB-donee workloads: `gcc_pp_O2_1869`, `perlbench_diff_20021`, and
  `perlbench_split_8843` put both MBTB SRAMs in the top utility group.
- Mid-TAGE-donee workloads: `blender_26411`, `deepsjeng_88772`,
  `exchange2_101257`, `mcf_17135`, and `omnetpp_21279` are dominated by
  `tage_t2`/`tage_t3`/`tage_t4`, with `mcf_17135` especially concentrated on
  `tage_t3`.
- Low-history TAGE-donee workload: `leela_60438` favors
  `tage_t1`, `tage_t2`, and `tage_t0`.
- Deep-history TAGE-donee workload: `xalancbmk_6102` is the clearest high-table
  outlier, with `tage_t7`, `tage_t6`, and `tage_t5` as the top scopes while
  `tage_t0`/`tage_t1`/`tage_t2` are donor-like.
- Stable donor candidates across many workloads are `tage_t7` and `tage_t6`;
  notable exceptions are `xalancbmk_6102`, where those become donee-like.
- `mcf_17135` shows near-zero MBTB utility (`mbtb_sram0` among the bottom
  scopes), making it a strong MBTB-donor / mid-TAGE-donee case for Task B.
