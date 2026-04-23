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
