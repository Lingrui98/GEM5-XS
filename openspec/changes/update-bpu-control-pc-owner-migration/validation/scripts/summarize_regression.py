#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path


def load_task_status(run_root: Path, task: str) -> tuple[str, str]:
    status_path = run_root / task / "status.json"
    if not status_path.exists():
        return "BLOCKED", "missing status.json"
    try:
        data = json.loads(status_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        return "FAIL", f"invalid status.json: {exc}"
    status = str(data.get("status", "unknown"))
    if status == "passed":
        return "PASS", "task status passed"
    if status == "blocked":
        return "BLOCKED", "task status blocked"
    return "FAIL", f"task status {status}"


def count_fs(root: Path) -> tuple[dict[str, int], list[str]]:
    counts = {"total": 0, "completed": 0, "abort": 0, "running": 0, "pending": 0, "missing_stats": 0}
    failures: list[str] = []
    for task_dir in sorted(p for p in root.iterdir() if p.is_dir()):
        counts["total"] += 1
        if (task_dir / "completed").exists():
            counts["completed"] += 1
            if not (task_dir / "stats.txt").exists():
                counts["missing_stats"] += 1
                failures.append(f"{task_dir.name}: completed without stats.txt")
        elif (task_dir / "abort").exists():
            counts["abort"] += 1
            failures.append(f"{task_dir.name}: abort")
        elif (task_dir / "running").exists():
            counts["running"] += 1
            failures.append(f"{task_dir.name}: still running")
        else:
            counts["pending"] += 1
            failures.append(f"{task_dir.name}: pending")
    return counts, failures


def count_trace(root: Path) -> tuple[dict[str, int], list[str]]:
    counts = {
        "total": 0,
        "completed": 0,
        "running": 0,
        "aborted": 0,
        "missing_trace": 0,
        "launch_failed": 0,
        "pending": 0,
        "missing_stats": 0,
    }
    failures: list[str] = []
    for task_dir in sorted(p for p in root.iterdir() if p.is_dir()):
        counts["total"] += 1
        state = "pending"
        if (task_dir / "missing_trace").exists():
            counts["missing_trace"] += 1
            state = "missing_trace"
        elif (task_dir / "launch_failed").exists():
            counts["launch_failed"] += 1
            state = "launch_failed"
        elif (task_dir / "abort").exists():
            counts["aborted"] += 1
            state = "aborted"
        elif (task_dir / "running").exists():
            counts["running"] += 1
            state = "running"
        elif (task_dir / "completed").exists():
            counts["completed"] += 1
            state = "completed"
        else:
            counts["pending"] += 1

        if state == "completed":
            if not (task_dir / "stats.txt").exists():
                counts["missing_stats"] += 1
                failures.append(f"{task_dir.name}: completed without stats.txt")
        elif state != "pending":
            failures.append(f"{task_dir.name}: {state}")
        else:
            failures.append(f"{task_dir.name}: pending")
    return counts, failures


def gate_result(
    counts: dict[str, int],
    expected_total: int,
    completed_key: str,
    fail_keys: list[str],
) -> tuple[str, str]:
    ok = counts.get("total", 0) == expected_total and counts.get(completed_key, 0) == expected_total
    ok = ok and all(counts.get(key, 0) == 0 for key in fail_keys)
    result = "PASS" if ok else "FAIL"
    detail = ", ".join(f"{k}={v}" for k, v in counts.items())
    return result, detail


def write_csv(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=["item", "path", "result", "detail"])
        writer.writeheader()
        writer.writerows(rows)


def write_report(
    path: Path,
    run_root: Path,
    task_rows: list[dict[str, str]],
    fs_root: Path,
    fs_counts: dict[str, int],
    fs_failures: list[str],
    trace_root: Path,
    trace_counts: dict[str, int],
    trace_failures: list[str],
) -> None:
    lines = [
        "# Control-PC Tail-Halfword Regression Report",
        "",
        f"- Run root: `{run_root}`",
        f"- FS root: `{fs_root}`",
        f"- Trace root: `{trace_root}`",
        "",
        "## Task Status",
        "",
        "| Item | Result | Detail |",
        "| --- | --- | --- |",
    ]
    for row in task_rows:
        lines.append(f"| {row['item']} | {row['result']} | {row['detail']} |")

    lines.extend(
        [
            "",
            "## FS Gate",
            "",
            *(f"- {k}: {v}" for k, v in fs_counts.items()),
            "",
            "### FS Failures",
            "",
        ]
    )
    lines.extend([f"- {item}" for item in fs_failures] or ["- none"])

    lines.extend(
        [
            "",
            "## Trace Gate",
            "",
            *(f"- {k}: {v}" for k, v in trace_counts.items()),
            "",
            "### Trace Failures",
            "",
        ]
    )
    lines.extend([f"- {item}" for item in trace_failures] or ["- none"])

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize control-PC regression runs")
    parser.add_argument("--run-root", required=True)
    parser.add_argument("--fs-root", required=True)
    parser.add_argument("--trace-root", required=True)
    parser.add_argument("--expected-fs", type=int, required=True)
    parser.add_argument("--expected-trace", type=int, required=True)
    parser.add_argument("--task", dest="tasks", action="append", default=[])
    parser.add_argument("--out-final-status", required=True)
    parser.add_argument("--out-report", required=True)
    args = parser.parse_args()

    run_root = Path(args.run_root).resolve()
    fs_root = Path(args.fs_root).resolve()
    trace_root = Path(args.trace_root).resolve()

    rows: list[dict[str, str]] = []
    overall_ok = True

    for task in args.tasks:
        result, detail = load_task_status(run_root, task)
        rows.append({"item": task, "path": str(run_root / task), "result": result, "detail": detail})
        overall_ok = overall_ok and result == "PASS"

    fs_counts, fs_failures = count_fs(fs_root)
    fs_result, fs_detail = gate_result(
        fs_counts,
        expected_total=args.expected_fs,
        completed_key="completed",
        fail_keys=["abort", "running", "pending", "missing_stats"],
    )
    rows.append({"item": "fs_gate", "path": str(fs_root), "result": fs_result, "detail": fs_detail})
    overall_ok = overall_ok and fs_result == "PASS"

    trace_counts, trace_failures = count_trace(trace_root)
    trace_result, trace_detail = gate_result(
        trace_counts,
        expected_total=args.expected_trace,
        completed_key="completed",
        fail_keys=["running", "aborted", "missing_trace", "launch_failed", "pending", "missing_stats"],
    )
    rows.append({"item": "trace_gate", "path": str(trace_root), "result": trace_result, "detail": trace_detail})
    overall_ok = overall_ok and trace_result == "PASS"

    rows.append(
        {
            "item": "overall",
            "path": str(run_root),
            "result": "PASS" if overall_ok else "FAIL",
            "detail": "all required tasks and gates passed" if overall_ok else "see gate/task failures above",
        }
    )

    write_csv(Path(args.out_final_status), rows)
    write_report(
        Path(args.out_report),
        run_root,
        rows,
        fs_root,
        fs_counts,
        fs_failures,
        trace_root,
        trace_counts,
        trace_failures,
    )

    for row in rows:
        print(f"{row['item']}: {row['result']} ({row['detail']})")
    return 0 if overall_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
