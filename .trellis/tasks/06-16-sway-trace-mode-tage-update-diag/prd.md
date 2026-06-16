# Diagnose why TAGE update path is silent in trace mode

## TL;DR

In trace mode (`--enable-trace-mode`), `system.cpu.branchPred.tage.updateMispred`
and all per-table `updateTableMispreds::N` counters read **0** across every
championship trace SWAY has run (3 IPC1 + 2 CVP1 + 2 CBP2025 at W=100K,
all 5M instr). The same binary in cpt mode (10 SPEC17 cpts × 3 W) produces
the expected ~10⁴ updates per workload. Lookup-side counters
(`tage.predHit` ≈ 262 601 on `ipc1_client_001`) are non-zero, so TAGE
*reads* the tables but never *updates* them.

The SWAY stranded probe (`wayVisitCnt`) is bumped on lookup hits, so the
**probe still has data** in trace mode (the `valid` and `active` fields
in `sway_stranded_by_phase.csv` are populated). But because update never
fires, the TAGE tables never *allocate* fresh entries on a mispred —
the `valid_ways` column stays at whatever the cold tables held at the
start of trace replay, and `active_ways` only counts the few patterns
that happen to tag-match those cold entries. That is why champ-trace
TAGE bars in `figure1_stranded_champ.pdf` are flat near 0.

This blocks using championship traces as supporting evidence for the
Q2 stranded → IPC causal chain. For CAL-class SWAY paper the SPEC17
cpt cohort is already sufficient (149× cross-workload spread,
correlations ≥ 0.92). The fix becomes blocking only when the HPCA
extension starts pulling in trace-mode workloads (DaCapo javac, etc.).

## Why now

- CHECKPOINT 1 is done on the SPEC17 cpt cohort (`PLAN.md` Week 5–6, ✅
  2026-06-15 refreshed 2026-06-16). Trace mode is **not** on the critical
  path for the CAL short paper.
- But the moment we run anything trace-mode in §V (workload coverage),
  this silence will produce misleading stranded numbers. We must
  document it now and fix before the HPCA extension lands.
- The bug also produces a teaching moment for §III: SWAY's stranded
  metric depends on a working update path; the diagnostics here will
  surface exactly which invariants the metric assumes.

## Evidence already collected

### Trace-mode (broken)
```
$ grep "tage\." runs/W100K_champ/ipc1_W100K/ipc1_client_001/stats.txt | head
system.cpu.branchPred.tage.predHit             262601   # non-zero
system.cpu.branchPred.tage.updateMispred            0   # ← broken
system.cpu.branchPred.tage.updateTableMispreds::0   0
... (all 8 tables zero)
system.cpu.branchPred.tage.updateAllocSuccess       0   # never allocates
system.cpu.branchPred.tage.updateAllocFailure       0
system.cpu.branchPred.tage.predNoHitUseBim    1401854   # lookups happen
```

Stranded CSV from the same run:
```
$ grep "^[0-9]*,tage" runs/W100K_champ/ipc1_W100K/ipc1_client_001/sway_stranded_by_phase.csv | head
1,tage_t0,4096,0,0    # 0 valid entries - tables stay cold
1,tage_t1,4096,0,0
...
```

### cpt-mode (working)
```
$ grep "tage.updateMispred " runs/W100K/main_W100K/gcc_pp_O2_1869/stats.txt
system.cpu.branchPred.tage.updateMispred        47038   # ← lots of updates

$ head -3 runs/W100K/main_W100K/gcc_pp_O2_1869/sway_stranded_by_phase.csv
phaseID,scope,total_ways,valid_ways,active_ways
1,mbtb_sram0,4096,28,28
1,tage_t0,4096,34,32   # entries get allocated
```

## Source-level reconnaissance done so far

The update call chain is:

```
commit.cc:1418  commitInfo[tid].doneFtqId = head_inst->getFtqId() - 1
                                                       └─ only set if getFtqId() > 1
                                                       │
fetch.cc:1719   dbpbtb->commit(commitInfo[tid].doneFtqId, tid)
                                                        │
decoupled_bpred.cc:718  updatePredictorComponents(target)
                                                        │
decoupled_bpred.cc:828  for components[i]: components[i]->update(target)
                                                                       │
btb_tage.cc:903         BTBTAGE::update(FetchTarget &stream) { ... }
                          ↑ this is where updateMispred++ etc. live
```

Trace mode still goes through `fetch.cc:1857 instruction->setFtqId(dbpbtb->ftqHeadId(tid))`
because `buildInst()` is shared (we confirmed it does run under
`--enable-trace-mode`). So the suspect locations narrow to:

1. **`dbpbtb->ftqHeadId(tid)` returns 0 in trace mode** because the
   trace replay path bypasses the FSQ/FTQ allocation that normal fetch
   does. → every inst gets `ftqId = 0`, `commit.cc:1418`'s `getFtqId() > 1`
   never fires, `doneFtqId` stays unset, `fetch.cc:1719` is skipped.
2. **OR** trace mode does push targets onto the FTQ but the targets are
   never `target.isHit || target.exeTaken`, so
   `updatePredictorComponents()` early-exits at line 832.
3. **OR** trace mode's `setUpdateBTBEntries()` flag stays false so the
   component update loop runs but each `BTBTAGE::update` body short-circuits.

Hypothesis 1 is the cheapest to falsify — see Phase 1.

## Implementation plan

### Phase 1 — Instrument and confirm (≤ 2 hours)

Add four light-weight stat counters that don't depend on touching the
update path. Build, run a single 500K-instr trace and a 500K-instr cpt,
diff the counters:

```cpp
// In DecoupledBPUWithBTB (decoupled_bpred.cc/hh):
statistics::Scalar commitCallsTotal;            // bumped in commit()
statistics::Scalar commitWithDoneFtqId;         // bumped when target_id > 1
statistics::Scalar updatePredictorComponentsTotal;  // bumped on entry
statistics::Scalar updatePredictorComponentsHitTaken;  // bumped when
                                                       // (target.isHit || target.exeTaken)
```

This will tell us with one experiment which of the three branches above
fires the silence.

Smoke commands (use existing infra; new gem5.opt build is required):

```bash
# trace
mkdir -p ~/project/papers/sway-paper/runs/tageupd_diag/trace
GEM5_HOME=~/project/GEM5-sway TRACE_FORMAT=cbp2025 \
  XS_MAX_INSTS=500000 XS_PHASE_SIZE_BY_INST=50000 \
  OUTDIR=$(pwd)/runs/tageupd_diag/trace \
  bash ~/project/mytools/tools/distributed-trace-scheduler/scripts/run_trace_gem5.sh \
    /nfs/home/share/zzf/traces/champSim-cbp2025-trace/int/int_0_trace.gz

# cpt baseline
mkdir -p ~/project/papers/sway-paper/runs/tageupd_diag/cpt
GEM5_HOME=~/project/GEM5-sway \
  XS_MAX_INSTS=500000 XS_PHASE_SIZE_BY_INST=50000 \
  OUTDIR=$(pwd)/runs/tageupd_diag/cpt \
  bash ~/project/mytools/tools/distributed-trace-scheduler/scripts/run_cpt_gem5.sh \
    /nfs/home/share/zyy/spec17-rv64gcb-O3-20m-gcc12.2.0-mix-with-special_wrf/checkpoint-0-0-0/blender/26411/_26411_0.174363_.zstd

# diff the four new counters
diff <(grep "branchPred\.(commitCalls|commitWith|updatePredictor)" runs/tageupd_diag/cpt/stats.txt)  \
     <(grep "branchPred\.(commitCalls|commitWith|updatePredictor)" runs/tageupd_diag/trace/stats.txt)
```

Expected outcomes (each implies a different root cause):

| `commitCallsTotal` | `commitWithDoneFtqId` | `updatePredictorComponentsTotal` | `updatePredictorComponentsHitTaken` | Verdict |
|---|---|---|---|---|
| trace ≈ 0 | trace = 0 | trace = 0 | trace = 0 | **hypothesis 1**: ftqId path broken; trace fetch never sets ftqId > 1. Fix in TraceFetch.cc / dbpbtb->ftqHeadId() to allocate trace-targets onto FTQ. |
| trace > 0 | trace ≈ 0 | trace = 0 | trace = 0 | commit() runs but `doneFtqId` always 0. Likely getFtqId() always == 1 in trace because there's exactly one synthetic FTQ entry. |
| trace > 0 | trace > 0 | trace > 0 | trace ≈ 0 | **hypothesis 2**: targets are never `isHit || exeTaken` in trace mode; trace BPU just doesn't mark them. Fix in TraceFetch to populate those fields. |
| trace > 0 | trace > 0 | trace > 0 | trace > 0 | **hypothesis 3**: update body short-circuits inside BTBTAGE::update. Need a per-component update counter to localise. |

### Phase 2 — Root cause (depends on Phase 1 verdict)

Branch the investigation:

- **If hypothesis 1**: read `TraceFetch::tick()` / `TraceFetch::bindPendingTraceMetadata()`
  + `dbpbtb->ftqHeadId(tid)` interaction. Verify whether
  `FetchTargetQueue::enqueue()` is called on trace-mode boundaries.
  Likely fix: have TraceFetch synthesise one-entry FTQ pushes per trace
  branch, or route the trace branch through a minimal BPU prediction so
  the FTQ is exercised.
- **If hypothesis 2**: inspect `target.exeTaken` setter on the trace
  decode path (`decode.cc` around line 942 — it consumes
  `inst_pair[1]->ftqId` but not necessarily `isHit/exeTaken`). The
  trace branch outcome is known by `dbpbtb->mispred(...)` paths in
  fetch.cc; cross-reference the call sites.
- **If hypothesis 3**: enable `--debug-flags=BTBTAGE` for 50K insts in
  both modes, diff the trace, look for the early-return predicates
  that fire only in trace.

### Phase 3 — Fix or document

Two acceptable outcomes:

- **Code fix**: minimum diff to enable TAGE update in trace mode without
  breaking cpt. Validation gate: cpt smoke (blender_26411 500K W=50K)
  still produces ≥ the current `updateMispred ≈ 1700` value; trace
  smoke (cbp2025/int_0 500K W=50K) now produces a non-zero
  `updateMispred` and the BTBTAGE bars in
  `figures/figure1_stranded_champ.pdf` rise above noise.
- **Document the limitation**: if the fix is intrusive (e.g. needs a
  shadow BPU prediction inside trace mode), record the constraint in
  `~/.claude/projects/.../memory/sway-trace-mode-tage.md` and add a
  `warn()` at runtime when trace mode + SWAY probe coincide. Update
  PENDING.md / PLAN.md to acknowledge that champ-trace stranded
  numbers are MBTB-only.

Add a new memory entry either way:
```
~/.claude/projects/-nfs-home-goulingrui-project-papers-sway-paper/memory/sway-trace-mode-tage.md
```

## Open questions

- Does the FTQ even need to fire updates in trace mode if the trace
  reader already carries ground truth? Conceptually yes — TAGE needs
  the *resolution* to update its counters, and the trace gives us
  exactly that. The only question is whether the existing
  `update(FetchTarget)` signature is compatible with trace-injected
  resolution data.
- Does fixing this affect the SPEC17 cpt numbers? Risk: low (cpt mode
  already exercises the path), but the Phase 3 validation gate above
  guards against it.

## Done definition

- [ ] Phase 1 instrumented build runs both modes; verdict table filled in
- [ ] Root cause documented in this PRD's "Open questions" section
- [ ] Either: code fix committed on `sway/phase-stranded-probe` with cpt
      smoke value unchanged and trace smoke shows `tage.updateMispred > 0`;
      or: limitation documented and acknowledged in SWAY paper PENDING.md
- [ ] Memory entry written: `memory/sway-trace-mode-tage.md`
- [ ] If a code fix is upstreamable (XS-GEM5 main), open a PR; otherwise
      keep the patch on the sway branch and note for the HPCA extension.

## Triggers that escalate this from P2 to P1

- HPCA extension planning starts (need DaCapo javac + champ trace coverage
  for §V).
- Reviewer asks "why are championship traces absent from Figure 1?" at
  CAL submission.
- New trace-mode workload (e.g. open-source SPEC trace bundle) needs to
  be evaluated with the SWAY probe.
