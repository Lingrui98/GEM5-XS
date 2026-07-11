#!/usr/bin/env python3
"""Validate and derive metrics from a SWAY Stage 3 phase stream.

``maxinsts`` is the required observed instruction count, not merely a launch
limit.  Every complete phase through that count must be present; a formal run
that terminates early fails validation.
"""

from __future__ import annotations

import argparse
import csv
import fcntl
import math
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Mapping, Optional, Sequence, Tuple


SCHEMA_VERSION = "1"
DEFAULT_PHASE_SIZE = 100_000
UINT64_MAX = (1 << 64) - 1
LIKELY_WRAPPED_DELTA = 1 << 63

CSV_FIELDS = (
    "schema_version",
    "phase_id",
    "phase_start_inst",
    "phase_end_inst",
    "phase_inst_count",
    "segment",
    "replay_eligible",
    "stats_epoch_start",
    "stats_epoch_end",
    "component",
    "scope",
    "scope_kind",
    "hits_delta",
    "misses_delta",
    "updates_delta",
    "alloc_success_delta",
    "evicts_delta",
    "resident_entries_end",
    "capacity_entries",
)

EVENT_FIELDS = (
    "hits_delta",
    "misses_delta",
    "updates_delta",
    "alloc_success_delta",
    "evicts_delta",
)

MBTB_WAYS = tuple(
    f"mbtb_bank{bank}_way{way}" for bank in range(2) for way in range(4)
)
ITTAGE_TABLES = tuple(f"ittage_t{table}" for table in range(5))
TAGE_TABLES = tuple(f"tage_t{table}" for table in range(8))

VALIDATION_TSV_FIELDS = (
    "status",
    "stream_path",
    "schema_version",
    "maxinsts",
    "warmup_insts",
    "phase_size",
    "rows",
    "phases",
    "warmup_phases",
    "transition_phases",
    "sample_phases",
    "derived_metrics",
    "na_metrics",
    "error",
)

EXPECTED_SCOPES = {
    "MBTB": MBTB_WAYS + ("mbtb_all",),
    "ITTAGE": ITTAGE_TABLES + ("ittage_all",),
    "TAGE": TAGE_TABLES,
}


class ValidationError(ValueError):
    """The input is not a replayable Stage 3 phase stream."""


@dataclass(frozen=True)
class RawRow:
    schema_version: str
    phase_id: int
    phase_start_inst: int
    phase_end_inst: int
    phase_inst_count: int
    segment: str
    replay_eligible: bool
    stats_epoch_start: int
    stats_epoch_end: int
    component: str
    scope: str
    scope_kind: str
    hits_delta: Optional[int]
    misses_delta: Optional[int]
    updates_delta: Optional[int]
    alloc_success_delta: Optional[int]
    evicts_delta: Optional[int]
    resident_entries_end: int
    capacity_entries: int
    line_number: int


@dataclass(frozen=True)
class Phase:
    phase_id: int
    phase_start_inst: int
    phase_end_inst: int
    phase_inst_count: int
    segment: str
    replay_eligible: bool
    stats_epoch_start: int
    stats_epoch_end: int
    rows: Mapping[Tuple[str, str], RawRow]


@dataclass(frozen=True)
class ValidationResult:
    path: Path
    phases: Tuple[Phase, ...]
    derived_metrics: Tuple[Mapping[str, Optional[float]], ...]

    @property
    def row_count(self) -> int:
        return sum(len(phase.rows) for phase in self.phases)

    def segment_count(self, segment: str) -> int:
        return sum(phase.segment == segment for phase in self.phases)

    @property
    def na_metric_count(self) -> int:
        return sum(
            value is None
            for metrics in self.derived_metrics
            for value in metrics.values()
        )


def _parse_nonnegative_int(value: str, field: str, line_number: int) -> int:
    value = value.strip()
    if not value:
        raise ValidationError(f"line {line_number}: {field} must not be empty")
    try:
        parsed = int(value, 10)
    except ValueError as error:
        raise ValidationError(
            f"line {line_number}: {field} is not an integer: {value!r}"
        ) from error
    if parsed < 0:
        raise ValidationError(
            f"line {line_number}: {field} must be non-negative, got {parsed}"
        )
    return parsed


def _parse_optional_delta(
    value: str, field: str, line_number: int
) -> Optional[int]:
    if not value.strip():
        return None
    parsed = _parse_nonnegative_int(value, field, line_number)
    if parsed > UINT64_MAX:
        raise ValidationError(
            f"line {line_number}: {field} exceeds UINT64_MAX"
        )
    if parsed >= LIKELY_WRAPPED_DELTA:
        raise ValidationError(
            f"line {line_number}: {field}={parsed} is likely a wrapped delta"
        )
    return parsed


def _parse_row(values: Sequence[str], line_number: int) -> RawRow:
    if tuple(value.strip() for value in values) == CSV_FIELDS:
        raise ValidationError(
            f"line {line_number}: repeated CSV header in data rows"
        )
    if len(values) != len(CSV_FIELDS):
        raise ValidationError(
            f"line {line_number}: expected {len(CSV_FIELDS)} columns, "
            f"got {len(values)}"
        )

    raw = dict(zip(CSV_FIELDS, (value.strip() for value in values)))
    if raw["schema_version"] != SCHEMA_VERSION:
        raise ValidationError(
            f"line {line_number}: unsupported schema_version "
            f"{raw['schema_version']!r}; expected {SCHEMA_VERSION!r}"
        )
    if raw["segment"] not in {"warmup", "transition", "sample"}:
        raise ValidationError(
            f"line {line_number}: invalid segment {raw['segment']!r}"
        )
    if raw["replay_eligible"] not in {"true", "false"}:
        raise ValidationError(
            f"line {line_number}: replay_eligible must be lowercase "
            f"'true' or 'false'"
        )

    events = {
        field: _parse_optional_delta(raw[field], field, line_number)
        for field in EVENT_FIELDS
    }
    return RawRow(
        schema_version=raw["schema_version"],
        phase_id=_parse_nonnegative_int(
            raw["phase_id"], "phase_id", line_number
        ),
        phase_start_inst=_parse_nonnegative_int(
            raw["phase_start_inst"], "phase_start_inst", line_number
        ),
        phase_end_inst=_parse_nonnegative_int(
            raw["phase_end_inst"], "phase_end_inst", line_number
        ),
        phase_inst_count=_parse_nonnegative_int(
            raw["phase_inst_count"], "phase_inst_count", line_number
        ),
        segment=raw["segment"],
        replay_eligible=raw["replay_eligible"] == "true",
        stats_epoch_start=_parse_nonnegative_int(
            raw["stats_epoch_start"], "stats_epoch_start", line_number
        ),
        stats_epoch_end=_parse_nonnegative_int(
            raw["stats_epoch_end"], "stats_epoch_end", line_number
        ),
        component=raw["component"],
        scope=raw["scope"],
        scope_kind=raw["scope_kind"],
        hits_delta=events["hits_delta"],
        misses_delta=events["misses_delta"],
        updates_delta=events["updates_delta"],
        alloc_success_delta=events["alloc_success_delta"],
        evicts_delta=events["evicts_delta"],
        resident_entries_end=_parse_nonnegative_int(
            raw["resident_entries_end"],
            "resident_entries_end",
            line_number,
        ),
        capacity_entries=_parse_nonnegative_int(
            raw["capacity_entries"], "capacity_entries", line_number
        ),
        line_number=line_number,
    )


def _read_rows(path: Path) -> List[RawRow]:
    try:
        stream = path.open("r", encoding="utf-8", newline="")
    except OSError as error:
        raise ValidationError(f"cannot open {path}: {error}") from error

    with stream:
        reader = csv.reader(stream)
        try:
            header = next(reader)
        except StopIteration as error:
            raise ValidationError("empty CSV file") from error

        stripped_header = tuple(field.strip() for field in header)
        duplicates = sorted(
            {field for field in stripped_header if stripped_header.count(field) > 1}
        )
        if duplicates:
            raise ValidationError(
                "duplicate CSV header fields: " + ", ".join(duplicates)
            )
        if stripped_header != CSV_FIELDS:
            raise ValidationError(
                "CSV schema mismatch; expected fields in this order: "
                + ",".join(CSV_FIELDS)
            )

        rows = []
        for line_number, values in enumerate(reader, start=2):
            if not values or all(not value.strip() for value in values):
                continue
            rows.append(_parse_row(values, line_number))

    if not rows:
        raise ValidationError("CSV contains no phase rows")
    return rows


def _required_optional_fields(row: RawRow) -> Tuple[set[str], set[str]]:
    if row.component == "MBTB" and row.scope in MBTB_WAYS:
        return {"hits_delta", "evicts_delta"}, {
            "misses_delta",
            "updates_delta",
            "alloc_success_delta",
        }
    if row.component == "MBTB" and row.scope == "mbtb_all":
        return {"hits_delta", "misses_delta", "evicts_delta"}, {
            "updates_delta",
            "alloc_success_delta",
        }
    if row.component == "ITTAGE" and row.scope in ITTAGE_TABLES:
        return {"updates_delta", "alloc_success_delta", "evicts_delta"}, {
            "hits_delta",
            "misses_delta",
        }
    if row.component == "ITTAGE" and row.scope == "ittage_all":
        return {"hits_delta", "misses_delta", "evicts_delta"}, {
            "updates_delta",
            "alloc_success_delta",
        }
    if row.component == "TAGE" and row.scope in TAGE_TABLES:
        return {"updates_delta", "alloc_success_delta", "evicts_delta"}, {
            "hits_delta",
            "misses_delta",
        }
    raise ValidationError(
        f"line {row.line_number}: unexpected component/scope "
        f"{row.component}/{row.scope}"
    )


def _validate_row_shape(row: RawRow) -> None:
    if row.component not in EXPECTED_SCOPES:
        raise ValidationError(
            f"line {row.line_number}: invalid component {row.component!r}"
        )
    if row.scope not in EXPECTED_SCOPES[row.component]:
        raise ValidationError(
            f"line {row.line_number}: invalid scope {row.scope!r} for "
            f"{row.component}"
        )

    expected_kind = "component" if row.scope.endswith("_all") else (
        "way" if row.component == "MBTB" else "table"
    )
    if row.scope_kind != expected_kind:
        raise ValidationError(
            f"line {row.line_number}: {row.component}/{row.scope} requires "
            f"scope_kind={expected_kind!r}, got {row.scope_kind!r}"
        )

    required, blank = _required_optional_fields(row)
    for field in required:
        if getattr(row, field) is None:
            raise ValidationError(
                f"line {row.line_number}: {row.component}/{row.scope} "
                f"requires {field}"
            )
    for field in blank:
        if getattr(row, field) is not None:
            raise ValidationError(
                f"line {row.line_number}: {row.component}/{row.scope} "
                f"requires non-applicable {field} to be empty"
            )

    if row.component in {"ITTAGE", "TAGE"} and row.scope_kind == "table":
        assert row.evicts_delta is not None
        assert row.alloc_success_delta is not None
        if row.evicts_delta > row.alloc_success_delta:
            raise ValidationError(
                f"line {row.line_number}: {row.component}/{row.scope} "
                f"evicts_delta={row.evicts_delta} exceeds "
                f"alloc_success_delta={row.alloc_success_delta}"
            )

    if row.capacity_entries <= 0:
        raise ValidationError(
            f"line {row.line_number}: capacity_entries must be positive"
        )
    if row.resident_entries_end > row.capacity_entries:
        raise ValidationError(
            f"line {row.line_number}: resident_entries_end "
            f"{row.resident_entries_end} exceeds capacity_entries "
            f"{row.capacity_entries}"
        )


def _phase_metadata(row: RawRow) -> Tuple[object, ...]:
    return (
        row.phase_start_inst,
        row.phase_end_inst,
        row.phase_inst_count,
        row.segment,
        row.replay_eligible,
        row.stats_epoch_start,
        row.stats_epoch_end,
    )


def _expected_segment(start: int, end: int, warmup_insts: int) -> str:
    if warmup_insts == 0 or start >= warmup_insts:
        return "sample"
    if end <= warmup_insts:
        return "warmup"
    return "transition"


def _validate_epoch_transition(
    phase_id: int,
    *,
    start_inst: int,
    end_inst: int,
    warmup_insts: int,
    segment: str,
    replay_eligible: bool,
    epoch_start: int,
    epoch_end: int,
    previous_epoch_end: Optional[int],
) -> None:
    if epoch_end < epoch_start:
        raise ValidationError(
            f"phase {phase_id}: stats epoch decreases within phase: "
            f"{epoch_start}->{epoch_end}"
        )
    if epoch_end - epoch_start > 1:
        raise ValidationError(
            f"phase {phase_id}: stats epoch jumps by more than one within "
            f"phase: {epoch_start}->{epoch_end}"
        )

    if previous_epoch_end is not None:
        if epoch_start < previous_epoch_end:
            raise ValidationError(
                f"phase {phase_id}: stats epoch decreases at boundary: "
                f"{previous_epoch_end}->{epoch_start}"
            )
        if epoch_start - previous_epoch_end > 1:
            raise ValidationError(
                f"phase {phase_id}: stats epoch jumps by more than one at "
                f"boundary: {previous_epoch_end}->{epoch_start}"
            )
        if (
            warmup_insts > 0
            and start_inst == warmup_insts
            and epoch_start != previous_epoch_end + 1
        ):
            raise ValidationError(
                f"phase {phase_id}: aligned warmup boundary requires one "
                f"epoch increment, got {previous_epoch_end}->{epoch_start}"
            )

    crosses_warmup = start_inst < warmup_insts < end_inst
    changes_within_phase = epoch_start != epoch_end
    if crosses_warmup and not changes_within_phase:
        raise ValidationError(
            f"phase {phase_id}: unaligned warmup crossing requires an epoch "
            "jump"
        )

    if changes_within_phase:
        expected_segment = "transition"
    else:
        expected_segment = _expected_segment(
            start_inst, end_inst, warmup_insts
        )
    if segment != expected_segment:
        raise ValidationError(
            f"phase {phase_id}: segment={segment!r}, "
            f"expected {expected_segment!r} from instruction and epoch state"
        )

    expected_replay = expected_segment != "transition"
    if replay_eligible != expected_replay:
        raise ValidationError(
            f"phase {phase_id}: replay_eligible={replay_eligible}, "
            f"expected {expected_replay} for {expected_segment}"
        )


def _validate_aggregate(
    phase: Phase,
    component: str,
    aggregate_scope: str,
    child_scopes: Iterable[str],
) -> None:
    aggregate = phase.rows[(component, aggregate_scope)]
    children = [phase.rows[(component, scope)] for scope in child_scopes]
    checks = {
        "evicts_delta": sum(child.evicts_delta or 0 for child in children),
        "resident_entries_end": sum(
            child.resident_entries_end for child in children
        ),
        "capacity_entries": sum(child.capacity_entries for child in children),
    }
    for field, expected in checks.items():
        actual = getattr(aggregate, field)
        if actual != expected:
            raise ValidationError(
                f"phase {phase.phase_id}: {component}/{aggregate_scope} "
                f"{field}={actual}, expected child sum {expected}"
            )


def _build_phases(
    rows: Sequence[RawRow], maxinsts: int, warmup_insts: int, phase_size: int
) -> Tuple[Phase, ...]:
    grouped: Dict[int, List[RawRow]] = {}
    for row in rows:
        _validate_row_shape(row)
        grouped.setdefault(row.phase_id, []).append(row)

    expected_phase_count = maxinsts // phase_size
    expected_ids = list(range(1, expected_phase_count + 1))
    actual_ids = sorted(grouped)
    if actual_ids != expected_ids:
        missing = sorted(set(expected_ids) - set(actual_ids))
        extra = sorted(set(actual_ids) - set(expected_ids))
        raise ValidationError(
            "phase ID coverage mismatch: "
            f"expected 1..{expected_phase_count}, missing={missing}, extra={extra}"
        )

    phases = []
    previous_epoch_end = None
    for phase_id in expected_ids:
        phase_rows = grouped[phase_id]
        first = phase_rows[0]
        metadata = _phase_metadata(first)
        row_map: Dict[Tuple[str, str], RawRow] = {}
        for row in phase_rows:
            if _phase_metadata(row) != metadata:
                raise ValidationError(
                    f"phase {phase_id}: inconsistent phase metadata at "
                    f"line {row.line_number}"
                )
            key = (row.component, row.scope)
            if key in row_map:
                raise ValidationError(
                    f"phase {phase_id}: duplicate key {row.component}/{row.scope}"
                )
            row_map[key] = row

        expected_keys = {
            (component, scope)
            for component, scopes in EXPECTED_SCOPES.items()
            for scope in scopes
        }
        actual_keys = set(row_map)
        if actual_keys != expected_keys or len(phase_rows) != 23:
            missing = sorted(expected_keys - actual_keys)
            extra = sorted(actual_keys - expected_keys)
            raise ValidationError(
                f"phase {phase_id}: expected 23 unique rows; "
                f"got {len(phase_rows)}, missing={missing}, extra={extra}"
            )

        expected_start = (phase_id - 1) * phase_size
        expected_end = phase_id * phase_size
        if (
            first.phase_start_inst != expected_start
            or first.phase_end_inst != expected_end
            or first.phase_inst_count != phase_size
            or first.phase_end_inst - first.phase_start_inst != phase_size
        ):
            raise ValidationError(
                f"phase {phase_id}: expected complete interval "
                f"[{expected_start}, {expected_end}) with count {phase_size}"
            )

        _validate_epoch_transition(
            phase_id,
            start_inst=expected_start,
            end_inst=expected_end,
            warmup_insts=warmup_insts,
            segment=first.segment,
            replay_eligible=first.replay_eligible,
            epoch_start=first.stats_epoch_start,
            epoch_end=first.stats_epoch_end,
            previous_epoch_end=previous_epoch_end,
        )

        phase = Phase(
            phase_id=phase_id,
            phase_start_inst=first.phase_start_inst,
            phase_end_inst=first.phase_end_inst,
            phase_inst_count=first.phase_inst_count,
            segment=first.segment,
            replay_eligible=first.replay_eligible,
            stats_epoch_start=first.stats_epoch_start,
            stats_epoch_end=first.stats_epoch_end,
            rows=row_map,
        )
        _validate_aggregate(phase, "MBTB", "mbtb_all", MBTB_WAYS)
        _validate_aggregate(
            phase, "ITTAGE", "ittage_all", ITTAGE_TABLES
        )
        phases.append(phase)
        previous_epoch_end = first.stats_epoch_end
    return tuple(phases)


def _finite_ratio(numerator: int, denominator: int, name: str) -> Optional[float]:
    if denominator == 0:
        return None
    try:
        value = numerator / denominator
    except OverflowError as error:
        raise ValidationError(f"{name}: derived metric overflow") from error
    if not math.isfinite(value):
        raise ValidationError(f"{name}: derived metric is non-finite")
    return value


def _bc_metric(
    evicts: int,
    accesses: int,
    resident: int,
    capacity: int,
    name: str,
) -> Optional[float]:
    evict_rate = _finite_ratio(evicts, accesses, name)
    utilization = _finite_ratio(resident, capacity, name)
    if evict_rate is None or utilization is None:
        return None
    value = evict_rate * utilization
    if not math.isfinite(value):
        raise ValidationError(f"{name}: derived metric is non-finite")
    return value


def derive_phase_metrics(phase: Phase) -> Mapping[str, Optional[float]]:
    """Derive the replay metrics for one already validated phase."""

    metrics: Dict[str, Optional[float]] = {}
    mbtb_hits = [phase.rows[("MBTB", scope)].hits_delta or 0 for scope in MBTB_WAYS]
    mbtb_hit_sum = sum(mbtb_hits)
    for scope in ("mbtb_bank0_way3", "mbtb_bank1_way3"):
        candidate = phase.rows[("MBTB", scope)].hits_delta or 0
        metrics[f"B-b_MBTB_{scope}"] = _finite_ratio(
            candidate * len(MBTB_WAYS),
            mbtb_hit_sum,
            f"phase {phase.phase_id} B-b {scope}",
        )

    ittage_updates = [
        phase.rows[("ITTAGE", scope)].updates_delta or 0
        for scope in ITTAGE_TABLES
    ]
    ittage_update_sum = sum(ittage_updates)
    for scope in ("ittage_t2", "ittage_t3"):
        candidate = phase.rows[("ITTAGE", scope)].updates_delta or 0
        metrics[f"B-b_ITTAGE_{scope}"] = _finite_ratio(
            candidate * len(ITTAGE_TABLES),
            ittage_update_sum,
            f"phase {phase.phase_id} B-b {scope}",
        )

    mbtb_all = phase.rows[("MBTB", "mbtb_all")]
    metrics["B-c_MBTB"] = _bc_metric(
        mbtb_all.evicts_delta or 0,
        (mbtb_all.hits_delta or 0) + (mbtb_all.misses_delta or 0),
        mbtb_all.resident_entries_end,
        mbtb_all.capacity_entries,
        f"phase {phase.phase_id} B-c MBTB",
    )

    ittage_all = phase.rows[("ITTAGE", "ittage_all")]
    metrics["B-c_ITTAGE"] = _bc_metric(
        ittage_all.evicts_delta or 0,
        (ittage_all.hits_delta or 0) + (ittage_all.misses_delta or 0),
        ittage_all.resident_entries_end,
        ittage_all.capacity_entries,
        f"phase {phase.phase_id} B-c ITTAGE",
    )

    for scope in TAGE_TABLES:
        row = phase.rows[("TAGE", scope)]
        metrics[f"B-c_TAGE_{scope}"] = _bc_metric(
            row.evicts_delta or 0,
            (row.updates_delta or 0) + (row.alloc_success_delta or 0),
            row.resident_entries_end,
            row.capacity_entries,
            f"phase {phase.phase_id} B-c {scope}",
        )
    return metrics


def validate_phase_stream(
    path: Path | str,
    *,
    maxinsts: int,
    warmup_insts: int,
    phase_size: int = DEFAULT_PHASE_SIZE,
) -> ValidationResult:
    """Validate a phase stream and return replay-ready rows and metrics.

    ``maxinsts`` is the required observed total.  Missing any complete phase
    below it is an early-termination failure, including formal 40M runs.
    """

    if phase_size <= 0:
        raise ValidationError("phase_size must be positive")
    if maxinsts <= 0:
        raise ValidationError("maxinsts must be positive")
    if warmup_insts < 0:
        raise ValidationError("warmup_insts must be non-negative")
    if maxinsts <= warmup_insts:
        raise ValidationError(
            f"invalid run: maxinsts ({maxinsts}) must exceed "
            f"warmup_insts ({warmup_insts})"
        )
    if maxinsts < phase_size:
        raise ValidationError(
            "invalid run: maxinsts does not contain one complete phase"
        )

    normalized_path = Path(path)
    rows = _read_rows(normalized_path)
    phases = _build_phases(rows, maxinsts, warmup_insts, phase_size)
    metrics = tuple(derive_phase_metrics(phase) for phase in phases)
    return ValidationResult(normalized_path, phases, metrics)


def _parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate a SWAY Stage 3 sway_phase_bc_raw.csv stream"
    )
    parser.add_argument("csv", type=Path, help="path to sway_phase_bc_raw.csv")
    parser.add_argument(
        "--maxinsts",
        type=int,
        required=True,
        help=(
            "required observed total instruction count; missing any complete "
            "phase through this count is an early-termination failure"
        ),
    )
    parser.add_argument("--warmup-insts", type=int, required=True)
    parser.add_argument("--phase-size", type=int, default=DEFAULT_PHASE_SIZE)
    parser.add_argument(
        "--output-tsv",
        "--output",
        dest="output_tsv",
        type=Path,
        help="write a one-row phase_stream_validation.tsv audit record",
    )
    return parser.parse_args(argv)


def _write_validation_tsv(
    path: Path,
    *,
    status: str,
    stream_path: Path,
    maxinsts: int,
    warmup_insts: int,
    phase_size: int,
    result: Optional[ValidationResult] = None,
    error: str = "",
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    row = {
        "status": status,
        "stream_path": str(stream_path),
        "schema_version": SCHEMA_VERSION,
        "maxinsts": maxinsts,
        "warmup_insts": warmup_insts,
        "phase_size": phase_size,
        "rows": "",
        "phases": "",
        "warmup_phases": "",
        "transition_phases": "",
        "sample_phases": "",
        "derived_metrics": "",
        "na_metrics": "",
        "error": error,
    }
    if result is not None:
        row.update(
            rows=result.row_count,
            phases=len(result.phases),
            warmup_phases=result.segment_count("warmup"),
            transition_phases=result.segment_count("transition"),
            sample_phases=result.segment_count("sample"),
            derived_metrics=sum(
                len(metrics) for metrics in result.derived_metrics
            ),
            na_metrics=result.na_metric_count,
        )

    try:
        with path.open("a+", encoding="utf-8", newline="") as stream:
            fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
            stream.seek(0)
            reader = csv.reader(stream, delimiter="\t")
            try:
                existing_header = tuple(next(reader))
            except StopIteration:
                existing_header = ()
            if existing_header and existing_header != VALIDATION_TSV_FIELDS:
                raise ValidationError(
                    f"existing validation TSV {path} has an incompatible "
                    "header"
                )

            stream.seek(0, os.SEEK_END)
            writer = csv.DictWriter(
                stream,
                fieldnames=VALIDATION_TSV_FIELDS,
                delimiter="\t",
                lineterminator="\n",
            )
            if not existing_header:
                writer.writeheader()
            writer.writerow(row)
    except OSError as output_error:
        raise ValidationError(
            f"cannot write validation TSV {path}: {output_error}"
        ) from output_error


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _parse_args(argv)
    try:
        result = validate_phase_stream(
            args.csv,
            maxinsts=args.maxinsts,
            warmup_insts=args.warmup_insts,
            phase_size=args.phase_size,
        )
    except ValidationError as error:
        if args.output_tsv is not None:
            try:
                _write_validation_tsv(
                    args.output_tsv,
                    status="FAIL",
                    stream_path=args.csv,
                    maxinsts=args.maxinsts,
                    warmup_insts=args.warmup_insts,
                    phase_size=args.phase_size,
                    error=str(error),
                )
            except ValidationError as output_error:
                print(f"FAIL\t{args.output_tsv}\t{output_error}", file=sys.stderr)
        print(f"FAIL\t{args.csv}\t{error}", file=sys.stderr)
        return 2

    if args.output_tsv is not None:
        try:
            _write_validation_tsv(
                args.output_tsv,
                status="PASS",
                stream_path=args.csv,
                maxinsts=args.maxinsts,
                warmup_insts=args.warmup_insts,
                phase_size=args.phase_size,
                result=result,
            )
        except ValidationError as error:
            print(f"FAIL\t{args.output_tsv}\t{error}", file=sys.stderr)
            return 2

    print(
        "PASS\t"
        f"{result.path}\trows={result.row_count}\t"
        f"phases={len(result.phases)}\t"
        f"warmup={result.segment_count('warmup')}\t"
        f"transition={result.segment_count('transition')}\t"
        f"sample={result.segment_count('sample')}\t"
        f"na_metrics={result.na_metric_count}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
