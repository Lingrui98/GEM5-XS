# SWAY Phase 1 Task C — D6 Heterogeneous-Substrate PoC Rework

## Spec authority

This PRD implements the locked design in
`/nfs/home/goulingrui/project/papers/sway-paper/docs/SWAY_MECHANISM_SPEC_v2.md`.
On conflict with PLAN.md / INSIGHT.yaml mechanism sections (which are
pending rewrite), SPEC v2 is authoritative. Open items in SPEC §7
(PENDING-16~22) flow into this PoC's PENDING list.

This task supersedes `archive/2026-06/06-XX-sway-realloc-controller-poc`.
That archived PoC's 10-scope per-table + sidecar approach is retained as
a strawman ablation (per SPEC §8.1); do not delete its data or commits.

## Goal

Reimplement the SWAY PoC under the design captured in SPEC v2:

1. **Component-level ownership** (P1) — owner space `{MBTB, ITTAGE, TAGE}`,
   no per-TAGE-table ownership.
2. **Donee-history-scaled cool-down** (P2) — borrowed capacity into TAGE
   pinned long enough for the deepest donee history to train.
3. **TAGE-only-donee** (P3') — TAGE never donates; MBTB and ITTAGE donate.
4. **D6 4-slot heterogeneous substrate** —
   2 MBTB tight slots (way-level, same prediction cycle) +
   2 ITTAGE table-level relaxed slots (atomic-table donate, +1 cycle).
5. **R1-R5 index/packing rules** — set-count alignment via extended index
   (R2a, S_d > S_e) or extra-tag bits (R2b, S_d < S_e), packing factor
   computed with extra-tag overhead (R4, **non-orthogonal to R2**).
6. **Useful-bit lifecycle fixes** — Fix1/2/3 from SPEC §5.
7. **Parameterized donee_set sweep** — Phase 1 D6 picks main result from
   {[T7], [T6,T7], [T3,T7], [T4,T7], [T3,T5,T7]}.

The PoC must produce: (a) main IPC speedup numbers under the locked
substrate, (b) Phase 0 ITTAGE per-table provider heatmap (A20) to settle
PENDING-17, (c) over-provisioning upper-bound comparison vs the archived
PoC (PENDING-18), and (d) supporting data for §IV/§V rewrite.

## Confirmed component shapes (from runs/eval_baseline_W100K config.json)

| Component | numEntries / numSets | numWays | per-entry width estimate |
|---|---|---|---|
| MBTB        | 8192 (numEntries) → 1024 sets per SRAM | 4-way per SRAM | ~70 bit (tagBits=20 + target + ctrl) |
| BTBTAGE T0..T7 | 2048 indices / sets per table | 2-way | ~18 bit (TTagBits=13 + ctr 3 + useful 1 + valid 1) |
| BTBITTAGE T0..T4 | [256,256,512,512,512]            | 1-way (direct-mapped) | ~53 bit (TTagBits=9 + target ~40 + ctr 2 + useful 1 + valid 1) |

Substrate pair index/packing rule selection (R1-R5):

```
MBTB way      (S=1024, W=70)  → TAGE T_k (S=2048, W=18)
  R2b (extra_tag = 1 bit) + R4 (sub_entry = 19; packing = ⌊70/19⌋ = 3)

ITTAGE T0/T1  (S=256,  W=53)  → TAGE T_k (S=2048, W=18)
  R3 excluded (8× set-count ratio)

ITTAGE T2/3/4 (S=512,  W=53)  → TAGE T_k (S=2048, W=18)
  R2b (extra_tag = 2 bit) + R4 (sub_entry = 20; packing = ⌊53/20⌋ = 2)
```

ITTAGE T0/T1 crosses R3's 4× ratio bound under current source semantics and
is excluded unless a later SPEC update changes the hardware mapping.

## Requirements

### Substrate

- `sway_realloc.hh` exposes:
  - `NumComponents = 3` (MBTB, ITTAGE, TAGE);
  - `DonorSlot` struct with `id`, `source_component`, `source_index`,
    `kind ∈ {way_level, table_level}`, `latency_class ∈ {tight, relaxed}`,
    `capacity_bytes`;
  - `DoneeTableSet` parameter (vector of TAGE table ids), with candidate
    sets `{[7], [6,7], [3,7], [4,7], [3,5,7]}` selectable at run time;
  - `ExtraBitSource` enum (`GhrFoldHigh`, `PathHistBit`, `PcHighBit`,
    etc.), one entry per donor-donee pair, **design-time fixed**.
- Owner field on each donor slot is a 2-bit enum `{native, donee_A, donee_B}`
  (extend to 2 bits per slot, not per way).
- For table-level donor slots (ITTAGE), donation is atomic over the whole
  table: while donated, ITTAGE skips lookup and update on that table; the
  table's SRAM output is routed to the donee path with a 1-cycle delay.

### Lookup paths

- MBTB tight slot path: when owner ∈ {donee_A, donee_B}, the donor SRAM
  output gates through the donee table's hit-compare logic in the same
  prediction cycle. Under current source semantics this uses the R2b
  extra-tag scheme (low native-index bits select the donor set, one high
  native-index bit is appended to the borrowed tag). R4 packing factor splits
  the donor way into 3 TAGE-style sub-entries; parallel tag compare across
  the 3 sub-entries.
- ITTAGE table-level relaxed slot path: while donated, the table is logically
  removed from ITTAGE prediction; its SRAM output is routed to the donee
  TAGE table's hit-compare with a 1-cycle delay. On borrowed-bank hit,
  trigger a single-cycle redirect (squash + re-predict via borrowed entry).
  R2b extra-tag scheme used. R4 packing = 2.
- Borrowed-bank lookup uses the donee table's own `(index, tag)` computation
  (i.e., extends donee's hash to address the borrowed bank); no
  cross-table hash sharing on the donor side.
- Native lookup paths are bit-exact preserved when `enableSwayRealloc=False`.

### Useful-bit lifecycle (SPEC §5)

- **Fix 1**: `squashNativeWay()` in MBTB / ITTAGE / TAGE additionally
  resets `useful = 0` and `counter = 0` (currently only resets `valid`).
- **Fix 2**: When the controller shrinks a borrowed sidecar (way-level)
  or returns a donated table (table-level), the affected sub-entries are
  explicitly cold-reset before the storage is released, not implicitly
  truncated.
- **Fix 3**: New stat `system.cpu.branchPred.sway.usefulWastedByRealloc`
  (counter + histogram) records the number of `useful=1` entries lost
  per `transferSwayWays` event.

### Controller

- Per-component utility:
  `utility(c) = sum_scope_in_c(active_ways) / sum_scope_in_c(total_ways)`.
  TAGE utility aggregates all 8 tables. ITTAGE utility aggregates all 5
  tables but **per-table active/total** is also tracked separately for
  table-level donate decisions.
- 5-action enumeration per phase boundary (SPEC §4.1).
- Hysteresis margins:
  - tight slot: `hysteresis_margin_tight = 0.10`;
  - relaxed slot: `hysteresis_margin_relaxed = 0.20`
    (must exceed `redirect_cost_estimate × hit_freq_estimate`).
- ITTAGE table-level donate gates (SPEC §4.4):
  - C1: table active/total < 0.30 for K=5 consecutive phases;
  - C2: chosen donee table active/total > 0.70;
  - C3: utility_gap × packing_factor > redirect_cost × hit_freq_estimate.
- Per-component cool-down (SPEC §4.3):
  - MBTB → TAGE: scale by donee history length; T7 ≈ 20 phases, T3 ≈ 8;
  - ITTAGE table → TAGE: `max(donee scale, K phases)`;
  - Return: 1 phase.

### Stats

Add or extend (preserving existing `sway.reallocCount`,
`sway.reallocBlockedQuiesce`, `sway.ownerOverrides`):

- `sway.slotOwnerTransitionsPerSlot[slot_id]` (per-slot counter);
- `sway.borrowedHitsByDonee[table_id]` (per-donee-table hit count);
- `sway.relaxedRedirectsTriggered` (single counter for ITTAGE-borrow hits);
- `sway.usefulWastedByRealloc` (counter + histogram, Fix 3);
- `sway.coolDownBlockedByDonee[table_id]`;
- `sway.tableDonationActiveCycles[ittage_table_id]`.

### Backward compatibility

- New SimObject parameters live under `DecoupledBPUWithBTB`:
  - `swayDoneeTableSet` (string, comma-separated TAGE table ids, default empty);
  - `swayExtraBitSourcePerPair` (string, JSON-encoded mapping, default empty);
  - `swayEnableITTAGEDonor` (bool, default False);
  - `swayEnableRelaxedSlot` (bool, default False — staged enable for D7);
  - keep `enableSwayRealloc`, `swayReallocWays`, `swayReallocHysteresis`;
  - deprecate `swayReallocTageDoneeHysteresis` (no longer used; warn if set).
- `enableSwayRealloc=False` → bit-exact baseline behavior, identical to
  archived PoC's baseline mode.

### Compute & validation budget

- 10 SPEC17 cpt workloads × 5M instr × W=100K (matches Task A/B).
- Baseline + 4 donee_set candidates ({T7}, {T6,T7}, {T3,T7}, {T4,T7}) × 10 workloads = 50 runs.
- Add 1 strawman re-run of archived PoC under identical 10-workload config to anchor over-provisioning bracketing (PENDING-18).
- All runs distributed via mytools cpt-mode scheduler.

## Acceptance criteria

- [x] **D0 prep**: SPEC v2 read and PoC plan locked. BTBTAGE / MBTB / BTBITTAGE
      shapes confirmed against `runs/eval_baseline_W100K/main_W100K/<wl>/config.json`.
      Evidence: `docs/exec-plans/active/sway-d6-substrate-rework.md` records
      SPEC/PRD reads and confirms shapes from
      `runs/eval_baseline_W100K/main_W100K/gcc_pp_O2_1869/config.json`.
- [x] **D1 substrate scaffold**: `sway_realloc.hh` refactored to
      `NumComponents=3`; donor-slot + donee-table-set + extra-bit-source
      parameterization added; compiles clean.
      Evidence: D1 scaffold landed in `src/cpu/pred/btb/sway_realloc.hh`,
      `src/cpu/pred/BranchPredictor.py`, and top-level BPU param wiring;
      `scons build/RISCV/gem5.opt --gold-linker -j32` passed on
      2026-06-30. D2 lookup/packing still waits on the recorded shape-semantics
      decision.
- [x] **D2 MBTB tight slot**: MBTB owner field added on 2 designated ways;
      source-semantics R2b geometry recorded for the MBTB→TAGE tight path;
      baseline behavior bit-exact when
      `enableSwayRealloc=False`. BTB unit test suite 19/19 pass.
      Evidence: `src/cpu/pred/btb/sway_realloc.hh` now computes
      MBTB→TAGE as R2b (`extra_tag=1`, packing=3);
      `src/cpu/pred/btb/mbtb.cc` only transfers the two designated MBTB tight
      slots and disables old synthetic sidecar accounting; `btb.test.opt`
      passed 21/21 including two D2 SWAY tests; `tage.test.opt` passed 28/28;
      `build/RISCV/gem5.opt --gold-linker -j32` passed after one generated
      header-race retry; gcc_pp_O2 5M baseline-off smoke exited at
      `tick 1370629998`, with IPC `1.214771`, branchMispredicts `84205`,
      `tage.updateMispred=38549`, `sway.reallocCount=0`, and exact
      `sway_stranded_by_phase.csv` / `sway_utility_by_phase.csv` sha256 match
      vs `eval_baseline_W100K/main_W100K/gcc_pp_O2_1869`.
- [x] **D3 TAGE borrow path**: Borrowed-bank lookup implemented for designated
      TAGE donee table(s) over the MBTB tight donor slots; R2b + R4 packing;
      Fix 1/2/3 useful-bit lifecycle for TAGE native / old sidecar /
      MBTB-borrowed storage.
      Evidence: BTBTAGE unit test suite 33/33 pass, including five SWAY
      borrowed-bank tests covering MBTB->T_k prediction, configured-donee
      gating, R2b extra-tag alias disambiguation, legal ITTAGE-mid-table
      packing geometry (`512 -> 2048`, extra_tag=2, packing=2), and borrowed
      capacity in the donee snapshot; BTB unit test suite 21/21 pass;
      `gem5.opt` serial build pass after generated-header races at higher
      parallelism; gcc_pp_O2 5M SWAY T7 smoke exited cleanly at
      `tick 1364341626` with `simInsts=5000005`. The smoke did not trigger
      controller transfer (`sway.reallocCount=0`), so active borrowed hits are
      covered by unit tests. ITTAGE relaxed-slot runtime wiring remains D5/D7
      scope and PENDING-17 heatmap-gated.
- [x] **D4 controller**: 5-action enumeration; per-component utility; tight
      and relaxed hysteresis margins; per-donee cool-down (P2); 1-cycle
      quiesce preserved.
      Evidence: `sway::ControllerAction` now enumerates none,
      MBTB->TAGE tight donate, MBTB tight return, ITTAGE->TAGE relaxed donate,
      and ITTAGE relaxed return. The executable D4 policy aggregates MBTB and
      TAGE component utility, selects donee tables only from
      `swayDoneeTableSet`, applies default tight/relaxed margins 0.10/0.20,
      arms TAGE donee cooldown (`T3=8`, `T7=20` phases), preserves the
      existing one-cycle quiesce, and records
      `sway.coolDownBlockedByDonee`. MBTB tight donate/return paths are
      unit-tested in BTB 21/21; BTBTAGE 33/33 still passes; `gem5.opt -j32`
      passed after one generated-header race retry; gcc_pp_O2 1M SWAY T7
      smoke exited cleanly at `tick 376073550` with `simInsts=1000002`.
      The smoke did not trigger donation (`sway.reallocCount=0`) because
      measured component utility had MBTB hotter than TAGE in the first phases
      (`tage_avg - mbtb_avg` negative), so ITTAGE relaxed runtime remains D5/D7
      scope and heatmap-gated.
- [x] **D5 ITTAGE wiring + Phase 0 heatmap**:
      - [x] `sway.borrowedHitsByDonee` and ITTAGE per-table provider stats
        (`useProviderTable[t]`, `mispredictUseProviderTable[t]`,
        `activeEntriesByTable[t]`, `validEntriesByTable[t]`,
        `totalEntriesByTable[t]`) emitted. Evidence: gcc_pp_O2 1M SWAY T7
        smoke under
        `.regression_runs/sway_d5_ittage_active_stats_gcc_pp_O2_1m_v3/`
        exited at `tick 376073550` with `simInsts=1000002` and emitted
        `system.cpu.branchPred.ittage.useProviderTable::ittage_t0..t4`
        (`2988,1361,754,739,272`) and
        `system.cpu.branchPred.ittage.mispredictUseProviderTable::ittage_t0..t4`
        (`119,99,63,63,33`), plus `activeEntriesByTable`
        (`114,111,136,90,76`), `validEntriesByTable`
        (`238,215,244,176,159`), and `totalEntriesByTable`
        (`256,256,512,512,512`).
      - [x] 10-workload baseline rerun produced ITTAGE per-table provider /
        mispredict / active heatmap CSV under
        `runs/phase0_ittage_heatmap/main_W100K/`; scheduler completed
        10/10 tasks with no aborts and wrote 50 aggregate rows plus
        per-workload `ittage_per_table_heatmap.csv` files.
      - [x] PENDING-17 decision recorded: expose ITTAGE T2 and T3 as relaxed
        table-level donor slots. Evidence:
        `ittage_per_table_heatmap_summary.csv` shows among R2b-eligible
        T2/T3/T4: T2 use=30574, mispredict=507, active_ratio=0.0914;
        T3 use=29470, mispredict=1298, active_ratio=0.0957; T4 has higher
        use=48738 and mispredict=3377.
- [x] **D6 main rerun**:
      - [x] 10 SPEC17 cpt × {baseline, 4 donee_set candidates} × W=100K =
        50 final runs complete under
        `runs/eval_sway_W100K_d6/<donee_set>/<wl>/`. Note: T6T7/T3T7
        dispatch logs retain an initial aborted attempt caused by unquoted
        comma StringParam values; fixed by passing
        `swayDoneeTableSet="6,7"` / `"3,7"` / `"4,7"` and reran to
        completed latest statuses.
      - [x] `figure4_ipc_uplift.pdf` regenerated from nominal main
        `{T3,T7}` and new `figure5_donee_set_sweep.pdf` generated.
      - [x] geomean / `gcc_pp_O2` / `mcf` ΔIPC recorded per donee_set in
        `figures/figure5_donee_set_sweep.csv`. All swept sets tied at
        geomean `+0.253835%`, `gcc_pp_O2=+0.460910%`, `mcf=0.000000%`,
        and `total_reallocCount=0`; PENDING-16 is therefore a no-active-
        transfer tie, resolved nominally to `{T3,T7}` by user preference.
- [x] **D7 補強 + over-provisioning bracket + analysis**:
      - D6 does **not** motivate landing ITTAGE relaxed runtime: all D6 donee
        sets tied with `total_reallocCount=0`, so relaxed table donation would
        be unexercised rather than evidence-backed. The PoC keeps the relaxed
        knobs default-off and emits zero-valued relaxed observability stats.
      - PENDING-18: archived PoC `a48d6355f1` (10-scope per-table + sidecar,
        K=1 + `swayReallocTageDoneeHysteresis=0.45`) reran under the same
        10-workload W=100K 5M-instruction config with 10/10 completed and
        0 aborts. Artifacts:
        `runs/over_provisioning_bracket/archive_k1_tageh045_rerun_a48d6355f1/`
        and `runs/over_provisioning_bracket/bracket_summary.csv`.
      - Bracket result: archived sidecar PoC geomean `+0.773573%`;
        D6 nominal `{T3,T7}` geomean `+0.253835%`; over-provisioning gap
        `+0.519738%`. `gcc_pp_O2` gap is `+1.849649%`; `mcf` gap is
        `+0.501948%`.
      - PENDING-19 recorded as "废止": D6 accepts but ignores
        `swayReallocTageDoneeHysteresis`; tight/relaxed margins replace it.
      - PENDING-20 recorded as "not used by D6 active pairs": current
        MBTB->TAGE and ITTAGE T2/T3->TAGE mappings are R2b, so extra tag bits
        come from the donee native index high bits; no R2a extra-bit source is
        selected in this PoC.
- [x] **Negative-result honesty (A18)**: if ITTAGE table-level donate
      does not trigger or yields negative IPC delta, record it explicitly
      as a negative ablation result; do not hide, retune, or drop it from the
      data set. D6 is recorded as a no-active-transfer tie; ITTAGE relaxed
      runtime is not landed because the measured D6 controller never reaches
      a transfer on this cohort.
- [x] **Branch hygiene**: final change split into ≥3 commits — (a) substrate
      scaffold + MBTB tight + TAGE borrow + useful-bit fixes; (b) controller +
      ITTAGE wiring + stats; (c) eval / figures / Trellis closeout. PoC branch
      `sway/d6-substrate-rework` based on `sway/phase-stranded-probe`.
      Evidence: `aedcb84d0f` (substrate/tests), `9faf270f99`
      (controller/stats), and the Trellis closeout commit.

## Definition of Done

- Owner-field + borrowed-bank lookup behavior is implemented for MBTB
  tight slots and TAGE designated donee tables.
- Controller produces per-phase decisions consistent with SPEC §4.
- Baseline (`enableSwayRealloc=False`) behavior bit-exact vs Task A/B baseline.
- 50-run D6 main batch completes without aborts.
- Phase 0 ITTAGE heatmap collected and PENDING-17 decision recorded.
- Over-provisioning bracket between archived PoC and D6 PoC recorded
  (PENDING-18).
- Final commits, figures, and SPEC v2 PENDING table reflect measured results.

Strict performance note: this PoC is allowed to land **any** geomean delta
(positive, zero, or negative). The honesty rule in A18 takes priority over
the previous PoC's "must clear +0.5%" strict gate. SPEC §8.3 enumerates
fallback narratives for情形 A/B/C; do not retune to force a positive number.

## Technical approach

### Substrate construction (sway_realloc.hh + BranchPredictor.py)

Replace the 10-scope owner space with:

```cpp
enum class Component : uint8_t { MBTB = 0, ITTAGE = 1, TAGE = 2, Invalid = 0xff };

struct DonorSlot {
    uint8_t id;
    Component source;            // owning component
    uint8_t source_index;        // bank id (MBTB) or table id (ITTAGE)
    uint8_t source_way;          // way id for way_level; 0xff for table_level
    enum Kind { WAY_LEVEL, TABLE_LEVEL } kind;
    enum Latency { TIGHT, RELAXED } latency;
    uint32_t capacity_bytes;
    // Per-pair extra bit source: indexed by donee table id
    std::array<ExtraBitSource, NumTageTables> extra_bit_per_donee;
};

struct DoneeSetConfig {
    std::vector<uint8_t> table_ids;       // e.g. {3, 7}
};
```

`DecoupledBPUWithBTB` constructs the 4 donor slots from python params at
init time; `enableSwayRealloc=False` short-circuits all slot logic so the
baseline path is untouched.

### MBTB tight slot (mbtb.{hh,cc})

- Add `swayOwnerSlot[bank][way]` for the 2 designated slots (0xff for non-slot ways);
- Lookup: when slot owner == donee_A or donee_B and the lookup is from
  that donee table, **skip MBTB hit-compare for that way** and route the
  SRAM data to the borrowed-bank output bus;
- Update path: while slot is donated, MBTB never alloc / update on that way;
- Per-pair borrowed-index function records the source-semantics geometry. For
  MBTB→TAGE this is R2b: `idx_borrow` uses the low native-index bits and the
  high native-index bit becomes `extra_tag`.

### TAGE borrowed-bank lookup (btb_tage.{hh,cc})

- For each donee table in the active `DoneeSetConfig`, add a `BorrowedBankView`
  struct storing per-slot pointer + pre-computed packing factor + extra-tag
  width;
- In `generateSinglePrediction`, after native ways are scanned, iterate
  active borrowed bank views: compute borrowed-bank index via R2a/R2b rule,
  fetch packing-factor sub-entries, parallel-compare donee tag (and extra
  tag for R2b);
- `mutableTageEntry` extended: a `TageTableInfo::isBorrowed = true` case
  resolves to the borrowed slot's storage;
- `handleNewEntryAllocation`: when a donee table runs out of native
  invalid/weak-not-useful candidates, alloc into borrowed bank using the
  same victim priority (R5);
- Fix 1: extend `squashNativeWay` to reset useful + counter;
- Fix 2: borrowed-bank shrink path explicitly resets sub-entries before
  resize;
- Fix 3: `transferSwayWays` counts useful=1 entries lost per call.

### ITTAGE table-level donate (btb_ittage.{hh,cc})

- `tableDonatedFlag[table_id]` bool array;
- When donated: bypass that table in `predictTaken` / `update` / `allocate`;
  the table's SRAM data is forwarded to the donee bus with a 1-cycle delay
  (simulated by 1-cycle latch in GEM5);
- Stat `tableDonationActiveCycles[table_id]` increments while donated;
- ITTAGE per-table provider stats (`useProviderTable[t]`,
  `mispredictUseProviderTable[t]`) — mirror BTBTAGE-side stats if missing.

### SwayController rewrite (decoupled_bpred.{hh,cc} + decoupled_bpred_stats.cc)

- Per-phase boundary:
  1. Snapshot per-component utility + per-table active/total;
  2. Enumerate 5 action candidates per slot;
  3. Apply hysteresis (tight vs relaxed) and cool-down filter;
  4. Pick the action with maximum net utility gain; tie-break by donee
     table mispredict frequency;
  5. Issue `transferSwayWays` or `donateITTAGETable` accordingly;
  6. Enter 1-cycle quiesce; update stats.
- Cool-down state: per-slot `coolDownRemaining` counter, decremented per
  phase; transitions blocked while > 0.
- Add `swayReallocTageDoneeHysteresis` deprecation warning (SimObject
  param still accepted but ignored; log a warn on first run).

### Phase 0 ITTAGE provider heatmap (A20)

- Add a `--sway-heatmap-only` runtime mode (or a one-off `enableHeatmapDump`
  param) that runs baseline + dumps the ITTAGE per-table provider /
  mispredict / active table to a CSV under
  `runs/phase0_ittage_heatmap/<wl>/ittage_per_table_heatmap.csv`;
- After 10-workload heatmap collection, glr / codex selects ITTAGE donor
  candidates (0, 1, or 2 tables) based on:
  - low `useProviderTable[t]` (dead-weight tables);
  - low `mispredictUseProviderTable[t]` (loss-tolerant tables);
  - low `active_ways/total_ways` (idle tables);
- Recorded decision back into SPEC §2.1 slot 2/3 + this PRD.

### Eval harness

- Reuse mytools cpt-mode scheduler:
  - workload list: `runs/configs/cpt_workloads.lst` (existing 10-workload SPEC17);
  - server list: `runs/configs/servers.lst`;
  - new backend configs:
    - `runs/configs/backends/sway_d6_donee_T7.backend.json`;
    - `runs/configs/backends/sway_d6_donee_T6T7.backend.json`;
    - `runs/configs/backends/sway_d6_donee_T3T7.backend.json`;
    - `runs/configs/backends/sway_d6_donee_T4T7.backend.json`;
    - `runs/configs/backends/sway_d6_overprovisioning_bracket.backend.json`
      (= archived PoC's K=1 config, rerun against the same workload set for
      bracketing).

## Decision (ADR-lite)

**Context**: SPEC v2 locked component-level ownership (P1), TAGE-only-donee
(P3'), and the D6 4-slot heterogeneous substrate. The archived PoC's
10-scope per-table + sidecar design no longer matches the locked physical
story (esp. that sidecar over-provisions storage and breaks the H1 "owner
gating over shared bank" claim). We need a new PoC that exercises the
locked substrate end-to-end.

**Decision**: Build a new PoC on a fresh branch `sway/d6-substrate-rework`
that implements P1/P2/P3' + D6 + R1-R5 + Fix1/2/3, while keeping the
archived PoC untouched as strawman/over-provisioning bracket. New PoC is
default-off; existing baseline runs unaffected.

**Consequences**:
- D6 PoC's IPC delta may differ substantially from archived PoC's +0.774%.
  情形 A/B/C in SPEC §8.3 cover the writing narrative for each outcome.
- ITTAGE donor candidate decision is data-driven (A20 heatmap) rather than
  intuition-driven.
- Over-provisioning bracket experiment (PENDING-18) is mandatory; without
  it we cannot honestly claim "real" vs "over-provisioned" gains.
- 0.45 hand-tuned hysteresis is废止 in this PoC; the locked design
  uses tight/relaxed hysteresis margins instead of TAGE-donee-specific guard.

## Out of scope

- No TAGE→MBTB or TAGE→ITTAGE transfers (P3' enforced at design time).
- No cross-TAGE-table transfers (P1 enforced at owner-space level).
- No RTL synthesis (RTL critical-path light claim is CHECKPOINT 3,
  PENDING-22).
- No CACTI overhead estimation in this task (PENDING-21, CHECKPOINT 3).
- No `swayReallocTageDoneeHysteresis` resurrection; the parameter is
  deprecated.
- No MicroTAGE, ABTB, RAS, or fast predictor involvement.
- No physical SRAM data migration model.
- No changes to mytools scheduler; reuse existing cpt-mode backend.

## Technical notes

- Branch `sway/d6-substrate-rework` based on `sway/phase-stranded-probe`
  (current SWAY worktree HEAD). The archived PoC branch / commits are
  retained; do not rebase over them.
- `BranchPredictor.py` BTBITTAGE config: `numPredictors=5`,
  `tableSizes=[256,256,512,512,512]`, `TTagBitSizes=[9]*5`,
  `histLengths=[4,8,13,16,32]`, 1-way per table (direct-mapped).
- BTBTAGE config (from baseline config.json): `numPredictors=8`,
  `tableSizes=[2048]*8`, `numWays=[2]*8`, `TTagBitSizes=[13]*8`,
  `histLengths=[4,9,17,29,56,109,211,397]`.
- MBTB config: `numEntries=8192`, `numWays=4` → 1024 sets per SRAM,
  2-bank (sram0/sram1) split by PC bit 5.
- Existing `phaseSizeByInst=100000`, `swayReallocWays`, and other Task A/B
  hooks remain valid; this PoC adds new parameters alongside, does not
  remove old ones (except the deprecated TAGE-donee hysteresis).
- DRAMsim3 submodule symlink still required: keep
  `ext/dramsim3/DRAMsim3 -> ~/project/GEM5/ext/dramsim3/DRAMsim3`.
- Build: `scons build/RISCV/gem5.opt --gold-linker -j32` (avoid -j160
  generated-header race observed in Task B).

## Open questions (to resolve before / during this PoC)

- **PENDING-17 ITTAGE donor candidate**: which 0/1/2 ITTAGE tables become
  table-level donor slots. Driven by D5 Phase 0 heatmap, not by short-history
  intuition. Until decision made, slot 2/3 remain disabled in D1-D4.
- **extra_bit_source per pair**: design-time-fixed enum; need to pick one
  history fold bit per (donor, donee) pair that is statistically independent
  of the donee's native index. Choice can be confirmed during D2/D3 unit
  testing; final mapping recorded in this PoC PRD + SPEC §6.
- **donee_set main result choice (PENDING-16)**: chosen empirically from
  the D6 sweep based on geomean ΔIPC. If two candidates are within ±0.1%
  geomean, prefer the broader-coverage set (e.g., {T3,T7} over {T6,T7}).
- **PENDING-18 over-provisioning bracket**: archived PoC rerun must use
  identical workload list, identical seed paths, identical W=100K, identical
  `--maxinsts=5000000` to be apples-to-apples comparable.
- **Failure mode for D5 heatmap collection**: if a stat does not exist
  in current ITTAGE codebase, add the stat (mirror BTBTAGE-side) before
  the heatmap rerun, not after.

## Final evidence template (fill at completion)

- Unit tests:
  - `./build/RISCV/cpu/pred/btb/test/tage.test.opt`: ? / 28 pass (existing
    + new borrowed-bank lookup tests);
  - `./build/RISCV/cpu/pred/btb/test/btb.test.opt`: ? / 19 pass.
- Build: `scons build/RISCV/gem5.opt --gold-linker -j32`: ?.
- Baseline regression: all 10 `eval_baseline_W100K/main_W100K/*/sway_stranded_by_phase.csv` bit-exact vs Task A.
- Phase 0 ITTAGE heatmap: CSV at `runs/phase0_ittage_heatmap/...`; donor
  decision PENDING-17 recorded.
- D6 sweep: 50-run table (10 workload × 5 config) with per-workload ΔIPC,
  geomean per config, main result config recorded in SPEC §6 PENDING-16.
- Over-provisioning bracket: bracketing gap (D6 PoC ΔIPC vs archived PoC
  ΔIPC under conservation gap).
- Negative results (if any): recorded explicitly per A18.

## Next iteration recommendations (fill at completion)

- Replace `active_ways/total_ways` utility with outcome-aware signals
  (BTB target miss, TAGE updateMispred/MPKI, donor-donee cooldown,
  rollback-on-regression) — PENDING-19.
- Add CACTI overhead estimation for D6 substrate — PENDING-21.
- Add RTL critical-path light claim with synthesis flow — PENDING-22.
- HPCA extension: TAGE-as-donor under integer-light workloads (P3'
  relaxation), cross-bar widening, full RTL.
