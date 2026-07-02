# CODEX GOAL PROMPT v3 — F1 miss-rate utility smoke (before committing to F2 ATD)

> **Date**: 2026-07-02
> **Supersedes**: v2 prompt Phase B directive only
> **Preserves**: v2 prompt Round 1 decisions (D-Return / D-1 方向 Y / D-4 / D-7), E-1/E-2/E-3 protocol, Phase C/D/E structure, all STOP conditions
> **Reader**: codex CLI, continuing from Phase B (Direction X) STOP audit dated 2026-07-02

---

## Why this rev exists (根本原因)

v2 Phase A Step 2 + Phase B Direction X both closed with `reallocCount=0`.
Cross-checked evidence in `notes/2026-07-01-utility-trace-analysis.md` +
`notes/2026-07-01-direction-X-experiment.md` +
`notes/2026-07-02-v2-stop-audit.md`:

- RC7 controller stub → **excluded** (Step 1 verified call chain)
- RC4 owner-gated exec bug → **excluded** (no valid decision ever generated)
- RC1 hysteresis too strict → **excluded** (best gap only -0.007, even 0.05 threshold would not trigger)
- RC6 aggregate MBTB masking → **excluded** (Direction X's per-slot donor utility still lost)
- **RC5 utility semantic mismatch → confirmed dominant cause**

Data (gcc_pp_O2_1869, 49 phases):

| Component / table | avg active/total utility | meaning |
|---|---:|---|
| MBTB | 0.188 | 19% way访问率 |
| TAGE aggregate | ~0.045 | 4.5% |
| T3 (hist=29) | 0.049 | 5% |
| T7 (hist=397) | **0.001** | **0.1%** |

**T7 utility=0.001 是 TAGE 长表本征特性**——只有当短表都不能预测时才 fall
through 到 T7，命中稀不代表"不缺容量"。**`active_ways / total_ways` 这个信号从
根本上度量错了**：它反映的是"最近 phase 内被访问过的 way 密度"，与
"该 component 是否会从借入容量获益" **零相关甚至反相关**。

任何 hysteresis / cool-down / alloc priority / per-slot vs per-component 调整
都救不回来。**必须换 utility 公式本身**。

**glr 2026-07-02 决定**：先走 **F1 (miss/mispredict rate)** 因为它 0.5-1 day
工程量，复用现有 stat；F1 若成功 → 继续 Phase C/D/E；F1 若失败 → 升级到 F2
(ATD marginal MPKI, 3-5 day)。**本 prompt 只覆盖 F1 阶段**。

---

## What changes in this rev (vs v2 prompt)

**Change**:

- SPEC v2 §4.2 utility formula: `active_ways / total_ways` → **miss/mispredict
  rate** (see §"F1 utility formula" below)
- Controller `chooseReallocation()` reads new utility signal
- Phase A diagnostic CSV extends to include the new signal alongside the old one
  (for auditability)

**Unchanged from v2 prompt (all Round 1 decisions and evaluation protocol
carry forward)**:

- D-Return T2 primary / T3 regression / T4 donor pressure / W-a cold-reset
- D-1 alloc priority 方向 Y (borrowed 与 native 平等)
- D-4 ITTAGE time-share (single port + owner mux)
- D-7 B4 static-resize baseline (加进 §V)
- E-1 seed protocol (3 seeds × instr-count-weighted SimPoint aggregation,
  perlbench_diff / perlbench_split 保持独立 workload)
- E-2 held-out A (7 traces, primary) + held-out B (5 SPEC17 disjoint, codex
  selects and confirms back to glr)
- E-3 3-arm bracket (Arm 1 archived / Arm 2 sidecar / Arm 3 D6 main),
  upgraded to D6 debug tool
- Phase C 5 donee_set candidates sweep
- Phase D/E structure
- All STOP conditions

**Explicitly not changed**:

- P3' TAGE-only-donee (P3' 是否结构性错误留待 Q-U4 重新审查, 不在本 rev)
- SPEC v2 §4.1 5-action enumeration (只 T2 replaces old idle-driven Action 5)
- Fix 1/2/3 useful-bit lifecycle
- SPEC v2 §3 R1/R2a/R2b/R3/R4/R5 index/packing rules

---

## F1 utility formula (definitive spec for this rev)

**Signal source** (all existing gem5 stats, no new hardware modeling required):

- MBTB miss: `mbtb->btbStats.predMiss` (`src/cpu/pred/btb/mbtb.hh:498`)
- MBTB hit:  `mbtb->btbStats.predHit`  (`src/cpu/pred/btb/mbtb.hh:499`)
- TAGE mispredict: `tage->tageStats.updateMispred`
  (`src/cpu/pred/btb/btb_tage.hh:464`)
- TAGE filtered cond entries (denominator candidates):
  `tage->tageStats.updateFilteredEntries`
  (`src/cpu/pred/btb/btb_tage.hh:474`)
- TAGE per-table mispredict:
  `tage->tageStats.updateTableMispreds[t]`
  (`src/cpu/pred/btb/btb_tage.hh:517`)
- ITTAGE side: locate corresponding predHit / predMiss stats in `btb_ittage.hh`
  during D5; if absent, mirror TAGE-side stats

**Utility definition**:

```
utility(MBTB, phase p) =
    (predMiss_delta_this_phase) / (predMiss_delta + predHit_delta)
    # per-phase delta = current cumulative - snapshot at prev phase boundary

utility(TAGE, phase p) =
    (updateMispred_delta_this_phase) / max(1, updateFilteredEntries_delta_this_phase)

utility(TAGE_T_k, phase p) =
    (updateTableMispreds_delta[k]_this_phase) / max(1, updateFilteredEntries_delta_this_phase)
    # normalized to TAGE overall访问数, not per-table access counts,
    # because per-table access counts confound "how often T_k is provider" with
    # "whether T_k mispredict when it is provider"; using overall denominator
    # keeps utility comparable across tables

utility(ITTAGE, phase p) =
    (predMiss_delta_this_phase) / (predMiss_delta + predHit_delta)
    (fallback: use ITTAGE resolveTotal - resolveCorrect if predHit/predMiss absent)
```

**Semantic**: **higher utility = more mispredict/miss pressure = needs more
capacity**. Controller decides to transfer TO the component with highest utility,
FROM the component with lowest utility. Under P3' constraint, only wide→TAGE
direction is legal; controller triggers `MbtbToTageTight` when
`utility(TAGE_donee) - utility(MBTB) > hysteresis_margin_tight (=0.10)`.

**Phase snapshot delta computation**:

- Controller maintains per-component "prev cumulative" snapshot updated at end
  of each `chooseReallocation()` call.
- utility for phase p = (current cumulative - prev cumulative) / (denominator delta)
- First phase's utility uses 0 as prev snapshot (i.e., counts since simulation
  start until first phase boundary).

**Do NOT delete the old `active_ways / total_ways` calculation**:

- Keep it in the CSV as a comparison column (old_utility_MBTB, old_utility_TAGE, etc.)
- New CSV columns: new_utility_MBTB, new_utility_TAGE, new_utility_TAGE_T3,
  new_utility_TAGE_T7 (add as many donee tables as configured)
- Controller decision uses NEW utility; CSV records both for cross-check

---

## Execution plan (this rev)

### Phase B-rev Step 1 | F1 controller wiring (~0.5 day)

- Modify `SwayController::chooseReallocation()` in `decoupled_bpred_stats.cc`:
  - Add per-component cumulative snapshots (MBTB predMiss/predHit, TAGE
    updateMispred/updateFilteredEntries, per-table mispreds, ITTAGE)
  - Compute F1 utility as delta-since-prev-phase (formula above)
  - Replace utility comparison in transfer decision predicate with new signal
  - Keep old active/total computation alive for CSV audit
- Extend `sway_phase_a_diag.csv` header with new_utility_* columns
- Preserve all Round 1 decisions:
  - D-1 alloc priority already 方向 Y in current code (verify with `grep -n
    "scan_native.*invalid\|scan_borrowed.*invalid" src/cpu/pred/btb/btb_tage.cc`;
    if there's still a native-first ordering, restore to baseline三级 平等)
  - D-Return T2 cool-down expiry re-eval (should already be there from v2
    Direction X work; verify by reading `SwayController::chooseReallocation`
    control flow for cooldown handling)
  - D-Return T3/T4 wire the stat infra but keep opt-in (default disabled;
    enable in Phase C sensitivity if needed)

Build:
```sh
scons build/RISCV/gem5.opt --gold-linker -j32
```

Unit test smoke (do NOT skip):
```sh
./build/RISCV/cpu/pred/btb/test/btb.test.opt
./build/RISCV/cpu/pred/btb/test/tage.test.opt
```

Expected: 19/19 and 28/28 pass (baseline behavior bit-exact preserved).

### Phase B-rev Step 2 | Single-workload smoke on gcc_pp_O2_1869

- Run same command as v2 Phase A Step 2 with the newly built binary:
  ```
  workload: gcc_pp_O2_1869
  checkpoint: /nfs/home/share/zyy/spec17-rv64gcb-O3-20m-gcc12.2.0-mix-with-special_wrf/checkpoint-0-0-0/gcc_pp_O2/1869/_1869_0.206095_.zstd
  params: enableSwayRealloc=True, swayDoneeTableSet="3,7", phaseSizeByInst=100000, maxinsts=5000000
  ```
- Output directory: `runs/phase_b_rev_f1_gcc_pp_O2/`
- Expected outcome (glr's expectation, based on Phase A data):
  - gcc_pp_O2 TAGE mispredict rate ~ 12% (updateMispred/updateFilteredEntries
    ≈ 38252/328949)
  - MBTB miss rate typically < 5% for gcc-class workloads
  - F1 gap: TAGE utility (~0.12) - MBTB utility (~0.03) = +0.09
  - **Marginally below** tight hysteresis 0.10, but should trigger occasionally
  - If reallocCount > 0 → Phase B-rev succeeds, proceed to Step 3
  - If reallocCount = 0 → check CSV: is TAGE utility indeed higher than MBTB? if
    yes but < 0.10 hysteresis, try lower threshold **as diagnostic only**
    (still write result to notes, do NOT accept as final config); if TAGE
    utility is not higher than MBTB, that is a **BIGGER** problem that
    invalidates F1 → escalate to glr immediately

Write findings to
`.trellis/tasks/06-30-sway-d6-poc-rework/notes/2026-07-02-f1-smoke-gcc.md`
with:
- decision counts (hold / transfer / cooldown-blocked / etc.)
- reallocCount cumulative
- IPC delta vs baseline
- F1 utility min/max/mean for MBTB / TAGE / T3 / T7
- gap TAGE - MBTB min/max/mean
- side-by-side old vs new utility scatter (if useful)

### Phase B-rev Step 3 | Broader smoke on mcf_17135

Only if Step 2 shows reallocCount > 0 on gcc_pp_O2.

- Run same params on `mcf_17135` checkpoint
- mcf is expected to behave differently:
  - mcf is MBTB-donor / mid-TAGE-donee in Task A profile
  - MBTB miss rate typically HIGHER than gcc (indirect + memory workload)
  - TAGE mispredict rate also HIGH (mcf 数据依赖 branches 难预测)
  - Gap may be smaller or negative — need to see
- Report findings to `notes/2026-07-02-f1-smoke-mcf.md`
- Expected: reallocCount pattern differs from gcc (this is normal, not a bug)

### Phase B-rev Step 4 | Decision point

Analyze Step 2+3 combined:

**Case α**: Both workloads show reallocCount > 0 with sensible transfer
patterns (transfers concentrate in phases where TAGE utility genuinely spikes)
→ **F1 works, proceed to Phase C 180-run sweep**. Report to glr with proposal
to lock F1 as SPEC v2 §4.2 rewrite candidate.

**Case β**: Only gcc succeeds, mcf still 0 → **F1 partial success**. Report to
glr with:
- Per-phase F1 utility trace for mcf
- Hypothesis: mcf's high MBTB miss rate correctly signals MBTB is under
  capacity pressure → controller correctly refuses to donate → this is P3'
  structural limitation, not F1 bug
- Decision needed: escalate to F2 ATD (marginal MPKI captures a different
  signal) or accept partial coverage.

**Case γ**: Both fail → **F1 is inadequate** → escalate to F2 ATD or Q-U4
"重新审查 P3'". Do NOT try random hyperparameter tweaks; that is hand-tuning.

**In all cases**: write decision recommendation to
`notes/2026-07-02-f1-verdict.md` and STOP; do not proceed to Phase C without
glr approval on F1 verdict.

---

## Phase C/D/E carry-forward (unchanged from v2 prompt, effective if Phase B-rev succeeds)

If F1 succeeds:

- **Phase C**: 180 runs, 5 donee_set × 10 workload × 3 seed × baseline + SWAY.
  Use F1 utility. Output `runs/eval_sway_W100K_d6_v2/`.
- **Phase D**: full validation (held-out A traces, held-out B disjoint SPEC17,
  E-3 3-arm bracket, B4 static-resize).
- **Phase E**: aggregation + reporting + SPEC v2 rev proposal (glr reviews and
  merges).

All Phase C/D/E details identical to v2 prompt. Do NOT re-derive; refer to
`CODEX_GOAL_PROMPT_v2.md` §"Phase C/D/E".

---

## Hard constraints (STOP conditions, all inherited from v2)

- **If Phase B-rev Step 2 shows F1 controller isn't wired correctly**
  (e.g., decision predicate still uses active/total) → STOP, fix, retry.
- **If Phase B-rev Step 2 shows TAGE mispredict rate LOWER than MBTB miss rate
  on gcc_pp_O2** → this is unexpected given predictor statistics; STOP and
  escalate, likely a stat aggregation bug.
- **If any workload smoke shows reallocCount > 0 but IPC delta is negative
  and large (e.g., < -1%)** → STOP, that's a working controller but a bad
  transfer decision; escalate before doing 180-run sweep.
- **Trace-mode TAGE update regression on SWAY branch** (D5 verify) → STOP,
  open PENDING-24, do NOT downgrade held-out A.
- **Hyperparameter tuning without held-out justification** → forbidden.
- **`swayReallocTageDoneeHysteresis=0.45`** deprecated → log warn if set.
- **P3' TAGE-only-donee** enforced at design time.
- **`enableSwayRealloc=False`** → bit-exact baseline (`btb.test.opt` and
  `tage.test.opt` must pass unchanged).

---

## Build & environment (unchanged)

- Branch: `sway/d6-substrate-rework` (based on `sway/phase-stranded-probe`)
- Build: `scons build/RISCV/gem5.opt --gold-linker -j32` (avoid -j160 header
  race)
- DRAMsim3 symlink: `ext/dramsim3/DRAMsim3 → ~/project/GEM5/ext/dramsim3/DRAMsim3`
- Scheduler: mytools `tools/distributed-trace-scheduler/distributed_trace_scheduler.py`
  with `scripts/run_cpt_gem5.sh` backend

---

## Communication contract (unchanged)

- Write per-step notes to
  `.trellis/tasks/06-30-sway-d6-poc-rework/notes/<date>-<topic>.md`
- Update `task.json` `current_phase` and `next_action` after each Step
- Tick `prd.md` Acceptance Criteria checkboxes with evidence
- **Do NOT modify SPEC v2 or DESIGN_DECISIONS_PENDING_2026-07-01.md** without
  glr approval; SPEC v2 §4.2 rewrite happens AFTER F1 verdict, not before

---

## Start point

Begin with **Phase B-rev Step 1**: modify controller to use F1 miss-rate
utility, add per-component snapshots, extend CSV audit columns, rebuild.

Report Step 1 findings (code changes, unit test results, build success)
before proceeding to Step 2 smoke.
