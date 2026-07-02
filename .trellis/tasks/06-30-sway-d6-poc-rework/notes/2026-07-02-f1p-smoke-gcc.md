# Phase B-rev F1' smoke on gcc_pp_O2_1869

## Conclusion

F1' cond-aligned utility was implemented and the required gcc smoke completed.

The result is v4 Case C:

- `sway.reallocCount = 0`
- Whole-run F1' gap `TAGE - MBTB_cond = -0.225453`
- Per-phase mean F1' gap `TAGE - MBTB_cond = -0.223284`
- `0 / 49` phases have `new_utility_tage > new_utility_mbtb_cond`

Do not run `mcf_17135`; do not enter Phase C/D/E.

## Run

- Output directory: `runs/phase_b_rev_f1p_gcc_pp_O2/`
- Workload: `gcc_pp_O2_1869`
- Checkpoint:
  `/nfs/home/share/zyy/spec17-rv64gcb-O3-20m-gcc12.2.0-mix-with-special_wrf/checkpoint-0-0-0/gcc_pp_O2/1869/_1869_0.206095_.zstd`
- Params:
  `enableSwayRealloc=True`, `swayDoneeTableSet="3,7"`,
  `phaseSizeByInst=100000`, `maxinsts=5000000`
- Binary: `build/RISCV/gem5.opt`, rebuilt after F1' code changes.

Run completed normally:

- `simInsts = 5000005`
- `system.cpu.ipc = 1.220370`
- Exit reason: max instruction count reached.

## F1' rationale

- MBTB `condMisses / (condHits + condMisses)` measures conditional-branch BTB
  miss pressure on the branch subpopulation TAGE can affect.
- TAGE `updateMispred / updateFilteredEntries` remains the conditional-update
  mispredict pressure signal.

## Whole-run stats

From `runs/phase_b_rev_f1p_gcc_pp_O2/stats.txt`:

| metric | numerator | denominator | rate |
|---|---:|---:|---:|
| MBTB broad F1 `predMiss/(predMiss+predHit)` | 518011 | 2237746 | 0.231488 |
| MBTB F1' cond `condMisses/(condHits+condMisses)` | 259259 | 758648 | 0.341738 |
| TAGE F1 `updateMispred/updateFilteredEntries` | 38252 | 328949 | 0.116286 |
| TAGE - MBTB broad |  |  | -0.115202 |
| TAGE - MBTB cond |  |  | -0.225453 |

Other counters:

- `system.cpu.branchPred.sway.reallocCount = 0`
- all `sway.slotOwnerTransitionsPerSlot::* = 0`
- all `tage.swayBorrowedHitsByDonee::* = 0`

## CSV evidence

From `runs/phase_b_rev_f1p_gcc_pp_O2/sway_phase_a_diag.csv`:

- rows: 49
- `decision_counts = {'hold': 49}`
- `execution_counts = {'not_valid': 49}`
- `reallocCount min/max = 0/0`

F1' utility summary:

| metric | min | max | mean |
|---|---:|---:|---:|
| `new_utility_mbtb` | 0.055882 | 0.405279 | 0.211704 |
| `new_utility_mbtb_cond` | 0.232396 | 0.475315 | 0.340690 |
| `new_utility_ittage` | 0.005658 | 0.214754 | 0.087417 |
| `new_utility_tage` | 0.047255 | 0.226334 | 0.117407 |
| `new_utility_tage_t3` | 0.001005 | 0.008747 | 0.004387 |
| `new_utility_tage_t7` | 0.000000 | 0.000606 | 0.000073 |
| `new_utility_tage - new_utility_mbtb_cond` | -0.404871 | -0.120694 | -0.223284 |

Phase-count evidence:

- phases where `new_utility_tage > new_utility_mbtb_cond`: `0 / 49`
- best positive gap: `0.000000`
- tight hysteresis reference: `0.10`

## Gating

This is v4 Case C because the gcc mean F1' gap remains below `-0.05`.

Per v4, stop after this gcc note and write the F1' verdict note recommending
F2 ATD escalation.
