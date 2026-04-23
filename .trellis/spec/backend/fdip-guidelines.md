# FDIP Guidelines

> Executable contracts for the current FTQ-directed ICache prefetch model in
> `fdip-phase2-xsdev`.

---

## Overview

Read this file before changing any of the following:

- `src/cpu/o3/fetch.cc`
- `src/cpu/o3/fetch.hh`
- `src/cpu/o3/fdip_cleanup.hh`
- `src/cpu/pred/btb/ftq.hh`
- `src/cpu/pred/btb/decoupled_bpred.hh`
- `src/cpu/pred/btb/decoupled_bpred.cc`
- `src/cpu/pred/BranchPredictor.py`
- `configs/common/Options.py`
- `configs/common/xiangshan.py`
- `configs/example/kmhv3.py`
- `src/mem/request.hh`
- `src/mem/cache/base.hh`
- `src/mem/cache/base.cc`
- `src/mem/cache/cache.cc`
- `src/mem/cache/cache_probe_arg.hh`
- `src/mem/cache/xs_l2/SlicedCacheAccessor.*`

This contract documents the **current implemented** FDIP model, not the full
paper-faithful or RTL-complete future design.

---

## Scenario: Current FDIP Runtime Contract

### 1. Scope / Trigger

- Trigger: any change to FTQ-directed FDIP issue, predictor-side FDIP knobs,
  fetch-side FDIP state/lifecycle, request metadata, cache-side old-path refill
  drop, recent-unused suppression, or trace-mode FDIP observability.

### 2. Surfaces

Current parameter/config surface:

- `DecoupledBPUWithBTB.enable_fdip`
- `DecoupledBPUWithBTB.bpu_runahead_entries`
- `DecoupledBPUWithBTB.fdip_lookahead_entries`
- `DecoupledBPUWithBTB.fdip_issue_bandwidth`
- `DecoupledBPUWithBTB.fdip_max_outstanding`
- `DecoupledBPUWithBTB.prefetch_lines_per_ftq`
- `DecoupledBPUWithBTB.fdip_flush_partial_on_epoch_change`
- `DecoupledBPUWithBTB.fdip_drop_refill_on_epoch_mismatch`
- `DecoupledBPUWithBTB.fdip_recent_unused_cycles`

Current runtime/state surface:

- `Fetch::runFdip(ThreadID tid)`
- `Fetch::computeFdipLineAddrs(...)`
- `Fetch::resetFdipPartialState(ThreadID tid)`
- `Fetch::shouldDropFdipRefill(ContextID, const Request::XsMetadata &) const`
- `BaseCache::shouldSuppressFdipLine(Addr addr, bool is_secure, uint64_t cooldown_cycles) const`

Current request metadata surface:

- `Request::XsMetadata.fdipEpoch`
- `Request::XsMetadata.fdipFtqId`
- `Request::XsMetadata.fdipStartPC`
- `Request::XsMetadata.fdipSelectedWayValid`
- `Request::XsMetadata.fdipSelectedWay`
- `Request::XsMetadata.fdipSelectedWayTick`

### 3. Contracts

1. **FDIP-off invariance**
   - When `enable_fdip == false`, FDIP must not change architectural behavior.
   - `bpu_runahead_entries` must not throttle normal predictor behavior unless
     FDIP is enabled.

2. **Coverage contract**
   - FDIP cacheline coverage must follow the same actual fetch coverage used by
     demand fetch:
     - `startPC`
     - `predEndPC`
     - `fetchCoverageSpan(...)`
     - `fetchCoverageLastLineAddr(...)`
   - A cross-boundary 4B control-tail line must be included only when actual
     fetch coverage naturally reaches that line.

3. **Issue contract**
   - FDIP issue is best-effort only.
   - Demand fetch must remain functionally prior to FDIP.
   - Current tunable limits are:
     - `fdip_issue_bandwidth` (cachelines per cycle)
     - `fdip_max_outstanding` (cachelines)

4. **Redirect / partial-state cleanup contract**
   - Redirect/squash/reset must clear per-thread FDIP partial state:
     - thread-local state object
     - pending FDIP requests for that thread
     - outstanding-line accounting for removed in-flight requests
     - probe hints for that thread
   - The helper-level contract is implemented in:
     - `src/cpu/o3/fdip_cleanup.hh`

5. **Old-path refill contract**
   - When `fdip_drop_refill_on_epoch_mismatch == true`, old-path FDIP refills
     must not install into L1I on epoch mismatch.
   - This policy must stay scoped to FDIP traffic only.

6. **Recent-unused suppression contract**
   - Suppression must key on physical cacheline identity plus security domain:
     - `(blkAddr, isSecure)`
   - It must never suppress demand fetch.

7. **Probe / selected-way contract**
   - Direct probe hit may complete an FDIP line without allocating a real miss.
   - The resulting selected-way hint is current-model behavior and must remain
     explicitly FDIP-scoped.

### 4. Validation & Error Matrix

| Condition | Expected Behavior | Enforcement / Symptom |
|-----------|-------------------|------------------------|
| `enable_fdip == false` | FDIP path inactive; no FDIP requests issued | smoke / behavior parity |
| `prefetch_lines_per_ftq == cover_actual_fetch_range` and target spans cross-boundary control-tail | second cacheline is included | `fetch_coverage.test` witness |
| redirect/squash happens with pending FDIP partial state | partial state cleared, old hints removed | `fdip_cleanup.test` witness |
| epoch-mismatched FDIP refill with drop policy enabled | refill not installed into ICache | `fdipDroppedRefill` rises |
| recently-unused FDIP line revisited within cooldown window | FDIP line suppressed, demand unaffected | `fdipFilteredRecentUnused` rises |
| trace-mode high-I$ sanity run with FDIP enabled | `fetch.icacheStallCycles` changes and timeliness stats are non-trivial | `crypto14` / `compute_int_32` |

### 5. Good / Base / Bad Cases

- Good:
  - `crypto14` and `compute_int_32` show non-trivial:
    - `fetch.icacheStallCycles`
    - `fdipUsefulHits`
    - `fdipLate`
    - `fdipUnused`
- Base:
  - `srv67` smoke / 5M with tuned-on config stays performance-neutral to
    slightly positive while reducing repeated bad lines.
- Bad:
  - reintroducing `stallReason[0]`-based stall accounting
  - keying recent-unused tracking by raw `Addr` only
  - treating helper-level cleanup witness as a full mid-flight redirect harness

### 6. Tests Required

For current-model edits, the narrow required validation set is:

- `openspec validate add-fdip-icache-prefetch --strict`
- `scons build/RISCV/gem5.opt -j<N>`
- `scons build/RISCV/cpu/pred/btb/test/fetch_coverage.test.opt --unit-test -j<N>`
- `build/RISCV/cpu/pred/btb/test/fetch_coverage.test.opt`
- `scons build/RISCV/cpu/o3/fdip_cleanup.test.opt --unit-test -j<N>`
- `build/RISCV/cpu/o3/fdip_cleanup.test.opt`

For tuned-on sanity / regression:

- one focused `srv67` smoke
- one focused `srv67` 5M run
- two high-I$ sanity traces:
  - `crypto14`
  - `compute_int_32`

### 7. Current Limitations

- This is still a Phase 1 / 1.5 model, not full RTL alignment.
- No full tag/data split ICache interface exists yet.
- `prefetchPtr` / FTQ peek plumbing is not yet paper/RTL complete.
- `system.cpu.iew.fetchStallReason::IcacheStall` still remains zero in current
  trace-mode runs even though `system.cpu.fetch.icacheStallCycles` now moves.
- Redirect cleanup proof is helper-level, not a full fetch-stage directed
  harness.

### 8. Decision Guidance

Current recommendation:

- treat the current stack as complete for the P0/P1 stabilization cut
- stop at Phase 1 / 1.5 unless a new research question specifically requires
  deeper RTL-fidelity work
