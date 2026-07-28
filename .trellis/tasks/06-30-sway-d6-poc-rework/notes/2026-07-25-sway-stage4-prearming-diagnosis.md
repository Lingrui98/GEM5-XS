# SWAY Stage 4 — pre-fire pre-arming diagnosis (handoff)

> **RESOLVED 2026-07-25 (second session): bug, C-fix validated — see §13.**
> Mechanism: `updateBTBEntrySway` placement tie-break divergence at 0
> borrows. Fix: `hasActiveBorrows()` gate (uncommitted, mbtb.hh +9 /
> mbtb.cc +18). 200K + 40M(θ=1000) bit-exact vs baseline; θ=1.5 first 4
> fires bit-identical to open-loop replay. §5's "no effect" verdict was a
> stale-binary artifact. New follow-up: post-donation frozen donor-z
> (§13.7).

> **Date**: 2026-07-25
> **Author**: Claude Code (ARS session)
> **Audience**: the next agent diagnosing the SWAY substrate pre-fire
> pre-arming. This file is self-contained: it records every evidence step,
> the data, the code sites audited, the contradiction, and the open
> by-design-vs-bug question. Do not re-derive; verify + extend from here.
> **Branch**: `sway/d6-substrate-rework`
> **Sealed baseline**: tag `sway-substrate-prearming` (commit `99943d7e7e`,
> "bpu: SWAY Stage 4 live beta controller + smoke/debug tooling"). This is
> the substrate WITH the pre-arming property. Start diagnosis from this tag.
> **Uncommitted on top**: the MBTB `hasActiveBorrows()` gating (C-fix
> attempt 1) — it had **zero effect** (see §5) and can be reverted.

## 0. TL;DR

`enableSwayRealloc=True` with **0 fires** (controller never donates, way
ownership never changes) still changes the predictor: +0.32% IPC for gcc,
and the MBTB **donor-candidate way (bank0.way3 = `MbtbTightSlotWay=3`)** gets
its prediction hits **halved** (shifted to other ways) starting from phase 0.
The MBTB update path was gated on active-borrows with **no effect** (the SWAY
update path is bit-identical to the baseline at 0 borrows). Every audited
`enableSwayRealloc` path in mbtb + btb_tage is neutral at 0 borrows, yet the
hit-shift is real. The mechanism is in an unaudited path. Open question for
glr: is the donor-slot hit-shift **by-design** (reserve way3 when armed) or a
**bug**? This decides whether C (pre-fire-neutrality fix) is even correct.

## 1. Context

Stage 4A wired a live β controller (faithful C++ port of
`util/xs_scripts/sway/replay_beta.py`) into the predictor; the substrate
(mbtb/btb_tage borrowed-slot machinery) was **not** modified by 4A. The
4A live-vs-replay smoke failed for gcc (live fires 13 vs open-loop 4 at
θ=1.5). Root-cause investigation found `enableSwayRealloc=True` engages the
substrate **before any donation** ("pre-arming"), changing the pressure
distribution the controller observes. This file documents the diagnosis.

The β controller itself is **verified correct** (10/10 unit tests, mcf smoke
matches open-loop, debug CSV confirms faithful B-c derivation + z + fire).
The pre-arming is a **substrate** issue, not a controller issue.

## 2. Control experiment (gcc_pp_O2_1869)

Same binary (4A β-wiring build), same SPEC17 cpt, 20M warmup + 20M sample
(40M total), W=100K. Only `enableSwayRealloc` + θ differ. `committedInsts`
in stats.txt shows 20M (sample segment, post-warmup-reset) for all three.

| run | enableSwayRealloc | θ | fires | reallocCount | sample IPC | numCycles | Δ vs off |
|---|---|---|---|---|---|---|---|
| `gcc_base_curr` | False | — | 0 | 0 | 2.069490 | 9664214 | — |
| `gcc_armed_0fire` | True | 1000 | 0 | 0 | **2.076186** | 9633047 | **+0.3236%** |
| `sway_gcc_pp_O2_1869_debug` | True | 1.5 | 5 | 5 | 2.005915 | 9970510 | −3.072% |

`gcc_armed_0fire`'s `sway_realloc_events.csv` is **0 rows** and
`reallocCount=0` — controller never fired, no way moved, yet IPC +0.32%.
gem5 is deterministic ⇒ +0.32% is the substrate-engagement effect, not noise.

Run dirs: `runs/stage4a_verify/{gcc_base_curr, gcc_armed_0fire,
gcc_armed_0fire_v2fix, sway_gcc_pp_O2_1869_debug}/` (each has `stats.txt`,
`sway_beta_debug.csv`, `sway_phase_bc_raw.csv`, `sway_realloc_events.csv`).

## 3. Phenomenon B — T7 pressure distribution diverges from phase 0

`sway_beta_debug.csv` (armed-0-fire) vs Stage-3 baseline raw CSV
(`runs/stage3_beta_replay/spec17/gcc_pp_O2_1869/sway_phase_bc_raw.csv`,
enableSwayRealloc=False):

- phase 202 (3rd sample phase, pre-fire): live `b_c_tage_t7 = 0.057305` vs
  open-loop `0.043679`.
- warmup reference (phases 0–199, all pre-fire; frozen at first sample phase):
  live `mean=0.011813, std=0.024964, count=128` vs open-loop
  `mean=0.024090, std=0.036521, count=128`.

`count=128` both (history buffer correct). mean/std differ ⇒ T7 B-c diverges
from the **first warmup phase**, pre-fire. T7 is a TAGE donee; its B-c
divergence is **indirect** (MBTB behavior changes → different branch stream →
TAGE T7 trained differently) — TAGE itself is pre-fire-neutral (§7).

## 4. Phenomenon C — donor-candidate way3 hits halved (sharpest clue)

Raw hits (not ratios — excludes `deriveBetaPhaseMetrics` arithmetic as a
confound), armed-0-fire raw CSV vs Stage-3 raw CSV:

| phase | mbtb_all hits fix/s3 | bank0.way3 hits fix/s3 |
|---|---|---|
| 1 | 48556 / 49408 (~same) | **5043 / 10149 (halved)** |
| 2 | 45757 / 46345 | 7206 / 11305 |
| 3 | 44559 / 44365 | 8462 / 10725 |
| 5 | 42534 / 42628 | 9044 / 11257 |
| 10 | 33665 / 32717 | 7229 / 7799 |

`mbtb_all` ≈ unchanged, `bank0.way3` (donor candidate, `MbtbTightSlotWay=3`)
halved ⇒ hits **shift away from the donor candidate** to other ways, making
it artificially cold. bank0.way3 is the SWAY pinned donor slot (taxonomy §3.2).

## 5. C-fix attempt 1 — MBTB-update gating (NO effect, reverted-hypothesis)

**Hypothesis (now retracted):** `updateBTBEntrySway` (mbtb.cc:890) uses a
different insertion policy (`choose_invalid_native`, first-invalid-by-way +
oldest-visible victim) than `updateBTBEntry`'s `replaceOldestInSRAMSet`
(`pop_heap` oldest), diverging at 0 borrows.

**Fix applied (uncommitted, on top of the sealed tag):**
- `mbtb.hh`: add `int swayActiveBorrowCount{0};` + `bool hasActiveBorrows() const;`.
- `mbtb.cc transferSwayWays`: `swayActiveBorrowCount += moved` on donate,
  `-= moved` on return.
- `mbtb.cc:1190 updateBTBEntry`: `if (enableSwayRealloc && hasActiveBorrows())`
  → `updateBTBEntrySway`.

**Result:** rebuilt (BTB 23/23, sway_beta 10/10 — no regression); reran
armed-0-fire ⇒ **IPC = 2.076186, bit-identical to pre-fix**.

**Conclusion:** at 0 borrows `updateBTBEntrySway` ≡ `updateBTBEntry` (the two
insertion policies agree when no way is donated). The **update path is NOT
the cause**. The "insertion policy divergence" hypothesis is wrong;
disregard it. The `hasActiveBorrows` gating can be reverted (it's inert).

## 6. Per-path audit — mbtb `enableSwayRealloc` sites (all neutral at 0 borrows)

| site | function / role | 0-borrow behavior | neutral? |
|---|---|---|---|
| mbtb.cc:204 | `swayNativeWayVisible`: `!enabled→true; else owners[way]==native` | owners init all-native (constructor line 150-151) ⇒ all true | ✓ |
| mbtb.cc:518 | `transferSwayWays` (donate_from/return_to) | 0 fires ⇒ not called | ✓ |
| mbtb.cc:589 | `syncSwayBorrowedWayCounts` (resizeSwayExtraWays→0) | extra already 0 | ✓ |
| mbtb.cc:805 | `lookupSingleBlock` `if(!swayNativeWayVisible) continue` | all true ⇒ no skip | ✓ |
| mbtb.cc:808 | `lookupSingleBlock` extra_set scan | `swayExtra0/1` resized (line 152) but each BTBSet default-empty ⇒ no-op | ✓ |
| mbtb.cc:856/869 | `collectAndResetWayVisitCounts` snapshot (scans extra) | empty ⇒ no-op; `const` (read-only) | ✓ |
| mbtb.cc:918/977/1008 | `updateBTBEntrySway` internals | gated (§5), not called at 0 borrows | ✓ |
| mbtb.cc:1190 | `updateBTBEntry` gate | 0 borrows ⇒ normal path | ✓ |

Constructor init (mbtb.cc:148-155): `swayOwner0.assign(numWays, MbtbSram0)`,
`swayOwner1.assign(numWays, MbtbSram1)`, `swayExtra0/1.resize(numSets)`
(each BTBSet default-empty), `swayPhaseHitsPerWay.assign(2*numWays, 0)`.

Hit recording: `recordPredHitPerWay` (mbtb.cc:434) increments
`swayPhaseHitsPerWay[stage2WayStatIndex(entry.sourceSram, entry.sourceWay)]`,
called from `processEntries` (mbtb.cc:676) per lookup hit; `entry.sourceWay`
comes from `lookupSingleBlock`'s `res.emplace_back(way, …, sram_id, way_idx)`.

## 7. Per-path audit — btb_tage `enableSwayRealloc` sites (TAGE pre-fire-neutral)

- 252 `setSwayReallocEnabled`: sets flag only (`if(!enabled)` runs on disable).
- 266 `swayNativeWayVisible`: 0 borrows ⇒ true.
- 369 `swayMbtbSlotActiveForTable`: checks `slot.active` ⇒ 0 borrows false.
- 467 `syncSwayBorrowedWayCounts`: loops tables; 0 borrows no-op.
- 492 `legal_doneee`: checks `state.donatedToTage()` ⇒ 0 borrows false.
- 597 `extraWays = enabled ? swayExtraWayCount : 0` ⇒ 0.
- 620/634 visit scans extra / borrowed slots: 0 borrows empty/inactive ⇒ continue.
- 886/929 lookup scans borrowed / extra: 0 borrows ⇒ no-op.
- 1412 `scan_borrowed`: 0 borrows no active slot ⇒ false.
- **2285 `commitBranch` (TAGE update): NO `enableSwayRealloc` gate ⇒ baseline.**

So TAGE is pre-fire-neutral; the T7 B-c shift (§3) is the indirect effect of
the MBTB way3 hit-shift via the branch stream.

## 8. The contradiction + unaudited candidates

**Data proves** way3 (donor candidate) hits are halved at 0 fires (§4).
**Audit proves** every `enableSwayRealloc` mbtb+tage path is neutral at 0
borrows (§6, §7) + the update-gating experiment confirms the update is
neutral (§5). So the hit-shifting mechanism is in an **unaudited path**.

Candidates not yet audited:
- `decoupled_bpred` predict/commit flow `enableSwayRealloc` branches.
  Known site: `decoupled_bpred.cc:353 swayController.consumeQuiesceCycle()`
  (but 0 fires ⇒ quiesce=0 ⇒ returns false ⇒ should be neutral — verify).
- A redirect / spec-update / squash path in decoupled_bpred or mbtb.
- The dual phase clocks: legacy `numInstCommitted` (drives
  `collectSwayWayVisitForPhase`, decoupled_bpred_stats.cc:1823) vs
  architectural `swayRawArchInstCommitted = cpu->totalInsts()` (drives
  `collectSwayPhasePressureForPhase`, :1797). These differ in cadence but
  should not change hit *counts* — likely unrelated; verify if needed.
- β engine: `executeSwayBetaPhase` runs every architectural phase when armed
  but is pure-logic + 0-fire-no-mutate ⇒ **excluded**.

Each hypothesis costs a build (~5 min) + 40M-instr run (~30 min); the
update-gating experiment already falsified one hypothesis, so prefer
glr's substrate knowledge to point at the path before more build/run cycles.

## 9. SWAY design context — why by-design vs bug decides C's validity

- taxonomy §3.2: donor candidates pinned at design time (MBTB bank0.way3 +
  bank1.way3, routing-limited).
- taxonomy §4.1: `B-b = hits(candidate)/mean(sibling ways)`; `candidate
  cold ⟺ B-b < 1.0`. **The substrate observes the candidate's coldness; it
  does not enforce it.**
- So "armed shifts hits away from way3, making it artificially cold" violates
  the observe-don't-enforce semantics — unless the substrate intentionally
  **reserves the donor slot when armed** (keep way3 free/cold so it can be
  donated without evicting useful entries).

Decision tree:
- **Bug** (unintended hit-shift from way3): C is correct — find + gate the
  path on `hasActiveBorrows()` so 0 borrows ≡ baseline. Then smoke realigns
  (gcc live fires → ~4) and the frozen 2-arm is clean.
- **By-design** (reserve donor slot when armed): **C is wrong** — making
  way3 pre-fire-neutral would un-reserve the donor slot, so donation evicts
  useful entries. Switch to **B** (baseline arm = enableSwayRealloc=True +
  θ=∞, 0 fires; both arms armed ⇒ +0.32% cancels ⇒ ΔIPC = pure
  reallocation) or **A** (frozen 2-arm, document +0.32% in §V).

## 10. Repro / verification commands

```bash
cd /nfs/home/goulingrui/project/GEM5-sway
# build (never -j160; gold-linker)
scons build/RISCV/gem5.opt --gold-linker -j32
# unit tests (no regression)
build/RISCV/cpu/pred/btb/test/btb.test.opt     # 23/23
build/RISCV/cpu/pred/btb/test/tage.test.opt    # 34/34
build/RISCV/cpu/pred/btb/test/sway_beta.test.opt  # 10/10

# rerun a control run via taskpool (see runs/stage4a_verify/<dir>/_run_backend.sh)
~/.local/bin/taskpool submit --name <name> --project sway-stage4 \
  --tag sway-stage4a-control --cwd <abs run dir> --profile gem5 \
  --priority normal -- bash _run_backend.sh

# extract sample-epoch IPC (stats.txt has per-epoch duplicates; take last)
python3 -c "import re;[print(l) for l in open('<run>/stats.txt') if re.match(r'^system\.cpu\.(ipc|numCycles) ',l)]"

# compare raw hits per way (the way3-halving check)
python3 util/xs_scripts/sway/verify_stage4a.py \
  --new-baseline-stats runs/stage4a_verify/gcc_base_curr/stats.txt \
  --retained-stats runs/stage3_beta_replay/spec17/x264_seek_80312/stats.txt \
  --gcc-events runs/stage4a_verify/sway_gcc_pp_O2_1869_debug/sway_realloc_events.csv \
  --mcf-events runs/stage4a_verify/sway_mcf_17135/sway_realloc_events.csv
```

Open-loop replay reference (θ=1.5 fire counts, from
`runs/stage3_beta_replay/beta_events.csv`): gcc MBTB→TAGE 4 / ITTAGE→TAGE 4;
mcf MBTB→TAGE 4 / ITTAGE→TAGE 0.

## 11. Concrete next steps for the diagnosing agent

1. Start from tag `sway-substrate-prearming` (clean pre-arming substrate).
   Revert the uncommitted MBTB `hasActiveBorrows` gating (§5) — it's inert.
2. Find the path that shifts hits away from bank0.way3 at 0 fires. Likely in
   `decoupled_bpred` predict/commit flow or an mbtb redirect/squash path not
   in the §6 table. Cross-check: does `recordPredHitPerWay` get called fewer
   times for sourceWay==3, or do entries get placed in way3 less? The data
   shows mbtb_all ≈ constant while way3 halved ⇒ entries are placed in
   other ways (placement divergence) OR way3 hits are routed elsewhere
   (recording divergence). Pin down which.
3. Determine by-design vs bug (§9) with glr. If bug: gate the path on
   `hasActiveBorrows()`, rebuild, rerun armed-0-fire ⇒ expect +0.32% → ~0
   + gcc smoke live fires → ~4. If by-design: switch to B (amend §0.2
   baseline arm to armed-0-fire).
4. Whichever path: do NOT change the locked raw CSV schema
   (`sway_phase_bc_raw.csv`), the β metric formulas, the 128/32 history, the
   T3=8/T7=20 cooldowns, or the arbitration (taxonomy §3-§5; contract
   `.trellis/spec/backend/sway-phase-stream-contract.md`).

## 12. Memory pointer

Project memory `sway-stage4-ittage-advisory-only` records the ITTAGE→TAGE
advisory-only decision (substrate absent) + the β-controller wiring. This
pre-arming finding is a separate substrate issue; update that memory or add
a new one once the by-design/bug question is resolved.

---

## 13. Resolution addendum (2026-07-25, second session) — RESOLVED: bug, C-fix validated

> Status: 200K-instr cell **decisive** (bit-exact); 40M controls in flight
> (verdict lines marked PENDING below, fill on completion).

### 13.1 §5 "no effect" verdict is VOID — stale-binary artifact

The v2fix experiment never tested the gate. Evidence:

- `gem5.opt` mtime `2026-07-24 18:50` **predates** `mbtb.o` mtime
  `2026-07-24 23:57` — the object was recompiled but the binary was never
  relinked (consistent with the known `-lpng` link failure without
  `--config=force`; memory `gem5-taskpool-build-png-cache`).
- `nm -C build/RISCV/gem5.opt | grep -c hasActiveBorrows` → **0** on the
  binary used by `gcc_armed_0fire_v2fix`. The "bit-identical IPC" result
  was trivially expected — it was the same binary as the pre-fix run.
- Rebuilt properly (`scons --gold-linker --config=force -j32`, run dir
  `runs/stage4a_diag/build_v2/`): nm count ≥ 1, BTB 23/23, sway_beta 10/10.

**Lesson**: for any fix-validation run, verify the fix symbol in the
*linked binary* (`nm`/`strings` on `gem5.opt`), never infer from object
mtimes or a green-looking build script. A silent link failure leaves a
stale `gem5.opt` that runs fine and produces plausible, wrong results.

### 13.2 Mechanism pinned (§8 contradiction resolved)

The divergent path was the **update path all along** — not the policy at
the semantic level but the **placement tie-break** for new entries:

- `updateBTBEntrySway` (mbtb.cc:973-982) `choose_invalid_native`: fills the
  **lowest-numbered invalid native way** first; full-set victim scan
  (mbtb.cc:1007-1017) breaks tick ties by **way order**.
- `updateBTBEntry` → `replaceOldestInSRAMSet` (mbtb.cc:1314): `pop_heap` on
  the min-tick heap (`older()` = `a->tick > b->tick`, mbtb.hh:321); equal-tick
  tie-break follows **heap array layout**, not way order.

At 0 borrows all ways are native-visible, so the two paths are semantically
equivalent but placement-wise different. Way-order first-fill systematically
starves high-numbered ways of new placements during set fill; entries that
the baseline spreads across ways land preferentially in ways 0–2 ⇒
**bank0.way3 (and bank1.way3) hits halved from phase 0**, `mbtb_all`
≈ constant (redistribution, not hit loss). Matches §4 exactly.

Empirical first-divergence (200K debug pair, `runs/stage4a_diag/{armed,base}_200k`,
flags `BTB,Override,DecoupleBP`): identical squash history until tick
**7917408**, where armed predicts loop branch `0x124f5a` taken (BTB hit,
entry `tick:7894764`) while base falls through to `0x124f5c` (entry
absent) — a BTB **content** divergence downstream of the placement
divergence. All upstream `check misprediction` / `Commit target` lines
identical (e.g. the 7895430 `0x1253be` squash exists in both).

### 13.3 By-design vs bug (§9) — BUG; C is correct

The hit-shift is an incidental placement-order artifact, not an intentional
donor-slot reservation: at 0 borrows there is nothing to reserve, and the
taxonomy §4.1 semantics are observe-don't-enforce. With the update path
gated on `hasActiveBorrows()`, armed-0-fire is **bit-exact** with baseline
(13.4) — so C is the right fix; B (baseline arm = armed-θ-∞) is unnecessary.

### 13.4 200K-instr validation (decisive)

200K insts (100K warmup), gcc_pp_O2_1869 cpt, phase=100K, no debug flags;
`runs/stage4a_diag/{armed,base}_200k{,_gated}/`. Raw `hits_delta`:

| ph | run | all | b0w0 | b0w1 | b0w2 | b0w3 | b1w0 | b1w1 | b1w2 | b1w3 |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | armed ungated | 48556 | 21072 | 16054 | 8968 | **5043** | 24654 | 14665 | 10923 | 5076 |
| 1 | base ungated | 49408 | 10874 | 11193 | 20087 | **10149** | 11072 | 12405 | 23703 | 8754 |
| 1 | base gated | 49408 | 10874 | 11193 | 20087 | 10149 | 11072 | 12405 | 23703 | 8754 |
| 1 | **armed gated** | 49408 | 10874 | 11193 | 20087 | **10149** | 11072 | 12405 | 23703 | 8754 |
| 2 | armed ungated | 45757 | 19529 | 16226 | 10998 | **7206** | 19127 | 13429 | 11817 | 7439 |
| 2 | base ungated | 46345 | 11364 | 14634 | 18056 | **11305** | 12490 | 11922 | 17881 | 10966 |
| 2 | base gated | 46345 | 11364 | 14634 | 18056 | 11305 | 12490 | 11922 | 17881 | 10966 |
| 2 | **armed gated** | 46345 | 11364 | 14634 | 18056 | **11305** | 12490 | 11922 | 17881 | 10966 |

- armed-gated ≡ base-gated ≡ base-ungated: **0/18 cell mismatches** —
  pre-fire pre-arming eliminated; gate restores bit-exact baseline at 0
  borrows.
- base-gated ≡ base-ungated: gate is provably inert when SWAY is off
  (and confirms run determinism / debug-flag neutrality).

### 13.5 40M controls (the note's §2 scale)

- `runs/stage4a_verify/gcc_armed_0fire_gated/` (θ=1000, gated binary):
  **CONFIRMED** — sample IPC `2.069490`, numCycles `9664214`,
  committedInsts `19999998`, realloc rows 0: **bit-identical to
  `gcc_base_curr`** (+0.0000%; the ungated armed run was +0.3236%). The
  pre-arming effect is eliminated at the §2 scale.
- `runs/stage4a_verify/sway_gcc_pp_O2_1869_debug_gated/` (θ=1.5, gated
  binary): **CONFIRMED with a new finding (13.7)** — the first 4 live
  fires are **bit-identical to the open-loop replay** (phase, donor,
  donee, and z-scores to 6+ decimals): ph=211 ITTAGE.T3→T7 (zd 1.929),
  ph=243 ITTAGE.T3→T7 (1.609), ph=264 ITTAGE.T2→T7 (2.624), ph=287
  MBTB.bank1.way3→T7 (2.021). Controller faithfulness + pre-fire stream
  equivalence both validated end-to-end. Live run then diverges from
  replay after the first donation takes effect — see 13.7.
- Corrected reference: open-loop θ=1.5 gcc total is **8 fires** (4
  MBTB→TAGE: 287/310/330/386; 4 ITTAGE→TAGE: 211/243/264/362), not 4;
  §1's "13 vs 4" compared live-total vs MBTB-only. Ungated live had 14
  event rows; gated live has 11 (6 new donations + 5 re-fires of an
  already-donated slot).

### 13.7 Frozen donor-z — mechanism recon (2026-07-25, resolved to decision point)

**Mechanism chain (all links verified in code + data):**

1. Donation at phase P: `transferSwayWays` sets `owners[way3]=donee`;
   native lookups skip the way (`swayNativeWayVisible` false,
   mbtb.cc:805) ⇒ the way's native hits ≡ 0 from P+1.
2. `deriveBetaPhaseMetrics` (decoupled_bpred_stats.cc:471-475) computes
   B-b = candidate hits / mean(all 8 ways) = **0** — no NA path, real 0
   (debug CSV: `b_b_mbtb_bank1_way3 = 0.000000` every phase 288–400).
3. Donor z is sign-flipped vs the frozen warmup reference:
   z = (μ_ref − 0)/σ_ref = 7.881 **constant** (numerically confirmed:
   μ_ref≈0.988, σ_ref≈0.125 from ph 287/288 pairs; matches warmup
   reference of a healthy candidate hovering near B-b≈1).
4. The candidacy loop (sway_beta_controller.cc:313-314) iterates **all**
   donor metrics with only *donee-side* guards (cooldown :335, active
   borrow :338-342) — **no donor-side "currently donated" guard** — so
   the frozen 7.881 crosses θ every phase and keeps winning arbitration.

**Blast radius (gated θ=1.5 run):** 11 event rows but
`reallocCount=1`, `slotOwnerTransitions[sram1_way3]=1` — exactly one
real transfer (ph=287 bank1.way3→T7, held to end). The other 10 rows:
- 3 ITTAGE advisory fires (no transfer by design),
- 5 re-fires (307/327/347/367/387, `is_new_donation=false`): cooldown
  renewal on T7's active borrow; substrate no-op,
- 2 phantom "new" donations (306, 362 → T3): `transferSwayWays` moved 0
  ways (donate_from takes tight slots only — way3 — and way3 was already
  T7's; mbtb.cc:536-549), but the controller still set
  `activeBorrowSlot(T3)=true` and armed T3's 20-phase cooldown —

  **controller bookkeeping diverged from substrate** (phantom borrow
  blocks other T3 pairs until the phantom "return" at 314/370).

Also broken: the T4 donor-pressure-return channel — return evaluation
(:219) needs `zDonor <= θ`, but the frozen 7.881 never drops, so an
active borrow can only return via donee regression, never donor
pressure. In the ungated run the same artifact appears as z stuck at
6.0532/6.0999 (its bank1.way3 saw 4 owner transitions — donate/return
cycles driven by donee-side returns — with `reallocCount=3` total).

**Options:**

- **(a) keep B-b history updating** — *rejected*: a donated way has no
  native-side observable (hits structurally 0); mapping to betaNA()
  would poison `evaluablePairCount` accounting (conflates "reference
  not ready" with "donated").
- **(b) donor-side candidacy guard** — *recommended*: skip a donor
  candidate while its pinned tight slot is not native-owned. State is
  already available controller-side (activeBorrow per donee + the
  fires_/return tracking used at :191-198), or pass an ownership mask
  from MBTB. Kills re-fires, phantom donations, the T3 phantom-block,
  and the events-CSV inflation in one move; semantics = "a way already
  donated cannot be donated again" (physically true anyway). On return,
  the slot turns native, B-b resumes genuine observations, candidacy
  restores naturally. ~15-25 lines + unit tests.
- **(c) reset/decay on donation** — *rejected alone*: input stays
  B-b≡0 ⇒ z stays pinned; only meaningful as an add-on to (b), and the
  natural post-return recovery makes it unnecessary.

**Residual design question for glr (even with (b)):** T4
donor-pressure-return stays inert while a borrow is active (frozen
donor-z > θ ⇒ borrow always "sustained"). Stage-4 scope options:
accept + document (donations return only via donee regression), or
redefine T4's donor-pressure source (e.g. donor bank's *sibling-way*
pressure post-donation) — the latter touches locked metric semantics
(taxonomy §4/§5) and needs a spec amendment.

**Hygiene addendum (optional, with (b)):** record the events-CSV row /
bump realloc bookkeeping only when `moved > 0` (executeSwayBetaPhase
:573-577), so the CSV can never again show transfers that didn't
happen; (b) already prevents the situation, this is belt-and-braces.

### 13.6 What did NOT change

Per §11 step 4: raw CSV schema, β metric formulas, 128/32 history,
T3=8/T7=20 cooldowns, arbitration — untouched. The gate only routes
`updateBTBEntry` at 0 borrows; post-fire (borrows active) the SWAY path
runs exactly as before. The `hasActiveBorrows` diff (mbtb.hh +9,
mbtb.cc +18) stays; §5's "can be reverted" is superseded.
