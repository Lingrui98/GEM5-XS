# Phase A Step 2 utility trace analysis

## Conclusion

Phase A Step 2 reproduced the `reallocCount=0` symptom and ruled out a
controller stub / owner-mux execution bug for this `gcc_pp_O2_1869` smoke.

Run:

- Workload: `gcc_pp_O2_1869`
- Checkpoint:
  `/nfs/home/share/zyy/spec17-rv64gcb-O3-20m-gcc12.2.0-mix-with-special_wrf/checkpoint-0-0-0/gcc_pp_O2/1869/_1869_0.206095_.zstd`
- Config: `enableSwayRealloc=True`, `swayDoneeTableSet="3,7"`,
  `phaseSizeByInst=100000`, `maxinsts=5000000`
- Output:
  `runs/phase_a_diag/gcc_pp_O2_utility_trace.csv`

The CSV has 49 phase rows. Every row is:

- `decision=hold`
- `execution=not_valid`
- `moved_ways=0`
- `reallocCount=0`

`stats.txt` confirms `simInsts=5000005`,
`system.cpu.branchPred.sway.reallocCount=0`,
`system.cpu.branchPred.tage.updateFilteredEntries=328949`, and
`system.cpu.branchPred.tage.updateMispred=38252`. TAGE update is alive in
this CPT smoke; the failure is controller policy/input, not a silent predictor.

## Utility summary

Computed from `runs/phase_a_diag/gcc_pp_O2_utility_trace.csv`:

| metric | min | max | mean |
|---|---:|---:|---:|
| MBTB component utility | 0.0493164 | 0.321899 | 0.187759 |
| TAGE component utility | 0.0140076 | 0.0735474 | 0.0450334 |
| TAGE - MBTB component | -0.263092 | -0.0101013 | -0.142726 |
| max(T3,T7) utility | 0.0124512 | 0.109131 | 0.0493463 |
| max(T3,T7) - MBTB component | -0.275024 | 0.005127 | -0.138413 |
| coldest MBTB tight-slot utility | 0.0458984 | 0.274414 | 0.161990 |
| max(T3,T7) - coldest MBTB tight slot | -0.212890 | 0.008545 | -0.112644 |

No phase crosses the current tight hysteresis threshold:

- phases where `max(T3,T7) - MBTB_component > 0.10`: 0 / 49
- phases where `max(T3,T7) - coldest_MBTB_slot > 0.10`: 0 / 49

## Root-cause classification

- RC7 controller stub: **not supported**. Step 1 proved the call chain; Step 2
  recorded one controller trace row per phase.
- RC4 owner-gated semantic bug: **not supported by this run**. No valid
  transfer decision was generated, so the execution path was not asked to move
  a slot.
- RC1 hysteresis too strict: **not supported**. The best donee-vs-donor gap is
  only `0.008545` even against the coldest MBTB tight slot; relaxing 0.10 to
  0.05 would still not trigger.
- RC6 aggregate MBTB utility hides a cold donor slot: **not supported for
  `gcc_pp_O2_1869`**. The donor slots are also active/hot; per-slot utility is
  lower than component utility in some phases but never low enough to make
  T3/T7 win.
- RC5 utility semantic mismatch: **supported**. The current active-way utility
  formula says MBTB is more demanded than TAGE for almost the entire smoke, so
  a utility-maximizing controller correctly holds. This conflicts with the
  broader SWAY goal of moving capacity into TAGE when outcome/mispredict data
  suggests deep TAGE capacity could help.

## Phase B implication

Per the v2 prompt, the next required experiment for RC5/RC6 is Direction X:

`donor utility = active_of_donor_slot_specific_way / total_of_that_way`

and

`donee utility = per-component aggregate`.

This trace predicts Direction X alone will still not trigger on
`gcc_pp_O2_1869`, because TAGE component utility is below both MBTB component
utility and the MBTB tight-slot utility. I will still run Direction X as the
specified Phase B experiment; if the smoke keeps `reallocCount=0`, the prompt's
STOP condition applies.
