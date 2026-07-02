# Phase A Step 1 controller verification

## Conclusion

The D6 controller is not stubbed in the current worktree. The prompt's source
path is slightly stale: constructor/configuration still lives in
`src/cpu/pred/btb/decoupled_bpred.cc`, but
`SwayController::chooseReallocation()` is implemented in
`src/cpu/pred/btb/decoupled_bpred_stats.cc`.

The controller is entered from the normal phase-boundary path when
`enableSwayRealloc` is true:

1. `notifyInstCommit()` checks `numInstCommitted % phaseSizeByInst == 0`.
2. On a processed main phase, it calls `collectSwayWayVisitForPhase()`.
3. That snapshots MBTB and TAGE way visits into `phaseRows`.
4. If `enableSwayRealloc` is true, it calls `trySwayReallocForPhase()`.
5. `trySwayReallocForPhase()` calls `chooseReallocation()` and executes the
   returned action only if `decision.valid` and `transferSwayWays()` moves a
   donor slot.

Current evidence says `reallocCount=0` is more likely a policy/input condition
than a missing call path. `reallocCount` only increments after a valid decision
survives quiesce/min-owned-way checks and the transfer returns `moved > 0`.
There is no current stat that counts "controller evaluated" or "decision
generated but not executed", so Phase A Step 2's per-phase trace is still
required.

## Current call-chain evidence

- `decoupled_bpred.cc:115-130` reads `enableSwayRealloc`,
  `swayReallocWays`, `swayReallocHysteresis`, `swayDoneeTableSet`, and wires
  them into `swayController.configure(...)`.
- `decoupled_bpred_stats.cc:1330-1365` calls
  `collectSwayWayVisitForPhase(currentPhaseID)` at the main phase boundary.
- `decoupled_bpred_stats.cc:340-374` builds `phaseRows` from
  `mbtb->collectAndResetWayVisitCounts()` and
  `tage->collectAndResetWayVisitCounts()`, then calls
  `trySwayReallocForPhase(phaseRows)` when `enableSwayRealloc` is true.
- `decoupled_bpred_stats.cc:286-337` calls
  `swayController.beginPhase()`, captures MBTB tight-slot owner state via
  `mbtb->getSwayMbtbTightSlotStates()`, calls `chooseReallocation(...)`, and
  increments `swayStats.reallocCount` only after `transferSwayWays()` returns
  a non-zero moved count.
- `decoupled_bpred_stats.cc:65-231` implements real decision logic: component
  utility aggregation, active MBTB-donee tracking, cooldown blocking/renewal,
  return decisions, and MBTB-to-TAGE tight donation decisions.

## Decision logic observed

`chooseReallocation()` currently reads:

- the per-phase utility rows for MBTB/TAGE scopes;
- the configured donee table set;
- MBTB tight-slot owner state;
- per-donee cooldown state.

It can return:

- `MbtbToTageTight` when an available native MBTB tight slot exists, a
  configured TAGE donee row exists, the donee is not already active/cooling
  down, and `tageDonee->utility - donorComponentUtility` exceeds the tight
  hysteresis margin;
- `ReturnMbtbTight` when a donated MBTB slot's donee cooldown just expired
  and the donee table no longer beats the native MBTB slot by hysteresis;
- `blockedByCooldown` when all candidate donees are cooling down;
- an invalid/hold decision otherwise.

The current policy still compares the candidate TAGE table against aggregate
MBTB component utility for donation. If the aggregate MBTB utility is high
while the specific donor slot is cold, this can suppress all donations. That
matches the RC5/RC6 hypothesis and must be checked by Step 2 with per-slot
utility.

## Transfer-path evidence

- `mbtb.cc:273-353` exposes the two MBTB tight-slot owner states and executes
  MBTB-to-TAGE donation / TAGE-to-MBTB return only for the designated tight
  way.
- `btb_tage.cc:407-414` intentionally returns zero from
  `BTBTAGE::transferSwayWays()` because D6 models TAGE only as a donee. This
  is not a controller stub; MBTB-owned donor slots are expected to move through
  `MBTB::transferSwayWays()`.

## Recent commit / diff audit

Commands inspected:

- `git log --oneline --decorate -n 20 -- src/cpu/pred/btb/...`
- `git log -n 6 -G"chooseReallocation|trySwayReallocForPhase|reallocCount|enableSwayRealloc|MbtbToTageTight|ReturnMbtbTight" -- ...`
- `git diff -- src/cpu/pred/btb/btb_tage.cc src/cpu/pred/btb/decoupled_bpred.hh src/cpu/pred/btb/decoupled_bpred_stats.cc`

Recent relevant commits found by the `-G` search:

- `b2822df86c bpu: Add SWAY way owner realloc controller` added the original
  phase-boundary controller and `reallocCount` execution path.
- `aedcb84d0f bpu,tests: Add SWAY D6 borrowed substrate` changed TAGE into a
  non-donor substrate and added MBTB borrowed-bank plumbing.
- `9faf270f99 bpu: Add SWAY D6 controller observability` rewired the policy
  around component utility, donee tables, cooldown, and D6 stats.

No recent committed change was found that removes the phase-boundary call,
forces `chooseReallocation()` to always hold, or bypasses `trySwayReallocForPhase()`.

Current uncommitted worktree diff is not a stub either. It changes:

- TAGE allocation priority to scan native/borrowed/extra storage in the same
  `invalid -> weak+!useful -> any-!useful` tiers.
- cooldown expiry handling with `renewCooldown`;
- `chooseReallocation()` to pass MBTB tight-slot state and avoid choosing
  already-donated active donee tables.

## Step 2 implication

Proceed to Phase A Step 2. The first trace should distinguish:

- controller never evaluating: unexpected, not supported by current source;
- evaluated but no valid decision: likely utility/hysteresis/cooldown condition;
- valid decision but no movement: execution path bug between decision and
  `MBTB::transferSwayWays()`;
- movement but `reallocCount` still zero: stat/update bug.

The required per-slot MBTB utility is the highest-value missing evidence,
because the current donation predicate still uses aggregate MBTB utility.
