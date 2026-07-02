# CODEX GOAL PROMPT v2 — SWAY D6 rework + reallocCount=0 diagnosis + Round 1 decisions

> **Date**: 2026-07-01
> **Purpose**: Codex 执行入口, 一次读完即启动 Phase A 诊断
> **Reader**: codex CLI or agent
> **Human ping**: 任何 STOP 条件触发时联系 glr

---

## Goal

SWAY Phase 1 Task C v2 — D6 rework with reallocCount=0 diagnosis, Round 1 decisions (D-Return / D-1 / D-4 / D-7), and E-1/E-2/E-3 evaluation protocol.

Work in repo: `/nfs/home/goulingrui/project/GEM5-sway`
Authoritative spec: `/nfs/home/goulingrui/project/papers/sway-paper/docs/SWAY_MECHANISM_SPEC_v2.md`
Round 1 decisions (2026-07-01): `/nfs/home/goulingrui/project/papers/sway-paper/docs/DESIGN_DECISIONS_PENDING_2026-07-01.md`
Task PRD: `.trellis/tasks/06-30-sway-d6-poc-rework/prd.md` (extend, do not restart)
Editorial decision reference: `/nfs/home/goulingrui/project/papers/sway-paper/.ars/reviews/2026-06-30-spec-v2/00-editorial-decision.md`

INSIGHT.yaml `phase1_d6_result` shows current D6 conserved substrate `total_reallocCount: 0` on the 10-workload SPEC17 cohort — SWAY controller is **not triggering any transfers**, and the 0.254% geomean is baseline noise. First priority is to diagnose root cause, then land Round 1 decisions and complete the main evaluation.

---

## Round 1 locked decisions (SPEC v2 rev 1 时落地, but DO NOT rewrite SPEC now; keep as external decisions until D6 rework data lands)

### D-Return | 归还机制 (deletes SPEC §4.1 Action 5 idle-driven)

- **Primary trigger T2**: cool-down expiry re-evaluation
  - cool-down 期内不 re-evaluate、不归还、不切 owner
  - 到期 phase 边界 controller 强制评估:
    - `if donee_utility - donor_utility > hysteresis: 续借一个 W_c`
    - `else: 归还 → W-a cold-reset`
- **Secondary trigger T3**: regression detection (D7 opt-in)
  - Stat infra: per-donee mispredictRatePrePhase / mispredictRatePostPhase
  - `delta > 0.20 relative → emergency recall, break cool-down, blacklist K_blacklist=5 phases`
  - CAL 主线 land D5-D6 阶段准备 infra; 实际启用条件依赖 D6 outcome (情形 A/B/C)
- **Secondary trigger T4**: donor pressure opportunistic recall
  - donor 剩余 way utility > 0.85 连续 K_donor_pressure=2 phase → recall, may break cool-down
- **What**: W-a cold-reset only (Fix 3 stat tracks useful loss on recall)
- **触发优先级**: T3 > T4 > T2
- **参数**:
  - `delta_regression_threshold = 0.20`
  - `K_blacklist_phases = 5` (goes into E-4 sensitivity sweep)
  - `threshold_donor_hi = 0.85`
  - `K_donor_pressure_phases = 2`

### D-1 | Alloc priority = 方向 Y (borrowed 与 native 平等)

- **Revert** 当前 codex implementation 里 native-first / borrowed-second 两级
- 恢复 baseline TAGE 三级: `invalid → weak+!useful → any-!useful` (不区分归属)
- borrowed way 与 native way 在同一 alloc priority pool 内竞争
- **无 protection grace**, 依赖 D-Return 的 T2 cool-down 期契约 + T3 regression 自动纠错
- Fix 1 保持全 cold-reset 不变 (valid + useful + counter)

### D-4 | ITTAGE table-level donate substrate = time-share (single port + owner mux)

- ITTAGE table T_i 保留 baseline 1R/1W SRAM
- 加 2 个 owner mux 在 SRAM address 输入 (read + write)
- `owner=native`: ITTAGE 独占; `owner=donated`: donee TAGE 独占 (bypass ITTAGE 侧 predictTaken/update/allocate)
- 排他, 满足 P3'
- GEM5 modeling 已有 `tableDonatedFlag` 概念, 只需 verify 完整 bypass 语义
- SPEC v2 §4.4 rev 时含 ~120 words 描述 + baseline ITTAGE T2/T3 area 具体 μm² (由 CACTI 估算给出, PENDING-21)

### D-7 | Static-resize baseline B4 (加进 §V baseline 组)

- **配置** (workload-agnostic, 与 D6 main donee_set 对齐):
  - `MBTB: numEntries=4096` (从 8192, way3 让出 = -4096 entry)
  - `ITTAGE: T2/T3 disabled` (numPredictors=3, tableSizes=[256,256,512])
  - `TAGE: uniformly enlarged`; 每张表 tableSizes 从 2048 增加 sum(donor_capacity_in_TAGE_sub_entries)/8
    - 计算: MBTB donor 14 KB × 2 slot = 28 KB → 28KB / 18bit ≈ 12444 sub-entries
    - ITTAGE T2+T3 donor 3.4 KB × 2 = 6.8 KB → 6800/18 ≈ 3020 sub-entries
    - Total: ~15464 sub-entries / 8 TAGE tables ≈ 1933 sub-entries per table
    - → tableSizes 从 2048 → 2048+1933=3981 → 向下取 **3840 per table**
- **配置 via XS_EXTRA_PARAMS** (no GEM5 rebuild):
  - `system.cpu[0].branchPred.mbtb.numEntries=4096`
  - `system.cpu[0].branchPred.ittage.numPredictors=3`
  - `system.cpu[0].branchPred.ittage.tableSizes=[256,256,512]`
  - `system.cpu[0].branchPred.tage.tableSizes=[3840]*8`
  - (verify these param paths actually exist in kmhv3.py before enabling)
- **Runs**: 10 workload × 3 seed = 30 runs
- **Output**: `runs/eval_static_resize_B4/`
- **Depends on**: PENDING-16 donee_set decision (B4 aligns with main SWAY donee_set choice)

---

## E-1 | Multi-seed protocol (revised, SimPoint semantics preserved)

**Workload aggregation rule (INVARIANT, never violate)**:
- Report the workload's IPC as the instruction-count-weighted aggregate across all its SimPoint slices
- Single-slice IPC numbers are **NEVER reported as data points**
- Current 10-workload cohort uses top-1 SimPoint slice per workload (e.g., `gcc_pp_O2_1869` = one slice); **DO NOT expand to multi-slice in this task** (deferred to HPCA extension); acknowledge in §V limitation

**Seed generation rule**:
- 3 seeds per workload
- Seed k = for each slice in that workload, offset the checkpoint start by offset_k, then aggregate all slices weighted by instr count
- `offset_k ∈ {0, +1M, +2M}` instructions from checkpoint start
- Each run remains 5M instructions
- Since current cohort has 1 slice per workload, seed k = single 5M run starting at checkpoint + offset_k

**perlbench_diff / perlbench_split**: keep as independent workloads (each with 3 seed offsets), NOT collapsed into "perlbench 2 inputs"

---

## E-2 | Held-out validation

### Held-out A: 7 championship traces (primary)

- IPC1 × 3 + CVP1 × 2 + CBP2025 × 2
- Trace-mode TAGE update was fixed at 2026-06-16 (see archived task `06-16-sway-trace-mode-tage-update-diag`); **verify at D5 audit** that it still works on `sway/d6-substrate-rework` branch by running 1 trace baseline + smoke check that `BTBTAGE::updateFilteredEntries` and `updateMispred` are non-zero
- **If TAGE update regressed on SWAY branch**: **STOP D6 main line**, open new debug task PENDING-24 "trace-mode TAGE update regression under SWAY branch", **DO NOT downgrade held-out A to supplementary**
- 3 seeds × 7 traces × 2 config (baseline + SWAY main) = **42 runs**

### Held-out B: 5 SPEC17 disjoint (codex selects)

- **Selection criteria**:
  1. NO overlap with current 10 workloads (gcc/mcf/leela/omnetpp/deepsjeng/blender/exchange2/xalancbmk/perlbench*)
  2. Prefer 3 int + 2 fp mix
  3. Verify checkpoint availability under `/nfs/home/share/zyy/spec17-rv64gcb-O3-20m-...`
  4. Recommended pool: int candidates x264/xz; fp candidates bwaves/namd/povray/roms/imagick/nab/lbm
- **Confirm 5 workload list back to glr before running**
- 3 seeds × 5 workload × 2 config = **30 runs**

### Reporting

- §V reports Training (10 SPEC17) / Held-out A (7 traces) / Held-out B (5 SPEC17 disjoint) geomean separately
- Overfitting gate: `|Training - avg(A, B)| / Training < 30%` acceptable; `> 50%` requires hyperparameter re-selection

---

## E-3 | 3-arm over-provisioning bracket (UPGRADED to D6 debug tool, not D7 补跑)

Because current D6 conserved substrate `reallocCount=0`, Arm 2 becomes root-cause debug tool.

### Arm 1: Archived PoC baseline

- 10-scope per-table owner + sidecar + hysteresis 0.45 + K=1
- Config: reuse archived commit `a48d6355f1` branch settings
- 10 workload × 3 seed = 30 runs

### Arm 2: Isolation control (new D6 controller + sidecar semantics)

- D6 substrate 决策逻辑 (component owner + P2 cool-down + tight/relaxed hysteresis + Fix 1/2/3 + D-Return T2/T3/T4 + D-1 alloc priority 方向 Y)
- **BUT**: donee receives sidecar (newly allocated SRAM), NOT owner-gated wide-bank way
- Add controller flag `enforceCapacityConservation` (default True in Arm 3; False in Arm 2)
- When False: `transferSwayWays` calls sidecar resize instead of owner mux (equivalent to archived PoC's `swayExtraTable` behavior); MBTB way3 stays fully valid
- 10 workload × 3 seed = 30 runs

### Arm 3: D6 main (locked design, current PoC)

- Full D6 substrate with owner-gated capacity conservation
- Reuse PENDING-16 main sweep data if 3-seed-aligned; else 10 workload × 3 seed = 30 runs

### Attribution table

- **Arm 1 vs Arm 2**: controller improvement (owner grain / hysteresis / cool-down / fix / return / alloc)
- **Arm 2 vs Arm 3**: pure over-provisioning gap
- **Arm 1 vs Arm 3**: original PENDING-18 (混合, not informative)

---

## Execution order (STRICT sequential — do NOT parallelize)

### Phase A | reallocCount=0 diagnosis (before D6 main sweep, ~2 hour)

**Step 1**: Verify D6 controller actually runs
- Read `src/cpu/pred/btb/decoupled_bpred.cc` `SwayController::chooseReallocation` function
- Confirm it is called every phase boundary, reads utility snapshot, produces decisions (not stubbed)
- Grep for any recent commits on `sway/d6-substrate-rework` that might have accidentally stubbed the controller
- Write findings to `.trellis/tasks/06-30-sway-d6-poc-rework/notes/2026-07-01-controller-verify.md`

**Step 2**: Dump per-phase per-component utility trace
- Add temporary debug flag (or existing DPRINTF) to log at every phase boundary:
  - `utility(MBTB)` via current SPEC §4.2 formula (sum active_ways / sum total_ways)
  - `utility(ITTAGE)`
  - `utility(TAGE)` — aggregated over all 8 tables
  - utility per candidate donee TAGE table (e.g., T3, T7 individually)
  - **ALSO** **per-slot utility** (donor slot's specific way active/total) — this is NEW, for RC6 hypothesis test
  - hysteresis margin (0.10 tight)
  - controller decision (`transfer_MBTB_to_T3` / `transfer_MBTB_to_T7` / `hold` / `cool-down blocked`)
  - `reallocCount` cumulative
- Run `gcc_pp_O2_1869` for 5M instr with `donee_set=[T3,T7]` under D6 substrate
- Dump to CSV: `runs/phase_a_diag/gcc_pp_O2_utility_trace.csv`
- **Analyze**:
  - If per-component MBTB utility >> TAGE utility with negative gap throughout → **RC5/RC6 confirmed** → apply direction X (per-slot utility)
  - If per-slot MBTB utility is low but per-component MBTB is high → **RC6 confirmed**
  - If both utilities close (gap < 0.03 always) → **RC1 confirmed** → try direction Y
  - If utility gap sometimes exceeds hysteresis but reallocCount stays 0 → **RC4 (owner-gated semantic bug) or RC7 (stub)**
- Write conclusion to `notes/2026-07-01-utility-trace-analysis.md`

### Phase B | Root cause fix (based on Phase A findings)

**If RC5/RC6 (utility semantic mismatch)**:
- **Direction X**: modify SPEC v2 §4.2 utility formula
  - `donor utility = active_of_donor_slot_specific_way / total_of_that_way` (per-slot)
  - `donee utility = per-component aggregate` (unchanged, since borrowed capacity benefits the whole donee pool)
- Implement in controller
- Rerun `gcc_pp_O2_1869` smoke: expect `reallocCount > 0`
- If successful, **DO NOT immediately rewrite SPEC v2**; write findings to `notes/2026-07-01-direction-X-experiment.md`; SPEC rewrite deferred to after D6 main data lands

**If RC1 (hysteresis too strict) after direction X still failing**:
- **Direction Y**: relax tight hysteresis 0.10 → 0.05, relaxed 0.20 → 0.10
- Add sensitivity sweep {0.05, 0.10, 0.15, 0.20} in D6 main runs
- Because this expands C2 hand-tune attack surface, **MUST run held-out validation (E-2)** to confirm not overfitting

**If RC4/RC7 (implementation bug)**:
- Add stat `sway.controllerDecisionsMade` (decisions generated) vs existing `sway.reallocCount` (decisions executed)
- Compare; delta indicates bug location
- Debugger step-through `transferSwayWays` / owner mux

### Phase C | D6 main sweep with fixes (~2 day wall-clock)

Run all 5 donee_set candidates × 10 SPEC17 training × 3 seed × baseline + SWAY:
- **Baseline**: 10 workload × 3 seed = 30 runs
- **SWAY donee_set candidates**: 10 workload × 3 seed × 5 candidates = 150 runs
- **Total Phase C**: **180 runs**

Output: `runs/eval_sway_W100K_d6_v2/<donee_set>/<workload>/<seed_id>/`
Generate `figure4_ipc_uplift.pdf` (per candidate) and `figure5_donee_set_sweep.pdf` (across candidates with error bars)
Pick main result (PENDING-16 close): highest geomean, tie-break by broader-history-coverage set

### Phase D | E-1/E-2/E-3 full validation

**D-a**: Held-out A (championship traces)
- **Verify TAGE update on trace mode still works** (baseline trace smoke check first)
- 3 seed × 7 trace × 2 config = **42 runs**

**D-b**: Held-out B (5 SPEC17 disjoint)
- codex selects 5 workload list, confirms with glr, then runs
- 3 seed × 5 workload × 2 config = **30 runs**

**D-c**: E-3 3-arm bracket
- Arm 1: reuse archived commit config, 10 × 3 = 30 runs
- Arm 2: new `enforceCapacityConservation=False` flag, 10 × 3 = 30 runs
- Arm 3: reuse Phase C main result data if 3-seed aligned

**D-d**: D-7 B4 static-resize baseline
- After PENDING-16 main donee_set is locked, run B4 with matched TAGE capacity
- 10 × 3 = 30 runs

Total Phase D: **132 runs** (Arm 3 skipped if reusable)

### Phase E | Report generation and SPEC rewrite

- Aggregate all data into `figures/` (figure4, figure5, figure6 3-arm, figure7 held-out, figure8 static-resize)
- Compute:
  - Training geomean (± stddev) per config
  - Held-out A / B geomean
  - Overfitting gate check
  - 3-arm bracketing: `Arm 2 - Arm 3 = over-provisioning gap`; `Arm 3 - Arm 1 = controller improvement`
  - B4 vs SWAY: `dynamic value = SWAY - B4`
- Write findings to `.trellis/tasks/06-30-sway-d6-poc-rework/notes/2026-07-01-phase-E-report.md`
- Recommend SPEC v2 rev 1 revisions based on data (which utility formula, which hysteresis, which donee_set) — **DO NOT rewrite SPEC yet; leave that for glr to review**

---

## Hard constraints (STOP conditions)

- **If Phase A Step 2 shows controller stub (RC7)** → STOP, ask glr before proceeding
- **If Phase A / B iteration cannot get reallocCount > 0 on gcc_pp_O2 smoke** → STOP, escalate to glr with utility trace analysis
- **Trace-mode TAGE update regression on SWAY branch** → STOP D6 line, open PENDING-24
- **If direction X + direction Y both applied and Phase C main result is still 情形 C** (geomean < +0.3%) → STOP before Phase D, discuss with glr whether to walk fallback path (SPEC §8.3 情形 C 退路 1)
- Any hyperparameter tuning MUST have justification (either principled physics or held-out validation); **NO hand-tuning to force positive geomean gate**
- `0.45 swayReallocTageDoneeHysteresis` is deprecated; log warn if set, do not use
- P3' TAGE-only-donee enforced at design time (TAGE never donates)
- `enableSwayRealloc=False` → bit-exact baseline behavior

---

## Build & environment

- **Branch**: `sway/d6-substrate-rework` (based on `sway/phase-stranded-probe`)
- **Build**: `scons build/RISCV/gem5.opt --gold-linker -j32`
- **DRAMsim3 symlink**: `ext/dramsim3/DRAMsim3 → ~/project/GEM5/ext/dramsim3/DRAMsim3`
- **Scheduler**: mytools `tools/distributed-trace-scheduler/distributed_trace_scheduler.py` with `scripts/run_cpt_gem5.sh` backend

---

## Communication

- Write per-phase notes to `.trellis/tasks/06-30-sway-d6-poc-rework/notes/<date>-<topic>.md`
- Update `task.json` `current_phase` and `next_action` after each Phase completion
- Tick `prd.md` Acceptance Criteria checkboxes with evidence
- On any STOP: write status note and ping glr; **do NOT modify SPEC v2 or DESIGN_DECISIONS_PENDING_2026-07-01.md without explicit approval**

---

## Start point

Start with **Phase A Step 1 (~10 min)**: verify controller is actually running on `sway/d6-substrate-rework` branch. Report findings back before proceeding to Step 2.
