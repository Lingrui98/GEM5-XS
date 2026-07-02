# Stage 0 trace-mode stat sanity

## Traces

| trace | source list | full path | inst count | format |
|---|---|---|---:|---|
| cvp1_public_compute_int_32 | /nfs/home/share/glr/champsim_traces/champsim_traces_btb_intensive.lst | /nfs/home/share/glr/champsim_traces/cvp1_public/compute_int_32.gz | 30756812 | champsim |
| int_10_trace | /nfs/home/share/glr/cbp_traces/cbp_traces.lst | /nfs/home/share/glr/cbp_traces/int/int_10_trace.gz | 60000052 | cbp2025 |

## Runs

| trace | outdir | invocation | exit reason | simInsts |
|---|---|---|---|---:|
| cvp1_public_compute_int_32 | runs/stage0_trace_sanity/cvp1_public_compute_int_32 | `GEM5_HOME=/nfs/home/goulingrui/project/GEM5-sway OUTDIR=/nfs/home/goulingrui/project/GEM5-sway/runs/stage0_trace_sanity/cvp1_public_compute_int_32 TRACE_FORMAT=champsim XS_MAX_INSTS=30000000 XS_WARMUP_INSTS_NO_SWITCH=15000000 /nfs/home/goulingrui/project/mytools/tools/distributed-trace-scheduler/scripts/run_trace_gem5.sh /nfs/home/share/glr/champsim_traces/cvp1_public/compute_int_32.gz` | `Program aborted at tick 3330`; `Fetch::lookupAndUpdateNextPC` assertion failed | N/A |
| int_10_trace | runs/stage0_trace_sanity/int_10_trace | N/A | not run; stopped after cvp1_public_compute_int_32 abort | N/A |

## Group A - MBTB

| trace | predHit | predMiss | condHits | condMisses |
|---|---:|---:|---:|---:|
| cvp1_public_compute_int_32 | N/A | N/A | N/A | N/A |
| int_10_trace | N/A | N/A | N/A | N/A |

## Group B - TAGE

| trace | updateCalls | updateFilteredEntries | updateMispred |
|---|---:|---:|---:|
| cvp1_public_compute_int_32 | N/A | N/A | N/A |
| int_10_trace | N/A | N/A | N/A |

TAGE updateTableMispreds (per-table t0..t7):

| trace | t0 | t1 | t2 | t3 | t4 | t5 | t6 | t7 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| cvp1_public_compute_int_32 | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |
| int_10_trace | N/A | N/A | N/A | N/A | N/A | N/A | N/A | N/A |

## Group C - ITTAGE

| trace | commitHits | commitMisses | commitIndirectHits | updateCalls |
|---|---:|---:|---:|---:|
| cvp1_public_compute_int_32 | N/A | N/A | N/A | N/A |
| int_10_trace | N/A | N/A | N/A | N/A |

## Verdict

- champsim trace: FAIL - gem5 aborted before stats dump; `stats.txt` is 0 bytes.
- CBP trace: FAIL - not run because Stage 0 stopped after the first run aborted.

## Recommendation

- Open a trace-mode fix follow-up; halt Stage 2 planning.

## Root cause (added 2026-07-02 EOD by glr)

Not a generic gem5 trace-mode bug. Root cause = **base branch too old**:
`sway/d6-substrate-rework` was branched from `add587d180` (2026-06-16),
before `trace-new` accumulated 8 FDIP commits (30f9fc3d24..f9957d56cb,
2026-06-24) that touch `src/cpu/o3/fetch.cc`, `cpu.cc`,
`decoupled_bpred.cc/hh`, etc. The `Fetch::lookupAndUpdateNextPC`
assertion very likely fires because SWAY's local trace-mode changes
(1e6bed3e98 among them) diverge from what current trace-new expects
after those FDIP fetch-path refactors.

**Blocked by**: Task #7 rebase `sway/d6-substrate-rework` onto `trace-new`
(sway-paper repo docs/SESSION_RESUME_2026-07-02.md §6 + tasks board).
Do NOT retry Stage 0 until rebase completes; retrying on the same base
will hit the same abort.

**Rerun after rebase**: same two traces, same invocation from this note's
"Runs" table. Expect PASS if rebase is clean.
