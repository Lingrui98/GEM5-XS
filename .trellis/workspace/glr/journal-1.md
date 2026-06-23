# Journal - glr (Part 1)

> AI development session journal
> Started: 2026-04-01

---



## Session 1: PR 805 owner-migration simplification, docs, and CI fix

**Date**: 2026-04-13
**Task**: PR 805 owner-migration simplification, docs, and CI fix
**Branch**: `review-owner-migration-simplification`

### Summary

Simplified split-control owner migration on top of PR #805, refreshed frontend docs/PR metadata, and fixed the difftest smoke crash by delaying handoff until partial decode is active.

### Main Changes

| Area | Description |
|------|-------------|
| Simplification | Consolidated split-control owner predicates into shared `FetchTarget` helpers and removed duplicated handoff logic from fetch/tests. |
| Documentation | Updated frontend docs for `startPC` / `controlPC` / `ownerStartPC`, clarified owner-migration semantics, and restacked PR #805 without `openspec/` content. |
| CI Debugging | Reproduced the failing difftest smoke run, traced the `SIGSEGV` to an early owner handoff before the decoder held a partial instruction, and gated migration on `decoder[tid]->hasPartialInst()`. |
| Validation | Rebuilt `build/RISCV/gem5.opt`, passed `btb.test.opt`, passed `fetch_coverage.test.opt`, and reran the CI-equivalent smoke command to `m5_exit`. |

**PR**: `OpenXiangShan/GEM5#805`

**Verification Commands**:
- `CC=gcc CXX=g++ scons build/RISCV/gem5.opt --linker=gold -j32`
- `build/RISCV/cpu/pred/btb/test/btb.test.opt`
- `build/RISCV/cpu/pred/btb/test/fetch_coverage.test.opt`
- `GCBV_REF_SO=/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-notama-tvalref-so ./build/RISCV/gem5.opt ./configs/example/kmhv3.py --raw-cpt --generic-rv-cpt=/nfs/home/share/gem5_ci/checkpoints/coremark-riscv64-xs.bin`


### Git Commits

| Hash | Message |
|------|---------|
| `6cef98f253` | (see git log) |
| `546514ed09` | (see git log) |
| `94bd711647` | (see git log) |

### Testing

- [OK] (Add test results)

### Status

[OK] **Completed**

### Next Steps

- None - task complete


## Session 2: Wave-1 IPF runtime skeleton

**Date**: 2026-04-17
**Task**: Wave-1 IPF runtime skeleton
**Branch**: `ipf-research-framework-wave1`

### Summary

(Add summary)

### Main Changes

| Area | Result |
|------|--------|
| Runtime | Added the Wave-1 O3 instruction-prefetch runtime skeleton with normalized fetch/trace demand observers, null policy, submit queue, and scoreboard skeleton |
| Config | Wired XiangShan CLI knobs for `enableInstPrefetchRuntime`, policy, queue size, and issue width |
| Validation | Built `build/RISCV/gem5.opt`, passed `build/NULL/cpu/o3/inst_prefetch.test.opt`, and validated both trace-path and FS/XiangShan fetch-path stats |
| Docs | Added English/Chinese runtime docs and updated backend code-spec/contracts |

**Validation Evidence**:
- `build/NULL/cpu/o3/inst_prefetch.test.opt` passed (3 tests)
- `/tmp/ipf_trace_smoke/stats.txt`: `demandBlocksFromTrace = 4`
- `/tmp/ipf_fs_smoke/stats.txt`: `demandBlocksFromFetch = 2386`

**Notes**:
- The root-repo Trellis task `04-02-openspec-add-instruction-prefetch-research-framework` was archived after code commit because the Wave-1 scope is complete.
- Per research policy, SE-based timing validation is not counted as closeout evidence.
- `openspec validate add-instruction-prefetch-research-framework --strict` remains externally blocked in this root worktree because the local openspec change entry is absent.


### Git Commits

| Hash | Message |
|------|---------|
| `e419db4428` | (see git log) |

### Testing

- [OK] (Add test results)

### Status

[OK] **Completed**

### Next Steps

- None - task complete


## Session 3: FDIP P0/P1 stabilization cut on sub-worktree

**Date**: 2026-04-23
**Task**: FDIP P0/P1 stabilization cut on sub-worktree
**Branch**: `fdip-phase2-xsdev`

### Summary

Recorded the completed FDIP stabilization cut from the fdip-phase2-xsdev worktree, archived the Trellis task, and captured validation/results/stop recommendation in root Trellis.

### Main Changes

| Area | Result |
|------|--------|
| Worktree | Work was implemented in sub-worktree `.worktrees/fdip-phase2-xsdev`, but recorded into root repo Trellis (`~/project/GEM5/.trellis/`). |
| Task lifecycle | Archived `04-02-openspec-add-fdip-icache-prefetch` after code and validation were complete for the Trellis-defined P0/P1 scope. |
| Commits | `917eb08d37` predictor/config semantics, `5925a5da8c` cache contract/refill gating, `ce57b30b6c` fetch engine/probe/tracking, `f02d1a1f62` redirect/partial-state cleanup witness. |
| Validation | Passed `openspec validate add-fdip-icache-prefetch --strict`, `scons build/RISCV/gem5.opt -j8`, `build/RISCV/cpu/pred/btb/test/fetch_coverage.test.opt`, and `build/RISCV/cpu/o3/fdip_cleanup.test.opt`. |
| Performance evidence | `srv67` 5M: IPC `+0.39%`; `crypto14`: `fetch.icacheStallCycles 35513 -> 27065`; `compute_int_32`: `12778 -> 6187`. |
| Cleanup | Emptied repo-local `.tmp/` and moved prior transient artifacts out of the repo so the task no longer depends on temporary directories. |
| Trellis knowledge | Added/updated root Trellis notes: `fdip-p0-analysis-2026-04-13.md`, `fdip-slice-status-2026-04-14.md`, `fdip-cleanup-witness-2026-04-22.md`, `fdip-params-stats-limitations-2026-04-22.md`, and backend spec `fdip-guidelines.md`. |
| Decision | Recommended stopping at Phase 1 / 1.5 for this task because the remaining gaps are mainly deeper RTL-fidelity work, not blockers for the current P0/P1 conclusions. |

**Key conclusion**:
The FDIP task is complete for the Trellis-owned P0/P1 stabilization scope. The current model now has parameter/stats documentation, two high-I$ sanity traces, old-path refill-drop evidence, recent-unused suppression evidence, and a helper-level redirect/partial-state cleanup witness.


### Git Commits

| Hash | Message |
|------|---------|
| `917eb08d37` | (see git log) |
| `5925a5da8c` | (see git log) |
| `ce57b30b6c` | (see git log) |
| `f02d1a1f62` | (see git log) |

### Testing

- [OK] (Add test results)

### Status

[OK] **Completed**

### Next Steps

- None - task complete


## Session 4: SWAY paper CHECKPOINT 1 + trace-mode TAGE update fix

**Date**: 2026-06-15 → 2026-06-23
**Task**: SWAY paper CHECKPOINT 1 motivation data; xs-dev merge; trace-mode TAGE silence root-cause fix
**Branch**: `sway/phase-stranded-probe`

### Summary

Three task arcs completed across the SWAY paper / GEM5-sway / mytools repos.
The work resolved CHECKPOINT 1 of the SWAY paper four weeks ahead of
schedule (target was Week 5–6, delivered Week 1), merged the upstream
xs-dev sync into the SWAY worktree along with three trace-mode fixes,
then root-caused and fixed a separate trace-mode TAGE-update silence
that surfaced once trace-mode runs became possible. Final figures and
analysis use a post-merge binary.

### Arcs and results

| Arc | Result |
|------|--------|
| Arc 1 — cpt-mode batch infra | Added `run_cpt_gem5.sh` + `backend.gem5_cpt.sample.json` in mytools so the distributed scheduler can drive GCB-V cpt workloads. Smoke verified end-to-end (blender_26411 cpt 500K W=50K). |
| Arc 2 — SPEC17 cpt batch + Figure 1 | 10 SPEC17 cpts × 3 W ∈ {10K,100K,200K} × 5M instr = 30 runs, 0 abort across 19 nodes. Cross-workload aggregate stranded fraction = 33.95× at W=100K on the pre-merge binary; thesis-collapse risk (PENDING-9 ≥ 3×) cleared. Generated Figure 1 (main + W sweep sensitivities) and the cohort-tidy CSV. Closed PENDING-1/9/10/11 in the SWAY paper. |
| Arc 3 — xs-dev merge | Merged `trace-new-sync-xs-dev-WIP` (`8c16e392c1 cpu,util: Merge xs-dev into trace-new` + three trace fixes) into `sway/phase-stranded-probe`. Three conflicts resolved (`btb_tage.cc/hh`, `decoupled_bpred.cc`). MicroTAGE detached from BTBTAGE upstream → dropped from SWAY probe (matches the user's fast-predictor exclusion policy). Post-merge rebuild clean. |
| Arc 4 — refresh on post-merge binary | Re-ran all 30 SPEC17 cpts plus 7 championship traces (3 IPC1 + 2 CVP1 + 2 CBP2025) at W=100K, 0 abort. Refreshed Figure 1, Figure 2 (per-phase time series, stability CV, per-scope extremes), and Q2 stranded→IPC causal-chain analysis. Cross-workload spread climbed to 148.6× on the SPEC17 cohort under the post-merge binary; the conclusion direction (max=gcc_pp_O2, min=perlbench_split) is stable across all three W values. Q2 Pearson r: str_T_lo ↔ commit_mpki = +0.918, str_T_lo ↔ IPC = −0.349, commit_mpki ↔ IPC = −0.496. Loose Belady IPC uplift upper bound: arith mean +3.82%, max mcf +16.4%. |
| Arc 5 — trace-mode TAGE silence | Diagnosed and fixed the unrelated symptom that championship-trace runs gave `tage.updateMispred = 0` while `tage.predHit` was non-zero. Root cause: trace-mode squash paths recorded predicted outcome as actual outcome, so MBTB never cleared `alwaysTaken` and `BTBTAGE::prepareUpdateEntries()` filtered every conditional. Fix: Decode::selfSquash, IEW::squashDueToBranch, and Commit::commit now use `traceBranchTaken()`/`traceBranchNextPC()` when trace branch metadata is present; BTBTAGE keeps the normal `!alwaysTaken` filter. Follow-up audit fixed four more trace-metadata fan-out gaps (`DynInst::is*` accessors, `CommitTrace` re-classify, `ChampSimTraceReader` mapped target, `fetch.cc` DynInst forwarding into squash redirects). Added 17 lightweight diagnostic stat counters along the commit → updatePredictorComponents → BTBTAGE::update chain so future trace-mode update silences can be localised in one experiment. |

### Cross-repo commits

| Repo | Hash | Title |
|------|------|-------|
| mytools | `1fb282c` | feat(distributed-trace-scheduler): add cpt-mode XS-GEM5 backend |
| mytools | `ed68452` | feat(distributed-trace-scheduler): inject XS_PHASE_SIZE_BY_INST in trace backend |
| GEM5-sway | `9296c14b94` | misc: close CHECKPOINT 1 trellis tasks for SWAY paper |
| GEM5-sway | `927f6e3613` | cpu-o3,bpu: Merge xs-dev sync into sway probe |
| GEM5-sway | `338a25cb88` | misc: Open trellis task for trace-mode TAGE update silence |
| GEM5-sway | `1e6bed3e98` | cpu-o3,bpu: Fix trace-mode TAGE update silence |
| GEM5-sway | `483f546ef6` | misc: Close trace-mode TAGE diag, add trace contracts spec |
| sway-paper | `1d7e9ac` | docs: handoff for session 2026-06-15 |
| sway-paper | `593c85f` | CHECKPOINT 1 cleared: Figure 1 + PENDING-1/9/10/11 resolved |
| sway-paper | `78039cb` | CHECKPOINT 1 refresh on post-xs-dev-merge binary |
| sway-paper | `a4c938e` | docs: refresh NEXT_ACTIONS with post-merge numbers |

### Validation evidence

- 30 SPEC17 cpt + 7 championship trace runs all completed, 0 abort
- Trace smoke `cbp2025/int_0 500K W=50K`: `tage.updateMispred 0 → 3`,
  `updateAllocSuccess 0 → 3`, `mbtb.condHitNotTakens 0 → 4` post-fix
- CPT regression smoke `blender/26411 500K W=50K`: `tage.updateMispred`
  stays at 2835, `updateAllocSuccess` stays at 2810 (within
  expected range)
- Figure 1 PDFs regenerated and committed; tidy CSV has 20 rows for
  SPEC17 W=100K main (10 workloads × 2 groups)
- 60 500-row merged data file (`runs/figure1_merged_stranded.csv`) is
  the durable summary feeding the plot scripts

### Code-spec / docs

- `.trellis/spec/backend/trace-mode-contracts.md` added (211 lines)
  with executable contracts for synthetic-vs-trace meaning,
  Decode/IEW/Commit/CommitTrace/DynInst/ChampSimTraceReader rules,
  and the BTBTAGE alwaysTaken filter invariant
- `.trellis/spec/backend/index.md` cross-links the new doc with a
  read-this-before guard
- SWAY paper docs: `PLAN.md`, `PENDING.md`, `HANDOFF.md`,
  `docs/NEXT_ACTIONS.md` all reflect the post-merge numbers
- Project memory: `sway-paper-checkpoint1.md`,
  `sway-scope-exclusions.md`, `sway-gem5-build-quirks.md`,
  `mytools-cpt-backend.md` added under
  `~/.claude/projects/-nfs-home-goulingrui-project-papers-sway-paper/memory/`

### Status

[OK] **Completed**

### Next Steps

- `/ars-plan` in `~/project/papers/sway-paper` to fold the
  post-merge numbers (149× spread, Q2 correlations, IPC uplift
  bound) into §I/§III prose
- Phase 1 SWAY mechanism: open a new Trellis task for the
  marginal-utility controller + realloc PoC in `cpu/pred/btb/`
