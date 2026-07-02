# CODEX_GOAL_PROMPT Stage 2 — B-b/B-c narrow stats + baseline sweep

> **Prerequisite**: Stage 0 (`CODEX_GOAL_PROMPT_stage0.md`) verdict = PASS on both champsim + CBP trace modes. Do NOT start Stage 2 until glr has confirmed Stage 0 note and unblocked.
>
> **Scope**: (a) add narrow per-way / per-table stats needed to compute B-b (stranded/imbalance) and B-c (evict × utilization) offline; (b) run baseline (no SWAY) across an expanded workload set covering SPEC17 + champsim BTB-intensive + CBP int, so we can offline-verify the Stage-3 decision procedure (`self-normalized β with warm-up half`) before wiring controller changes.
>
> **Effort**: 1-2 days (stat plumbing ~half day + sweep scheduling + first-pass QA).
>
> **Deliverables**: (i) minimal-diff stat additions on `sway/d6-substrate-rework`; (ii) baseline stats.txt for ~70-80 workloads under `runs/stage2_baseline_expanded/`; (iii) per-workload W0' analysis CSV; (iv) one note per workload family + one aggregated verdict note.

## Context (read first)

- `~/project/papers/sway-paper/docs/SESSION_RESUME_2026-07-02.md`
- `~/project/papers/sway-paper/docs/SWAY_MECHANISM_SPEC_v2.md` §1 (P1/P2/P3'), §2.1 (donor slots), §2.2 (donee tables)
- `~/project/papers/sway-paper/docs/DESIGN_DECISIONS_PENDING_2026-07-01.md`
- `~/project/GEM5-sway/.trellis/tasks/06-30-sway-d6-poc-rework/CODEX_GOAL_PROMPT_v4.md` and its verdict note
- `~/project/GEM5-sway/.trellis/tasks/06-30-sway-d6-poc-rework/notes/2026-07-XX-trace-mode-stat-sanity.md` (Stage 0 output)

Key SPEC v2 facts you must respect:

- **P3'**: TAGE is never a donor. Legal donate directions: `MBTB→ITTAGE`, `ITTAGE→MBTB`, `MBTB→TAGE`, `ITTAGE→TAGE`. Do NOT plumb any TAGE-side donor stat as if it were candidate.
- **Design-time pinned donor slots (§2.1)**:
  - MBTB donor candidate = `bank0.way3` and `bank1.way3` (per bank, exactly 1 way).
  - ITTAGE donor candidate = `T2` and `T3` (per A20 heatmap).
  - Everything else is R3-excluded or native-only; do NOT allocate stat storage for non-candidate ways/tables in B-b context (waste of design intent).
- **Phase**: `phaseSizeByInst = 100000 (= W_base)`. Do NOT change.
- **Trace convention**: champsim + CBP traces already tag first half as warmup, second half as sample. Runner already respects this (verified in Stage 0).

## Task A — narrow stats (minimal-diff)

Design principle: **only add scalars/vectors that map directly to B-b or B-c ingredients for the pinned candidates or their normalization set**. Do NOT expose entire private stat groups. Do NOT change any prediction/update logic.

### A.1 MBTB per-way hit counters

Add to `btbStats` in `src/cpu/pred/btb/mbtb.cc` / `.hh`:

```cpp
Vector predHitsPerWay;   // size = numBanks × numWays (2 × 4 = 8 by default)
Vector predMissesPerWay; // same size
```

Wire-up:

- At the same call site as existing `btbStats.predHit++` / `btbStats.predMiss++` (see `mbtb.cc:500-507`), also increment `predHitsPerWay[bank_idx * numWays + way_idx]` / `predMissesPerWay[bank_idx * numWays + way_idx]`.
- For hit: `way_idx` is the way containing the hit entry.
- For miss: `way_idx` is undefined; do NOT increment `predMissesPerWay` on miss (or use a separate `predAccessesPerBank` — Vector[numBanks] — for the miss denominator; pick whichever is simpler). Document choice in the note.
- Add narrow scalar getters `getPredHitsPerWay(bank, way)` mirroring the v3 pattern from `2026-07-02-f1-step1.md`.
- Register in `regStats()`.

### A.2 MBTB per-way evict counter

Add:

```cpp
Vector predEvictsPerWay; // same shape as predHitsPerWay
```

Wire-up: at the point where a MBTB entry is replaced (the `update()` / new-entry-allocation path — trace where `newEntryWithCond++` / `newEntryWithUncond++` fires and identify the victim way; that increment site is the evict site for the victim), increment `predEvictsPerWay[bank * numWays + victim_way]`. If MBTB does not have a clean victim-way concept in a call, skip that call — do NOT invent one.

### A.3 MBTB utilization proxy

Add a periodic sampler (once per phase boundary, driven by existing sway controller `beginPhase()` if enabled, else add a small in-mbtb "instructions since last sample" counter):

```cpp
Vector activeEntriesPerWay;    // Snapshot, sampled at phase boundary
Scalar totalEntriesPerBank;    // constant, set in regStats
```

`activeEntriesPerWay[bank * numWays + way]` = number of valid entries in that way at sample time. If MBTB has no "valid" bit already, use "entry with non-zero tag" or the closest equivalent. Add the sampler regardless of whether SWAY is enabled (this stat is baseline, not SWAY-gated).

### A.4 TAGE per-table access + evict counters

TAGE already has `updateTableMispreds[t]` and `updateFilteredEntries`. Add:

```cpp
Vector perTableUpdates;       // size = numPredictors (=8); increments once per TAGE table t
                              // whenever an entry in T_t is updated
Vector perTableAllocSuccess;  // size = numPredictors; the existing alloc-success path,
                              // decomposed per table
Vector perTableEvicts;        // size = numPredictors; when alloc replaces an existing valid entry
```

Wire-up:

- `perTableUpdates[t]` fires alongside the existing per-entry update loop (find where `updateMispred` / `updateTableMispreds[t]` fires and split hit/mispred; the hit case increments `perTableUpdates[t]`, mispred also increments `updateTableMispreds[t]` as today).
- `perTableAllocSuccess[t]` fires at the existing "alloc success when update" site (grep `alloc success` in `btb_tage.cc`), keyed by which table t received the new entry.
- `perTableEvicts[t]` fires only if the alloc replaced a **valid** entry (u=0 with valid tag). If alloc always writes to a stale/empty slot in this codebase, `perTableEvicts[t]` stays zero and we note it.

### A.5 TAGE per-table occupancy sampler

```cpp
Vector activeEntriesPerTable; // sampled at phase boundary; count of valid entries in T_t
```

Same phase-boundary sampler pattern as A.3.

### A.6 ITTAGE per-table access + evict + occupancy

Mirror A.4 + A.5 for `BTBITTAGE`. It has 5 tables. Same stat names, one file over.

### A.7 CSV audit column (baseline compatibility)

Baseline runs do NOT enable SWAY, so `sway_phase_a_diag.csv` is not emitted. Do NOT add SWAY controller writes on the baseline path. The new stats only live in `stats.txt`; offline python will consume them.

### A.8 Build + tests

- `git diff --check`
- `scons build/RISCV/gem5.opt --gold-linker -j32` (NOT `-j160`)
- `./build/RISCV/cpu/pred/btb/test/btb.test.opt` — must be 21/21
- `./build/RISCV/cpu/pred/btb/test/tage.test.opt` — must be 33/33
- Bit-exact baseline preserved (`enableSwayRealloc=False` path unaffected)

If any test fails, STOP and open a follow-up note; do NOT proceed to Task B.

## Task B — baseline sweep

### B.1 Workload set

Assemble the following list in `runs/stage2_baseline_expanded/workload_manifest.tsv` (columns: family, id, source_path, inst_count, format):

- **SPEC17** (target: full 35): iterate `/nfs/home/share/zyy/spec17-rv64gcb-O3-20m-gcc12.2.0-mix-with-special_wrf/checkpoint-0-0-0/*/` — one representative simpoint per workload directory. If multiple simpoints exist (e.g. `gcc_pp_O2/1869/`, `gcc_pp_O2/xxxx/`), pick the one already used in `~/project/papers/sway-paper/runs/eval_baseline_W100K/main_W100K/` if present, else pick numerically lowest. Format = `cpt` (checkpoint mode).
- **Champsim BTB-intensive** (target: top 30): `/nfs/home/share/glr/champsim_traces/champsim_traces_btb_intensive.lst`, take first 30. Format = `champsim`.
- **CBP int** (target: 10-15): `/nfs/home/share/glr/cbp_traces/int/` — pick 12-15 by increasing filename. Format = `cbp2025`.

Log actual final counts in the manifest.

### B.2 Run parameters

Uniform across all workloads:

- Baseline: `enableSwayRealloc=False`. No SWAY.
- Instructions: 20M (checkpoint) or 30M (trace, of which half = warmup per convention). If a specific trace is shorter, use its native length.
- `phaseSizeByInst = 100000`
- Binary: freshly built `build/RISCV/gem5.opt` (after Task A).
- Output dir: `runs/stage2_baseline_expanded/<family>/<workload_id>/`
- One `stats.txt` per workload.

### B.3 Scheduler

Use `~/project/mytools/tools/distributed-trace-scheduler/distributed_trace_scheduler.py` per project convention. Backend script:

- Traces → `run_trace_gem5.sh` (already in `~/project/mytools/tools/distributed-trace-scheduler/scripts/`).
- Checkpoints → `run_cpt_gem5.sh` (same dir).

Concurrency: whatever the scheduler defaults to for this host inventory. If ssh multi-host is enabled per user's global config, use it; else local `-j` = min(host_cpu / 8, num_workloads).

If a workload run fails (crash / timeout / non-zero exit), log to `runs/stage2_baseline_expanded/_failures.tsv` and continue. Do NOT retry silently; note failure list at end.

### B.4 Time budget

Expected 8-16 hours wall-clock end-to-end depending on host availability. If wall-clock exceeds 24h, STOP the sweep and report progress.

## Task C — offline W0' analysis

For each completed run, compute per-workload:

**Footprint (workload selection sanity)**
- `mbtb_footprint = predHit + predMiss`
- `tage_footprint = updateFilteredEntries`
- `ittage_footprint = commitHits + commitMisses`

**B-b — donor-side stranded on pinned candidates only**
- `B-b_MBTB = min(predHits_bank0_way3, predHits_bank1_way3) / mean(predHitsPerWay over all 8 way)`
- `B-b_ITTAGE = min(perTableUpdates[T2], perTableUpdates[T3]) / mean(perTableUpdates over all 5 table)`
- (TAGE: NOT computed as donor — P3'.)

**B-c — donee-side evict × utilization**
- `evict_rate = perTableEvicts[t] / perTableUpdates[t]` (or MBTB way-analog)
- `util = activeEntriesPerTable[t] / total_entries_per_table` (or MBTB way-analog)
- `B-c = evict_rate × util`
- Compute per pinned candidate (MBTB way3s + ITTAGE T2/T3), and for TAGE per donee table (T3, T7 per SPEC v2 §2.2 main_result_choice; also compute for T4, T5, T6 as candidates for later sweep).

**Legal direction gaps (all 4 P3'-allowed directions)**

For each workload, compute `donor_score` (B-b for MBTB / ITTAGE pinned candidates) and `donee_score` (B-c for target component), and report whether the workload is a "SWAY-target" candidate in each direction:

- Direction MBTB→ITTAGE: donor = MBTB pinned; donee = ITTAGE overall (aggregate B-c)
- Direction ITTAGE→MBTB: donor = ITTAGE pinned; donee = MBTB overall (aggregate B-c)
- Direction MBTB→TAGE: donor = MBTB pinned; donee = TAGE candidate table (T3, T7)
- Direction ITTAGE→TAGE: donor = ITTAGE pinned; donee = TAGE candidate table (T3, T7)

Since B-b and B-c live on incompatible scales, do NOT subtract them. Instead, per-direction indicator:
- `sway_target[direction] = (donor_score < 1.0)  AND  (donee_score > median(donee_score over all workloads in this family))`
- (This is the whole-run version. Phase-level self-normalized β decision procedure will be validated in Stage 3 offline sim, but this whole-run cut is enough for A-layer workload picking.)

Write:

- `runs/stage2_baseline_expanded/w0p_per_workload.csv` — one row per workload with all above columns.
- `runs/stage2_baseline_expanded/w0p_per_direction_summary.tsv` — for each of 4 directions × 3 families, count of sway-target workloads / total.
- `runs/stage2_baseline_expanded/w0p_target_workloads.tsv` — for each direction, list of workload ids flagged as target.

## Task D — notes

Write one note per family:

- `notes/2026-07-XX-stage2-spec17.md`
- `notes/2026-07-XX-stage2-champsim-btb.md`
- `notes/2026-07-XX-stage2-cbp-int.md`

Each with: (a) sweep manifest, (b) failure list, (c) family-level top-5 workloads per direction by donee_score.

Plus one aggregated verdict:

- `notes/2026-07-XX-stage2-verdict.md`

Format:

```markdown
# Stage 2 baseline sweep verdict

## Sweep summary
| family | attempted | completed | failed |
|---|---:|---:|---:|

## Direction × family target count
| direction | SPEC17 | champsim-btb | CBP-int | total |
|---|---:|---:|---:|---:|
| MBTB→ITTAGE | | | | |
| ITTAGE→MBTB | | | | |
| MBTB→TAGE | | | | |
| ITTAGE→TAGE | | | | |

## Top target workloads per direction (union across families)
- MBTB→ITTAGE: [list]
- ITTAGE→MBTB: [list]
- MBTB→TAGE: [list]
- ITTAGE→TAGE: [list]

## Recommendation
- If any direction has ≥ 5 target workloads → proceed to Stage 3 (offline decision procedure sim).
- If all 4 directions have < 3 target workloads → escalate to glr; the SWAY thesis may need reformulation.
```

## Hard constraints

- Do NOT modify `docs/SWAY_MECHANISM_SPEC_v2.md`, `docs/DESIGN_DECISIONS_PENDING_2026-07-01.md`, `INSIGHT.yaml`, `PLAN.md`. SPEC evolution is glr's job after seeing this data.
- Do NOT modify SWAY controller code. Task A is stat plumbing on baseline path only.
- Do NOT enable SWAY in any run in Task B.
- Do NOT tune anything. No hysteresis, no phase-size, no threshold changes.
- Do NOT downgrade the workload set silently. If some family has < 30 valid entries, log it and continue with what's available; do NOT substitute unrelated traces.
- Do NOT run more than the specified workload set. Adding more traces requires glr approval.
- Bit-exact baseline: `enableSwayRealloc=False` must remain byte-identical stat output between pre-Task-A and post-Task-A for the same seed/checkpoint (except the newly added stats being non-zero). Verify with one paired run before launching the full sweep.

## Reporting contract

Data-tables-only for the notes. No editorial commentary. glr reads the CSV/TSV artifacts and the verdict recommendation line, then decides Stage 3.
