# D1 source research before behavior edits

## Conclusion

The current SWAY code is the archived Task B shape: 10-scope owner IDs
(`mbtb_sram0`, `mbtb_sram1`, `tage_t0..tage_t7`) plus sidecar extra vectors.
The D6 substrate cannot be implemented by small parameter retuning; it needs a
new fixed donor-slot abstraction and controller legality checks. Because the
set-count conflict is unresolved, no predictor behavior was changed.

## Current source map

- `src/cpu/pred/btb/sway_realloc.hh`
  - Defines `MbtbSram0`, `MbtbSram1`, `TageBase`, `NumTageTables`,
    `NumScopes=10`, and owner helpers.
  - Needs D1 replacement/extension with component-level ownership, donor-slot
    descriptors, donee table set, and extra-bit-source enum.
- `src/cpu/pred/BranchPredictor.py`
  - Currently exposes `enableSwayRealloc`, `swayReallocWays`,
    `swayReallocHysteresis`, and `swayReallocTageDoneeHysteresis`.
  - Needs new default-off params: `swayDoneeTableSet`,
    `swayExtraBitSourcePerPair`, `swayEnableITTAGEDonor`,
    `swayEnableRelaxedSlot`.
  - `swayReallocTageDoneeHysteresis` must remain accepted but become
    warn-once + ignored.
- `src/cpu/pred/btb/mbtb.{hh,cc}`
  - Current Task B code has `swayOwner0/1` per native way and
    `swayExtra0/1` sidecar storage.
  - Lookup skips native ways whose owner is not the native MBTB scope and
    additionally scans `swayExtra*`.
  - D6 needs fixed MBTB donor slots instead of variable sidecar capacity and
    must route donated MBTB slots to TAGE borrowed-bank lookup.
- `src/cpu/pred/btb/btb_tage.{hh,cc}`
  - Current Task B code has `swayWayOwner[table][way]` and
    `swayExtraTable[table]`.
  - Lookup scans native ways first, then sidecar extra ways; allocation also
    considers sidecar entries.
  - D6 needs TAGE-only-donee borrowed-bank views, packing metadata, and
    no TAGE-as-donor or cross-TAGE transfers.
- `src/cpu/pred/btb/btb_ittage.{hh,cc}`
  - Currently has no SWAY owner/donation state and no `collectAndReset*`
    heatmap API.
  - It already has `predTableHits` and `updateTableHits` distributions.
  - D5 needs per-table provider/mispredict/active heatmap plumbing before
    PENDING-17 can be decided.
- `src/cpu/pred/btb/decoupled_bpred.{hh,cc}`
  - Constructor wires old params and calls `setSwayReallocEnabled()` on MBTB
    and TAGE only.
  - Needs parsing and validation of D6 params and pass-through of substrate
    config to MBTB/TAGE/ITTAGE.
- `src/cpu/pred/btb/decoupled_bpred_stats.cc`
  - Current controller picks lowest utility donor and highest utility donee
    over 10 scopes.
  - D4 needs component utility aggregation, legal 5-action enumeration,
    per-donee cooldown, tight/relaxed hysteresis, and new stats.

## Unit-test hooks found

- `src/cpu/pred/btb/test/btb.test.cc` has 19 existing MBTB tests and unit-test
  constructors that can exercise default-off behavior.
- `src/cpu/pred/btb/test/btb_tage.test.cc` has 28 existing BTBTAGE tests, and
  unit-test mode exposes otherwise private members in `btb_tage.hh`.
- `src/cpu/pred/btb/test/SConscript` builds `btb.test` and `tage.test`
  separately, matching the D2/D3 validation gates.

## Safe preparatory work completed before shape resolution

The first source patch was kept limited to D1 scaffold:

1. Added component and donor-slot types in `sway_realloc.hh`.
2. Added Python params with default values that preserve baseline behavior.
3. Parsed `swayDoneeTableSet` in the top-level BPU and kept
   `enableSwayRealloc=False` as the default.
4. Ignored deprecated `swayReallocTageDoneeHysteresis` with a warn-once log.
5. Added a temporary legality filter so the old controller cannot choose TAGE
   as a donor; empty `swayDoneeTableSet` now means no SWAY transfer.
6. Did not replace MBTB/TAGE lookup paths; R2a/R2b/R3 mapping remains blocked
   on glr's shape-semantics decision.

## D1 validation

- `scons build/RISCV/cpu/pred/btb/test/btb.test.opt build/RISCV/cpu/pred/btb/test/tage.test.opt --unit-test -j32`: pass.
- `./build/RISCV/cpu/pred/btb/test/btb.test.opt --gtest_brief=1`: 19/19 pass.
- `./build/RISCV/cpu/pred/btb/test/tage.test.opt --gtest_brief=1`: 28/28 pass.
- `scons build/RISCV/gem5.opt --gold-linker -j32`: pass.
