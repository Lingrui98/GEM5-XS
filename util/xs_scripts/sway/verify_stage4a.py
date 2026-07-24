#!/usr/bin/env python3
"""Stage 4A gate verification: baseline-equivalence + live-vs-replay smoke.

Gate 1 (baseline-equivalence): whole-file diff of the new enableSwayRealloc=False
x264_seek stats.txt against the retained Stage 3 run, excluding the five host
fields (hostSeconds/hostMemory/hostInstRate/hostOpRate/hostTickRate). Empty diff
=> bit-exact baseline preserved by the beta wiring.

Gate 2 (live-vs-replay smoke): live fire counts (from sway_realloc_events.csv of
the two theta=1.5 SWAY smoke runs) vs the open-loop replay fire counts at theta
1.5. Advisory band is +-10%; for small counts, report the delta and flag if live
fires are ~0 (wiring mismatch, STOP condition) or wildly off.
"""
from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path
from collections import defaultdict

HOST_FIELDS = {
    "hostSeconds", "hostMemory", "hostInstRate", "hostOpRate", "hostTickRate",
}


def strip_host(path: Path) -> list[str]:
    out = []
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            name = line.split(None, 1)[0] if line.strip() else ""
            if name in HOST_FIELDS:
                continue
            out.append(line)
    return out


def live_fire_counts(events_path: Path) -> dict:
    counts = {"total": 0, "MBTB->TAGE": 0, "ITTAGE->TAGE": 0}
    if not events_path.is_file():
        return counts
    with events_path.open(newline="") as f:
        for r in csv.DictReader(f):
            counts["total"] += 1
            d = r.get("direction", "")
            if d in ("MBTB->TAGE", "ITTAGE->TAGE"):
                counts[d] += 1
    return counts


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--new-baseline-stats", type=Path, required=True)
    p.add_argument("--retained-stats", type=Path, required=True,
                   help="retained Stage 3 x264_seek stats.txt")
    p.add_argument("--gcc-events", type=Path, required=True,
                   help="sway_realloc_events.csv for gcc_pp_O2_1869 smoke run")
    p.add_argument("--mcf-events", type=Path, required=True,
                   help="sway_realloc_events.csv for mcf_17135 smoke run")
    p.add_argument("--gcc-mbtb-expected", type=int, default=4)
    p.add_argument("--gcc-ittage-expected", type=int, default=4)
    p.add_argument("--mcf-mbtb-expected", type=int, default=4)
    p.add_argument("--mcf-ittage-expected", type=int, default=0)
    args = p.parse_args(argv)

    ok = True

    print("=== Gate 1: baseline equivalence (x264_seek) ===")
    if not args.new_baseline_stats.is_file():
        print(f"MISSING: {args.new_baseline_stats}")
        ok = False
    elif not args.retained_stats.is_file():
        print(f"MISSING retained: {args.retained_stats}")
        ok = False
    else:
        new = strip_host(args.new_baseline_stats)
        old = strip_host(args.retained_stats)
        if new == old:
            print(f"PASS: whole-file diff empty (excl 5 host fields); "
                  f"{len(new)} lines each")
        else:
            n = min(len(new), len(old))
            diffs = [i for i in range(n) if new[i] != old[i]]
            print(f"FAIL: {len(diffs)} differing lines (new={len(new)}, "
                  f"retained={len(old)}); first diff at line "
                  f"{diffs[0]+1 if diffs else '?'}")
            for i in diffs[:5]:
                print(f"  -{old[i].rstrip()}")
                print(f"  +{new[i].rstrip()}")
            ok = False

    print("\n=== Gate 2: live-vs-replay smoke (theta=1.5) ===")
    for name, ev, em, ei in [
        ("gcc_pp_O2_1869", args.gcc_events, args.gcc_mbtb_expected,
         args.gcc_ittage_expected),
        ("mcf_17135", args.mcf_events, args.mcf_mbtb_expected,
         args.mcf_ittage_expected),
    ]:
        c = live_fire_counts(ev)
        if not ev.is_file():
            print(f"{name}: MISSING events file {ev}")
            ok = False
            continue
        print(f"{name}: live fires total={c['total']} "
              f"MBTB->TAGE={c['MBTB->TAGE']} (replay {em}), "
              f"ITTAGE->TAGE={c['ITTAGE->TAGE']} (replay {ei})")
        if c["MBTB->TAGE"] == 0 and c["ITTAGE->TAGE"] == 0 and (em or ei):
            print(f"  STOP: live fires ~0 where replay fired => wiring mismatch")
            ok = False
    print(f"\n=== 4A gate verdict: {'PASS' if ok else 'FAIL'} ===")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
