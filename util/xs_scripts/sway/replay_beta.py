#!/usr/bin/env python3
"""Replay the Stage 3 self-normalized beta policy from a raw phase stream.

This tool is deliberately open loop.  It derives the locked B-b/B-c metrics
from ``sway_phase_bc_raw.csv`` and reports trigger feasibility only.  It does
not estimate IPC impact or false fires from a baseline-only stream.
"""

from __future__ import annotations

import argparse
import csv
import math
import re
import statistics
from collections import defaultdict, deque
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Mapping, Sequence


THETAS = (0.5, 1.0, 1.5, 2.0, 2.5)
HISTORY_LIMIT = 128
MIN_HISTORY = 32

MBTB_DONORS = (
    "b_b_mbtb_bank0_way3",
    "b_b_mbtb_bank1_way3",
)
ITTAGE_DONORS = (
    "b_b_ittage_t2",
    "b_b_ittage_t3",
)
TAGE_DONEES = (
    "b_c_tage_t3",
    "b_c_tage_t7",
)
ALL_METRICS = MBTB_DONORS + ITTAGE_DONORS + TAGE_DONEES

DIRECTIONS = (
    ("MBTB->TAGE", MBTB_DONORS),
    ("ITTAGE->TAGE", ITTAGE_DONORS),
)

METRIC_LABELS = {
    "b_b_mbtb_bank0_way3": "MBTB.bank0.way3",
    "b_b_mbtb_bank1_way3": "MBTB.bank1.way3",
    "b_b_ittage_t2": "ITTAGE.T2",
    "b_b_ittage_t3": "ITTAGE.T3",
    "b_c_tage_t3": "TAGE.T3",
    "b_c_tage_t7": "TAGE.T7",
}

DONEE_COOLDOWN = {
    "b_c_tage_t3": 8,
    "b_c_tage_t7": 20,
}

RAW_REQUIRED_FIELDS = {
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
    "updates_delta",
    "alloc_success_delta",
    "evicts_delta",
    "resident_entries_end",
    "capacity_entries",
}

EVENT_FIELDS = (
    "family",
    "workload",
    "phase_id",
    "phase_start_inst",
    "phase_end_inst",
    "direction",
    "theta",
    "evaluable",
    "evaluable_pair_count",
    "unavailable_pair_count",
    "unavailable_candidates",
    "fallback_reason",
    "donor_z_scores",
    "donee_z_scores",
    "crossed_donors",
    "crossed_donees",
    "joint_threshold_crossing",
    "cooldown_blocked",
    "arbitration_lost_crossing",
    "open_loop_fire",
    "selected_donor",
    "selected_donee",
    "selected_donor_z",
    "selected_donee_z",
    "cooldown_t3_before",
    "cooldown_t7_before",
    "cooldown_t3_after",
    "cooldown_t7_after",
    "IPC_delta_estimate",
    "false_fire_count",
)

PER_WORKLOAD_FIELDS = (
    "family",
    "workload",
    "direction",
    "theta",
    "warmup_phases",
    "transition_phases",
    "sample_phases",
    "evaluable_phases",
    "evaluable_pairs",
    "unavailable_pairs",
    "joint_threshold_crossings",
    "cooldown_blocked_crossings",
    "arbitration_lost_crossings",
    "open_loop_fires",
    "t3_fires",
    "t7_fires",
    "short_history_fallbacks",
    "zero_std_fallbacks",
    "sample_na_fallbacks",
    "not_evaluable_phases",
    "IPC_delta_estimate",
    "false_fire_count",
)

SUMMARY_FIELDS = (
    "direction",
    "theta",
    "workload_count",
    "beta_evaluable_workloads",
    "hard_gate_min_workloads",
    "hard_gate_met",
    "sample_phases",
    "evaluable_phases",
    "evaluable_pairs",
    "unavailable_pairs",
    "joint_threshold_crossings",
    "cooldown_blocked_crossings",
    "arbitration_lost_crossings",
    "open_loop_fires",
    "t3_fires",
    "t7_fires",
    "short_history_fallbacks",
    "zero_std_fallbacks",
    "sample_na_fallbacks",
    "not_evaluable_phases",
    "IPC_delta_estimate",
    "false_fire_count",
)


class ReplayInputError(ValueError):
    """Raised when the raw stream cannot support deterministic replay."""


@dataclass(frozen=True)
class WorkloadInput:
    family: str
    workload: str
    raw_csv: Path


@dataclass(frozen=True)
class PhaseMetrics:
    phase_id: int
    phase_start_inst: int
    phase_end_inst: int
    segment: str
    replay_eligible: bool
    values: Mapping[str, float | None]


@dataclass(frozen=True)
class ReferenceDistribution:
    status: str
    sample_count: int
    mean: float | None = None
    stddev: float | None = None


def _parse_int(value: str, field: str, context: str) -> int:
    try:
        parsed = int(value)
    except (TypeError, ValueError) as exc:
        raise ReplayInputError(
            f"{context}: {field} must be an integer, got {value!r}"
        ) from exc
    if parsed < 0:
        raise ReplayInputError(f"{context}: {field} must be non-negative")
    return parsed


def _parse_bool(value: str, field: str, context: str) -> bool:
    normalized = value.strip().lower()
    if normalized in {"1", "true", "yes"}:
        return True
    if normalized in {"0", "false", "no"}:
        return False
    raise ReplayInputError(
        f"{context}: {field} must be a boolean, got {value!r}"
    )


def _parse_number(
    row: Mapping[str, str], field: str, context: str, *, allow_empty: bool = False
) -> float | None:
    raw = row.get(field, "").strip()
    if not raw:
        if allow_empty:
            return None
        raise ReplayInputError(f"{context}: required field {field} is empty")
    try:
        value = float(raw)
    except ValueError as exc:
        raise ReplayInputError(
            f"{context}: {field} is not numeric: {raw!r}"
        ) from exc
    if not math.isfinite(value):
        raise ReplayInputError(f"{context}: {field} must be finite")
    if value < 0:
        raise ReplayInputError(f"{context}: {field} must be non-negative")
    return value


def _canonical_scope(component: str, scope: str) -> str | None:
    component = component.strip().upper()
    normalized = re.sub(r"[^a-z0-9]+", "_", scope.strip().lower()).strip("_")

    if component == "MBTB":
        match = re.search(r"bank_?([01])_?way_?([0-3])(?:_|$)", normalized)
        if not match:
            match = re.search(r"(?:^|_)b([01])_?w([0-3])(?:_|$)", normalized)
        if match:
            return f"MBTB.bank{match.group(1)}.way{match.group(2)}"
        return None

    if component in {"ITTAGE", "TAGE"}:
        match = re.search(r"(?:^|_)(?:table_?|t_?)([0-9]+)(?:_|$)", normalized)
        if match:
            return f"{component}.T{int(match.group(1))}"
    return None


def _phase_metadata(row: Mapping[str, str], context: str) -> tuple[object, ...]:
    return (
        _parse_int(row["phase_start_inst"], "phase_start_inst", context),
        _parse_int(row["phase_end_inst"], "phase_end_inst", context),
        _parse_int(row["phase_inst_count"], "phase_inst_count", context),
        row["segment"].strip().lower(),
        _parse_bool(row["replay_eligible"], "replay_eligible", context),
        _parse_int(row["stats_epoch_start"], "stats_epoch_start", context),
        _parse_int(row["stats_epoch_end"], "stats_epoch_end", context),
    )


def _require_scopes(
    phase_id: int, rows: Mapping[str, Mapping[str, str]], scopes: Iterable[str]
) -> None:
    missing = [scope for scope in scopes if scope not in rows]
    if missing:
        raise ReplayInputError(
            f"phase {phase_id}: missing replay scopes: {', '.join(missing)}"
        )


def _safe_ratio(numerator: float, denominator: float) -> float | None:
    if denominator == 0.0:
        return None
    value = numerator / denominator
    if not math.isfinite(value):
        raise ReplayInputError("derived metric is non-finite")
    return value


def _derive_phase_metrics(
    phase_id: int,
    metadata: tuple[object, ...],
    rows: Mapping[str, Mapping[str, str]],
) -> PhaseMetrics:
    mbtb_scopes = tuple(
        f"MBTB.bank{bank}.way{way}" for bank in range(2) for way in range(4)
    )
    ittage_scopes = tuple(f"ITTAGE.T{table}" for table in range(5))
    tage_scopes = ("TAGE.T3", "TAGE.T7")
    _require_scopes(phase_id, rows, mbtb_scopes + ittage_scopes + tage_scopes)

    mbtb_hits = [
        _parse_number(rows[scope], "hits_delta", f"phase {phase_id} {scope}")
        for scope in mbtb_scopes
    ]
    assert all(value is not None for value in mbtb_hits)
    mbtb_mean = statistics.fmean(value for value in mbtb_hits if value is not None)

    ittage_updates = [
        _parse_number(rows[scope], "updates_delta", f"phase {phase_id} {scope}")
        for scope in ittage_scopes
    ]
    assert all(value is not None for value in ittage_updates)
    ittage_mean = statistics.fmean(
        value for value in ittage_updates if value is not None
    )

    values: dict[str, float | None] = {
        "b_b_mbtb_bank0_way3": _safe_ratio(mbtb_hits[3], mbtb_mean),
        "b_b_mbtb_bank1_way3": _safe_ratio(mbtb_hits[7], mbtb_mean),
        "b_b_ittage_t2": _safe_ratio(ittage_updates[2], ittage_mean),
        "b_b_ittage_t3": _safe_ratio(ittage_updates[3], ittage_mean),
    }

    for table in (3, 7):
        scope = f"TAGE.T{table}"
        context = f"phase {phase_id} {scope}"
        updates = _parse_number(rows[scope], "updates_delta", context)
        alloc_success = _parse_number(
            rows[scope], "alloc_success_delta", context
        )
        evicts = _parse_number(rows[scope], "evicts_delta", context)
        resident = _parse_number(rows[scope], "resident_entries_end", context)
        capacity = _parse_number(rows[scope], "capacity_entries", context)
        assert None not in (updates, alloc_success, evicts, resident, capacity)
        if capacity == 0.0:
            raise ReplayInputError(f"{context}: capacity_entries must be positive")
        if resident > capacity:
            raise ReplayInputError(
                f"{context}: resident_entries_end exceeds capacity_entries"
            )
        accesses = updates + alloc_success
        pressure = None
        if accesses != 0.0:
            pressure = (evicts / accesses) * (resident / capacity)
            if not math.isfinite(pressure):
                raise ReplayInputError(f"{context}: derived B-c is non-finite")
        values[f"b_c_tage_t{table}"] = pressure

    start, end, count, segment, replay_eligible, _epoch_start, _epoch_end = metadata
    assert isinstance(start, int)
    assert isinstance(end, int)
    assert isinstance(count, int)
    assert isinstance(segment, str)
    assert isinstance(replay_eligible, bool)
    if end <= start or count != end - start:
        raise ReplayInputError(
            f"phase {phase_id}: inconsistent phase instruction interval"
        )
    if segment not in {"warmup", "transition", "sample"}:
        raise ReplayInputError(f"phase {phase_id}: unknown segment {segment!r}")
    if segment == "transition" and replay_eligible:
        raise ReplayInputError(
            f"phase {phase_id}: transition phase cannot be replay eligible"
        )
    if segment != "transition" and not replay_eligible:
        raise ReplayInputError(
            f"phase {phase_id}: {segment} phase must be replay eligible"
        )

    return PhaseMetrics(
        phase_id=phase_id,
        phase_start_inst=start,
        phase_end_inst=end,
        segment=segment,
        replay_eligible=replay_eligible,
        values=values,
    )


def read_phase_stream(path: Path) -> list[PhaseMetrics]:
    """Read the replay-relevant subset of a Stage 3 raw phase CSV."""

    try:
        stream = path.open("r", encoding="utf-8", newline="")
    except OSError as exc:
        raise ReplayInputError(f"cannot open phase stream {path}: {exc}") from exc

    with stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames is None:
            raise ReplayInputError(f"{path}: missing CSV header")
        duplicate_headers = sorted(
            field for field in set(reader.fieldnames) if reader.fieldnames.count(field) > 1
        )
        if duplicate_headers:
            raise ReplayInputError(
                f"{path}: duplicate CSV headers: {', '.join(duplicate_headers)}"
            )
        missing_fields = sorted(RAW_REQUIRED_FIELDS - set(reader.fieldnames))
        if missing_fields:
            raise ReplayInputError(
                f"{path}: missing required fields: {', '.join(missing_fields)}"
            )

        rows_by_phase: dict[int, dict[str, Mapping[str, str]]] = defaultdict(dict)
        metadata_by_phase: dict[int, tuple[object, ...]] = {}
        for line_number, row in enumerate(reader, start=2):
            context = f"{path}:{line_number}"
            phase_id = _parse_int(row["phase_id"], "phase_id", context)
            metadata = _phase_metadata(row, context)
            if phase_id in metadata_by_phase and metadata_by_phase[phase_id] != metadata:
                raise ReplayInputError(
                    f"{context}: inconsistent metadata within phase {phase_id}"
                )
            metadata_by_phase[phase_id] = metadata

            canonical = _canonical_scope(row["component"], row["scope"])
            if canonical is None:
                continue
            if canonical in rows_by_phase[phase_id]:
                raise ReplayInputError(
                    f"{context}: duplicate scope {canonical} in phase {phase_id}"
                )
            rows_by_phase[phase_id][canonical] = row

    if not metadata_by_phase:
        raise ReplayInputError(f"{path}: phase stream is empty")

    phases = [
        _derive_phase_metrics(
            phase_id, metadata_by_phase[phase_id], rows_by_phase[phase_id]
        )
        for phase_id in metadata_by_phase
    ]
    phases.sort(key=lambda phase: (phase.phase_start_inst, phase.phase_id))

    seen_sample = False
    previous_end: int | None = None
    for phase in phases:
        if previous_end is not None and phase.phase_start_inst < previous_end:
            raise ReplayInputError("phase intervals overlap or are out of order")
        previous_end = phase.phase_end_inst
        if phase.segment == "sample":
            seen_sample = True
        elif phase.segment == "warmup" and seen_sample:
            raise ReplayInputError("warmup phase appears after sample execution")
    return phases


def _build_references(
    phases: Sequence[PhaseMetrics],
) -> dict[str, ReferenceDistribution]:
    histories = {metric: deque(maxlen=HISTORY_LIMIT) for metric in ALL_METRICS}
    for phase in phases:
        if phase.segment != "warmup" or not phase.replay_eligible:
            continue
        for metric in ALL_METRICS:
            value = phase.values[metric]
            if value is not None and math.isfinite(value):
                histories[metric].append(value)

    references: dict[str, ReferenceDistribution] = {}
    for metric, history in histories.items():
        values = list(history)
        if len(values) < MIN_HISTORY:
            references[metric] = ReferenceDistribution(
                status="short_history", sample_count=len(values)
            )
            continue
        mean = statistics.fmean(values)
        stddev = statistics.pstdev(values, mu=mean)
        if stddev == 0.0:
            references[metric] = ReferenceDistribution(
                status="zero_std",
                sample_count=len(values),
                mean=mean,
                stddev=stddev,
            )
            continue
        references[metric] = ReferenceDistribution(
            status="ok",
            sample_count=len(values),
            mean=mean,
            stddev=stddev,
        )
    return references


def _phase_z_scores(
    phase: PhaseMetrics,
    metrics: Sequence[str],
    references: Mapping[str, ReferenceDistribution],
) -> tuple[dict[str, float], dict[str, str]]:
    scores: dict[str, float] = {}
    statuses: dict[str, str] = {}
    for metric in metrics:
        reference = references[metric]
        if reference.status != "ok":
            statuses[metric] = reference.status
            continue
        value = phase.values[metric]
        if value is None or not math.isfinite(value):
            statuses[metric] = "sample_na"
            continue
        assert reference.mean is not None
        assert reference.stddev is not None
        if metric.startswith("b_b_"):
            score = (reference.mean - value) / reference.stddev
        else:
            score = (value - reference.mean) / reference.stddev
        if not math.isfinite(score):
            statuses[metric] = "sample_na"
            continue
        scores[metric] = score
        statuses[metric] = "ok"
    return scores, statuses


def _select_winning_pair(
    pairs: Sequence[tuple[str, str, str]], scores: Mapping[str, float]
) -> tuple[str, str, str]:
    """Select by donor z, donee z, then stable direction/donor/donee id."""

    direction_order = {
        direction: index for index, (direction, _donors) in enumerate(DIRECTIONS)
    }
    donor_order = {
        donor: index for index, donor in enumerate(MBTB_DONORS + ITTAGE_DONORS)
    }
    donee_order = {donee: index for index, donee in enumerate(TAGE_DONEES)}
    return max(
        pairs,
        key=lambda pair: (
            scores[pair[1]],
            scores[pair[2]],
            -direction_order[pair[0]],
            -donor_order[pair[1]],
            -donee_order[pair[2]],
        ),
    )


def _format_float(value: float | None) -> str:
    return "" if value is None else format(value, ".17g")


def _format_scores(metrics: Sequence[str], scores: Mapping[str, float]) -> str:
    return ";".join(
        f"{METRIC_LABELS[metric]}={_format_float(scores.get(metric))}"
        for metric in metrics
    )


def _format_names(metrics: Sequence[str]) -> str:
    return ";".join(METRIC_LABELS[metric] for metric in metrics)


def _format_unavailable(
    metrics: Sequence[str], statuses: Mapping[str, str]
) -> str:
    return ";".join(
        f"{METRIC_LABELS[metric]}={statuses[metric]}"
        for metric in metrics
        if statuses[metric] != "ok"
    )


def replay_workload(
    workload: WorkloadInput, phases: Sequence[PhaseMetrics]
) -> tuple[list[dict[str, object]], list[dict[str, object]]]:
    """Replay every locked theta for one workload.

    Directions share per-donee cooldown and global arbitration.  Each
    ``(theta, phase)`` can fire at most one legal pair.  A missing candidate
    only removes pairs containing that candidate; it does not poison other
    legal pairs in the same direction.  The ``*_fallbacks`` summaries count
    unavailable candidate evaluations; ``unavailable_pairs`` separately
    counts legal pairs removed by those candidates.
    """

    references = _build_references(phases)
    warmup_phases = sum(phase.segment == "warmup" for phase in phases)
    transition_phases = sum(phase.segment == "transition" for phase in phases)
    sample_phases = [
        phase
        for phase in phases
        if phase.segment == "sample" and phase.replay_eligible
    ]

    counter_names = (
        "evaluable_phases",
        "evaluable_pairs",
        "unavailable_pairs",
        "joint_threshold_crossings",
        "cooldown_blocked_crossings",
        "arbitration_lost_crossings",
        "open_loop_fires",
        "t3_fires",
        "t7_fires",
        "short_history_fallbacks",
        "zero_std_fallbacks",
        "sample_na_fallbacks",
        "not_evaluable_phases",
    )
    counters_by_config = {
        (direction, theta): {name: 0 for name in counter_names}
        for direction, _donors in DIRECTIONS
        for theta in THETAS
    }

    events: list[dict[str, object]] = []
    for theta in THETAS:
        cooldown = {donee: 0 for donee in TAGE_DONEES}
        for phase in sample_phases:
            for donee in TAGE_DONEES:
                if cooldown[donee] > 0:
                    cooldown[donee] -= 1
            cooldown_before = dict(cooldown)
            scores, statuses = _phase_z_scores(
                phase, ALL_METRICS, references
            )

            direction_state: dict[str, dict[str, object]] = {}
            globally_available_pairs: list[tuple[str, str, str]] = []
            for direction, donors in DIRECTIONS:
                candidates = donors + TAGE_DONEES
                pairs = [
                    (direction, donor, donee)
                    for donor in donors
                    for donee in TAGE_DONEES
                ]
                evaluable_pairs = [
                    pair
                    for pair in pairs
                    if statuses[pair[1]] == "ok" and statuses[pair[2]] == "ok"
                ]
                crossed_pairs = [
                    pair
                    for pair in evaluable_pairs
                    if scores[pair[1]] > theta and scores[pair[2]] > theta
                ]
                available_pairs = [
                    pair for pair in crossed_pairs if cooldown[pair[2]] == 0
                ]
                globally_available_pairs.extend(available_pairs)
                direction_state[direction] = {
                    "donors": donors,
                    "candidates": candidates,
                    "evaluable_pairs": evaluable_pairs,
                    "crossed_pairs": crossed_pairs,
                    "available_pairs": available_pairs,
                }

            winner: tuple[str, str, str] | None = None
            if globally_available_pairs:
                winner = _select_winning_pair(globally_available_pairs, scores)
                cooldown[winner[2]] = DONEE_COOLDOWN[winner[2]]

            phase_fire_count = 0
            for direction, donors in DIRECTIONS:
                state = direction_state[direction]
                candidates = state["candidates"]
                evaluable_pairs = state["evaluable_pairs"]
                crossed_pairs = state["crossed_pairs"]
                available_pairs = state["available_pairs"]
                assert isinstance(candidates, tuple)
                assert isinstance(evaluable_pairs, list)
                assert isinstance(crossed_pairs, list)
                assert isinstance(available_pairs, list)

                counters = counters_by_config[(direction, theta)]
                evaluable = bool(evaluable_pairs)
                counters["evaluable_pairs"] += len(evaluable_pairs)
                counters["unavailable_pairs"] += 4 - len(evaluable_pairs)
                if evaluable:
                    counters["evaluable_phases"] += 1
                else:
                    counters["not_evaluable_phases"] += 1

                unavailable_candidates = [
                    metric for metric in candidates if statuses[metric] != "ok"
                ]
                reasons = tuple(
                    reason
                    for reason in ("short_history", "zero_std", "sample_na")
                    if any(
                        statuses[metric] == reason
                        for metric in unavailable_candidates
                    )
                )
                for metric in unavailable_candidates:
                    counters[f"{statuses[metric]}_fallbacks"] += 1

                joint_crossing = bool(crossed_pairs)
                cooldown_blocked = joint_crossing and not available_pairs
                fired = winner is not None and winner[0] == direction
                arbitration_lost = (
                    joint_crossing and bool(available_pairs) and not fired
                )
                if joint_crossing:
                    counters["joint_threshold_crossings"] += 1
                if cooldown_blocked:
                    counters["cooldown_blocked_crossings"] += 1
                if arbitration_lost:
                    counters["arbitration_lost_crossings"] += 1
                if fired:
                    phase_fire_count += 1
                    counters["open_loop_fires"] += 1
                    table = "t3" if winner[2].endswith("t3") else "t7"
                    counters[f"{table}_fires"] += 1

                crossed_donors = [
                    donor
                    for donor in donors
                    if any(pair[1] == donor for pair in crossed_pairs)
                ]
                crossed_donees = [
                    donee
                    for donee in TAGE_DONEES
                    if any(pair[2] == donee for pair in crossed_pairs)
                ]
                selected_donor = winner[1] if fired else None
                selected_donee = winner[2] if fired else None
                events.append(
                    {
                        "family": workload.family,
                        "workload": workload.workload,
                        "phase_id": phase.phase_id,
                        "phase_start_inst": phase.phase_start_inst,
                        "phase_end_inst": phase.phase_end_inst,
                        "direction": direction,
                        "theta": format(theta, ".1f"),
                        "evaluable": int(evaluable),
                        "evaluable_pair_count": len(evaluable_pairs),
                        "unavailable_pair_count": 4 - len(evaluable_pairs),
                        "unavailable_candidates": _format_unavailable(
                            candidates, statuses
                        ),
                        "fallback_reason": ";".join(reasons),
                        "donor_z_scores": _format_scores(donors, scores),
                        "donee_z_scores": _format_scores(TAGE_DONEES, scores),
                        "crossed_donors": _format_names(crossed_donors),
                        "crossed_donees": _format_names(crossed_donees),
                        "joint_threshold_crossing": int(joint_crossing),
                        "cooldown_blocked": int(cooldown_blocked),
                        "arbitration_lost_crossing": int(arbitration_lost),
                        "open_loop_fire": int(fired),
                        "selected_donor": (
                            METRIC_LABELS[selected_donor] if selected_donor else ""
                        ),
                        "selected_donee": (
                            METRIC_LABELS[selected_donee] if selected_donee else ""
                        ),
                        "selected_donor_z": _format_float(
                            scores.get(selected_donor) if selected_donor else None
                        ),
                        "selected_donee_z": _format_float(
                            scores.get(selected_donee) if selected_donee else None
                        ),
                        "cooldown_t3_before": cooldown_before["b_c_tage_t3"],
                        "cooldown_t7_before": cooldown_before["b_c_tage_t7"],
                        "cooldown_t3_after": cooldown["b_c_tage_t3"],
                        "cooldown_t7_after": cooldown["b_c_tage_t7"],
                        "IPC_delta_estimate": "NA",
                        "false_fire_count": "NA",
                    }
                )
            assert phase_fire_count <= 1

    summaries: list[dict[str, object]] = []
    for direction, _donors in DIRECTIONS:
        for theta in THETAS:
            summaries.append(
                {
                    "family": workload.family,
                    "workload": workload.workload,
                    "direction": direction,
                    "theta": format(theta, ".1f"),
                    "warmup_phases": warmup_phases,
                    "transition_phases": transition_phases,
                    "sample_phases": len(sample_phases),
                    **counters_by_config[(direction, theta)],
                    "IPC_delta_estimate": "NA",
                    "false_fire_count": "NA",
                }
            )
    return events, summaries


def _write_rows(
    path: Path,
    fieldnames: Sequence[str],
    rows: Sequence[Mapping[str, object]],
    *,
    delimiter: str,
) -> None:
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(
            stream,
            fieldnames=fieldnames,
            delimiter=delimiter,
            lineterminator="\n",
            extrasaction="raise",
        )
        writer.writeheader()
        writer.writerows(rows)


def _aggregate_summaries(
    per_workload: Sequence[Mapping[str, object]],
) -> list[dict[str, object]]:
    count_fields = (
        "sample_phases",
        "evaluable_phases",
        "evaluable_pairs",
        "unavailable_pairs",
        "joint_threshold_crossings",
        "cooldown_blocked_crossings",
        "arbitration_lost_crossings",
        "open_loop_fires",
        "t3_fires",
        "t7_fires",
        "short_history_fallbacks",
        "zero_std_fallbacks",
        "sample_na_fallbacks",
        "not_evaluable_phases",
    )
    result: list[dict[str, object]] = []
    for direction, _donors in DIRECTIONS:
        for theta in THETAS:
            theta_text = format(theta, ".1f")
            rows = [
                row
                for row in per_workload
                if row["direction"] == direction and row["theta"] == theta_text
            ]
            evaluable_workloads = sum(int(row["evaluable_pairs"]) > 0 for row in rows)
            summary: dict[str, object] = {
                "direction": direction,
                "theta": theta_text,
                "workload_count": len(rows),
                "beta_evaluable_workloads": evaluable_workloads,
                "hard_gate_min_workloads": 5,
                "hard_gate_met": int(evaluable_workloads >= 5),
            }
            for field in count_fields:
                summary[field] = sum(int(row[field]) for row in rows)
            summary["IPC_delta_estimate"] = "NA"
            summary["false_fire_count"] = "NA"
            result.append(summary)
    return result


def _read_manifest(path: Path) -> list[WorkloadInput]:
    try:
        stream = path.open("r", encoding="utf-8", newline="")
    except OSError as exc:
        raise ReplayInputError(f"cannot open manifest {path}: {exc}") from exc
    with stream:
        reader = csv.DictReader(stream, delimiter="\t")
        if reader.fieldnames is None:
            raise ReplayInputError(f"{path}: missing manifest header")
        field_set = set(reader.fieldnames)
        workload_field = "workload" if "workload" in field_set else "id"
        path_field = next(
            (
                name
                for name in ("phase_stream_path", "raw_csv", "phase_stream")
                if name in field_set
            ),
            None,
        )
        if "family" not in field_set or workload_field not in field_set or not path_field:
            raise ReplayInputError(
                f"{path}: manifest needs family, workload/id, and "
                "phase_stream_path/raw_csv/phase_stream"
            )
        inputs: list[WorkloadInput] = []
        seen: set[tuple[str, str]] = set()
        for line_number, row in enumerate(reader, start=2):
            family = row["family"].strip()
            workload = row[workload_field].strip()
            raw_text = row[path_field].strip()
            if not family or not workload or not raw_text:
                raise ReplayInputError(f"{path}:{line_number}: empty manifest field")
            key = (family, workload)
            if key in seen:
                raise ReplayInputError(
                    f"{path}:{line_number}: duplicate workload {family}/{workload}"
                )
            seen.add(key)
            raw_csv = Path(raw_text)
            if not raw_csv.is_absolute():
                raw_csv = path.parent / raw_csv
            inputs.append(WorkloadInput(family, workload, raw_csv))
    if not inputs:
        raise ReplayInputError(f"{path}: manifest has no workloads")
    return inputs


def run_replay(inputs: Sequence[WorkloadInput], output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    all_events: list[dict[str, object]] = []
    all_per_workload: list[dict[str, object]] = []
    for workload in inputs:
        phases = read_phase_stream(workload.raw_csv)
        events, summaries = replay_workload(workload, phases)
        all_events.extend(events)
        all_per_workload.extend(summaries)

    _write_rows(
        output_dir / "beta_events.csv",
        EVENT_FIELDS,
        all_events,
        delimiter=",",
    )
    _write_rows(
        output_dir / "beta_theta_per_workload.tsv",
        PER_WORKLOAD_FIELDS,
        all_per_workload,
        delimiter="\t",
    )
    _write_rows(
        output_dir / "beta_theta_summary.tsv",
        SUMMARY_FIELDS,
        _aggregate_summaries(all_per_workload),
        delimiter="\t",
    )


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--input", type=Path, help="one sway_phase_bc_raw.csv")
    source.add_argument(
        "--manifest",
        type=Path,
        help="TSV with family, workload/id, and phase_stream_path/raw_csv",
    )
    parser.add_argument("--family", help="family metadata for --input")
    parser.add_argument("--workload", help="workload metadata for --input")
    parser.add_argument("--output-dir", type=Path, required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    if args.input is not None:
        if not args.family or not args.workload:
            parser.error("--input requires both --family and --workload")
        inputs = [WorkloadInput(args.family, args.workload, args.input)]
    else:
        if args.family or args.workload:
            parser.error("--family/--workload are only valid with --input")
        try:
            inputs = _read_manifest(args.manifest)
        except ReplayInputError as exc:
            parser.error(str(exc))

    try:
        run_replay(inputs, args.output_dir)
    except ReplayInputError as exc:
        parser.error(str(exc))
    print(
        f"replayed_workloads={len(inputs)} output_dir={args.output_dir} "
        "IPC_delta_estimate=NA false_fire_count=NA"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
