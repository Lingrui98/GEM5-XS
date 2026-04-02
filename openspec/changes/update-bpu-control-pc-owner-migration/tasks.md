# Tasks: fetch ownership migration for split control instructions

## 1. Spec updates

- [x] Add the OpenSpec change `update-bpu-control-pc-owner-migration`.
- [x] Record that it builds on `update-bpu-control-pc-tail-halfword`.
- [x] Add delta specs for `decoupled-btb-control-pc-semantics`.

## 2. Predictor / FTQ plumbing

- [x] Add `decodeStartPC` to `FetchTarget`.
- [x] Keep the default contract `decodeStartPC == startPC`.
- [x] When a following target carries a split predicted-taken control instruction,
      set `decodeStartPC = predBranchInfo.startPC()`.
- [x] Keep predictor key/index/tag/position logic on `controlPC`.
- [x] Keep stats / trace branch keys on architectural `startPC`.

## 3. Fetch ownership migration

- [x] In `Fetch::processSingleInstruction()`, check owner target before `buildInst()`.
- [x] If the following target's `decodeStartPC` is already at or before the
      current instruction start PC, consume the current target and switch to
      the following one.
- [x] Ensure the resulting DynInst / `ftqId` ownership lands on the following target.
- [x] Remove `trigger_covered` from fetch taken matching.
- [x] Match taken redirect using owner-target `predBranchInfo.startPC()`.

## 4. Validation

- [x] Reuse the validation scheme from `update-bpu-control-pc-tail-halfword`.
- [x] Add the manifest-driven validation assets under `validation/`:
  - [x] `env.example.sh`
  - [x] `tasks.csv`
  - [x] `fs_spec_target12.lst`
  - [x] `scripts/check_trace_gate.py`
  - [x] `scripts/run_fs_checkpoint.sh`
  - [x] `scripts/summarize_regression.py`
- [x] Update or add directed unit tests for:
  - [x] `Rvi4B_ControlPC_CrossBoundaryPredictInNextBlock`
  - [x] `SplitControlOwnershipMigratesBeforeBuildInst`
  - [x] `TakenMatchUsesOwnerTargetStartPC`
- [x] Run `openspec validate update-bpu-control-pc-owner-migration --strict`.
- [x] Build `build/RISCV/gem5.opt` with `scons -j128`.
- [x] Build the BTB unit tests with `scons -j128 --unit-test ...`.
- [x] Run `btb.test.opt`.
- [x] Run `fetch_coverage.test.opt`.
- [x] Run the manifest prerequisites `setup_env`, `trace_preflight`, and `fs_dry_run`.
- [x] Run the representative 12-workload FS checkpoint gate `fs_spec_target12`.
- [x] Run the mytools-based 66-trace gate `trace_top66` in parallel with the FS gate.
- [x] Run `final_verify` and archive the PASS summary in `final_status.csv` and `final_report.md`.
