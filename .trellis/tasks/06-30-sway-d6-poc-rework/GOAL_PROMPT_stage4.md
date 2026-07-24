# SWAY Stage 4: live-controller beta wiring and paired causal train/held-out evaluation

> **Status**: ACTIVE 2026-07-24 — §0.1–0.4 FROZEN by glr 2026-07-24; §0.5 CLOSED (champsim fix verified same day). Ready for executor dispatch starting at Stage 4A.
>
> **Executor**: any coding agent (Claude Code / kimi code cli / zcode-GLM). This
> prompt is agent-agnostic by glr decision 2026-07-24 (codex-by-default
> execution is retired; GPT 5.6 no longer economical at the provider). Assume
> no codex-specific tooling. Naming note: the `CODEX_GOAL_PROMPT_*` convention
> ends here; this series continues as `GOAL_PROMPT_*`.
>
> **Scope**: wire the locked self-normalized beta semantics into the live SWAY
> controller, then run paired baseline/SWAY causal experiments with a frozen
> training/held-out protocol. Produce the first causal IPC evidence for the
> CAL letter, or a clean gate failure. Keep Phase C blocked.

## 0. Pre-dispatch decision record (0.1–0.4 FROZEN by glr 2026-07-24; 0.5 added same day)

| # | Decision | Frozen content | State |
|---|---|---|---|
| 0.1 | **A-layer cohort rule** | **Retire the whole-run `SWAY-target` prefilter.** Primary analysis cohort per direction = Stage 3 beta-evaluable sets (26 `MBTB→TAGE` / 20 `ITTAGE→TAGE`); full 39-workload cohort reported as secondary no-harm evidence. Taxonomy §6.2 gets an amendment note paper-side after the gate, not in this task. | **FROZEN 2026-07-24** |
| 0.2 | **Family-aware train/held-out split** | **Held-out = CBP int {12, 13, 15} + SPEC17 {cactuBSSN_128179, gcc_ref32_O3_888, imagick_37934, x264_pass2_9146, xz_combined_26239}** (8 total); **train = remaining 31 SPEC17**. Held-out SPEC17 excludes the 10-workload Phase-0/1 cpt cohort (tuning history). Held-out data is never used for θ selection or any tuning. **Amendment path**: if §0.5 restores champsim before 4B dispatch, glr may amend held-out to add a `srv_*` subset (restoring E-2 trace coverage) by a short addendum here; without an addendum, the frozen split above stands. | **FROZEN 2026-07-24** (amendment path open per §0.5) |
| 0.3 | **θ sweep set (live, train)** | **{1.0, 1.5, 2.0}** at 1 seed for selection; 3-seed confirm at the selected value. Full {0.5..2.5} grid deferred to HPCA extension. | **FROZEN 2026-07-24** |
| 0.4 | **Causal gate criteria** | (i) live controller observably fires (≥1 realloc on advisory ≥50% of train beta-evaluable workloads at selected θ); (ii) **train** primary cohort: paired 3-seed geomean ΔIPC > 0 with Wilcoxon signed-rank p < 0.05 (paired t-test also reported per C-DA-1); (iii) **held-out** at frozen θ: geomean ΔIPC > 0 AND no workload regresses beyond seed-variance noise floor; (iv) false-fire rate (def §4) reported, advisory < 50%; (v) baseline bit-exactness preserved. Gate verdict is glr's call; executor reports data against these criteria. | **FROZEN 2026-07-24** |
| 0.5 | **Champsim trace support (PENDING-24 promoted to blocking, then CLOSED same day)** | glr 2026-07-24: "champsim trace 支持必须搞定". Root cause identified and fixed same day: sway lacked the fabricated-target-removal + lookahead-target-resolution fix that BTBP Stage-A carries as `01a6ff1c1e` (CBP2025TraceReader already had `fixTakenTargetMismatch=true`; ChampSimTraceReader did not). Fix migrated as cherry-pick **`468955b91d`** + regression-test migration **`d545cefdd6`** on `sway/d6-substrate-rework`. **Verified**: (a) original PENDING-24 reproduce (`srv_0.gz`, main-repo kmhv3.py, 15.4M instr) — clean max-inst exit at tick 2189053089, no abort (previously SIGABRT at tick 20979); (b) sway kmhv3.py + `srv_1.gz` (2M instr) — clean exit. Runs: `runs/pending24_fix_smoke/{srv_0,srv_1}/`, taskpool tag `sway-pending24-20260724`. Champsim `srv_*` family (135 traces in `cvp1_public`) returns to the workload inventory per taxonomy §6.3; full-family re-validation rides with Stage 4 workloads. glr may now amend §0.2 held-out with a `srv_*` subset per the amendment path. | **CLOSED (verified) 2026-07-24** |

## 1. Authority and reading order

1. `~/project/papers/sway-paper/docs/SWAY_METRIC_TAXONOMY.md` §3–§5 — locked metric and decision semantics. **Do not change formulas, buffer sizes, arbitration, or cooldowns.**
2. `~/project/papers/sway-paper/docs/experiments/2026-07-11-sway-stage3-beta-replay.md` — evaluability boundary and next-gate list.
3. `CODEX_GOAL_PROMPT_stage3.md` — raw phase-stream contract (schema v1 unchanged).
4. `~/project/GEM5-sway/runs/stage3_beta_replay/stage3_beta_report.md` — cohorts, theta sensitivity, candidate-set boundary.
5. `~/project/papers/sway-paper/PENDING.md` PENDING-23 (θ unresolved) and PENDING-24 (champsim excluded).
6. `~/project/papers/sway-paper/docs/SWAY_MECHANISM_SPEC_v2.md` §4.3 (cool-down) + `SESSION_RESUME_2026-07-02.md` §3 D-Return (T2 cool-down-expiry primary + T3 regression + T4 donor pressure; W-a cold reset on return).
7. Locked evaluation decisions: E-1 (3 seeds, offsets {0, +1M, +2M}, instruction-weighted aggregation; perlbench_diff/split treated independently), E-3 (Arm 3 = D6 main is this task; Arm 1/2 untouched), D-7 (B4 static-resize deferred to Phase C, not this task).

## 2. Stage 4A — live beta controller wiring

Replace the closed F1/F1' decision predicate (v3/v4 wiring, commits `f9e07e8578`, `b7f067748c`) with the locked beta semantics. Reuse the Stage 3 per-phase raw counter machinery in-sim so live metrics are the same quantities the open-loop replay consumed.

Requirements:

- **Metrics** (identical to replay): donor B-b candidates MBTB bank0.way3 + bank1.way3 (separate), ITTAGE T2 + T3 (separate); donee B-c for TAGE tables in the fixed donee_set {T3, T7}.
- **Warmup history**: circular 128-phase buffer per metric, appended during the warmup segment only; ≥32 finite values required else the pair is not evaluable; population std (ddof=0); strict `z > θ`; reference distribution never updated during the sample segment.
- **Decision**: per sample phase, among P3'-legal pairs with `z_donor > θ_donor AND z_donee > θ_donee` and no active cooldown — at most ONE action per phase across both directions; pick donor with highest donor z, then available donee with highest donee z; ties by stable direction/slot/table id; shared per-donee cooldowns T3=8, T7=20 phases. Short-history / zero-std → not evaluable, never silently fire.
- **Params**: `swayReallocThetaDonor` / `swayReallocThetaDonee` (default 1.5); donee_set fixed at {T3, T7}.
- **Return path**: unchanged — D-Return semantics (T2 cool-down-expiry re-eval primary, T3 regression, T4 donor pressure; W-a cold reset on return).
- **New event log** `sway_realloc_events.csv` (emitted only when `enableSwayRealloc=True`): one row per fire with `phase_id, direction, donor, donee, z_donor, z_donee, theta_donor, theta_donee`, plus the resolution tag appended when the borrowed way returns: `cooldown_expiry_return | t3_regression_return | t4_donor_pressure_return | active_at_end`.
- `enableSwayRealloc=False` → controller not invoked at all; bit-exact baseline preserved (taxonomy §5.4).

### 4A gates (all must pass before 4B)

- Build `gem5.opt` plus focused test binaries; BTB tests 23/23, TAGE tests 34/34; new controller unit tests (history buffer, z-score, arbitration, cooldown, segment-boundary handling) green.
- **Baseline equivalence**: rerun the x264_seek_80312 baseline-off pair against the retained Stage 3 run (same checkpoint, 20M+20M, W=100K). `stats.txt` whole-file diff empty after excluding the five host fields.
- **Live-vs-replay consistency smoke** at θ=1.5, 1 seed, on {gcc_pp_O2_1869, mcf_17135}: live fire counts vs open-loop replay fire counts — report delta; ±10% advisory, investigate beyond.
- **STOP conditions**: baseline diff non-empty; live fires ≈ 0 on all smoke workloads (live/replay wiring mismatch); any F1/F1' code path still reachable.

## 3. Stage 4B — train θ sweep (selection only)

- Train cohort = 31 SPEC17 (§0.2). Arms: baseline (`enableSwayRealloc=False`) vs SWAY. θ ∈ {1.0, 1.5, 2.0}. 1 seed (offset 0). 31 × 2 × 3 = 186 runs. Run shape: 20M warmup + 20M sample, W=100K, phase 100K instr.
- **Selection rule (freeze before looking at results)**: selected θ = argmax of paired geomean ΔIPC over the train primary cohort, subject to advisory false-fire rate < 50%; ties broken toward the higher θ (fewer fires, lower action risk).
- Only after the selection is recorded in the experiment notes: proceed to 4C.

## 4. Stage 4C — train 3-seed confirm (primary causal evidence)

- Selected θ only. 31 workloads × 2 arms × 3 seeds (offsets {0, +1M, +2M} per E-1) = 186 runs.
- Aggregation per E-1: instruction-weighted; perlbench_diff/split independent. Paired per workload per seed (SWAY vs baseline); report per-workload ΔIPC mean ± std, geomean over the train primary cohort (beta-evaluable subset) and over the full train set; Wilcoxon signed-rank and paired t-test (C-DA-1).
- **False-fire definition (executable)**: a fire whose borrowed way returns via `t3_regression_return`; `false_fire_rate = t3_regression_returns / total_fires` per workload and cohort-wide. Fires resolved by cooldown expiry or T4, or still active at end, are not false fires.
- Also report per-direction fire counts and T3/T7 split for continuity with the Stage 3 open-loop table.

## 5. Stage 4D — held-out, once, frozen

- 8 held-out workloads × 2 arms × 3 seeds = 48 runs at the frozen θ, dispatched only after 4C analysis is signed off. No re-tuning after seeing held-out data.
- Report separately: CBP int (n=3; explicitly not a family-level validation) and held-out SPEC17 (3 beta-evaluable + 2 non-evaluable; the non-evaluable pair is the no-harm check).

## 6. Batch execution contract

- Batch runs go through the mytools distributed scheduler (Stage 3 pattern: `runs/stage3_beta_replay/{configs,worklists}`, per-run `completed` markers) or the harness taskpool; either way require: per-run completion markers, stats validation, 0-abort gate, and a dispatch manifest per stage under ignored `runs/stage4_{train_theta_sweep,train_3seed,heldout}/`.
- Environment: `scons build/RISCV/gem5.opt --gold-linker -j32` (never `-j160`, header race); DRAMsim3 symlink `ext/dramsim3/DRAMsim3` must exist; difftest off (`--param 'system.cpu[0].enable_difftest=False'`); GCB_RESTORER `/nfs/home/share/gem5_ci/tools/normal-gcb-restorer.bin`; use `--maxinsts`, never `--maxtime`; `XS_MAX_INSTS` via backend-config env override.
- ChampSim traces are gated by §0.5: excluded until the `srv_0.gz` smoke passes on the fixed binary; once §0.5 closes, the `srv_*` family re-enters the workload inventory and this clause is lifted. CBP int traces are unaffected and always in scope.
- Seed offsets {0, +1M, +2M} implemented via the same warmup-injection runner contract as Stage 3 (`run_stage3_cpt_gem5.sh` pattern); document the exact mechanism in the dispatch manifest.

## 7. Artifacts

**Code** (commits on `sway/d6-substrate-rework`, existing commit style):

- Live beta controller + `sway_realloc_events.csv` + theta params + unit tests.
- `util/xs_scripts/sway/analyze_stage4.py` → paired ΔIPC tables, geomeans, Wilcoxon/t-test, false-fire rates, fire-count tables.

**Paper-side** (separate commit in `~/project/papers/sway-paper`):

- `docs/experiments/YYYY-MM-DD-sway-stage4-live-causal.md` modeled on the Stage 3 record: purpose / source-and-contract / workloads / results / interpretation boundary / verification / next gate.
- Update PENDING-23 (θ status) and the taxonomy §9 "to-be-updated after Stage 4" list.
- If the gate passes: declare Phase C unblocked and SPEC v2 rev-1 unblocked. If it fails: report to glr with the data; **do not hand-tune** (review C2).

## 8. Hard gates (executor)

- Do not change locked metric formulas, buffer sizes, arbitration, cooldown values, or the raw CSV schema.
- Do not select θ from open-loop data, from held-out data, or from fire counts alone.
- Do not run or analyze 4D before 4C sign-off.
- No hand-tuning to force positive ΔIPC; any deviation from defaults requires written justification in the experiment record.
- Phase C remains blocked until glr declares the causal gate passed.
- 4A STOP conditions apply for the whole task.

---

*Drafted 2026-07-24 by Claude Code (ARS status-evaluation session, `docs/STATUS_EVAL_2026-07-24.md`). Supersedes no prior prompt; Stage 0–3 prompts stand as audit trail.*
