# v2 STOP audit after Direction X

## Conclusion

The v2 prompt is blocked at the explicit Phase A/B hard gate.

Do not start Phase C, Phase D, or Phase E until glr chooses a new design
direction or explicitly overrides the gate.

## Prompt gate

The active prompt says:

> If Phase A / B iteration cannot get reallocCount > 0 on gcc_pp_O2 smoke,
> STOP, escalate to glr with utility trace analysis.

Current evidence satisfies the STOP condition:

- Phase A Step 2 ran `gcc_pp_O2_1869` for 5M instructions and produced
  `reallocCount=0`.
- Phase B Direction X changed donor utility to MBTB tight-slot utility, rebuilt
  `gem5.opt`, reran the same 5M smoke, and still produced `reallocCount=0`.
- Both diagnostic CSVs have 49 phase rows; every row is `decision=hold` and
  `execution=not_valid`.

## Current evidence

Artifacts:

- Phase A trace:
  `runs/phase_a_diag/gcc_pp_O2_utility_trace.csv`
- Phase A stats:
  `runs/phase_a_diag/stats.txt`
- Phase A analysis:
  `.trellis/tasks/06-30-sway-d6-poc-rework/notes/2026-07-01-utility-trace-analysis.md`
- Direction X stats and CSV:
  `runs/phase_b_direction_x_gcc_pp_O2/`
- Direction X analysis:
  `.trellis/tasks/06-30-sway-d6-poc-rework/notes/2026-07-01-direction-X-experiment.md`

Key values confirmed on 2026-07-02:

| run | simInsts | reallocCount | rows | decisions | executions |
|---|---:|---:|---:|---|---|
| Phase A Step 2 | 5000005 | 0 | 49 | hold=49 | not_valid=49 |
| Phase B Direction X | 5000005 | 0 | 49 | hold=49 | not_valid=49 |

Both runs also show TAGE update activity:

- `system.cpu.branchPred.tage.updateFilteredEntries = 328949`
- `system.cpu.branchPred.tage.updateMispred = 38252`

Direction X gap from `runs/phase_b_direction_x_gcc_pp_O2/sway_phase_a_diag.csv`:

- `TAGE_component - coldest_MBTB_tight_slot` min `-0.221954`
- max `-0.006683`
- mean `-0.116956`

This means TAGE aggregate utility never beats even the coldest MBTB tight donor
slot, so Direction X alone cannot trigger a donation under the current 0.10
tight hysteresis.

## Requirement-by-requirement status

| Requirement | Status | Evidence |
|---|---|---|
| Phase A Step 1 controller verification | Done | `2026-07-01-controller-verify.md` |
| Phase A Step 2 utility trace | Done | `runs/phase_a_diag/gcc_pp_O2_utility_trace.csv` |
| Phase A analysis note | Done | `2026-07-01-utility-trace-analysis.md` |
| Phase B Direction X implementation | Done | `SwayController::chooseReallocation()` now uses MBTB tight-slot donor utility |
| Direction X smoke expects `reallocCount > 0` | Failed | `runs/phase_b_direction_x_gcc_pp_O2/stats.txt` has `reallocCount=0` |
| Continue to Phase C/D/E | Blocked | v2 hard STOP condition applies |

## Current build/check status

Commands run on 2026-07-02:

```sh
git diff --check
python3 -m json.tool .trellis/tasks/06-30-sway-d6-poc-rework/task.json
scons build/RISCV/gem5.opt --gold-linker -j32
```

Results:

- `git diff --check`: passed.
- `task.json` JSON parse: passed.
- `scons`: passed; `build/RISCV/gem5.opt` is up to date.
- Warnings were limited to missing HDF5 and backtrace support.

## Handoff options for glr

1. Switch controller utility to an outcome-aware signal, such as TAGE
   mispredict/update pressure or marginal MPKI benefit, then rerun Phase B.
2. Accept the active-way utility controller as a negative conserved-substrate
   result and move to the SPEC fallback narrative.
3. Explicitly override the v2 gate and run Phase C/D/E despite
   `reallocCount=0`, with the known risk that the result remains baseline-noise
   rather than an exercised SWAY mechanism.
