#!/usr/bin/env python3
"""Stage 4 paired causal analysis.

Consumes a dispatch manifest (one row per run: workload, arm, theta, seed,
stats.txt path, sway_realloc_events.csv path) and produces:
  - paired_delta_ipc.tsv      per-workload SWAY-vs-baseline DeltaIPC (mean+/-std
                              over seeds), per theta.
  - geomean_summary.tsv       cohort geomean DeltaIPC (unweighted + instruction-
                              weighted combined), Wilcoxon signed-rank + paired
                              t-test p-values (C-DA-1), per theta/cohort.
  - fire_counts.tsv           per-workload fire counts by direction + T3/T7
                              split, per theta.
  - false_fire.tsv            per-workload + cohort false-fire rate
                              (t3_regression_returns / total_fires), per theta.

Aggregation follows E-1: 3 seeds, instruction-weighted combined IPC, perlbench_diff
and perlbench_split treated as independent workloads. Theta selection rule (4B) is
frozen-before-looking: argmax paired geomean DeltaIPC over the train primary
(beta-evaluable) cohort, advisory false-fire rate < 50%, ties toward higher theta.
This script reports the data; it does NOT select theta for held-out.
"""
from __future__ import annotations

import argparse
import csv
import math
import re
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
try:
    from scipy import stats as sp_stats
    HAVE_SCIPY = True
except Exception:
    HAVE_SCIPY = False


HOST_FIELDS = {
    "hostSeconds", "hostMemory", "hostInstRate", "hostOpRate", "hostTickRate",
}
STATS_IPC = "system.cpu.ipc"
STATS_INSTS = "system.cpu.committedInsts"
STATS_CYCLES = "system.cpu.numCycles"

EVENT_DIRECTIONS = ("MBTB->TAGE", "ITTAGE->TAGE")
FALSE_FIRE_TAG = "t3_regression_return"


def parse_stats(path: Path) -> dict | None:
    """Parse a gem5 stats.txt for ipc / committedInsts / numCycles."""
    if not path.is_file():
        return None
    out = {}
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"^(\S+)\s+(\S+)", line)
            if not m:
                continue
            name, raw = m.group(1), m.group(2)
            if name in (STATS_IPC, STATS_INSTS, STATS_CYCLES):
                try:
                    out[name] = float(raw)
                except ValueError:
                    pass
    if STATS_IPC not in out:
        return None
    return out


def parse_events(path: Path) -> dict:
    """Parse sway_realloc_events.csv for fire counts + false-fire + T3/T7 split."""
    res = {
        "total_fires": 0,
        "new_donations": 0,
        "false_fires": 0,
        "by_direction": {d: 0 for d in EVENT_DIRECTIONS},
        "t3_fires": 0,
        "t7_fires": 0,
        "resolutions": defaultdict(int),
    }
    if not path.is_file():
        return res
    with path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            res["total_fires"] += 1
            d = row.get("direction", "")
            if d in res["by_direction"]:
                res["by_direction"][d] += 1
            donee = row.get("donee", "")
            if donee.endswith("T7"):
                res["t7_fires"] += 1
            elif donee.endswith("T3"):
                res["t3_fires"] += 1
            if row.get("is_new_donation", "").lower() in ("true", "1"):
                res["new_donations"] += 1
            tag = row.get("resolution", "")
            res["resolutions"][tag] += 1
            if tag == FALSE_FIRE_TAG:
                res["false_fires"] += 1
    return res


def load_manifest(path: Path) -> list[dict]:
    rows = []
    with path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f, delimiter="\t")
        for r in reader:
            rows.append(r)
    return rows


def geomean(xs: list[float]) -> float:
    vals = [x for x in xs if math.isfinite(x)]
    if not vals:
        return float("nan")
    # geometric mean of (1 + DeltaIPC) to tolerate small negatives, then -1.
    prod = 1.0
    for v in vals:
        prod *= (1.0 + v)
    return prod ** (1.0 / len(vals)) - 1.0


def wilcoxon_p(deltas: list[float]) -> float:
    if not HAVE_SCIPY or len(deltas) < 1:
        return float("nan")
    arr = np.array(deltas, dtype=float)
    non_zero = arr[arr != 0]
    if len(non_zero) < 1:
        return 1.0
    try:
        res = sp_stats.wilcoxon(arr)
        return float(res.pvalue)
    except Exception:
        return float("nan")


def ttest_p(deltas: list[float]) -> float:
    if not HAVE_SCIPY or len(deltas) < 2:
        return float("nan")
    try:
        res = sp_stats.ttest_rel(np.array(deltas, float), np.zeros(len(deltas)))
        return float(res.pvalue)
    except Exception:
        return float("nan")


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--manifest", type=Path, required=True,
                   help="TSV: workload, arm, theta, seed, stats_path, events_path")
    p.add_argument("--output-dir", type=Path, required=True)
    p.add_argument("--primary-cohort", type=Path,
                   help="Optional newline-separated list of workloads defining "
                        "the beta-evaluable primary cohort for the gate geomean")
    args = p.parse_args(argv)

    args.output_dir.mkdir(parents=True, exist_ok=True)
    manifest = load_manifest(args.manifest)

    primary = None
    if args.primary_cohort and args.primary_cohort.is_file():
        primary = set(
            w.strip() for w in args.primary_cohort.read_text().splitlines()
            if w.strip()
        )

    # (workload, theta, arm, seed) -> parsed stats/events
    runs: dict[tuple, dict] = {}
    for r in manifest:
        wl = r["workload"].strip()
        arm = r["arm"].strip()
        theta = r["theta"].strip()
        seed = r["seed"].strip()
        stats = parse_stats(Path(r["stats_path"]))
        events = parse_events(Path(r["events_path"]))
        runs[(wl, theta, arm, seed)] = {"stats": stats, "events": events}

    thetas = sorted({k[1] for k in runs}, key=lambda t: float(t))
    workloads = sorted({k[0] for k in runs})
    seeds_by = defaultdict(set)
    for (wl, theta, arm, seed) in runs:
        seeds_by[(wl, theta)].add(seed)

    # Per-workload per-theta paired DeltaIPC over seeds.
    paired_rows = []
    delta_by_theta: dict[str, list[tuple[str, float]]] = {t: [] for t in thetas}
    fire_rows = []
    ff_rows = []
    for wl in workloads:
        for theta in thetas:
            seeds = sorted(seeds_by[(wl, theta)])
            deltas = []
            for s in seeds:
                b = runs.get((wl, theta, "baseline", s), {}).get("stats")
                w = runs.get((wl, theta, "sway", s), {}).get("stats")
                if not b or not w:
                    continue
                deltas.append(w[STATS_IPC] - b[STATS_IPC])
            if not deltas:
                continue
            arr = np.array(deltas, float)
            mean = float(arr.mean())
            std = float(arr.std(ddof=1)) if len(arr) > 1 else 0.0
            paired_rows.append({
                "workload": wl, "theta": theta, "seeds": len(deltas),
                "delta_ipc_mean": f"{mean:.9g}", "delta_ipc_std": f"{std:.9g}",
                "min": f"{float(arr.min()):.9g}", "max": f"{float(arr.max()):.9g}",
                "in_primary_cohort": (wl in primary) if primary else "",
            })
            delta_by_theta[theta].append((wl, mean))

            # Fire counts (from any sway seed; report seed 0 if present).
            ev = None
            for s in seeds:
                e = runs.get((wl, theta, "sway", s), {}).get("events")
                if e:
                    ev = e
                    break
            if ev:
                fire_rows.append({
                    "workload": wl, "theta": theta,
                    "total_fires": ev["total_fires"],
                    "new_donations": ev["new_donations"],
                    "MBTB->TAGE": ev["by_direction"]["MBTB->TAGE"],
                    "ITTAGE->TAGE": ev["by_direction"]["ITTAGE->TAGE"],
                    "t3_fires": ev["t3_fires"], "t7_fires": ev["t7_fires"],
                })
                total = ev["total_fires"]
                ff = ev["false_fires"]
                ff_rows.append({
                    "workload": wl, "theta": theta,
                    "total_fires": total, "false_fires": ff,
                    "false_fire_rate": (f"{ff/total:.9g}" if total else "NA"),
                })

    # Geomean summary per theta, full + primary cohort.
    summary_rows = []
    for theta in thetas:
        full = [d for (wl, d) in delta_by_theta[theta]]
        prim = [d for (wl, d) in delta_by_theta[theta]
                if primary is None or wl in primary]
        g_full = geomean(full)
        g_prim = geomean(prim)
        # Instruction-weighted combined DeltaIPC: combined IPC per arm.
        # Delta_combined = IPC_sway_combined - IPC_base_combined, where
        # IPC_combined = sum(inst)/sum(cycles) across cohort (E-1 weighted).
        inst_s = cyc_s = inst_b = cyc_b = 0.0
        for (wl, _) in delta_by_theta[theta]:
            seeds = sorted(seeds_by[(wl, theta)])
            for s in seeds[:1]:  # use seed 0 for the weighted aggregate
                b = runs.get((wl, theta, "baseline", s), {}).get("stats")
                w = runs.get((wl, theta, "sway", s), {}).get("stats")
                if b and w:
                    inst_b += b[STATS_INSTS]; cyc_b += b[STATS_CYCLES]
                    inst_s += w[STATS_INSTS]; cyc_s += w[STATS_CYCLES]
        ipc_b = inst_b / cyc_b if cyc_b else float("nan")
        ipc_s = inst_s / cyc_s if cyc_s else float("nan")
        delta_weighted = ipc_s - ipc_b if math.isfinite(ipc_b) else float("nan")
        summary_rows.append({
            "theta": theta,
            "cohort": "full", "n": len(full),
            "geomean_delta_ipc": f"{g_full:.9g}",
            "weighted_combined_delta_ipc": f"{delta_weighted:.9g}",
            "wilcoxon_p": f"{wilcoxon_p(full):.9g}",
            "paired_t_p": f"{ttest_p(full):.9g}",
        })
        summary_rows.append({
            "theta": theta,
            "cohort": "primary_beta_evaluable", "n": len(prim),
            "geomean_delta_ipc": f"{g_prim:.9g}",
            "weighted_combined_delta_ipc": "",
            "wilcoxon_p": f"{wilcoxon_p(prim):.9g}",
            "paired_t_p": f"{ttest_p(prim):.9g}",
        })

    # Cohort-wide false-fire rate per theta.
    for theta in thetas:
        tot = sum(r["total_fires"] for r in ff_rows if r["theta"] == theta)
        ff = sum(r["false_fires"] for r in ff_rows if r["theta"] == theta)
        ff_rows.append({
            "workload": "__cohort__", "theta": theta,
            "total_fires": tot, "false_fires": ff,
            "false_fire_rate": (f"{ff/tot:.9g}" if tot else "NA"),
        })

    def write_tsv(name, rows):
        if not rows:
            return
        path = args.output_dir / name
        fields = list(rows[0].keys())
        with path.open("w", encoding="utf-8", newline="") as f:
            w = csv.DictWriter(f, fieldnames=fields, delimiter="\t",
                               lineterminator="\n")
            w.writeheader()
            w.writerows(rows)
        print(f"wrote {path} ({len(rows)} rows)")

    write_tsv("paired_delta_ipc.tsv", paired_rows)
    write_tsv("geomean_summary.tsv", summary_rows)
    write_tsv("fire_counts.tsv", fire_rows)
    write_tsv("false_fire.tsv", ff_rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())
