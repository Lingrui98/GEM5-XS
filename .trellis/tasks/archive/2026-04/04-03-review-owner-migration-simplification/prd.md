# Review and simplify owner-migration implementation on top of PR #805

## Goal
PR #805 (`cpu-o3,arch-riscv,bpu: align decoupled BTB control coverage and fetch range`) established the
March 22, 2026 baseline for split 4B control handling around fetch coverage,
decoder incremental fill, and `trigger_covered`-based redirect gating.

The follow-up owner-migration series on March 25-26, 2026:

- `4ef1163cb6` `cpu: migrate split control ownership to next target`
- `a19ab7b953` `cpu: Fix control-PC FS/trace regressions`
- `cb522d9a45` `cpu: Fix start-PC callsites after control-PC switch`
- `ee4d353bd1` `cpu: Guard split-control owner handoff`

corrected the semantic model by making split taken control instructions belong
to the following fetch target. That direction is judged correct, but the current
implementation spreads ownership logic across predictor entry production, fetch
handoff, redirect matching, trace-mode exceptions, and mirrored test predicates.

This task tracks a simplification pass: preserve the owner-migration semantics,
but reduce fetch-local protocol complexity and maintenance risk.

## Assessment Summary
- `owner-migration` is directionally correct and SHOULD NOT regress to PR #805's
  `trigger_covered` patch model as the semantic source of truth.
- The current implementation increases complexity by:
  - introducing `FetchTarget.decodeStartPC()` as a third ownership address
    alongside `FetchTarget.startPC`, `BranchInfo.startPC()`, and
    `BranchInfo.controlPC()`
  - encoding split-owner handoff as a multi-term predicate in fetch
  - duplicating ownership reasoning across production code and tests
  - requiring immediate follow-up fixes for trace mode, start-PC callsites, and
    false owner handoff
- The preferred simplification direction is not rollback. It is to encapsulate
  owner semantics more cleanly and shrink the number of places that understand
  the FTQ handoff protocol.

## Requirements
1. Preserve the semantic contracts introduced by `update-bpu-control-pc-tail-halfword`
   and `update-bpu-control-pc-owner-migration`:
   - predictor-facing branch identity remains `controlPC`
   - stats/debug/trace attribution remains `startPC`
   - split 4B taken control instructions are owned by the following fetch target
   - no return to `trigger_covered` as the semantic gate for split-control owner selection

2. Produce a concrete simplification design for owner migration:
   - prefer a single-purpose owner abstraction and naming scheme
   - if `decodeStartPC` is retained, justify why it is not an ownership alias
     with a misleading name
   - if a simpler representation is viable, specify the migration path and affected callsites

3. Centralize the split-owner handoff logic:
   - fetch SHOULD NOT duplicate the handoff protocol at multiple callsites
   - the handoff predicate SHOULD live behind a shared helper or a single FTQ/FetchTarget-facing API
   - tests SHOULD target the shared contract instead of re-encoding the same predicate ad hoc

4. Audit and explain the follow-up fixes:
   - `a19ab7b953`: trace-mode and history-recovery regression
   - `cb522d9a45`: start-PC callsite fallout after control-PC switch
   - `ee4d353bd1`: false handoff to ordinary taken-target follower entries
   - Each fix should map to either necessary semantic cost or avoidable implementation complexity

5. If the simplification is implemented in code:
   - refactor fetch-side ownership logic to reduce direct knowledge of FTQ internals
   - keep branch/fetch/trace contracts aligned
   - update docs/spec notes if the ownership abstraction or validation contract changes

6. If the simplification is rejected after deeper review:
   - document why simpler alternatives are invalid
   - record the minimum set of invariants/tests needed to keep the current protocol maintainable

## Acceptance Criteria
1. A written baseline comparison exists between PR #805 and owner-migration,
   and it explicitly explains why rollback to PR #805 semantics is rejected.

2. A concrete simplification proposal exists and identifies:
   - current ownership state
   - desired ownership state
   - exact files/functions that must change
   - residual complexity that is truly unavoidable

3. If code changes are made:
   - split-owner handoff logic is centralized behind one shared contract
   - fetch no longer carries duplicated owner-migration reasoning at multiple sites
   - tests cover:
     - sequential split-control handoff
     - ordinary taken-target follower that must not trigger handoff
     - trace-mode behavior
     - startPC/controlPC-sensitive callsites such as RAS/uRAS or trace wrong-path sizing

4. Validation passes for the touched scope, including helper-level tests and at
   least one trace-visible sanity check if fetch/trace logic changes.

5. Trellis tracking is updated with the final decision:
   - implemented simplification
   - or documented no-change rationale

## Technical Notes
- Review baseline PR:
  - `https://github.com/OpenXiangShan/GEM5/pull/805`
  - head branch: `impl-control-pc-views`
  - key commits:
    - `85adf7683a` `cpu-o3,bpu: Align decoupled BTB control-PC coverage`
    - `8412ba71a1` `arch-riscv,cpu-o3,bpu: Align fetch range with split decode`
    - `b92a9778c6` `arch-riscv,cpu-o3,bpu: Fix review follow-up issues`

- Owner-migration branch:
  - `impl-control-pc-tail-halfword`
  - key commits:
    - `4ef1163cb6` owner-migration implementation
    - `a19ab7b953` trace/FS follow-up
    - `cb522d9a45` start-PC callsite follow-up
    - `ee4d353bd1` false-handoff guard

- Key files to audit:
  - `src/cpu/o3/fetch.cc`
  - `src/cpu/pred/btb/common.hh`
  - `src/cpu/pred/btb/decoupled_bpred.cc`
  - `src/cpu/pred/btb/decoupled_bpred.hh`
  - `src/cpu/o3/trace/TraceFetch.cc`
  - `src/cpu/pred/btb/ras.cc`
  - `src/cpu/pred/btb/uras.cc`
  - `src/cpu/pred/btb/test/btb.test.cc`
  - `src/cpu/pred/btb/test/fetch_coverage.test.cc`
  - `src/cpu/pred/btb/test/ras_test.cc`
  - `src/cpu/pred/btb/test/uras_test.cc`

## Impact Notes
- This is a cross-layer frontend task. It spans predictor metadata, FTQ/Fetch
  ownership, trace-mode interaction, and helper/unit tests.
- The task is maintainability-driven, not feature-driven:
  - reduce duplicated ownership logic
  - improve naming clarity
  - make regressions easier to catch with smaller, more local invariants
- KISS:
  - prefer one ownership contract over several loosely-coupled conventions
- DRY:
  - avoid repeating the handoff predicate in both production code and tests
- YAGNI:
  - do not add a broader ownership framework unless the current split-control
    case truly needs it
- SOLID:
  - fetch should consume an ownership contract, not define the whole protocol itself

## Validation Notes
### 0. Diff / ancestry audit
- `git -C .worktrees/impl-control-pc-tail-halfword log --oneline impl-control-pc-views..impl-control-pc-tail-halfword -- src/cpu/o3/fetch.cc src/cpu/pred/btb/common.hh src/cpu/pred/btb/decoupled_bpred.cc src/cpu/o3/trace/TraceFetch.cc`
- `git -C .worktrees/impl-control-pc-tail-halfword diff --stat impl-control-pc-views..impl-control-pc-tail-halfword -- src/cpu/o3/fetch.cc src/cpu/pred/btb/common.hh src/cpu/pred/btb/decoupled_bpred.cc src/cpu/o3/trace/TraceFetch.cc`

### 1. Helper / unit tests
- `cd /nfs/home/goulingrui/project/GEM5/.worktrees/impl-control-pc-tail-halfword`
- `scons -j$(nproc) --unit-test build/RISCV/cpu/pred/btb/test/btb.test.opt build/RISCV/cpu/pred/btb/test/fetch_coverage.test.opt build/RISCV/cpu/pred/btb/test/ras_test.opt build/RISCV/cpu/pred/btb/test/uras_test.opt build/RISCV/arch/riscv/riscv_decoder.test.opt`
- `build/RISCV/cpu/pred/btb/test/btb.test.opt`
- `build/RISCV/cpu/pred/btb/test/fetch_coverage.test.opt`
- `build/RISCV/cpu/pred/btb/test/ras_test.opt`
- `build/RISCV/cpu/pred/btb/test/uras_test.opt`
- `build/RISCV/arch/riscv/riscv_decoder.test.opt`

### 2. Targeted trace sanity
- `cd /nfs/home/goulingrui/project/GEM5/.worktrees/impl-control-pc-tail-halfword`
- `XS_MAX_INSTS=10000 TRACE_FORMAT=champsim bash util/xs_scripts/trace/run_trace_champsim.sh /nfs/home/share/glr/champsim_traces/cvp1_public/compute_fp_1.gz`
- `rg -n 'panic|fatal|assert|Trace stream PC mismatch|Program aborted' m5out/*/log.txt`

### 3. Optional FS / manifest reuse
- Reuse the existing `control_pc_tail_halfword` regression manifest and final
  aggregation flow when simplification changes fetch or trace behavior materially.
- At minimum, preserve the ability to re-run:
  - `fs_spec_target12`
  - `trace_top66`
  - `final_verify`

### 4. Spec / tracking sync
- If the simplification changes executable contracts, update the relevant
  Trellis spec or OpenSpec notes before closing the task.
