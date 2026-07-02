# Phase B Direction X experiment

## Conclusion

Direction X was implemented and smoke-tested, but it did **not** make the D6
controller trigger a transfer on `gcc_pp_O2_1869`.

This hits the v2 prompt STOP condition:

> If Phase A / B iteration cannot get reallocCount > 0 on gcc_pp_O2 smoke,
> STOP, escalate to glr with utility trace analysis.

No Phase C/D/E sweep should be launched until glr decides the next design
direction.

## Code change tested

Controller donation now uses the prompt's Direction X utility semantics:

- donor utility: MBTB tight donor slot's own
  `active_of_specific_way / total_of_specific_way`
- donee utility: aggregate TAGE component utility

The implementation keeps the rest of the controller unchanged:

- same `swayDoneeTableSet="3,7"`
- same tight hysteresis `0.10`
- same cooldown / quiesce behavior
- no SPEC v2 rewrite
- no hyperparameter relaxation

## Validation run

- Output directory:
  `runs/phase_b_direction_x_gcc_pp_O2/`
- Workload: `gcc_pp_O2_1869`
- Checkpoint:
  `/nfs/home/share/zyy/spec17-rv64gcb-O3-20m-gcc12.2.0-mix-with-special_wrf/checkpoint-0-0-0/gcc_pp_O2/1869/_1869_0.206095_.zstd`
- Command source:
  `/nfs/home/goulingrui/project/papers/sway-paper/runs/eval_sway_W100K_d6/donee_T3T7/gcc_pp_O2_1869/_run_backend.sh`
- Effective params:
  `enableSwayRealloc=True`, `swayDoneeTableSet="3,7"`,
  `phaseSizeByInst=100000`, `maxinsts=5000000`
- Build before run:
  `scons build/RISCV/gem5.opt --gold-linker -j32` passed.

`stats.txt` evidence:

- `simInsts = 5000005`
- `system.cpu.ipc = 1.220370`
- `system.cpu.branchPred.sway.reallocCount = 0`
- all `sway.slotOwnerTransitionsPerSlot::* = 0`
- all `tage.swayBorrowedHitsByDonee::* = 0`
- `system.cpu.branchPred.tage.updateFilteredEntries = 328949`
- `system.cpu.branchPred.tage.updateMispred = 38252`

`sway_phase_a_diag.csv` evidence:

- rows: 49
- `decision_counts = {'hold': 49}`
- `execution_counts = {'not_valid': 49}`
- `reallocCount min/max = 0/0`
- `TAGE_component - coldest_MBTB_slot`:
  - min `-0.221954`
  - max `-0.006683`
  - mean `-0.116956`
- phases where `TAGE_component - coldest_MBTB_slot > 0.10`: `0 / 49`

## Interpretation

Direction X correctly removes aggregate-MBTB masking from the donor side, but
the selected workload still has TAGE aggregate utility below even the coldest
MBTB tight slot in every phase. Therefore:

- RC6 "aggregate MBTB hides a cold donor slot" is not the dominant cause for
  this smoke.
- RC1 "hysteresis too strict" is also not enough: even a 0.05 tight margin
  would not trigger because the best Direction-X gap is still negative.
- The remaining issue is a deeper RC5 utility semantics mismatch: active-entry
  density is not an adequate donee-benefit signal for deciding when to move
  capacity into TAGE.

## STOP recommendation for glr

Do not proceed to the 180-run Phase C or the held-out Phase D suite yet.

Recommended decision point:

1. Switch controller utility to an outcome-aware signal, e.g. TAGE
   mispredict/update pressure or marginal MPKI benefit, before rerunning
   Phase B.
2. Or explicitly accept that the active-way utility controller is a negative
   D6 result and walk the SPEC §8.3 fallback narrative.
3. Or override the prompt and run Phase C despite `reallocCount=0`, but that
   would only reproduce baseline-noise data and would not satisfy the stated
   root-cause gate.
