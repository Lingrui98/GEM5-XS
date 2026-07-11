#!/usr/bin/env python3

from __future__ import annotations

import csv
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPT_DIR))

import replay_beta as replay  # noqa: E402


RAW_FIELDS = (
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


def direct_phase(
    phase_id: int,
    segment: str,
    value: float,
    overrides: dict[str, float | None] | None = None,
) -> replay.PhaseMetrics:
    values = {metric: value for metric in replay.ALL_METRICS}
    if overrides:
        values.update(overrides)
    return replay.PhaseMetrics(
        phase_id=phase_id,
        phase_start_inst=phase_id * 100,
        phase_end_inst=(phase_id + 1) * 100,
        segment=segment,
        replay_eligible=True,
        values=values,
    )


def variable_warmup(count: int) -> list[replay.PhaseMetrics]:
    return [direct_phase(index, "warmup", float((index % 2) * 2)) for index in range(count)]


def raw_phase_rows(
    phase_id: int,
    segment: str,
    *,
    tage_t3_accesses_zero: bool = False,
) -> list[dict[str, object]]:
    common: dict[str, object] = {
        "schema_version": "1",
        "phase_id": phase_id,
        "phase_start_inst": phase_id * 100,
        "phase_end_inst": (phase_id + 1) * 100,
        "phase_inst_count": 100,
        "segment": segment,
        "replay_eligible": 1,
        "stats_epoch_start": 0 if segment == "warmup" else 1,
        "stats_epoch_end": 0 if segment == "warmup" else 1,
    }
    rows: list[dict[str, object]] = []
    for bank in range(2):
        for way in range(4):
            index = bank * 4 + way
            rows.append(
                {
                    **common,
                    "component": "MBTB",
                    "scope": f"mbtb_bank{bank}_way{way}",
                    "scope_kind": "way",
                    "hits_delta": index + 1,
                    "misses_delta": "",
                    "updates_delta": "",
                    "alloc_success_delta": "",
                    "evicts_delta": index,
                    "resident_entries_end": 10,
                    "capacity_entries": 16,
                }
            )
    rows.append(
        {
            **common,
            "component": "MBTB",
            "scope": "mbtb_all",
            "scope_kind": "component",
            "hits_delta": 36,
            "misses_delta": 4,
            "updates_delta": "",
            "alloc_success_delta": "",
            "evicts_delta": 28,
            "resident_entries_end": 80,
            "capacity_entries": 128,
        }
    )
    for table in range(5):
        rows.append(
            {
                **common,
                "component": "ITTAGE",
                "scope": f"ittage_t{table}",
                "scope_kind": "table",
                "hits_delta": "",
                "misses_delta": "",
                "updates_delta": table + 1,
                "alloc_success_delta": 1,
                "evicts_delta": table,
                "resident_entries_end": 20,
                "capacity_entries": 32,
            }
        )
    rows.append(
        {
            **common,
            "component": "ITTAGE",
            "scope": "ittage_all",
            "scope_kind": "component",
            "hits_delta": 10,
            "misses_delta": 2,
            "updates_delta": "",
            "alloc_success_delta": "",
            "evicts_delta": 10,
            "resident_entries_end": 100,
            "capacity_entries": 160,
        }
    )
    for table in range(8):
        updates = table + 1
        alloc_success = 1
        evicts = table
        resident = 50
        capacity = 100
        if table == 3:
            updates = 0 if tage_t3_accesses_zero else 6
            alloc_success = 0 if tage_t3_accesses_zero else 2
            evicts = 2
        if table == 7:
            updates = 8
            alloc_success = 2
            evicts = 5
            resident = 100
        rows.append(
            {
                **common,
                "component": "TAGE",
                "scope": f"tage_t{table}",
                "scope_kind": "table",
                "hits_delta": "",
                "misses_delta": "",
                "updates_delta": updates,
                "alloc_success_delta": alloc_success,
                "evicts_delta": evicts,
                "resident_entries_end": resident,
                "capacity_entries": capacity,
            }
        )
    return rows


def write_raw(path: Path, phases: list[list[dict[str, object]]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=RAW_FIELDS, lineterminator="\n")
        writer.writeheader()
        for rows in phases:
            writer.writerows(rows)


class MetricDerivationTest(unittest.TestCase):
    def test_derives_locked_b_b_and_b_c_formulas(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            raw = Path(directory) / "raw.csv"
            write_raw(raw, [raw_phase_rows(0, "warmup")])
            phase = replay.read_phase_stream(raw)[0]

        self.assertAlmostEqual(phase.values["b_b_mbtb_bank0_way3"], 4 / 4.5)
        self.assertAlmostEqual(phase.values["b_b_mbtb_bank1_way3"], 8 / 4.5)
        self.assertAlmostEqual(phase.values["b_b_ittage_t2"], 1.0)
        self.assertAlmostEqual(phase.values["b_b_ittage_t3"], 4 / 3)
        self.assertAlmostEqual(phase.values["b_c_tage_t3"], 0.125)
        self.assertAlmostEqual(phase.values["b_c_tage_t7"], 0.5)

    def test_zero_access_produces_na_metric(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            raw = Path(directory) / "raw.csv"
            write_raw(
                raw,
                [raw_phase_rows(0, "warmup", tage_t3_accesses_zero=True)],
            )
            phase = replay.read_phase_stream(raw)[0]

        self.assertIsNone(phase.values["b_c_tage_t3"])

    def test_duplicate_header_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            raw = Path(directory) / "raw.csv"
            raw.write_text("phase_id,phase_id\n0,0\n", encoding="utf-8")
            with self.assertRaisesRegex(replay.ReplayInputError, "duplicate CSV"):
                replay.read_phase_stream(raw)


class ReferenceDistributionTest(unittest.TestCase):
    def test_uses_population_stddev_and_last_128_finite_values(self) -> None:
        phases = [direct_phase(index, "warmup", 1000.0) for index in range(32)]
        phases.extend(
            direct_phase(index + 32, "warmup", float((index % 2) * 2))
            for index in range(128)
        )
        references = replay._build_references(phases)
        reference = references["b_b_mbtb_bank0_way3"]
        self.assertEqual(reference.sample_count, 128)
        self.assertAlmostEqual(reference.mean, 1.0)
        self.assertAlmostEqual(reference.stddev, 1.0)

    def test_short_history_is_not_evaluable(self) -> None:
        phases = variable_warmup(31)
        phases.append(direct_phase(31, "sample", 0.0))
        _, summaries = replay.replay_workload(
            replay.WorkloadInput("test", "short", Path("unused")), phases
        )
        row = next(
            item
            for item in summaries
            if item["direction"] == "MBTB->TAGE" and item["theta"] == "0.5"
        )
        self.assertEqual(row["short_history_fallbacks"], 4)
        self.assertEqual(row["evaluable_pairs"], 0)
        self.assertEqual(row["unavailable_pairs"], 4)
        self.assertEqual(row["not_evaluable_phases"], 1)
        self.assertEqual(row["open_loop_fires"], 0)

    def test_zero_std_is_not_evaluable(self) -> None:
        phases = [direct_phase(index, "warmup", 1.0) for index in range(32)]
        phases.append(direct_phase(32, "sample", 0.0))
        _, summaries = replay.replay_workload(
            replay.WorkloadInput("test", "constant", Path("unused")), phases
        )
        row = next(
            item
            for item in summaries
            if item["direction"] == "ITTAGE->TAGE" and item["theta"] == "0.5"
        )
        self.assertEqual(row["zero_std_fallbacks"], 4)
        self.assertEqual(row["evaluable_pairs"], 0)
        self.assertEqual(row["unavailable_pairs"], 4)
        self.assertEqual(row["not_evaluable_phases"], 1)
        self.assertEqual(row["open_loop_fires"], 0)


class ArbitrationTest(unittest.TestCase):
    def test_strict_threshold_stable_ties_and_per_donee_cooldown(self) -> None:
        phases = variable_warmup(32)
        phases.extend(direct_phase(32 + index, "sample", 0.0) for index in range(9))
        # Donors at 0 have z=1. Donees must be high rather than cold.
        phases = [
            phase
            if phase.segment == "warmup"
            else direct_phase(
                phase.phase_id,
                "sample",
                0.0,
                {"b_c_tage_t3": 2.0, "b_c_tage_t7": 2.0},
            )
            for phase in phases
        ]
        events, summaries = replay.replay_workload(
            replay.WorkloadInput("test", "cooldown", Path("unused")), phases
        )
        selected = [
            event
            for event in events
            if event["direction"] == "MBTB->TAGE"
            and event["theta"] == "0.5"
            and event["open_loop_fire"] == 1
        ]
        self.assertEqual(
            [event["selected_donee"] for event in selected],
            ["TAGE.T3", "TAGE.T7", "TAGE.T3"],
        )
        self.assertTrue(
            all(event["selected_donor"] == "MBTB.bank0.way3" for event in selected)
        )
        row = next(
            item
            for item in summaries
            if item["direction"] == "MBTB->TAGE" and item["theta"] == "0.5"
        )
        self.assertEqual(row["joint_threshold_crossings"], 9)
        self.assertEqual(row["cooldown_blocked_crossings"], 6)
        self.assertEqual(row["open_loop_fires"], 3)
        self.assertEqual(row["t3_fires"], 2)
        self.assertEqual(row["t7_fires"], 1)

        losing_row = next(
            item
            for item in summaries
            if item["direction"] == "ITTAGE->TAGE" and item["theta"] == "0.5"
        )
        self.assertEqual(losing_row["joint_threshold_crossings"], 9)
        self.assertEqual(losing_row["cooldown_blocked_crossings"], 6)
        self.assertEqual(losing_row["arbitration_lost_crossings"], 3)
        self.assertEqual(losing_row["open_loop_fires"], 0)

        phase_events: dict[tuple[int, str], list[dict[str, object]]] = {}
        for event in events:
            key = (int(event["phase_id"]), str(event["theta"]))
            phase_events.setdefault(key, []).append(event)
        self.assertTrue(
            all(
                sum(int(event["open_loop_fire"]) for event in rows) <= 1
                for rows in phase_events.values()
            )
        )

        mbtb_events = [
            event
            for event in events
            if event["direction"] == "MBTB->TAGE" and event["theta"] == "0.5"
        ]
        cooldown_states = [
            (
                event["cooldown_t3_before"],
                event["cooldown_t7_before"],
                event["cooldown_t3_after"],
                event["cooldown_t7_after"],
            )
            for event in mbtb_events
        ]
        self.assertEqual(
            cooldown_states,
            [
                (0, 0, 8, 0),
                (7, 0, 7, 20),
                (6, 19, 6, 19),
                (5, 18, 5, 18),
                (4, 17, 4, 17),
                (3, 16, 3, 16),
                (2, 15, 2, 15),
                (1, 14, 1, 14),
                (0, 13, 8, 13),
            ],
        )

    def test_comparison_is_strictly_greater_than_theta(self) -> None:
        phases = variable_warmup(32)
        phases.append(
            direct_phase(
                32,
                "sample",
                0.5,
                {"b_c_tage_t3": 1.5, "b_c_tage_t7": 1.5},
            )
        )
        _, summaries = replay.replay_workload(
            replay.WorkloadInput("test", "strict", Path("unused")), phases
        )
        row = next(
            item
            for item in summaries
            if item["direction"] == "MBTB->TAGE" and item["theta"] == "0.5"
        )
        self.assertEqual(row["joint_threshold_crossings"], 0)
        self.assertEqual(row["open_loop_fires"], 0)

    def test_na_candidate_only_removes_its_pairs(self) -> None:
        phases = variable_warmup(32)
        phases.append(
            direct_phase(
                32,
                "sample",
                0.0,
                {"b_c_tage_t3": None, "b_c_tage_t7": 2.0},
            )
        )
        events, summaries = replay.replay_workload(
            replay.WorkloadInput("test", "na", Path("unused")), phases
        )
        row = next(
            item
            for item in summaries
            if item["direction"] == "MBTB->TAGE" and item["theta"] == "0.5"
        )
        self.assertEqual(row["sample_na_fallbacks"], 1)
        self.assertEqual(row["evaluable_pairs"], 2)
        self.assertEqual(row["unavailable_pairs"], 2)
        self.assertEqual(row["not_evaluable_phases"], 0)
        self.assertEqual(row["open_loop_fires"], 1)
        event = next(
            item
            for item in events
            if item["direction"] == "MBTB->TAGE" and item["theta"] == "0.5"
        )
        self.assertEqual(event["selected_donee"], "TAGE.T7")
        self.assertEqual(event["evaluable_pair_count"], 2)
        self.assertIn("TAGE.T3=sample_na", event["unavailable_candidates"])

    def test_global_arbitration_uses_highest_donor_z(self) -> None:
        phases = variable_warmup(32)
        phases.append(
            direct_phase(
                32,
                "sample",
                0.4,
                {
                    "b_b_ittage_t2": 0.0,
                    "b_b_ittage_t3": 0.0,
                    "b_c_tage_t3": 2.0,
                    "b_c_tage_t7": 2.0,
                },
            )
        )
        events, _ = replay.replay_workload(
            replay.WorkloadInput("test", "global", Path("unused")), phases
        )
        rows = [event for event in events if event["theta"] == "0.5"]
        self.assertEqual(sum(int(row["open_loop_fire"]) for row in rows), 1)
        winner = next(row for row in rows if row["open_loop_fire"] == 1)
        loser = next(row for row in rows if row["direction"] == "MBTB->TAGE")
        self.assertEqual(winner["direction"], "ITTAGE->TAGE")
        self.assertEqual(winner["selected_donor"], "ITTAGE.T2")
        self.assertEqual(loser["arbitration_lost_crossing"], 1)


class ArtifactTest(unittest.TestCase):
    def test_outputs_required_artifacts_and_explicit_na_fields(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = root / "raw.csv"
            output = root / "output"
            raw_phases = [
                raw_phase_rows(index, "warmup") for index in range(32)
            ]
            raw_phases.append(raw_phase_rows(32, "sample"))
            write_raw(raw, raw_phases)
            replay.run_replay(
                [replay.WorkloadInput("spec17", "fixture", raw)], output
            )

            expected = {
                "beta_events.csv",
                "beta_theta_per_workload.tsv",
                "beta_theta_summary.tsv",
            }
            self.assertEqual({path.name for path in output.iterdir()}, expected)
            with (output / "beta_theta_per_workload.tsv").open(
                encoding="utf-8", newline=""
            ) as stream:
                row = next(csv.DictReader(stream, delimiter="\t"))
            self.assertEqual(row["IPC_delta_estimate"], "NA")
            self.assertEqual(row["false_fire_count"], "NA")


class HardGateSummaryTest(unittest.TestCase):
    @staticmethod
    def summary_row(workload: str, evaluable_pairs: int) -> dict[str, object]:
        return {
            "family": "spec17",
            "workload": workload,
            "direction": "MBTB->TAGE",
            "theta": "0.5",
            "sample_phases": 1,
            # Deliberately non-zero: the hard gate must use legal pairs, not
            # the older phase-only proxy.
            "evaluable_phases": 1,
            "evaluable_pairs": evaluable_pairs,
            "unavailable_pairs": 4 - evaluable_pairs,
            "joint_threshold_crossings": 0,
            "cooldown_blocked_crossings": 0,
            "arbitration_lost_crossings": 0,
            "open_loop_fires": 0,
            "t3_fires": 0,
            "t7_fires": 0,
            "short_history_fallbacks": 0,
            "zero_std_fallbacks": 0,
            "sample_na_fallbacks": 0,
            "not_evaluable_phases": int(evaluable_pairs == 0),
        }

    def test_hard_gate_fails_below_five_evaluable_workloads(self) -> None:
        rows = [self.summary_row(f"w{index}", 1) for index in range(4)]
        rows.append(self.summary_row("phase_proxy_only", 0))
        summary = next(
            row
            for row in replay._aggregate_summaries(rows)
            if row["direction"] == "MBTB->TAGE" and row["theta"] == "0.5"
        )
        self.assertEqual(summary["workload_count"], 5)
        self.assertEqual(summary["beta_evaluable_workloads"], 4)
        self.assertEqual(summary["hard_gate_met"], 0)

    def test_hard_gate_passes_at_exactly_five_evaluable_workloads(self) -> None:
        rows = [self.summary_row(f"w{index}", 1) for index in range(5)]
        summary = next(
            row
            for row in replay._aggregate_summaries(rows)
            if row["direction"] == "MBTB->TAGE" and row["theta"] == "0.5"
        )
        self.assertEqual(summary["beta_evaluable_workloads"], 5)
        self.assertEqual(summary["hard_gate_met"], 1)


if __name__ == "__main__":
    unittest.main()
