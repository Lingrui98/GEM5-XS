# topdownV2: D4-B dispatch-slot conservation ledger — usage contract

> Implementation branch `btbp/topdown-ledger-d4b` (base `de4d31f93d`).
> Design contract (authoritative, owner-frozen v4):
> `btb_prefetch_paper/.local/btb-morphology-spike-20260910/d4b-design/D4B_DESIGN_CONTRACT_DRAFT.md`.

## What it is

A statistics-only slot-accounting decomposition of the O3 pipeline
(group `system.cpu.topdownV2`), plus an overlap-diagnostics group
(`system.cpu.d4bDiag`).  The cut point is C4 — the IEW dispatch event
(the `dispatchedInsts` increment).  Every DynInst that crosses C4 is
registered once; every settlement (retirement or first squash
observation) is routed by the registration epoch stamp; every cycle
partitions its `renameWidth` slot budget into
crossings + FE/BE/Recovery/Drain/Unknown unused slots.

**None of the outputs feed any pipeline decision.**  The old
statistics, the old L1 formulas and the simulation control paths are
untouched; both new groups default to OFF.

## Enabling

```
--enable-d4b-topdown-ledger    # topdownV2 group (default off)
--enable-d4b-diag              # d4bDiag H-counter sampling (default off)
```

The groups always print (they are constructed unconditionally); when
disabled every counter is 0 and every ratio is `nan`
(`topdownV2.statEnabled=0` self-describes the quiescent arm).

## Supported domain (owner-frozen D-2)

`numThreads == 1`, `enableDispatchStage == false`, and
`decodeWidth == renameWidth` (the kmhv3 trace configuration).  Any
other configuration sets `topdownV2.ledgerState=1` (unsupported): the
raw counters still accumulate for debugging, `slotWidth` reports the
configured value unchanged, and every derived ratio is an explicit
quiet `nan` — never 0 or infinity, and never a zeroed `slotWidth` used
to manufacture the `nan`.  Guard violations
(`budgetOverrun`, `registeredNotInROB`, `liveOverCapacity`,
`identityViolations`, `allocatedIdentityViolations`) set
`ledgerState=2` (invalid), which outranks unsupported.  **Any
numerical acceptance requires `ledgerState==0` first.**

## Reading the output (ROI semantics)

- With `--warmup-insts-no-switch W --roi-insts R` the run produces
  three stats sections: seg1 = warmup (dump+reset at ROI begin),
  **seg2 = the ROI segment (dump at ROI end — this is the analysis
  segment)**, seg3 = the frozen tail (identical to seg2 for every
  `topdownV2` live-group member; the bridge formulas B1/B3 show the
  documented drain-tail drift there, B2 stays 0).
- The ROI-end snapshot is taken at the Option B end-of-tick anchor
  (end of the request tick, after every CPU-side phase), so
  `snapshot.slotBudget == slotWidth * numCycles` exactly and
  `snapshot.allocated == iew.dispatchedInsts` at the seg2 dump.
- Machine checks at the ROI-end dump (all print as
  `system.cpu.d4bDiag.bridge*`):
  - B1 `snapshot.allocated - iew.dispatchedInsts = 0`
  - B2 `snapshot.retiredCohort + settledOutOfEpochRetired -
    committedInsts = 0`
  - B3 `snapshot.slotBudget - issueWidth*numCycles = 0`
  - E2 `dispatchedInsts - committedInsts - (squashedCohort +
    liveAtClose - settledOutOfEpochRetired) = 0`
- `sumCheck` is a *representation invariant* (the six L1 ratios sum to
  1 when the identities hold), not a scientific acceptance criterion.

## Interpretation limits (do not over-claim)

- Conservation is a bookkeeping property of the hooks, not a causal
  proof: misclassification can coexist with a closed ledger.
- The P1..P6 attribution policy is owner-frozen *policy*, not truth;
  it is auditable and replayable (the full evidence tuple table), not
  self-evidently correct.
- Wrong-path cost incurred *before* C4 is out of scope for this
  ledger (known limit, contract section 8).
- The d4bDiag H1/H3/H4 counters are overlap proxies: no additivity,
  no "main cause" claims; mutual exclusivity is a measured conclusion
  (see the pairwise overlap matrix), not an assumption.

## Replay / audit layer

Each classified cycle's complete evidence tuple (all section 1.5a
inputs) is retained in an in-memory lossless table (resource-capped at
16384 distinct tuples; on overflow `replayComplete` goes 0 and
`replayTableOverflowTuples` counts the dropped records — no silent
truncation).  `slotPatternDist` (8×8) is only a display summary.
Replay covers policy changes expressible over the retained tuples;
policies needing new signals or cross-cycle history require a schema
bump and re-collection.
