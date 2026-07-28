# Stage 2 verdict v2 (2026-07-09 / 2026-07-10)

Supersedes `2026-07-03-stage2-verdict.md`. Same input data, different gate
+ champsim family excluded.

## Why v2

The 2026-07-03 codex verdict declared 20 target workloads per direction
(4 directions x 20) using:

```
sway_target = (workload_candidate == 1)
              AND (donor_score < 1.0)
              AND (donee_score > family_median_donee_score)
```

The 2026-07-09 recon (see this repo's session log) showed both cuts too
loose to distinguish real donor/donee pressure:

- `donor_score < 1.0` — passes almost every workload. `donor_score` is
  `min(pinned candidate hits) / mean(sibling hits)`, so `< 1.0` merely
  means the pinned candidate is at-or-below average; not the same as
  materially stranded. Example: `gcc_pp_O2_1869` has all 8 MBTB ways at
  1.74M-1.89M hits (max-min spread 8%) so way3 is not stranded at all,
  yet its `mbtb_donor_b_b = 0.288` (below the family mean of 1.0 = 8/8),
  and its `MBTB_to_ITTAGE_donor_score = 0.957`, which passes the `< 1.0`
  gate.
- `donee_score > family_median` — by construction 50% of the family
  passes this; independent of whether donee-side b_c is actually large.

Result: 20 targets per direction is a gate artifact. Real signal per
direction is much smaller.

## v2 gate

Applied per (direction, family):

```
donee_max_family = max(donee_score over candidate rows in dir & family)
thresh_family    = donee_max_family * 0.10

sway_target_v2 = (workload_candidate == 1)
                 AND (donor_score < 0.5)
                 AND (donee_score > thresh_family)
```

Rationale:

- `donor_score < 0.5` — B-b range is `[0, N]` with `1.0` = family mean.
  Values <= 0.5 mean the pinned candidate is receiving less than half
  the average sibling load, i.e. materially stranded per §4.1's cold-
  candidate criterion.
- `donee_score > 0.10 * per-family max` — relative because donee b_c is
  not comparable across (direction, family): ITTAGE-as-donee is 100x
  smaller than TAGE-as-donee for the same workload (5 tables of ~500
  entries each vs 8 tables of ~1024 each); CBP int 20M-inst b_c is 4
  orders of magnitude smaller than SPEC17 despite the same instruction
  budget because CBP inst mix has less BTB pressure. An absolute cutoff
  in either dimension would silently zero out one axis.

Both cutoffs are hyperparameters, but each is grounded: 0.5 comes from
the C1 stranded-verification semantics (§4.1); 0.10 is a keep-top-decile
rule of thumb. Neither is tuned against downstream IPC.

Script: `runs/stage2_baseline_expanded/scripts/analyze_w0p_v2.py`.
Outputs: `w0p_per_workload_v2.csv`, `w0p_per_direction_summary_v2.tsv`,
`w0p_target_workloads_v2.tsv`.

## Champsim family excluded

Codex Stage 2 scheduled 30 `cvp1_public_srv_*` champsim traces after
pre-excluding the known-broken `cvp1_public_compute_int_32`; all 30
aborted at `Fetch::lookupAndUpdateNextPC`. The 2026-07-02 stat-sanity
rerun note attributed this to a pre-existing trace-new incompatibility
after seeing the same abort for `compute_int_32` on pristine trace-new.

2026-07-09 differential-run (memory: `sway-champsim-regression`) proved
the abort is SWAY-side, not trace-new-side:

- SWAY `sway/d6-substrate-rework@6dfd0e1b3a` + `cvp1_public/srv_0.gz`
  -> abort tick=20979 (both with and without Stage 2 uncommitted stat
  probes, both with and without the `phaseSizeByInst` script param).
- pristine `my/trace-new@c7be4bd32a` + same trace -> ran 3 min to
  sn=14M with no abort.

Source-code delta between the two branches under `src/cpu/pred/btb/`
does not touch FTQ, fetch, predEndPC, predBranchInfo, or predictWidth
(grep-verified). Suspected UB / memory-write side effect not visible in
source. Root-cause deferred to PENDING-24 (this file, and
`docs/SWAY_METRIC_TAXONOMY.md` will link).

Champsim rows are therefore removed from the v2 gate. This weakens the
non-SPEC coverage story but does not affect the two directions that
pass gate.

## Results

Per-direction target counts on 36 SPEC17 + 4 CBP int completed runs:

| direction     | SPEC17 targets | CBP int targets | total | v2 gate | codex verdict |
|---|---:|---:|---:|:---:|---:|
| MBTB->ITTAGE  | 1              | 2               | 3     | fail (<5) | 20 |
| ITTAGE->MBTB  | 2              | 0               | 2     | fail (<5) | 20 |
| MBTB->TAGE    | 6              | 1               | 7     | **pass**  | 20 |
| ITTAGE->TAGE  | 5              | 0               | 5     | **pass** (marginal) | 20 |

Full list per direction, ranked by donee_score:

### MBTB->TAGE (7 targets, primary Stage 3 direction)
| workload                          | donee | donor | best TAGE table |
|---|---:|---:|---|
| spec17/omnetpp_21279              | 0.737 | 0.180 | T7 |
| spec17/deepsjeng_88772            | 0.590 | 0.341 | T7 |
| spec17/x264_pass1_17232           | 0.205 | 0.439 | T3 |
| spec17/exchange2_101257           | 0.169 | 0.143 | T7 |
| spec17/leela_60438                | 0.133 | 0.444 | T3 |
| spec17/xalancbmk_6102             | 0.080 | 0.025 | T7 |
| cbp_int/int_13_trace              | 0.038 | 0.091 | T7 |

### ITTAGE->TAGE (5 targets, primary Stage 3 direction)
| workload                          | donee | donor | best TAGE table |
|---|---:|---:|---|
| spec17/deepsjeng_88772            | 0.590 | 0.215 | T7 |
| spec17/x264_pass1_17232           | 0.205 | 0.484 | T3 |
| spec17/exchange2_101257           | 0.169 | 0.000 | T7 |
| spec17/leela_60438                | 0.133 | 0.304 | T3 |
| spec17/xalancbmk_6102             | 0.080 | 0.446 | T7 |

Workloads present in both `->TAGE` directions: 4 of 5 in
`ITTAGE->TAGE` (deepsjeng, x264_pass1, exchange2, leela, xalancbmk) are
also in `MBTB->TAGE`. `omnetpp_21279` is `MBTB->TAGE` only (ITTAGE T2/T3
already saturated per 2026-07-01 A20 heatmap-context).

### MBTB->ITTAGE (3 targets, fails gate)
| workload | donee | donor | note |
|---|---:|---:|---|
| spec17/xz_combined_26239 | 0.0315 | 0.025 | one SPEC17 hit |
| cbp_int/int_14_trace     | 1.5e-05 | 0.131 | absolute magnitude 4 orders below SPEC |
| cbp_int/int_13_trace     | 5.0e-06 | 0.091 | same |

### ITTAGE->MBTB (2 targets, fails gate)
| workload | donee | donor | note |
|---|---:|---:|---|
| spec17/cam4_118108       | 7.4e-04 | 0.381 | FP workload |
| spec17/gcc_ref32_O5_889  | 6.8e-04 | 0.361 | one gcc variant survives |

## A20 pinned-donor decision re-verified

Handoff (2026-07-09) flagged one reversal from Stage 2: on `gcc_pp_O2_1869`
MBTB way3 was not stranded (all 8 ways within 1.74M-1.89M hits), and
ITTAGE T4 was the coldest table (not the T2+T3 pinned by A20 heatmap).
This looked at first like a pin re-eval trigger.

Cross-check against `runs/phase0_ittage_heatmap/main_W100K/`:

- A20 aggregate (10 workloads averaged) picked T2+T3 as pinned donors:
  T0 use=68721 > T4=48738 > T2=30574 > T3=29470 > T1=29009. T0/T1 are
  R3-latency excluded; T4 mispred_per_use=0.069 too high; leaving T2+T3
  as the routing-viable pair.
- `gcc_pp_O2_1869` single point: T0=53163, T1=24126, T2=20783, T3=17943,
  T4=11652; min(T2,T3)/mean = 17943/25533 = 0.70. T2/T3 are cooler than
  average but not "very cold". T4 is coldest but is not a routing option.

This is exactly the C1 (aggregate design-time pin) vs C2 (runtime
per-phase verify) separation the taxonomy §3.2 codifies: A20 chose pins
that aggregate hold, F1' donor_b_b tests whether each workload's each
phase really strands them. Codex's verdict admitted gcc_pp_O2 as a target
only because donor<1.0 was loose; v2 gate (donor<0.5) reintroduces the
correct filter and gcc_pp_O2 drops out of every direction. Conclusion:
**no re-pin necessary**; the pin is correct, the earlier gate was wrong.

`exchange2_101257` and `mcf_17135` have zero ITTAGE updates over 20M
instructions (heatmap CSV row all 0), so any ITTAGE-side direction on
these workloads is undefined; codex verdict incorrectly listed
`exchange2` on `ITTAGE->TAGE`. v2 gate keeps it (donee side is TAGE, not
ITTAGE) which is correct — its ITTAGE-donor score is 0 (no updates
therefore `min/mean` degenerate to 0), which is <0.5, so it passes.

## Recommendation

- Advance `MBTB->TAGE` and `ITTAGE->TAGE` to Stage 3 offline β sim as
  primary Stage 3 target directions. Combined 8-workload superset:
  omnetpp_21279, deepsjeng_88772, x264_pass1_17232, exchange2_101257,
  leela_60438, xalancbmk_6102, cbp_int/int_13_trace (MBTB->TAGE only),
  plus overlap in ITTAGE->TAGE for 5/5.
- Move `MBTB<->ITTAGE` (both directions) to open item (taxonomy §7):
  neither direction has >= 5 real targets on current SPEC17+CBP data;
  donee-side b_c on ITTAGE-as-donee is 100x smaller than on TAGE-as-
  donee because ITTAGE table capacity is ~5x smaller and access
  intensity ~10x lower for the same workload. This is a systematic
  measurement floor, not a workload set issue.
- Open PENDING-24 for the SWAY champsim binary regression. Recovering
  the 30 champsim srv_* runs would let MBTB<->ITTAGE be re-evaluated
  under a family whose ITTAGE pressure profile matches SPEC's TAGE
  pressure profile.

## Data provenance

- Input: `runs/stage2_baseline_expanded/w0p_per_workload.csv` (40 rows,
  written by codex `scripts/analyze_w0p.py` 2026-07-03)
- Gate: `scripts/analyze_w0p_v2.py` (2026-07-09)
- Outputs: `w0p_per_workload_v2.csv`, `w0p_per_direction_summary_v2.tsv`,
  `w0p_target_workloads_v2.tsv`
- Cross-refs: `docs/SWAY_METRIC_TAXONOMY.md` §7, `PENDING.md` PENDING-24,
  memory files `sway-champsim-regression`, `sway-rebase-validation-gap`.
