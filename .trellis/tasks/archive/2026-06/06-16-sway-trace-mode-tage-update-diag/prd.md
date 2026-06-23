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

### Latest trace-fix commits are not the initial cause

The first check was whether the latest three trace-fix commits introduced the
silence:

- `064029e6ef cpu-o3,util: Fix trace recovery replay`
- `803b6ec090 cpu-o3,bpu: Fix trace return target recovery`
- `add587d180 cpu-o3,bpu: Fix trace non-control wrong-path`

Diff inspection shows these commits touch trace replay/PC recovery, return
target squash handling, wrong-path non-control prediction cleanup, and trace
helper scripts, but not `BTBTAGE::update()`. More importantly, an older ancestor
run from commit `24d73b0c1a cpu: align ChampSimTraceReader tests` already shows
the same symptom before all three commits:

```
$ grep -E "simInsts|branchPred\.fsqEntryCommitted|branchPred\.tage\.(predHit|updateMispred|updateAllocSuccess)|commit\.branchMispredicts" \
  /nfs/home/goulingrui/project/GEM5/.worktrees/baseline/m5out/reg_matrix/fixcmp7/baseline/cbp_int_0/stats.txt
simInsts                                        50005
system.cpu.branchPred.fsqEntryCommitted          4803
system.cpu.branchPred.tage.updateAllocSuccess       0
system.cpu.branchPred.tage.updateMispred            0
system.cpu.branchPred.tage.predHit               1553
system.cpu.commit.branchMispredicts               140
```

`24d73b0c1a` is an ancestor of current HEAD and of `add587d180`, so the issue
predates the three latest trace-fix commits. The current investigation should
therefore focus on the older trace-mode FTQ/update contract, not on reverting
those fixes.

### Phase 1 verdict and root cause

The diagnostic counters rule out the FTQ/commit path as the failure point. A
500K-instruction trace smoke on `cbp2025/int_0` reached the resolved-update path
and called `BTBTAGE::update()` 16478 times:

```
simInsts                                                   500002
system.cpu.branchPred.fsqEntryCommitted                     47043
system.cpu.branchPred.prepareResolveUpdateEntriesHitTaken    16478
system.cpu.branchPred.markCFIResolvedMatchedEntries          16197
system.cpu.branchPred.resolveUpdateHitTaken                  16478
system.cpu.branchPred.resolveUpdateBlocked                       0
system.cpu.branchPred.tage.updateCalls                       16478
system.cpu.branchPred.tage.updateNoPredMeta                      0
system.cpu.branchPred.tage.updateRawCondResolvedEntries      15776
system.cpu.branchPred.tage.updateRawAlwaysTakenEntries       16478
system.cpu.branchPred.tage.updateRawCondNotAlwaysTakenEntries    0
system.cpu.branchPred.tage.updateFilteredEntries                 0
system.cpu.branchPred.tage.updateMispred                         0
```

So the silence is not caused by missing FTQ ids, missing prediction metadata, or
resolved-update bank blocking. The first symptom was that
`BTBTAGE::prepareUpdateEntries()` received only `alwaysTaken=true` conditional
entries and therefore filtered them all out. A deeper check showed why: trace
mode had not been reporting any conditional BTB hit-not-taken updates. The trace
itself has not-taken branches, but decode-time trace squashes passed
`inst->readPredTaken() || inst->isUncondCtrl()` as the actual outcome. For a
trace branch that was predicted taken but actually not taken, this wrote
`actually_taken=true` into the FTQ target, so MBTB counted it as hit-taken and
never cleared `alwaysTaken`.

The fix is to make trace-mode squash paths use trace ground truth:

- `Decode::selfSquash()` sets `decodeInfo.branchTaken` from
  `inst->traceBranchTaken()` when trace branch metadata is present.
- `IEW::squashDueToBranch()` sets `toCommit->branchTaken` from
  `inst->traceBranchTaken()` for trace branches.
- `BTBTAGE::prepareUpdateEntries()` keeps the normal `!alwaysTaken` filter; no
  trace-only bypass is needed.

Root-fix trace smoke on the same input:

```
simInsts                                                   500006
system.cpu.branchPred.mbtb.condHits                         15695
system.cpu.branchPred.mbtb.condHitTakens                    15691
system.cpu.branchPred.mbtb.condHitNotTakens                     4
system.cpu.branchPred.mbtb.condMissNotTakens                 5390
system.cpu.branchPred.tage.updateCalls                       16480
system.cpu.branchPred.tage.updateRawCondResolvedEntries      15777
system.cpu.branchPred.tage.updateRawCondNotAlwaysTakenEntries  192
system.cpu.branchPred.tage.updateFilteredEntries               192
system.cpu.branchPred.tage.updateMispred                         3
system.cpu.branchPred.tage.updateAllocSuccess                    3
system.cpu.branchPred.tage.predHit                           15695
```

Root-fix CPT smoke on `blender/26411` remains unchanged at the relevant TAGE
counters:

```
simInsts                                                   500001
system.cpu.branchPred.mbtb.condHitNotTakens                 41811
system.cpu.branchPred.tage.updateCalls                       71389
system.cpu.branchPred.tage.updateFilteredEntries             60158
system.cpu.branchPred.tage.updateMispred                      2835
system.cpu.branchPred.tage.updateAllocSuccess                 2810
system.cpu.branchPred.tage.predHit                           56081
```

### Follow-up audit: other trace metadata paths

After the root cause was found, the trace metadata fan-out was audited for
similar "trace truth exists but downstream uses static/predicted state" gaps.
Three additional gaps were fixed:

- `Decode::selfSquash()` now uses `traceBranchNextPC()` for trace branch
  redirects, not a recomputed static branch target. This matters for formats
  that keep a branch target even when the actual trace outcome is not taken.
- `Commit` no longer overwrites an IEW-provided trace branch outcome with
  `true` just because the synthetic/static instruction decodes as unconditional.
- Trace branch type now carries conditional/call/return/indirect information
  into `DynInst` control-flow predicates and `DecoupledBPUWithBTB::BranchInfo`,
  so BPU classification and update metadata can prefer trace truth when the
  synthetic instruction is an approximation. `CommitTrace` also uses that trace
  type when checking committed instruction type.

One trace-reader-specific gap was also found and fixed: `ChampSimTraceReader`
computed estimated targets but did not attach them to taken trace branches, so
`setTraceBranchInfo()` would fall back to the fall-through PC. Taken ChampSim
branches now get the estimated mapped target.

Audit smoke after these fixes on `cbp2025/int_0` still matches the root fix:

```
simInsts                                                   500006
system.cpu.branchPred.mbtb.condHitNotTakens                     4
system.cpu.branchPred.tage.updateFilteredEntries               192
system.cpu.branchPred.tage.updateMispred                         3
system.cpu.branchPred.tage.updateAllocSuccess                    3
system.cpu.branchPred.tage.predHit                           15695
system.cpu.traceReader.stats.branchInstr                     21843
```

Audit CPT smoke on `blender/26411` stays unchanged at the relevant counters:

```
simInsts                                                   500001
system.cpu.branchPred.mbtb.condHitNotTakens                 41811
system.cpu.branchPred.tage.updateFilteredEntries             60158
system.cpu.branchPred.tage.updateMispred                      2835
system.cpu.branchPred.tage.updateAllocSuccess                 2810
system.cpu.branchPred.tage.predHit                           56081
```

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

- Does fixing this affect the SPEC17 cpt numbers? The 500K `blender/26411`
  smoke kept the relevant TAGE counters unchanged, but a broader SPEC17 sweep is
  still the right gate before using this for final paper figures.
- Should `MicroTAGE` receive the same trace-mode filter relaxation? The current
  task is specifically BTBTAGE/SWAY stranded accounting. `microtage.updateMispred`
  is already non-zero in the post-fix trace smoke, so this was left unchanged.

## Done definition

- [x] Phase 1 instrumented build runs both modes; verdict table filled in
- [x] Root cause documented in this PRD
- [x] Code fix implemented on `sway/phase-stranded-probe`; cpt smoke value is
      unchanged and trace smoke shows `tage.updateMispred > 0`
- [x] Memory entry written: `memory/sway-trace-mode-tage.md`
- [x] Patch kept on `sway/phase-stranded-probe`; regenerate championship trace
      SWAY data before using it for the HPCA extension.

## Triggers that escalate this from P2 to P1

- HPCA extension planning starts (need DaCapo javac + champ trace coverage
  for §V).
- Reviewer asks "why are championship traces absent from Figure 1?" at
  CAL submission.
- New trace-mode workload (e.g. open-source SPEC trace bundle) needs to
  be evaluated with the SWAY probe.
