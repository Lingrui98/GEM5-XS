#!/usr/bin/env python3
"""Prepare the fixed Stage 3 calibration workload set.

The generator deliberately does not launch simulations.  It converts the
audited Stage 2 manifest into a small, reproducible Stage 3 input bundle while
keeping instruction-window values in the worklists (not backend configs).
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import os
from dataclasses import dataclass
from pathlib import Path
import tempfile
from typing import Iterable, Mapping, Sequence


SOURCE_MANIFEST = Path(
    "/nfs/home/goulingrui/project/GEM5-sway/runs/"
    "stage2_baseline_expanded/workload_manifest.tsv"
)
ALLOWED_RUN_ROOT = Path("/nfs/home/goulingrui/project/GEM5-sway/runs")
SPEC_ROOT = Path(
    "/nfs/home/share/zyy/"
    "spec17-rv64gcb-O3-20m-gcc12.2.0-mix-with-special_wrf/"
    "checkpoint-0-0-0"
)
CBP_ROOT = Path("/nfs/home/share/glr/cbp_traces")

WARMUP_INSTS = 20_000_000
SAMPLE_INSTS = 20_000_000
TOTAL_INSTS = WARMUP_INSTS + SAMPLE_INSTS
MIN_CBP_INSTS = TOTAL_INSTS

EXPECTED_SPEC_COUNT = 36
EXPECTED_CBP_IDS = frozenset(
    {"int_12_trace", "int_13_trace", "int_15_trace"}
)
LEGACY_V2_SEEDS = frozenset(
    {
        "omnetpp_21279",
        "deepsjeng_88772",
        "x264_pass1_17232",
        "exchange2_101257",
        "leela_60438",
        "xalancbmk_6102",
        "int_13_trace",
    }
)

MANIFEST_FIELDS = (
    "family",
    "id",
    "source_path",
    "phase_stream_path",
    "warmup_insts",
    "sample_insts",
    "total_insts",
    "legacy_v2_seed",
)


@dataclass(frozen=True)
class SourceRow:
    family: str
    workload_id: str
    source_path: Path
    inst_count: int
    trace_format: str


@dataclass(frozen=True)
class OutputFile:
    relative_path: Path
    content: str


@dataclass(frozen=True)
class PreparationPlan:
    run_root: Path
    spec_rows: tuple[SourceRow, ...]
    cbp_rows: tuple[SourceRow, ...]
    files: tuple[OutputFile, ...]


def _resolved_within(path: Path, root: Path, label: str) -> Path:
    """Resolve *path* and reject lexical or symlink escapes from *root*."""

    resolved = path.expanduser().resolve(strict=False)
    resolved_root = root.expanduser().resolve(strict=False)
    if resolved != resolved_root and resolved_root not in resolved.parents:
        raise ValueError(f"{label} is outside allowed root {resolved_root}: {resolved}")
    return resolved


def _parse_source_manifest(path: Path) -> list[SourceRow]:
    required = {"family", "id", "source_path", "inst_count", "format"}
    data_lines: list[str] = []
    with path.open("r", encoding="utf-8", newline="") as source:
        for line in source:
            if not line.startswith("#") and line.strip():
                data_lines.append(line)

    reader = csv.DictReader(data_lines, delimiter="\t")
    missing = required.difference(reader.fieldnames or ())
    if missing:
        raise ValueError(
            f"source manifest is missing columns: {', '.join(sorted(missing))}"
        )

    rows: list[SourceRow] = []
    seen: set[tuple[str, str]] = set()
    for line_no, raw in enumerate(reader, start=2):
        key = (raw["family"], raw["id"])
        if key in seen:
            raise ValueError(f"duplicate workload at data line {line_no}: {key}")
        seen.add(key)
        try:
            inst_count = int(raw["inst_count"])
        except ValueError as error:
            raise ValueError(
                f"invalid inst_count at data line {line_no}: {raw['inst_count']!r}"
            ) from error
        if inst_count < 0:
            raise ValueError(f"negative inst_count at data line {line_no}")
        rows.append(
            SourceRow(
                family=raw["family"],
                workload_id=raw["id"],
                source_path=Path(raw["source_path"]),
                inst_count=inst_count,
                trace_format=raw["format"],
            )
        )
    return rows


def _json_config(env: Mapping[str, str]) -> str:
    forbidden = {"XS_MAX_INSTS", "XS_WARMUP_INSTS_NO_SWITCH"}
    overlap = forbidden.intersection(env)
    if overlap:
        raise ValueError(
            f"backend config must not set scheduler-owned values: {sorted(overlap)}"
        )
    return json.dumps({"env": dict(env), "args": []}, indent=2) + "\n"


def _relative_source(row: SourceRow, root: Path, strip_gz: bool) -> str:
    resolved = _resolved_within(
        row.source_path, root, f"source_path for {row.workload_id}"
    )
    relative = resolved.relative_to(root.resolve(strict=False)).as_posix()
    if strip_gz:
        if not relative.endswith(".gz"):
            raise ValueError(
                f"CBP source for {row.workload_id} must end in .gz: {relative}"
            )
        relative = relative[:-3]
    return relative


def _worklist(rows: Iterable[SourceRow], root: Path, strip_gz: bool) -> str:
    lines = ["# name path skip fw dw sample"]
    for row in rows:
        relative = _relative_source(row, root, strip_gz)
        lines.append(
            f"{row.workload_id}\t{relative}\t0\t{WARMUP_INSTS}"
            f"\t0\t{SAMPLE_INSTS}"
        )
    return "\n".join(lines) + "\n"


def _output_manifest(
    run_root: Path, rows: Sequence[SourceRow]
) -> str:
    output = io.StringIO(newline="")
    writer = csv.DictWriter(
        output,
        fieldnames=MANIFEST_FIELDS,
        delimiter="\t",
        lineterminator="\n",
    )
    writer.writeheader()
    for row in rows:
        phase_stream = (
            run_root / row.family / row.workload_id / "sway_phase_bc_raw.csv"
        )
        _resolved_within(
            phase_stream, run_root, f"phase_stream_path for {row.workload_id}"
        )
        writer.writerow(
            {
                "family": row.family,
                "id": row.workload_id,
                "source_path": str(row.source_path.resolve(strict=False)),
                "phase_stream_path": str(phase_stream),
                "warmup_insts": WARMUP_INSTS,
                "sample_insts": SAMPLE_INSTS,
                "total_insts": TOTAL_INSTS,
                "legacy_v2_seed": (
                    "true" if row.workload_id in LEGACY_V2_SEEDS else "false"
                ),
            }
        )
    return output.getvalue()


def build_plan(
    source_manifest: Path,
    run_root: Path,
    *,
    allowed_run_root: Path = ALLOWED_RUN_ROOT,
    spec_root: Path = SPEC_ROOT,
    cbp_root: Path = CBP_ROOT,
    enforce_production_set: bool = True,
) -> PreparationPlan:
    """Validate inputs and return all output bytes without writing files."""

    resolved_run_root = _resolved_within(run_root, allowed_run_root, "run_root")
    rows = _parse_source_manifest(source_manifest)
    spec_rows = tuple(sorted(
        (row for row in rows if row.family == "spec17"),
        key=lambda row: row.workload_id,
    ))
    cbp_rows = tuple(sorted(
        (
            row
            for row in rows
            if row.family == "cbp_int" and row.inst_count >= MIN_CBP_INSTS
        ),
        key=lambda row: row.workload_id,
    ))

    for row in spec_rows:
        if row.trace_format != "cpt":
            raise ValueError(
                f"SPEC17 workload {row.workload_id} has format {row.trace_format!r}"
            )
        _relative_source(row, spec_root, strip_gz=False)
    for row in cbp_rows:
        if row.trace_format != "cbp2025":
            raise ValueError(
                f"CBP workload {row.workload_id} has format {row.trace_format!r}"
            )
        _relative_source(row, cbp_root, strip_gz=True)

    if enforce_production_set:
        if len(spec_rows) != EXPECTED_SPEC_COUNT:
            raise ValueError(
                f"expected {EXPECTED_SPEC_COUNT} SPEC17 workloads, got {len(spec_rows)}"
            )
        cbp_ids = {row.workload_id for row in cbp_rows}
        if cbp_ids != EXPECTED_CBP_IDS:
            raise ValueError(
                "expected CBP calibration set "
                f"{sorted(EXPECTED_CBP_IDS)}, got {sorted(cbp_ids)}"
            )

    gem5_home = "/nfs/home/goulingrui/project/GEM5-sway"
    common_env = {
        "GEM5_HOME": gem5_home,
        "GEM5_BUILD_TYPE": "opt",
        "XS_PHASE_SIZE_BY_INST": "100000",
    }
    spec_env = {
        **common_env,
        "GCBV_REF_SO": (
            "/nfs/home/share/gem5_ci/ref/normal/"
            "riscv64-nemu-interpreter-so"
        ),
        "GCB_RESTORER": (
            "/nfs/home/share/gem5_ci/tools/normal-gcb-restorer.bin"
        ),
        "XS_MEM_SIZE": "128GB",
    }
    cbp_env = {**common_env, "TRACE_FORMAT": "cbp2025"}
    selected_rows = (*spec_rows, *cbp_rows)
    files = (
        OutputFile(Path("configs/spec17_cpt.json"), _json_config(spec_env)),
        OutputFile(Path("configs/cbp_trace.json"), _json_config(cbp_env)),
        OutputFile(
            Path("worklists/spec17_cal.lst"),
            _worklist(spec_rows, spec_root, strip_gz=False),
        ),
        OutputFile(
            Path("worklists/cbp_cal.lst"),
            _worklist(cbp_rows, cbp_root, strip_gz=True),
        ),
        OutputFile(
            Path("workload_manifest.tsv"),
            _output_manifest(resolved_run_root, selected_rows),
        ),
    )
    return PreparationPlan(resolved_run_root, spec_rows, cbp_rows, files)


def _atomic_write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        dir=path.parent, prefix=f".{path.name}.", suffix=".tmp"
    )
    temporary_path = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="") as output:
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_path, path)
    except BaseException:
        temporary_path.unlink(missing_ok=True)
        raise


def write_plan(plan: PreparationPlan) -> None:
    for output_file in plan.files:
        destination = _resolved_within(
            plan.run_root / output_file.relative_path,
            plan.run_root,
            f"output {output_file.relative_path}",
        )
        _atomic_write_text(destination, output_file.content)


def _summary(plan: PreparationPlan, dry_run: bool) -> dict[str, object]:
    return {
        "dry_run": dry_run,
        "run_root": str(plan.run_root),
        "warmup_insts": WARMUP_INSTS,
        "sample_insts": SAMPLE_INSTS,
        "total_insts": TOTAL_INSTS,
        "spec17_count": len(plan.spec_rows),
        "cbp_count": len(plan.cbp_rows),
        "cbp_ids": [row.workload_id for row in plan.cbp_rows],
        "outputs": [
            str(plan.run_root / output_file.relative_path)
            for output_file in plan.files
        ],
    }


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source-manifest",
        type=Path,
        default=SOURCE_MANIFEST,
        help=f"audited Stage 2 manifest (default: {SOURCE_MANIFEST})",
    )
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="validate and print the plan without creating files",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    source_manifest = args.source_manifest.resolve(strict=True)
    expected_manifest = SOURCE_MANIFEST.resolve(strict=True)
    if source_manifest != expected_manifest:
        raise ValueError(
            f"source manifest must be the audited input {expected_manifest}: "
            f"{source_manifest}"
        )
    plan = build_plan(source_manifest, args.run_root)
    if not args.dry_run:
        write_plan(plan)
    print(json.dumps(_summary(plan, args.dry_run), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
