# SWAY Phase 1 Task B: Way-Granularity Reallocation Controller PoC

## Goal

Implement a proof-of-concept SWAY controller that logically reallocates branch
predictor ways between MBTB and BTBTAGE logical components at phase boundaries.
The PoC must keep physical SRAM sizing unchanged, model only per-way ownership
and a 1-cycle quiesce cost, and provide baseline-vs-SWAY evidence for the SWAY
paper's Phase 1 evaluation.

## Requirements

- Add per-way owner registers to MBTB and BTBTAGE using `uint8_t` owner IDs.
- Keep physical table size unchanged; owner registers only control logical
  visibility and allocation eligibility.
- Filter lookup and allocation paths by owner so borrowed ways are invisible to
  the original logical component.
- When a way changes owner, squash that way's valid bits before the owner change
  becomes visible, preventing stale donor entries from being hit by the donee.
- Reuse the Task A phase hook and utility vector source:
  `DecoupledBPUWithBTB::notifyInstCommit()` ->
  `collectSwayWayVisitForPhase()`.
- At each phase boundary, compute donor/donee candidates from the per-scope
  utility vector using the paper's M5/M16 policy shape: lowest-utility donor,
  highest-utility donee.
- Transfer K ways per decision; K is parameterized and defaults to 1 for the
  initial PoC.
- Model a 1-cycle quiesce in the controller state machine; do not collapse it
  into zero-cycle behavior.
- Do not implement physical SRAM data migration.
- Add a backward-compatible SimObject parameter:
  `enableSwayRealloc=False` by default, enabled with
  `-P system.cpu[0].branchPred.enableSwayRealloc=True`.
- Expose stats:
  `system.cpu.branchPred.sway.reallocCount`,
  `system.cpu.branchPred.sway.reallocBlockedQuiesce`, and
  `system.cpu.branchPred.sway.ownerOverrides` per scope.
- Keep MicroTAGE and fast predictor structures out of the SWAY control scope
  unless a later task explicitly expands the scope.

## Acceptance Criteria

- [x] Baseline mode (`enableSwayRealloc=False`) preserves Task A
      `sway_stranded_by_phase.csv` behavior.
- [x] `scons build/RISCV/gem5.opt --gold-linker -j64` succeeds.
      Evidence: `-j64` completed successfully on the final code. `-j160`
      hit a known SCons generated-header race before generated headers had
      stabilized; `-j32` also completed successfully.
- [x] Single-workload smoke for `gcc_pp_O2` runs both baseline and SWAY for
      5M instructions with W=100K and no aborts.
- [x] Smoke comparison records:
      `system.cpu.ipc`, `system.cpu.frontendBound`,
      `system.cpu.commit.branchMispredicts`,
      `system.cpu.branchPred.tage.updateMispred`, and
      `system.cpu.branchPred.sway.reallocCount`.
- [x] Two 10-workload W=100K batches complete with no aborts:
      baseline under
      `/nfs/home/goulingrui/project/papers/sway-paper/runs/eval_baseline_W100K/`
      and SWAY under
      `/nfs/home/goulingrui/project/papers/sway-paper/runs/eval_sway_W100K/`.
- [x] `figure4_ipc_uplift.pdf` is generated from the 10-workload IPC deltas.
- [x] Geomean IPC delta is recorded. If geomean is not positive, record it as
      a Phase 1 negative finding rather than hiding or tuning it away.
- [x] Individual IPC deltas for `mcf` and `gcc_pp_O2` are recorded, with the
      target threshold of greater than 0.5 percent if the PoC works as hoped.
- [x] `/nfs/home/goulingrui/project/papers/sway-paper/PLAN.md` and
      `/nfs/home/goulingrui/project/papers/sway-paper/PENDING.md` PENDING-3
      are updated with measured numbers instead of placeholders.
- [x] Final change is split into 2-3 commits covering source/controller,
      evaluation/figure config, and Trellis closeout.

## Definition of Done

- Owner-register behavior is implemented for MBTB and BTBTAGE without changing
  default baseline behavior.
- Controller state and stats are visible in gem5 stats and the evaluation CSVs.
- Baseline and SWAY 10-workload runs complete without aborts.
- Figure 4 and paper planning docs reflect measured results.
- Trellis PRD and ExecPlan contain final evidence, risks, and next-step
  recommendations.

Strict performance note: the engineering/evaluation work is complete, but the
initial K=1 policy does not satisfy the full success target. Geomean IPC is
positive and `gcc_pp_O2` clears +0.5%, while `mcf` regresses. Treat this as a
Phase 1 policy finding, not as hidden pass/fail tuning.

## Technical Approach

Implement owner registers at the same granularity as the Task A utility scopes:
two MBTB SRAM scopes and `tage_t0` through `tage_t7`. MBTB ownership should be
tracked per SRAM way; BTBTAGE ownership should be tracked per table way. Lookup,
replacement, and allocation paths should skip ways whose owner does not match
the logical scope currently being accessed.

`SwayController` remains owned by `DecoupledBPUWithBTB`. At the main phase
boundary, it consumes the utility rows derived from the existing way-visit
snapshots, selects one donor and one donee, asks the affected predictor
component to transfer up to K ways, then enters a one-cycle quiesce state.

The initial policy is intentionally conservative:

```text
donor = scope with lowest utility and at least one transferable owned way
donee = scope with highest utility and not equal to donor
K = swayReallocWays, default 1
```

If no legal donor/donee pair exists, the controller records a blocked decision
without mutating ownership.

## Decision (ADR-lite)

**Context**: Task A already measures per-phase utility for the 10 SWAY scopes
without changing predictor behavior. Task B needs a logical PoC that is
credible enough for CAL-style evaluation but not a full RTL data-migration
model.

**Decision**: Use per-way owner registers plus valid-bit squash on owner
transfer. Keep SRAM data stationary, model 1-cycle quiesce, and gate all
behavior behind a default-off SimObject parameter.

**Consequences**: The model can show whether logical capacity reallocation is
directionally useful, while avoiding unsupported claims about physical
migration cost. Results below zero are still useful evidence that M5/M16
selection or K/quiesce policy needs another iteration.

## Out of Scope

- No physical SRAM migration.
- No RTL-level CAL datapath modeling.
- No MicroTAGE, ABTB, RAS, or fast predictor reallocation.
- No change to baseline mode behavior.
- No tuning by silently altering benchmarks, max instruction counts, or done
  criteria after seeing results.

## Technical Notes

- Task A source commit: `b99edc67a6` added `sway_utility_by_phase.csv`.
- Task A closeout commit: `bdcfcccb4e` recorded utility probe closeout.
- Task A controlled scopes: `mbtb_sram0`, `mbtb_sram1`, `tage_t0..tage_t7`.
- Task A donor/donee observations:
  - MBTB-donee cases: `gcc_pp_O2_1869`, `perlbench_diff_20021`,
    `perlbench_split_8843`.
  - Strong MBTB-donor / mid-TAGE-donee case: `mcf_17135`.
  - Deep-TAGE-donee case: `xalancbmk_6102`.
- This checkout does not have `.trellis/scripts/task.py` or
  `.trellis/scripts/get_context.py`; task files were created manually following
  the existing `.trellis/tasks/06-23-sway-utility-vector-impl/` layout.

## Open Questions

- Resolved for this PoC: donor/donee policy uses the simple M5/M16 shape
  requested by the task, selecting the lowest active-way utility donor and the
  highest active-way utility donee with optional hysteresis.
- Resolved for this PoC: keep K=1 as the main configuration. K=4 was evaluated
  as sensitivity and is worse on geomean and `mcf`.

## Final Evidence

- Unit tests:
  - `./build/RISCV/cpu/pred/btb/test/tage.test.opt`: 28/28 pass.
  - `./build/RISCV/cpu/pred/btb/test/btb.test.opt`: 19/19 pass.
- Build:
  - `scons build/RISCV/gem5.opt --gold-linker -j64`: pass.
  - `scons build/RISCV/gem5.opt --gold-linker -j32`: pass.
  - `scons build/RISCV/gem5.opt --gold-linker -j160`: failed in generated
    `params/*.hh` / `enums/*.hh` dependency race, not in SWAY source code.
- Baseline regression:
  - 10/10 `eval_baseline_W100K/main_W100K/*/sway_stranded_by_phase.csv`
    match Task A `utility_W100K/main_W100K` exactly.
- `gcc_pp_O2` smoke:
  - baseline IPC 1.214771, frontendBound 0.062114,
    branchMispredicts 84205, tage.updateMispred 38549,
    sway.reallocCount 0.
  - SWAY K=1 IPC 1.242839, frontendBound 0.061589,
    branchMispredicts 78579, tage.updateMispred 39724,
    sway.reallocCount 3, quiesce cycles 3.
  - ΔIPC = +2.31%.
- 10-workload K=1:
  - baseline: 10 completed, 0 abort.
  - SWAY: 10 completed, 0 abort.
  - geomean ΔIPC = +0.713%.
  - `gcc_pp_O2` ΔIPC = +2.31%.
  - `mcf` ΔIPC = -0.71%.
- K=4 sensitivity:
  - 10 completed, 0 abort.
  - geomean ΔIPC = -0.921%.
  - `mcf` ΔIPC = -11.56%.

## Next Iteration Recommendations

- Add a stronger donor guard: do not treat low active-way count as sufficient
  evidence that a TAGE table has low marginal utility.
- Add donor-donee cooldown and phase-stability tracking so the controller does
  not overfit a single phase sample.
- Add outcome-aware utility signals, such as BTB target misses, TAGE
  updateMispred/MPKI, or rollback-on-regression, before claiming paper-level
  speedup.
