# Tasks: Proactive BTB-entry prefetch (decode-ahead)

## 1. Spec & parameters
- [ ] Add OpenSpec delta specs for `frontend-btb-prefetch`.
- [ ] Add new enable/limit parameters (default off) and document them.

## 2. FTQ lookahead integration
- [ ] Consume the FTQ peek API (provided by `add-fdip-icache-prefetch`) to walk future fetch blocks.
- [ ] Add epoch/lifecycle tagging so old-path work is not consumed as new-path state.

## 3. Predecode implementation (RISC-V)
- [ ] Implement a lightweight predecode for direct CFIs (branch/jal; optional RVC).
- [ ] Consume bytes for the current FTQ entry’s **actual fetch coverage window** (`[startPC, predEndPC)` / complete-window-or-miss), not a synthetic fixed-66B window.
- [ ] Compute direct targets and build `BTBEntry` candidates in PC order.
- [ ] Keep BTB prefill/index identity at predictor-visible `controlPC`, while retaining explicit `startPC` metadata for bytes coverage and logging.
- [ ] Add unit tests for predecode correctness (encoding -> type/target).
- [ ] Trace mainline: use a deterministic trace window-assembler (built on `TraceFetch::createMachInstFromTrace`) to provide fetch-window bytes (`complete window or miss`) for predecode; document required trace configuration (e.g., `traceTrainBranches=true`) and lossy/degenerate cases.
- [ ] Add a predecode mismatch counter (or equivalent guard) to separate implementation errors from wrong-path effects in experiments.

## 4. BTB prefill path (MBTB first)
- [ ] Add a bounded-bandwidth prefill insertion path into the target BTB structure (MBTB preferred).
- [ ] Define and implement conditional-branch metadata initialization semantics for prefills (or explicitly prohibit prefill from mutating direction state).
- [ ] Provide knobs for branch-type filtering and wrong-path pollution policy.
- [ ] Implement a true no-pollution counterfactual for `allow_wrongpath_pollution=false` (recommended: epoch overlay) and add a minimal correctness check that squashed prefill cannot affect later correct-path prediction.
- [ ] Add stats sufficient to quantify benefit vs cost:
  - `blocks`, `decode_miss/bytes_miss`, `discovered`, `inserted`
  - `useful_hit`, `unused_evicted`
  - `wrongpath_inserted`, `wrongpath_hit_on_correct`, `harmful_wrongpath_use`

## 5. Docs
- [ ] Document the dependency on FDIP and the degrade behavior when bytes are unavailable.
- [ ] Document the supported CFI types and the meaning of each stat/knob.

## 6. Validation
- [ ] `openspec validate add-btb-entry-prefetch --strict`
- [ ] Build at least one relevant target (e.g. `build/RISCV/gem5.opt`) and ensure no compile errors.
- [ ] Run a small FS/trace smoke (FDIP + BTB-prefetch enabled) to sanity-check stats activity.
- [ ] Add a directed split-4B direct-CFI validation case:
  - [ ] complete window available -> predecode/prefill succeeds with `controlPC` key plus preserved `startPC` metadata
  - [ ] incomplete tail bytes -> no BTBEntry generated yet, rather than generating a tail-PC keyed artifact
- [ ] Research sanity: run at least 1 direct-heavy trace and 1 indirect-heavy trace; report CFI-type mix and confirm results respect the direct-only upper bound (i.e., explain “no benefit” when indirect dominates).
