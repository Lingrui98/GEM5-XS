#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
from pathlib import Path

from summarize_regression import count_trace, gate_result


def write_status(path: Path, passed: bool) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {"status": "passed" if passed else "failed"}
    path.write_text(json.dumps(payload) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize a trace regression gate and emit task status")
    parser.add_argument("--trace-root", required=True)
    parser.add_argument("--expected", type=int, required=True)
    parser.add_argument("--out-status", required=True)
    parser.add_argument("--out-summary")
    args = parser.parse_args()

    trace_root = Path(args.trace_root).resolve()
    counts, failures = count_trace(trace_root)
    result, detail = gate_result(
        counts,
        expected_total=args.expected,
        completed_key="completed",
        fail_keys=["running", "aborted", "missing_trace", "launch_failed", "pending", "missing_stats"],
    )

    if args.out_summary:
        summary_lines = [
            f"Work root: {trace_root}",
            f"Total tasks:     {counts['total']}",
            f"  completed:     {counts['completed']}",
            f"  running:       {counts['running']}",
            f"  aborted:       {counts['aborted']}",
            f"  missing_trace: {counts['missing_trace']}",
            f"  launch_failed: {counts['launch_failed']}",
            f"  pending:       {counts['pending']}",
            f"Completed without stats (possible failure): {counts['missing_stats']}",
        ]
        if failures:
            summary_lines.append("Failures:")
            summary_lines.extend(f"  - {failure}" for failure in failures)
        Path(args.out_summary).write_text("\n".join(summary_lines) + "\n", encoding="utf-8")

    passed = result == "PASS"
    write_status(Path(args.out_status), passed)
    print(f"trace_gate: {result} ({detail})")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
