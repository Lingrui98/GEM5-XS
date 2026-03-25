# Tasks: fetch ownership migration for split control instructions

## 1. Spec updates

- [ ] Add the OpenSpec change `update-bpu-control-pc-owner-migration`.
- [ ] Record that it builds on `update-bpu-control-pc-tail-halfword`.
- [ ] Add delta specs for `decoupled-btb-control-pc-semantics`.

## 2. Predictor / FTQ plumbing

- [ ] Add `decodeStartPC` to `FetchTarget`.
- [ ] Keep the default contract `decodeStartPC == startPC`.
- [ ] When a following target carries a split predicted-taken control instruction,
      set `decodeStartPC = predBranchInfo.startPC()`.
- [ ] Keep predictor key/index/tag/position logic on `controlPC`.
- [ ] Keep stats / trace branch keys on architectural `startPC`.

## 3. Fetch ownership migration

- [ ] In `Fetch::processSingleInstruction()`, check owner target before `buildInst()`.
- [ ] If the current instruction start PC is earlier than the current target's
      `decodeStartPC`, consume the current target and switch to the following one.
- [ ] Ensure the resulting DynInst / `ftqId` ownership lands on the following target.
- [ ] Remove `trigger_covered` from fetch taken matching.
- [ ] Match taken redirect using owner-target `predBranchInfo.startPC()`.

## 4. Validation

- [ ] Reuse the validation scheme from `update-bpu-control-pc-tail-halfword`.
- [ ] Update or add directed unit tests for:
  - [ ] `Rvi4B_ControlPC_CrossBoundaryPredictInNextBlock`
  - [ ] `SplitControlOwnershipMigratesBeforeBuildInst`
  - [ ] `TakenMatchUsesOwnerTargetStartPC`
- [ ] Run `openspec validate update-bpu-control-pc-owner-migration --strict`.
- [ ] Build `build/RISCV/gem5.opt` with `scons -j128`.
- [ ] Build the BTB unit tests with `scons -j128 --unit-test ...`.
- [ ] Run `btb.test.opt`.
- [ ] Run `fetch_coverage.test.opt`.
