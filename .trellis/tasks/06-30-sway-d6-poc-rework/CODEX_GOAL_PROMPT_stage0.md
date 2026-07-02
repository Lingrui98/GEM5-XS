# CODEX_GOAL_PROMPT Stage 0 — trace-mode stat sanity

> **Scope**: verify that trace-mode TAGE update fix (commit `1e6bed3e98 cpu-o3,bpu: Fix trace-mode TAGE update silence`) is behaviorally correct on real traces. No code changes expected. Read-only sanity + note.
>
> **Effort**: 30-60 min (2 trace runs + 1 note).
>
> **Blocker context**: Before Stage 2 baseline sweep (which will run on champsim + CBP traces at scale), we must confirm the following stats increment non-zero and are order-of-magnitude sane on trace mode. If trace-mode TAGE / ITTAGE update is still silent, Stage 2 data would be garbage and we would silently mis-conclude the SWAY thesis.

## Context (read first)

- `~/project/papers/sway-paper/docs/SESSION_RESUME_2026-07-02.md` — overall SWAY paper state
- `~/project/GEM5-sway/.trellis/tasks/06-30-sway-d6-poc-rework/CODEX_GOAL_PROMPT_v4.md` — most recent controller-side work (F1' verdict = Case C, closed 2026-07-02)
- `~/project/GEM5-sway/.trellis/tasks/06-30-sway-d6-poc-rework/notes/2026-07-02-f1p-*.md` — F1' closeout
- `git log --oneline sway/d6-substrate-rework` should show `1e6bed3e98 cpu-o3,bpu: Fix trace-mode TAGE update silence` and `483f546ef6 misc: Close trace-mode TAGE diag`
- `git show --stat 1e6bed3e98` — the fix commit; skim so you know what path was changed

Trace runner and options:

- Wrapper script: `~/project/mytools/tools/distributed-trace-scheduler/scripts/run_trace_gem5.sh`
- Env vars of interest: `GEM5_HOME`, `TRACE_FORMAT=champsim|cbp2025`, `XS_MAX_INSTS`, `XS_WARMUP_INSTS_NO_SWITCH`
- Traces already have half-instruction warmup convention: run the whole trace, first half = warmup, second half = sample (already tagged in trace list `.tsv` files under `/nfs/home/share/glr/{champsim_traces,cbp_traces}/`)

## Task

### Step 1 — pick 2 traces

1. From `/nfs/home/share/glr/champsim_traces/champsim_traces_btb_intensive.lst`, pick the first entry (or any BTB-intensive one with ≥ 30M instructions per `champsim_traces_inst_counts.tsv`).
2. From `/nfs/home/share/glr/cbp_traces/cbp_traces.lst`, pick the first `int/` entry (or any int trace with ≥ 30M instructions per `cbp_trace_inst_counts.tsv`).

Record: trace name, full trace path, instruction count.

### Step 2 — run each trace under baseline (no SWAY)

For each of the 2 traces:

- Output dir: `~/project/GEM5-sway/runs/stage0_trace_sanity/<workload_name>/`
- Config: baseline (no SWAY): `enableSwayRealloc=False`
- Total instructions: 30M (or whole trace if shorter), which under the half/half convention means 15M warmup + 15M sample. If the runner uses `XS_WARMUP_INSTS_NO_SWITCH` to gate warmup, set it to trace_len/2. If not (i.e. trace runner just replays), use `--maxinsts 30000000` (or full).
- Format: `champsim` for champsim trace, `cbp2025` for CBP trace.

Log the exact invocation for each run into the note. Run sequentially to avoid resource contention.

### Step 3 — inspect `stats.txt` for both runs

For each run, extract and record:

**Group A — MBTB stats (should always be non-zero if BTB is used):**
```
system.cpu.branchPred.mbtb.predHit
system.cpu.branchPred.mbtb.predMiss
system.cpu.branchPred.mbtb.condHits
system.cpu.branchPred.mbtb.condMisses
```

**Group B — TAGE stats (the ones the fix commit targets):**
```
system.cpu.branchPred.tage.updateCalls
system.cpu.branchPred.tage.updateFilteredEntries
system.cpu.branchPred.tage.updateMispred
system.cpu.branchPred.tage.updateTableMispreds  (all 8 entries)
```

**Group C — ITTAGE stats:**
```
system.cpu.branchPred.ittage.commitHits
system.cpu.branchPred.ittage.commitMisses
system.cpu.branchPred.ittage.commitIndirectHits  (or equivalent)
system.cpu.branchPred.ittage.updateCalls         (or equivalent update-side counter)
```

If the stat name is not exactly as above, use `grep -E "branchPred\.(mbtb|tage|ittage)\." stats.txt` to enumerate and pick the closest equivalent, and note the mapping.

### Step 4 — sanity criteria

Verdict = **PASS** iff ALL of:

- `mbtb.predHit + mbtb.predMiss > 1e6` (BTB is actually being looked up)
- `mbtb.condMisses > 0` and `mbtb.condHits > 0`
- `tage.updateCalls > 0` AND `tage.updateFilteredEntries > 0` AND `tage.updateMispred > 0`
- `tage.updateTableMispreds[t]` non-zero for at least 3 of 8 tables
- `ittage.commitHits + ittage.commitMisses > 0` (only if trace has indirect branches; if 0 and workload is exchange2/deepsjeng-like, note it but do not fail)
- Order of magnitude sanity: `tage.updateFilteredEntries` should be within 2-10× of `mbtb.condHits + mbtb.condMisses` (they measure adjacent populations; if `updateFilteredEntries = 0` while `condHits + condMisses` is millions → **fail**, TAGE update is still silent)

Verdict = **FAIL** if any of the above breaks.

### Step 5 — write note

Path: `~/project/GEM5-sway/.trellis/tasks/06-30-sway-d6-poc-rework/notes/2026-07-XX-trace-mode-stat-sanity.md`

Format (data-only, no editorial):

```markdown
# Stage 0 trace-mode stat sanity

## Traces

| trace | source list | full path | inst count | format |
|---|---|---|---:|---|
| ... | | | | |

## Runs

| trace | outdir | invocation | exit reason | simInsts |
|---|---|---|---|---:|

## Group A — MBTB

| trace | predHit | predMiss | condHits | condMisses |
|---|---:|---:|---:|---:|

## Group B — TAGE

| trace | updateCalls | updateFilteredEntries | updateMispred |
|---|---:|---:|---:|

TAGE updateTableMispreds (per-table t0..t7):

| trace | t0 | t1 | t2 | t3 | t4 | t5 | t6 | t7 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|

## Group C — ITTAGE

| trace | commitHits | commitMisses | commitIndirectHits | updateCalls |
|---|---:|---:|---:|---:|

## Verdict

- champsim trace: PASS / FAIL — reason if FAIL
- CBP trace: PASS / FAIL — reason if FAIL

## Recommendation

- If both PASS → Stage 2 baseline sweep may proceed on trace mode.
- If any FAIL → open new trellis task for trace-mode fix follow-up; halt Stage 2 planning.
```

## Hard constraints

- Do NOT modify any source files. This is pure sanity, no code changes.
- Do NOT touch `docs/SWAY_MECHANISM_SPEC_v2.md`, `docs/DESIGN_DECISIONS_PENDING_2026-07-01.md`, `INSIGHT.yaml`.
- Do NOT run more than 2 traces total. If runs fail with build/env errors, fix env only (e.g., set `GEM5_HOME`) and retry once. If still failing, log the failure into the note and STOP.
- Do NOT enable SWAY (`enableSwayRealloc=True`) in this stage. Sanity is on the baseline path only; the trace-mode fix is orthogonal to SWAY.
- Build: if `build/RISCV/gem5.opt` is stale after `git status` shows the branch has advanced, rebuild via `scons build/RISCV/gem5.opt --gold-linker -j32` (NOT `-j160`, header race per project quirks memory).

## Reporting contract

Write the note verbatim as specified. No summary paragraph, no interpretation beyond the verdict line. glr reads the tables and decides.
