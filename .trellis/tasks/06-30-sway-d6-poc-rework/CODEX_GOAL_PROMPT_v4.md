# CODEX_GOAL_PROMPT v4 — F1' cond-aligned utility rewrite

> **Supersedes v3 Phase B-rev formula only.** Phase A / Phase B wiring / Phase C/D/E semantics from v2 + v3 remain in force. This v4 is scoped to fix the F1 stat-comparability bug that triggered the 2026-07-02 STOP.

## Context

Phase B-rev Step 2 (gcc_pp_O2_1869, `notes/2026-07-02-f1-smoke-gcc.md` + `notes/2026-07-02-f1-verdict.md`) triggered v3 hard STOP:

- MBTB F1 miss rate 0.2315 vs TAGE F1 mispredict rate 0.1163 → gap −0.115
- `reallocCount = 0`, 47/49 phases have TAGE < MBTB.

Root cause per Step 2 analysis: MBTB numerator `predMiss` counts BTB misses on **all branch types** (cond + uncond + indirect + return), while TAGE numerator only counts mispredicts on **conditional branches that BTB already hit AND got filtered into TAGE update**. Different populations → not a fair "capacity pressure" comparison.

glr chose path 1 (F1' cond-aligned rewrite). Path 2 (F2 ATD) and path 3 (Q-U4) are held in reserve.

## F1' formula (this is the ONLY change from v3)

Restrict MBTB donor pressure to the conditional-branch subpopulation, so both numerator and denominator are conditional-branch counts:

```
new_utility_mbtb_cond = delta(condMisses) / max(1, delta(condHits) + delta(condMisses))
```

where `condHits` / `condMisses` come from MBTB's existing `btbStats` (see `src/cpu/pred/btb/mbtb.cc:1350,1394` for increment sites and `mbtb.cc:1448,1451` for stat definitions). No new stat wiring required.

TAGE donee pressure unchanged:

```
new_utility_tage      = delta(updateMispred) / max(1, delta(updateFilteredEntries))
new_utility_tage_tK   = delta(updateTableMispreds[K]) / max(1, delta(updateFilteredEntries))
```

Rationale to include in the smoke note (for glr's paper-defense record, not editorial):

- MBTB `condMisses` = conditional branches where BTB has no entry yet → BTB **capacity** pressure on the very population TAGE could ever help
- TAGE `updateMispred / updateFilteredEntries` = mispredicts on conditional branches where BTB hit and TAGE fired → TAGE **capacity** pressure
- Both denominators are now conditional-branch subpopulations. Populations are still not identical (MBTB's = never-seen conditional; TAGE's = seen-and-tage-eligible), but this is apples-to-apples "capacity pressure per conditional branch of that stage" — the F1 architectural signal that Phase B-rev intended.

## Code changes (minimal diff)

1. `DecoupledBPUWithBTB::SwayController::chooseReallocation()` — replace the `new_utility_mbtb` field consumed by the donate predicate with `new_utility_mbtb_cond`. Keep the computation of `new_utility_mbtb` (broad F1) purely as CSV audit column, do NOT delete.
2. `sway_phase_a_diag.csv` — **add** `new_utility_mbtb_cond` column between `new_utility_mbtb` and `new_utility_ittage`. Do not rename or remove any existing column. This preserves Phase A + v3 Step 2 audit chain intact.
3. `MBTB::getCondHitsStat()` / `getCondMissesStat()` — add two narrow scalar getters, same pattern as v3 Step 1 `getPredHitStat()` / `getPredMissStat()`. Do NOT expose the whole `btbStats` group.
4. Prev-cumulative snapshot fields in SwayController — add `prev_condHits_[mbtb_idx]`, `prev_condMisses_[mbtb_idx]`. Reset at the same site where v3 resets `prev_predMiss` / `prev_predHit`.

Nothing else in v3 Step 1 changes: TAGE aggregate + per-table utilities, D-1 direction Y alloc priority, D-Return T2 cool-down expiry re-eval — all preserved.

## Validation (run in order, STOP at first failure)

1. `git diff --check`
2. `scons build/RISCV/gem5.opt --gold-linker -j32` (do NOT use -j160 — header race)
3. `./build/RISCV/cpu/pred/btb/test/btb.test.opt` — must be 21/21
4. `./build/RISCV/cpu/pred/btb/test/tage.test.opt` — must be 33/33
5. Rerun Phase B-rev Step 2 gcc_pp_O2_1869 into a NEW dir `runs/phase_b_rev_f1p_gcc_pp_O2/` (do NOT overwrite the v3 dir; needed for audit diff)
   - Same params: `enableSwayRealloc=True`, `swayDoneeTableSet="3,7"`, `phaseSizeByInst=100000`, `maxinsts=5000000`
   - Same checkpoint: gcc_pp_O2 1869 (path in `notes/2026-07-02-f1-smoke-gcc.md`)
6. Write `notes/2026-07-XX-f1p-smoke-gcc.md` with:
   - Whole-run MBTB cond-miss rate, TAGE mispredict rate, gap
   - CSV row count, decision_counts, execution_counts, reallocCount min/max
   - F1' utility summary table (min/max/mean of `new_utility_mbtb_cond`, `new_utility_tage`, `new_utility_tage - new_utility_mbtb_cond`)
   - Count of phases where `new_utility_tage > new_utility_mbtb_cond`
   - Best positive gap; compare to 0.10 tight hysteresis

## Gating on gcc F1' result

**Case A — gcc gap flips positive AND reallocCount > 0** (majority of phases have TAGE > MBTB_cond, or best gap > 0.10):
- Proceed to Phase B-rev Step 3 mcf_17135 in `runs/phase_b_rev_f1p_mcf/`, write `notes/2026-07-XX-f1p-smoke-mcf.md`
- Then write `notes/2026-07-XX-f1p-verdict.md` classifying combined result as α/β/γ per v3 §"Phase B-rev Step 4"
- STOP. Do NOT enter Phase C without glr approval.

**Case B — gcc gap improves but still MBTB_cond > TAGE, reallocCount still 0** (e.g. mean gap in [−0.05, 0], best gap in [0.02, 0.08]):
- Do NOT rerun mcf. Do NOT sweep hysteresis.
- Write `notes/2026-07-XX-f1p-verdict.md` describing partial improvement and STOP.
- Interpretation: even cond-restricted MBTB pressure > TAGE pressure on this workload starts to look like a P3'-structural finding rather than an F1 formula bug. glr will decide F2 vs Q-U4.

**Case C — gcc still shows large negative gap (mean < −0.05)** (F1' also fails):
- Do NOT rerun mcf. Do NOT sweep hysteresis.
- Write `notes/2026-07-XX-f1p-verdict.md` recommending escalation to F2 ATD.
- STOP.

## Hard constraints (unchanged from v3)

- `swayReallocTageDoneeHysteresis=0.45` **deprecated**, log warn if set (do not use).
- P3' TAGE-only-donee enforced at design time — do not add MBTB or ITTAGE as donee.
- No hyperparameter tuning (hysteresis / cooldown / thresholds) without held-out justification. F1' formula rewrite is the ONLY tuning permitted this round.
- `enableSwayRealloc=False` path must remain bit-exact to baseline (btb.test.opt gates this).
- Do NOT modify `docs/SWAY_MECHANISM_SPEC_v2.md`, `docs/DESIGN_DECISIONS_PENDING_2026-07-01.md`, or `INSIGHT.yaml`. SPEC rev only after glr approves F1' verdict per Q-U3.
- Do NOT proceed to Phase C 180-run sweep, Phase D validation, or Phase E reporting under any case.

## Reporting contract

Verdict note must include:
1. F1' formula diff (v3 → v4), 2 lines max
2. Whole-run stats table (both formulas' rates for cross-check)
3. CSV summary table
4. Case classification A / B / C
5. Recommended next action for glr

No editorial commentary in verdict. Data + case + one-line recommendation.
