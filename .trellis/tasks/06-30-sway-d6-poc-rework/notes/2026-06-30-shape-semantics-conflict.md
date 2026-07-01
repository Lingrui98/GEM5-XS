# STOP: predictor shape semantics conflict

## Conclusion

D0 config values were confirmed, but D1 source research found that the PRD's
derived set-count interpretation conflicts with the current source code.
This affects the SWAY R1/R2a/R2b/R3/R4 pairing table, so implementation should
not proceed until glr decides which interpretation is authoritative.

## Evidence

- Baseline config:
  `/nfs/home/goulingrui/project/papers/sway-paper/runs/eval_baseline_W100K/main_W100K/gcc_pp_O2_1869/config.json`
  records:
  - MBTB `numEntries=8192`, `numWays=4`
  - BTBTAGE `tableSizes=[2048]*8`, `numWays=[2]*8`
  - BTBITTAGE `tableSizes=[256,256,512,512,512]`
- PRD currently interprets these as:
  - MBTB way: `S=2048`
  - BTBTAGE table: `S=1024`
  - ITTAGE T0/T1: `S=256`
  - ITTAGE T2/T3/T4: `S=512`
- Current source semantics:
  - `src/cpu/pred/btb/mbtb.cc`: `numSets = numEntries / (numWays * 2)`, so
    MBTB has 1024 sets per SRAM for `8192/(4*2)`.
  - `src/cpu/pred/btb/btb_tage.cc`: `tageTable[i].resize(tableSizes[i])`,
    then each index gets `getNumWays(i)` entries, so BTBTAGE has 2048
    indices/sets per table and 2 ways per index.
  - `src/cpu/pred/btb/btb_ittage.cc`: `tageTable[i].resize(tableSizes[i])`,
    so BTBITTAGE has 256/256/512/512/512 indices/sets.
- Existing baseline run artifacts also follow the current source semantics:
  `/nfs/home/goulingrui/project/papers/sway-paper/runs/eval_baseline_W100K/main_W100K/gcc_pp_O2_1869/sway_stranded_by_phase.csv`
  records `total_ways=4096` for `mbtb_sram0`, `mbtb_sram1`, and every
  `tage_t*` row. This matches MBTB `1024 sets * 4 ways` per SRAM and BTBTAGE
  `2048 indices * 2 ways` per table.

## Consequence if source semantics are used

- MBTB way `(S_d=1024, W=70)` to TAGE `(S_e=2048, W=18)` becomes R2b with
  `extra_tag=1`, not R2a with donor-larger extended index.
- ITTAGE T0/T1 `(S_d=256)` to TAGE `(S_e=2048)` has an 8x ratio and violates
  SPEC R3 (`ratio > 4` excluded).
- ITTAGE T2/T3/T4 `(S_d=512)` to TAGE `(S_e=2048)` is exactly 4x and remains
  legal under R2b with `extra_tag=2`.
- The PRD/SPEC statements that "No pair crosses R3" and that MBTB->TAGE uses
  R2a would be false for the current source implementation.

## Decision needed

Choose one:

1. Treat PRD/SPEC set counts as intended hardware semantics and change source
   interpretation or mapping so BTBTAGE donee effectively has `S_e=1024` for
   SWAY borrowed-bank lookup.
2. Treat current source code as ground truth and update PRD/SPEC pairing rules
   before D1/D2, likely disabling ITTAGE T0/T1 as donor candidates under R3.
3. Provide another intended mapping, for example grouping TAGE ways/banks so
   the SWAY donee index space differs from `tableSizes[i]`.

## Decision

glr confirmed option 2 on 2026-07-01: treat current source/runtime set-count
semantics as ground truth for the D6 PoC.

The implementation therefore uses:

- MBTB tight slot to TAGE: R2b with `extra_tag=1`, packing factor 3.
- ITTAGE T0/T1 to TAGE: R3-excluded because the ratio is 8x.
- ITTAGE T2/T3/T4 to TAGE: R2b with `extra_tag=2`, packing factor 2.

The task PRD and ExecPlan were updated to reflect this source-semantics
decision before D2 source edits continued.

## Current local state

- Branch created: `sway/d6-substrate-rework`.
- D0 checkbox was ticked based on config-value confirmation and ExecPlan
  creation before this source-level semantic conflict was found.
- No predictor source code behavior has been changed after finding this
  conflict.
- Additional runtime evidence was added from existing baseline CSVs; still no
  predictor source code behavior has been changed.
