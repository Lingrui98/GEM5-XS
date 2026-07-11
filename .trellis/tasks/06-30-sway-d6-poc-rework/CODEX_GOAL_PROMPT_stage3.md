# SWAY Stage 3: phase-stream contract and open-loop beta replay

> **Status**: active after the 2026-07-11 Stage 2 audit.
>
> **Scope**: repair the empirical data contract required by the locked
> self-normalized beta decision procedure. First emit replayable per-phase raw
> counters on the baseline path, then validate and replay them offline. Do not
> wire beta into the live controller in this stage.

## Authority and supersession

Read these before implementation:

1. `~/project/papers/sway-paper/docs/SWAY_METRIC_TAXONOMY.md` sections 3-5.
2. `~/project/papers/sway-paper/PENDING.md` PENDING-23 and PENDING-24.
3. `CODEX_GOAL_PROMPT_stage2.md` and
   `notes/2026-07-09-stage2-verdict-v2.md` for audit history only.

The Stage 2 v1/v2 workload verdict is **not** a valid Stage 3 replay input:

- its analyzer used the wrong B-c access denominators;
- undefined `0/0` donor ratios were converted to zero;
- its runs ended at the warmup boundary and have no independent sample segment;
- its so-called eight-workload union contains only seven unique workloads.

The seven old `->TAGE` workloads remain a `legacy_v2_seed` smoke set. The
formal Stage 3 candidate set must be regenerated from the corrected contract;
do not hard-code the old verdict as ground truth.

## Stage 3A: raw phase-stream contract

Add `sway_phase_bc_raw.csv`. The file must be emitted with
`enableSwayRealloc=False`; collection is observational and must not invoke or
change the live controller.

Each complete phase emits 23 rows:

- MBTB: eight native way rows plus one component aggregate;
- ITTAGE: five table rows plus one component aggregate;
- TAGE: eight table rows.

The canonical schema is version `1`. Components are `MBTB`, `ITTAGE`, and
`TAGE`; scope kinds are `way`, `component`, and `table`. Exact scopes are
`mbtb_bank0_way0`..`mbtb_bank1_way3` plus `mbtb_all`, `ittage_t0`..
`ittage_t4` plus `ittage_all`, and `tage_t0`..`tage_t7`.

CSV fields:

```text
schema_version
phase_id
phase_start_inst
phase_end_inst
phase_inst_count
segment
replay_eligible
stats_epoch_start
stats_epoch_end
component
scope
scope_kind
hits_delta
misses_delta
updates_delta
alloc_success_delta
evicts_delta
resident_entries_end
capacity_entries
```

Rules:

- Event fields are phase-local deltas maintained independently of gem5's
  global statistics reset.
- `resident_entries_end` is a boundary snapshot of valid resident entries; it
  is never differenced and must not be called `active`.
- Non-applicable fields are empty, not zero.
- `segment` is `warmup`, `transition`, or `sample`.
- A phase spanning a stats-reset/warmup boundary is `transition` and is not
  replay eligible.
- Partial tail phases are not emitted.
- Phase boundaries use the CPU architectural committed-instruction clock used
  by warmup/maxinsts. The legacy live-controller phase clock is unchanged.
- The Stage 3 runner must inject the same warmup instruction count into the
  CPU and the BPU phase-stream metadata.
- A valid run has `total_insts > warmup_insts`. For the primary checkpoint
  shape use 20M warmup plus 20M sample (`--maxinsts=40000000`).
- Validator `--maxinsts` is the required observed total, not merely a launch
  cap; a formal run that terminates before 40M is invalid.

### Required raw counter semantics

MBTB way rows:

- `hits_delta`: predictions supplied by that `(bank, way)`;
- `evicts_delta`: valid entries evicted from that `(bank, way)`;
- resident/capacity: native entries in that way at the phase boundary.

MBTB aggregate:

- `hits_delta` / `misses_delta`: component prediction hits/misses;
- `evicts_delta`: sum over native ways;
- resident/capacity: sum over native ways.

ITTAGE table rows:

- updates, alloc-success, evicts, resident, and capacity per table.

ITTAGE aggregate:

- `hits_delta` / `misses_delta`: commit hits/misses;
- `evicts_delta`: sum over tables;
- resident/capacity: sum over tables.

TAGE table rows:

- updates, alloc-success, evicts, resident, and capacity per table.

## Stage 3B: validation and metric derivation

The validator must reject duplicate headers, missing scopes, negative/wrapped
deltas, incomplete phases, `maxinsts <= warmup`, resident values above
capacity, and non-finite derived metrics.

Derive metrics without cross-component subtraction:

```text
B-b_MBTB_candidate = hits(candidate) / mean(hits over all eight MBTB ways)
B-b_ITTAGE_candidate = updates(candidate) / mean(updates over T0..T4)

B-c_MBTB = sum(way evicts) / (component hits + component misses)
             * component resident / component capacity
B-c_ITTAGE = sum(table evicts) / (commit hits + commit misses)
               * component resident / component capacity
B-c_TAGE_t = evicts[t] / (updates[t] + alloc_success[t])
               * resident[t] / capacity[t]
```

Zero denominators produce `NA`, never zero. MBTB bank0.way3 and bank1.way3
remain separate donor candidates; ITTAGE T2 and T3 remain separate donor
candidates.

Validation matrix:

| Case | Required result |
|---|---|
| Aligned 20M+20M | 200 warmup and 200 sample phases, no transition |
| Unaligned warmup | the warmup-crossing phase is transition and excluded |
| Warmup zero | all complete phases are sample |
| Zero access | raw deltas may be zero; derived metric is `NA` |
| Stats reset | rows remain; in-phase reset is transition, boundary reset advances the next epoch |
| Baseline off | raw CSV is populated and old architectural stats are unchanged |
| Invalid run | `maxinsts <= warmup` is rejected |

## Stage 3C: open-loop beta replay

Use the last 128 finite warmup values per metric. Require at least 32 values.
Use population standard deviation (`ddof=0`) and strict comparison `z > theta`.
Do not update the reference distribution during sample execution.

For each legal direction (`MBTB->TAGE`, `ITTAGE->TAGE`) and each
`theta in {0.5, 1.0, 1.5, 2.0, 2.5}` report:

- joint threshold crossings before cooldown;
- cooldown-blocked crossings;
- open-loop fires after deterministic arbitration;
- T3/T7 fire split;
- short-history and zero-std fallback counts;
- not-evaluable phases.

One phase may fire at most one action across both directions, which share the
per-donee cooldown. Among evaluable legal pairs, pick the donor with highest
donor z and the available donee with highest donee z; break ties by stable
direction/slot/table id. A non-evaluable candidate removes only its pairs, not
the entire direction. Match the current cooldown: T3=8, T7=20 phases.

The taxonomy's absolute fallback does not define a donee predicate. Therefore
short-history and zero-std cases are `not_evaluable`; do not silently fire.

Baseline-only replay cannot estimate causal IPC gain or false-fire. Emit
`IPC_delta_estimate=NA` and `false_fire_count=NA`. A non-persistent signal may
be reported under that name, but must not be relabeled as false-fire.

## Artifacts

Tracked implementation:

- predictor raw-snapshot APIs and `sway_phase_bc_raw.csv` dump;
- `util/xs_scripts/sway/validate_phase_stream.py`;
- `util/xs_scripts/sway/replay_beta.py`;
- `util/xs_scripts/sway/run_stage3_cpt_gem5.sh` for explicit CPU/BPU warmup
  injection without the Stage 2 maxinst subtraction wrapper;
- focused tests for raw delta, reset/segment semantics, validator, and replay.

Run artifacts under ignored `runs/stage3_beta_replay/`:

- `workload_manifest.tsv`;
- `phase_stream_validation.tsv`;
- `beta_events.csv`;
- `beta_theta_per_workload.tsv`;
- `beta_theta_summary.tsv`;
- `stage3_beta_report.md`.

## Hard gates

- Do not modify the live SWAY decision predicate in Stage 3.
- Do not launch Phase C or claim an IPC result from baseline replay.
- Do not select or lock theta from trigger count alone.
- Do not treat the old seven-workload set as a validated held-out protocol.
- If a primary direction has fewer than five beta-evaluable workloads after
  undefined/zero-std exclusion, stop before Stage 4 and report the gate miss.
- PENDING-24 remains separate; ChampSim is excluded from the CAL-primary
  Stage 3 run until that regression is fixed.
