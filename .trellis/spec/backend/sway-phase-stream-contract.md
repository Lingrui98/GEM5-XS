# SWAY Phase-Stream and Open-Loop Replay Contract

> Executable contracts for collecting baseline-safe SWAY phase pressure,
> validating `sway_phase_bc_raw.csv`, and replaying the self-normalized beta
> policy without changing live predictor behavior.

---

## Scenario: Baseline Phase Collection and Beta Replay

### 1. Scope / Trigger

Read this file before changing any of these surfaces:

- `src/cpu/pred/btb/{mbtb,btb_ittage,btb_tage}.*`
- `src/cpu/pred/btb/{sway_realloc,decoupled_bpred}.*`
- `src/cpu/pred/BranchPredictor.py`
- `configs/example/kmhv3.py`
- `util/xs_scripts/sway/`

The contract applies whenever a change affects SWAY phase-local counters,
committed-instruction boundaries, warmup/reset segmentation, raw CSV schema,
B-b/B-c derivation, replay arbitration, or Stage 3 workload preparation.

Stage 3 is observational. It must not invoke the live SWAY controller, mutate
ownership, or claim a causal performance result.

### 2. Signatures

#### C++ snapshot surface

```cpp
struct sway::PhaseCounterSnapshot {
    std::string component;
    std::string scope;
    std::string scopeKind;
    std::optional<uint64_t> hits;
    std::optional<uint64_t> misses;
    std::optional<uint64_t> updates;
    std::optional<uint64_t> allocSuccess;
    std::optional<uint64_t> evicts;
    uint64_t residentEntries;
    uint64_t capacityEntries;
};

std::vector<sway::PhaseCounterSnapshot>
MBTB::collectAndResetSwayPhaseCounters();

std::vector<sway::PhaseCounterSnapshot>
BTBITTAGE::collectAndResetSwayPhaseCounters();

std::vector<sway::PhaseCounterSnapshot>
BTBTAGE::collectAndResetSwayPhaseCounters();

void DecoupledBPUWithBTB::collectSwayPhasePressureForPhase(int phaseID);
```

`sway::phaseSegment(...)` maps a completed phase to `warmup`, `transition`, or
`sample` using its instruction interval, configured warmup boundary, and stats
epochs.

#### SimObject/config surface

```text
system.cpu[0].branchPred.phaseSizeByInst=<positive integer>
system.cpu[0].branchPred.swayWarmupInsts=<non-negative integer>
system.cpu[0].branchPred.enableSwayRealloc=False
system.cpu[0].warmupInstCount=<same value as swayWarmupInsts for CPT runs>
```

`configs/example/kmhv3.py` must forward the command-line warmup count to
`swayWarmupInsts`. The legacy live-controller instruction clock remains
unchanged.

#### Runner environment

`util/xs_scripts/sway/run_stage3_cpt_gem5.sh` requires:

```text
XS_MAX_INSTS
XS_WARMUP_INSTS_NO_SWITCH
```

It injects the same warmup count into the CPU and BPU plus
`enableSwayRealloc=False`. It rejects non-integers and
`XS_MAX_INSTS <= XS_WARMUP_INSTS_NO_SWITCH`.

#### Tool CLIs

```bash
python3 util/xs_scripts/sway/prepare_stage3_runs.py \
  [--source-manifest FILE] --run-root DIR [--dry-run]

python3 util/xs_scripts/sway/validate_phase_stream.py RAW.csv \
  --maxinsts N --warmup-insts N [--phase-size N] [--output-tsv FILE]

python3 util/xs_scripts/sway/replay_beta.py \
  (--input RAW.csv | --manifest MANIFEST.tsv) --output-dir DIR \
  [--family NAME] [--workload NAME]
```

Validator `--maxinsts` is the required observed instruction count, not only a
launch cap.

### 3. Contracts

#### Architectural clock and phase boundaries

- Raw phases use the CPU architectural committed-instruction clock used by
  warmup and maxinsts.
- Exactly one row group is emitted per complete phase. Partial tails are
  omitted.
- A boundary-aligned reset advances the next phase's stats epoch. A phase that
  contains a reset or crosses warmup is `transition` and not replay eligible.
- Component event counters are independent of gem5's global stats reset and
  reset only when collected by the snapshot API.
- `resident_entries_end` is a boundary snapshot, never a delta.

#### CSV schema v1

The header is exactly:

```text
schema_version,phase_id,phase_start_inst,phase_end_inst,phase_inst_count,
segment,replay_eligible,stats_epoch_start,stats_epoch_end,component,scope,
scope_kind,hits_delta,misses_delta,updates_delta,alloc_success_delta,
evicts_delta,resident_entries_end,capacity_entries
```

Line wrapping above is documentation-only; the file contains one header line.
Every complete phase has 23 rows:

- MBTB: eight `mbtb_bank{0,1}_way{0..3}` rows and `mbtb_all`.
- ITTAGE: five `ittage_t{0..4}` rows and `ittage_all`.
- TAGE: eight `tage_t{0..7}` rows.

Non-applicable event fields are empty. They must not be serialized as zero.

#### Counter meaning

- MBTB way: supplied prediction hits, valid-entry evictions, resident entries,
  and native capacity.
- MBTB aggregate: prediction hits/misses plus sums of way evictions,
  residents, and capacities.
- ITTAGE table: updates, successful allocations, valid-entry evictions,
  resident entries, and capacity.
- ITTAGE aggregate: commit hits/misses plus sums of table evictions,
  residents, and capacities.
- TAGE table: updates, successful allocations, valid-entry evictions,
  resident entries, and capacity.

#### Derived metrics

```text
B-b_MBTB_candidate = candidate_hits / mean(all eight MBTB way hits)
B-b_ITTAGE_candidate = candidate_updates / mean(ITTAGE T0..T4 updates)

B-c_MBTB = sum(way evicts) / (component hits + component misses)
             * component resident / component capacity
B-c_ITTAGE = sum(table evicts) / (commit hits + commit misses)
               * component resident / component capacity
B-c_TAGE_t = evicts[t] / (updates[t] + alloc_success[t])
               * resident[t] / capacity[t]
```

Zero denominators produce `NA`, never zero. The two MBTB way-3 candidates and
ITTAGE T2/T3 remain independent candidates.

#### Open-loop beta replay

- Reference history is the last 128 finite warmup values per metric; at least
  32 are required.
- Use population standard deviation (`ddof=0`) and strict `z > theta`.
- Sample values do not update reference history.
- History-short, zero-standard-deviation, and sample-NA states remove only
  affected legal pairs. They do not fire an absolute fallback.
- Sweep theta over `0.5, 1.0, 1.5, 2.0, 2.5`.
- A phase fires at most one action across both directions.
- Select highest donor z, then highest available donee z, with stable
  direction/slot/table-id tie breaks.
- Both directions share per-donee cooldowns: T3=8 phases, T7=20 phases.
- Baseline-only replay always reports `IPC_delta_estimate=NA` and
  `false_fire_count=NA`.

The Stage 3 hard gate counts input workloads with at least one evaluable legal
pair. It is not the paper's whole-run performance-selected `SWAY-target` filter.

### 4. Validation & Error Matrix

| Condition | Required result | Enforcement |
|---|---|---|
| Aligned 20M warmup + 20M sample, W=100K | 400 phases; 200 warmup, 0 transition, 200 sample; 9,200 rows | validator |
| Warmup boundary inside a phase | crossing phase is `transition`, not replay eligible | C++ segment test + validator |
| Warmup is zero | every complete phase is `sample` | smoke + validator |
| Global stats reset | raw event deltas remain valid; epoch changes are explicit | C++ tests + validator |
| Zero metric denominator | derived value is `NA` | Python tests |
| Missing/duplicate scope or header | reject input | validator |
| Negative/wrapped delta | reject input | validator |
| Resident exceeds capacity | reject input | validator |
| Aggregate does not equal native-scope sum | reject input | validator |
| Observed total below `--maxinsts` | reject as early termination | validator |
| `maxinsts <= warmup` | runner and validator reject | shell/Python tests |
| `enableSwayRealloc=False` | raw CSV exists; live ownership and old stats remain baseline-equivalent | paired smoke |
| Short/constant history | affected pair is not evaluable; no alpha fire | replay tests |
| Baseline-only replay | causal IPC and false-fire fields are `NA` | replay tests |

### 5. Good / Base / Bad Cases

Good:

- The run command injects identical CPU/BPU warmup values.
- `enableSwayRealloc=False`, `reallocCount=0`, and the raw CSV is populated.
- Every formal stream passes strict validation before replay.
- Reports call 26/20-style counts beta-evaluable, not performance targets.

Base:

- Existing architectural stats and the legacy live-controller phase clock keep
  their prior semantics.
- A raw stream may contain `NA` metrics without invalidating unrelated pairs.

Bad:

- Subtract warmup from maxinsts in a wrapper and stop at the warmup boundary.
- Difference resident snapshots as if they were counters.
- Convert `0/0` to zero or use a cross-component B-c subtraction.
- Fire alpha fallback when no donee predicate exists.
- Select theta from trigger count or claim IPC from open-loop baseline replay.

### 6. Tests Required

For contract changes, run at least:

```bash
scons build/RISCV/gem5.opt \
  build/RISCV/cpu/pred/btb/test/btb.test.opt \
  build/RISCV/cpu/pred/btb/test/tage.test.opt \
  --unit-test --gold-linker -j64

build/RISCV/cpu/pred/btb/test/btb.test.opt
build/RISCV/cpu/pred/btb/test/tage.test.opt

PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s util/xs_scripts/sway/tests -p 'test_*.py'
```

Also run aligned and unaligned warmup smokes plus one baseline-equivalence
pair. Before a formal verdict, require all manifest rows to be `completed`, no
abort markers, one PASS validation row per unique stream, and no
panic/fatal/trace-mismatch log hits.

### 7. Wrong vs Correct

#### Wrong

```cpp
// Global gem5 stats can reset at warmup, so differencing them corrupts phases.
auto phaseEvicts = stats.evicts.value() - previousGlobalEvicts;

// Resident entries are snapshots, not events.
auto phaseResident = residentNow - residentPrevious;
```

```python
# NA is not evidence of no pressure, and trigger count is not performance.
metric = numerator / denominator if denominator else 0.0
selected_theta = max(rows, key=lambda row: row["open_loop_fires"])["theta"]
```

#### Correct

```cpp
// Component-local event counters survive global stats reset and are reset only
// when the complete architectural phase is collected.
append(mbtb->collectAndResetSwayPhaseCounters());
```

```python
metric = None if denominator == 0 else numerator / denominator
ipc_delta_estimate = "NA"
false_fire_count = "NA"
```

**Rule of thumb**: Stage 3 establishes faithful observability and trigger
feasibility. Only paired live-controller runs can establish outcome quality.
