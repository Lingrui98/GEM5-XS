# Tasks: FDIP (FTQ-directed ICache prefetch)

## 0. Baseline lock (Phase 0, mandatory)
- [ ] Lock the research baseline to post-`update-bpu-control-pc-tail-halfword` fetch semantics.
- [ ] Confirm all FDIP evaluation uses actual fetch coverage, not legacy fixed-66B request-span semantics.
- [ ] Add/keep helper-level witnesses for:
  - [ ] actual coverage span calculation
  - [ ] cross-boundary 4B control trailing-halfword extension
  - [ ] redirect / partial-state cleanup
- [ ] Run baseline trace / FS smoke and archive stable witnesses before enabling FDIP.

## 1. Spec & parameters (Phase 1)
- [ ] Add OpenSpec delta specs for `frontend-fdip`.
- [ ] Add/update FDIP parameters (default off) and document semantics:
  - [ ] `enable_fdip`
  - [ ] `fdip_lookahead_entries` (prefetchPtr distance limit, not “scan N per cycle”)
  - [ ] `fdip_issue_bandwidth` (cachelines/cycle; default 1)
  - [ ] `fdip_max_outstanding` (cachelines)
  - [ ] `prefetch_lines_per_ftq` (`start_line_only` vs `cover_actual_fetch_range`)
  - [ ] `fdip_flush_partial_on_epoch_change`
  - [ ] `fdip_drop_refill_on_epoch_mismatch` (optional RTL-aligned mode)
- [ ] Add the CSV-based validation orchestration layer:
  - [ ] `validation/tasks.csv`
  - [ ] `validation/env.example.sh`

## 2. Predictor plumbing (FTQ peek + prefetchPtr, Phase 1)
- [ ] Add a read-only FTQ peek API for `DecoupledBPUWithBTB` (offset from fetch head).
- [ ] Add an independent prefetch head pointer (`prefetchPtr`) inside the predictor/FTQ:
  - [ ] init/reset to `fetchPtr`
  - [ ] advance-only API (does not affect `fetchPtr`)
  - [ ] redirect/squash aligns `prefetchPtr` to the new `fetchPtr`
- [ ] Enforce `fdip_lookahead_entries` as the max allowed `prefetchPtr - fetchPtr` distance.
- [ ] Add minimal unit tests (optional but recommended):
  - [ ] peek correctness + out-of-range behavior
  - [ ] redirect alignment semantics for `prefetchPtr`

## 3. FDIP engine (Fetch, Phase 1)
- [ ] Add a Fetch-local FDIP scheduler that drives `prefetchPtr` (RTL-like), not “scan N entries per cycle”.
- [ ] Derive per-entry FDIP coverage from the same post-`update-bpu-control-pc-tail-halfword` demand-fetch semantics (`startPC` + actual `predEndPC` coverage), not from a fixed 66B overfetch assumption.
- [ ] Issue FDIP prefetches in **cacheline granularity** with `fdip_issue_bandwidth` (default 1):
  - [ ] if two cachelines are needed, issue them over multiple cycles (arbiter-like)
- [ ] Enforce `fdip_max_outstanding` (cachelines) as a hard best-effort limit.
- [ ] Do not block demand fetch under any condition (demand wins on port/bandwidth).
- [ ] Integrate ITLB translation + request attribute filtering:
  - [ ] fault/exception: filter (no request issued) + count
  - [ ] uncacheable/mmio: filter + count
  - [ ] ITLB miss: best-effort wait/retry without blocking demand
- [ ] Ensure prefetch requests are demand-distinguishable and inst-fetch-typed:
  - [ ] prefer `Request::INST_FETCH | Request::PREFETCH` -> `MemCmd::SoftPFReq`
- [ ] Advance `prefetchPtr` only when the FTQ entry is fully processed:
  - [ ] all required cachelines issued or filtered
- [ ] Handle squash/redirect via epoch (lifecycle tagging) and `fdip_flush_partial_on_epoch_change`.
- [ ] Add stats sufficient for coverage/timeliness analysis:
  - `issued`, `dropped` (reason-coded if practical), `outstanding`
  - `epoch_mismatch` / old-path activity
  - `useful-hit`, `late`, `unused`
  - `lookahead_distance` / `lookahead_limited_cycles` (upper-bound explanation)

## 4. Contention / pollution approximation (Phase 1.5, recommended)
- [ ] Make demand > prefetch arbitration observable and stable:
  - [ ] add reason-coded dropped / throttled accounting
  - [ ] add witness for “demand wins” when port / bandwidth contention exists
- [ ] Add / lock minimal usefulness and timeliness accounting:
  - [ ] useful-hit
  - [ ] late
  - [ ] unused
  - [ ] epoch-mismatch / old-path return activity
- [ ] Add at least one regression case where wrong-path prefetch activity occurs but does not corrupt demand-path correctness.

## 5. Optional: RTL-aligned no-pollution mode (drop refill on redirect)
- [ ] Add an opt-in mode `fdip_drop_refill_on_epoch_mismatch` that prevents old-path FDIP
  prefetch refills from installing into ICache (to match XiangShan flush semantics).
- [ ] Ensure the policy applies only to FDIP prefetches (not demand fetch, not generic SW prefetch).
- [ ] Add targeted tests (if feasible) demonstrating refill drop under epoch mismatch.

## 6. Decision gate (go / no-go before deeper RTL alignment)
- [ ] Run the minimal research matrix on high-I$-pressure workloads.
- [ ] Summarize whether benefit mainly comes from miss-start timeliness or whether hit-path RTL mismatch is now the dominant gap.
- [ ] Decide and record one of:
  - [ ] stop after Phase 1 / 1.5
  - [ ] continue to Phase 2
  - [ ] justify Phase 3 exploration

## 7. Optional next stage: half-RTL alignment (Phase 2)
- [ ] Prototype a short-lifetime tag-probe / selected-way state object for Fetch/FDIP.
- [ ] Evaluate whether demand can safely reuse recent tag/way results without changing external cache API.
- [ ] Add directed redirect / squash / second-line-pending cases for the new state.
- [ ] Measure whether this stage changes hit-path latency / contention behavior enough to justify continued complexity.

## 8. Optional next stage: full RTL alignment spike (Phase 3)
- [ ] Scope the required ICache interface split:
  - [ ] tag/way lookup request/response
  - [ ] data read by selected way
- [ ] Scope resource model changes:
  - [ ] fetchMSHR / prefetchMSHR separation
  - [ ] tag/data port arbitration
  - [ ] old-path refill drop at cache side
- [ ] Produce a bounded implementation spike or design memo before committing mainline implementation work.

## 9. Docs
- [ ] Document how FDIP interacts with decoupled frontend and FTQ.
- [ ] Document parameters, stats, and known limitations.
- [ ] Document the phased roadmap and the explicit stop/go criteria for deeper RTL alignment.

## 10. Validation
- [ ] `openspec validate add-fdip-icache-prefetch --strict`
- [ ] Build at least one relevant target (e.g. `build/RISCV/gem5.opt`) and ensure no compile errors.
- [ ] Run a small FS/trace smoke to ensure FDIP off-by-default and on-by-config behavior.
- [ ] Add one directed cross-boundary 4B control-tail validation case/witness:
  - [ ] when the FTQ entry coverage naturally extends across the boundary, FDIP prefetches the trailing-halfword line as part of the same entry
  - [ ] when it does not, FDIP does not rely on legacy fixed-66B overfetch to fetch that line
- [ ] Research sanity: on at least 2 high-I$-pressure traces, confirm FDIP changes `fetch.icacheStallCycles` and that FDIP’s timeliness stats (useful/late/unused) are non-trivial and explainable.
- [ ] Add the same CSV-based execution orchestration layer used by `update-decoupled-btb-control-pc-views`:
  - [ ] add a machine-readable validation manifest (for example `validation/tasks.csv`)
  - [ ] encode dependency graph / failure policy / artifact contract for low-intervention sub-agent execution
  - [ ] include `setup_env`, `build_unit`, smoke gates, research batch, and `final_verify`
