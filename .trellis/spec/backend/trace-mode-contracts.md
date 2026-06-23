# Trace Mode Contracts

> Executable contracts for trace-driven simulation paths where gem5 uses
> synthetic instructions as pipeline carriers but must train frontend/BPU state
> from trace ground truth.

---

## Overview

Read this file before changing any of the following:

- `src/cpu/o3/dyn_inst.hh`
- `src/cpu/o3/decode.cc`
- `src/cpu/o3/iew.cc`
- `src/cpu/o3/commit.cc`
- `src/cpu/o3/fetch.cc`
- `src/cpu/o3/trace/TraceFetch.*`
- `src/cpu/o3/trace/*TraceReader.*`
- `src/cpu/o3/trace/CommitTrace.cc`
- `src/cpu/pred/btb/common.hh`
- `src/cpu/pred/btb/decoupled_bpred.*`
- `src/cpu/pred/btb/mbtb.*`
- `src/cpu/pred/btb/btb_tage.*`

Trace mode has two different sources of meaning:

- **Synthetic instruction**: an approximate RISC-V `MachInst` created so the
  gem5 pipeline can decode, schedule, execute, and commit a `DynInst`.
- **Trace metadata**: the recorded dynamic event, including actual branch
  direction, next PC, target when available, and branch type.

The synthetic instruction is an execution carrier. It is not the authority for
frontend/BPU control-flow truth when trace metadata exists.

---

## Scenario: Trace-Aware Control-Flow Modeling

### 1. Scope / Trigger

- Trigger: any trace-mode change that affects branch direction, redirect target,
  branch classification, FTQ/BTB update metadata, TAGE/ITTAGE/RAS training, or
  trace commit checking.
- Reason: trace replay often synthesizes approximate instructions. Static decode
  of those instructions may disagree with the original program event.

### 2. Surfaces

Trace metadata surface on `DynInst`:

- `hasTraceBranchInfo()`
- `traceBranchTaken()`
- `traceBranchHasTarget()`
- `traceBranchTarget()`
- `traceBranchNextPC()`
- `traceIsCond()`
- `traceIsCall()`
- `traceIsReturn()`
- `traceIsIndirect()`
- `hasTraceCtrlFlowChange()`

Binding surface:

- `TraceFetch::bindTraceMetadata(...)`
- `TraceInstruction::getBranchTaken()`
- `TraceInstruction::getHasBranchTarget()`
- `TraceInstruction::getBranchTarget()`
- `TraceInstruction::getFallThroughPC()`
- `TraceInstruction::getInstType()`

BPU/update surface:

- `Decode::selfSquash(...)`
- `IEW::squashDueToBranch(...)`
- `DecoupledBPUWithBTB::controlSquash(...)`
- `DecoupledBPUWithBTB::makeBranchInfo(...)`
- `DecoupledBPUWithBTB::commitBranch(...)`
- `FetchTarget::exeTaken`
- `FetchTarget::exeBranchInfo`
- `BTBEntry::alwaysTaken`
- `BTBTAGE::prepareUpdateEntries(...)`

### 3. Contracts

1. **Synthetic instruction is not branch truth**
   - A synthetic `MachInst` may be a legal approximation, a clamped immediate,
     a rewritten JAL/JALR, or a fallback NOP for an unsupported compressed case.
   - When `hasTraceBranchInfo()` is true, control-flow correctness and BPU
     update metadata must prefer trace metadata over `staticInst` semantics.

2. **Actual direction contract**
   - Trace branch actual direction is `traceBranchTaken()`.
   - Do not derive actual direction from:
     - `readPredTaken()`
     - `pcState().branching()`
     - `staticInst->isUncondCtrl()`
   - Those values may describe prediction state or synthetic static shape, not
     the recorded trace outcome.

3. **Redirect / next-PC contract**
   - Trace branch redirect target is `traceBranchNextPC()`.
   - Use `traceBranchTarget()` only when the branch is actually taken and the
     trace has a target.
   - For not-taken branches, the correct next PC is the trace fall-through.

4. **Branch type contract**
   - Trace branch class is carried by:
     - `traceIsCond()`
     - `traceIsCall()`
     - `traceIsReturn()`
     - `traceIsIndirect()`
   - `BranchInfo` and fine-grained BPU stats must be trace-aware when a
     `DynInst` has trace branch metadata.
   - Do not rely solely on `StaticInstPtr` to classify trace branches.

5. **TAGE update contract**
   - BTBTAGE should keep the normal update filter for conditional branches:
     resolved, conditional, and not `alwaysTaken`.
   - Do not add trace-only bypasses to TAGE update filtering to hide missing
     actual-outcome propagation.
   - A predicted-taken / actual-not-taken trace branch must be able to produce
     MBTB hit-not-taken accounting and clear `BTBEntry::alwaysTaken`.

6. **Reader target contract**
   - If a trace format does not provide exact taken targets, the reader may use
     an explicit estimated/mapped target.
   - If a branch is marked taken, `TraceInstruction` should carry a target when
     possible so `setTraceBranchInfo()` does not collapse the next PC to
     fall-through.

### 4. Validation & Error Matrix

| Condition | Expected Behavior | Enforcement / Symptom |
|-----------|-------------------|------------------------|
| Trace branch predicted taken but recorded not taken | `exeTaken=false`; MBTB can count hit-not-taken | `mbtb.condHitNotTakens > 0` on a suitable trace |
| Trace branch target exceeds synthetic immediate range | redirect uses `traceBranchNextPC()` | no redirect to clamped synthetic target |
| Synthetic compressed call falls back to NOP | BPU still classifies from trace type | branch class is not `unknown` solely due to NOP fallback |
| Taken trace branch has no reader target | reader must document/attach fallback target | otherwise target training collapses to fall-through |
| `BTBTAGE::update()` sees all conditional entries as `alwaysTaken` in trace mode | investigate upstream direction propagation | do not bypass `!alwaysTaken` without proving RTL contract |

### 5. Good / Base / Bad Cases

- Good:
  - Trace smoke has non-zero committed instructions.
  - `system.cpu.branchPred.mbtb.condHitNotTakens` is non-zero on a trace with
    predicted-taken / actual-not-taken conditional hits.
  - `system.cpu.branchPred.tage.updateFilteredEntries`,
    `system.cpu.branchPred.tage.updateMispred`, and
    `system.cpu.branchPred.tage.updateAllocSuccess` are non-zero when the trace
    exercises conditional direction mistakes.
  - `system.cpu.branchPred.branchClassCounts::unknown` does not grow from
    trace branches that have trace type metadata.

- Base:
  - CPT mode has no trace metadata, so normal `staticInst` control-flow
    semantics remain authoritative.

- Bad:
  - Trace mode passes `readPredTaken() || isUncondCtrl()` as the actual outcome.
  - Trace mode creates `BranchInfo` only from `StaticInstPtr` when trace type
    metadata is available.
  - Trace mode changes `BTBTAGE::prepareUpdateEntries()` to accept
    `alwaysTaken` conditional entries instead of fixing upstream truth.

### 6. Tests Required

For any change to these contracts, run at least:

- Build:
  - `scons build/RISCV/gem5.opt --gold-linker -j64`
- One short trace smoke with decoupled BP enabled.
  - Assert non-zero `simInsts`.
  - Check `mbtb.condHitNotTakens`.
  - Check `tage.updateFilteredEntries`.
  - Check `tage.updateMispred`.
  - Check `tage.updateAllocSuccess`.
- One short CPT smoke.
  - Assert non-zero `simInsts`.
  - Confirm CPT-side TAGE update counters remain qualitatively unchanged.

### 7. Wrong vs Correct

#### Wrong

```cpp
// Predicted direction or synthetic static shape is not trace truth.
toFetch->decodeInfo[tid].branchTaken =
    inst->readPredTaken() || inst->isUncondCtrl();

BranchInfo info(branch_pc, target_pc, inst->staticInst, inst_size);
```

#### Correct

```cpp
if (cpu->isTraceMode() && inst->hasTraceBranchInfo()) {
    toFetch->decodeInfo[tid].branchTaken = inst->traceBranchTaken();
    set(toFetch->decodeInfo[tid].nextPC,
        RiscvISA::PCState(inst->traceBranchNextPC()));
} else {
    toFetch->decodeInfo[tid].branchTaken =
        inst->readPredTaken() || inst->isUncondCtrl();
}

BranchInfo info = makeBranchInfo(
    branch_pc, target_pc, inst, inst->staticInst, inst_size);
```

**Rule of thumb**: synthetic instructions keep trace replay executable; trace
metadata keeps trace replay faithful for frontend/BPU training.
