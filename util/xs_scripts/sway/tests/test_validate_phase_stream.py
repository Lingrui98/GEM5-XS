#!/usr/bin/env python3

from __future__ import annotations

import csv
import io
import math
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPT_DIR))

import validate_phase_stream as validator  # noqa: E402


FIXTURE = Path(__file__).parent / "fixtures" / "valid_single_sample.csv"


def fixture_rows():
    with FIXTURE.open("r", encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def rows_for_run(maxinsts, warmup_insts, phase_size=100_000):
    rows = []
    initial_epoch = 7
    for phase_id in range(1, maxinsts // phase_size + 1):
        start = (phase_id - 1) * phase_size
        end = phase_id * phase_size
        if warmup_insts == 0 or start >= warmup_insts:
            segment = "sample"
            epoch_start = initial_epoch + (1 if warmup_insts > 0 else 0)
            epoch_end = epoch_start
        elif end <= warmup_insts:
            segment = "warmup"
            epoch_start = initial_epoch
            epoch_end = initial_epoch
        else:
            segment = "transition"
            epoch_start = initial_epoch
            epoch_end = initial_epoch + 1

        for template in fixture_rows():
            row = dict(template)
            row.update(
                phase_id=str(phase_id),
                phase_start_inst=str(start),
                phase_end_inst=str(end),
                phase_inst_count=str(phase_size),
                segment=segment,
                replay_eligible=(
                    "false" if segment == "transition" else "true"
                ),
                stats_epoch_start=str(epoch_start),
                stats_epoch_end=str(epoch_end),
            )
            rows.append(row)
    return rows


def write_rows(path, rows, fieldnames=validator.CSV_FIELDS):
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


class ValidatePhaseStreamTest(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tempdir.cleanup)
        self.path = Path(self.tempdir.name) / "sway_phase_bc_raw.csv"

    def assert_invalid(self, rows, message, **kwargs):
        write_rows(self.path, rows)
        with self.assertRaisesRegex(validator.ValidationError, message):
            validator.validate_phase_stream(
                self.path,
                maxinsts=kwargs.get("maxinsts", 100_000),
                warmup_insts=kwargs.get("warmup_insts", 0),
            )

    def test_valid_fixture_derives_expected_metrics(self):
        result = validator.validate_phase_stream(
            FIXTURE, maxinsts=100_000, warmup_insts=0
        )

        self.assertEqual(result.row_count, 23)
        self.assertEqual(result.segment_count("sample"), 1)
        metrics = result.derived_metrics[0]
        self.assertTrue(
            math.isclose(
                metrics["B-b_MBTB_mbtb_bank0_way3"], 32 / 36
            )
        )
        self.assertTrue(
            math.isclose(metrics["B-c_MBTB"], (8 / 120) * (16 / 32))
        )
        self.assertTrue(
            math.isclose(metrics["B-c_TAGE_tage_t3"], (3 / 24) * 0.25)
        )

    def test_aligned_20m_plus_20m_has_200_warmup_and_sample(self):
        write_rows(self.path, rows_for_run(40_000_000, 20_000_000))

        result = validator.validate_phase_stream(
            self.path, maxinsts=40_000_000, warmup_insts=20_000_000
        )

        self.assertEqual(result.segment_count("warmup"), 200)
        self.assertEqual(result.segment_count("transition"), 0)
        self.assertEqual(result.segment_count("sample"), 200)
        self.assertEqual(result.phases[199].stats_epoch_end, 7)
        self.assertEqual(result.phases[200].stats_epoch_start, 8)

    def test_unaligned_warmup_has_one_non_replayable_transition(self):
        write_rows(self.path, rows_for_run(300_000, 150_000))

        result = validator.validate_phase_stream(
            self.path, maxinsts=300_000, warmup_insts=150_000
        )

        self.assertEqual(
            [phase.segment for phase in result.phases],
            ["warmup", "transition", "sample"],
        )
        self.assertFalse(result.phases[1].replay_eligible)
        self.assertEqual(
            (
                result.phases[1].stats_epoch_start,
                result.phases[1].stats_epoch_end,
            ),
            (7, 8),
        )

    def test_zero_warmup_makes_all_phases_sample_without_epoch_jump(self):
        write_rows(self.path, rows_for_run(300_000, 0))

        result = validator.validate_phase_stream(
            self.path, maxinsts=300_000, warmup_insts=0
        )

        self.assertEqual(result.segment_count("sample"), 3)
        self.assertTrue(
            all(phase.stats_epoch_start == 7 for phase in result.phases)
        )

    def test_zero_access_produces_na_metrics(self):
        rows = fixture_rows()
        for row in rows:
            if row["hits_delta"]:
                row["hits_delta"] = "0"
            if row["misses_delta"]:
                row["misses_delta"] = "0"
            if row["updates_delta"]:
                row["updates_delta"] = "0"
            if row["alloc_success_delta"]:
                row["alloc_success_delta"] = "0"
            if row["evicts_delta"]:
                row["evicts_delta"] = "0"
        write_rows(self.path, rows)

        result = validator.validate_phase_stream(
            self.path, maxinsts=100_000, warmup_insts=0
        )

        self.assertTrue(
            all(value is None for value in result.derived_metrics[0].values())
        )

    def test_invalid_run_shape_is_rejected_before_reading(self):
        with self.assertRaisesRegex(validator.ValidationError, "must exceed"):
            validator.validate_phase_stream(
                FIXTURE, maxinsts=20_000_000, warmup_insts=20_000_000
            )

    def test_cli_appends_two_workloads_under_one_validation_tsv_header(self):
        output = Path(self.tempdir.name) / "phase_stream_validation.tsv"
        second_stream = Path(self.tempdir.name) / "workload_b.csv"
        write_rows(second_stream, fixture_rows())
        with redirect_stdout(io.StringIO()):
            statuses = [
                validator.main(
                    [
                        str(stream),
                        "--maxinsts",
                        "100000",
                        "--warmup-insts",
                        "0",
                        "--output-tsv",
                        str(output),
                    ]
                )
                for stream in (FIXTURE, second_stream)
            ]

        self.assertEqual(statuses, [0, 0])
        with output.open("r", encoding="utf-8", newline="") as stream:
            rows = list(csv.DictReader(stream, delimiter="\t"))
        self.assertEqual(len(rows), 2)
        self.assertEqual([row["status"] for row in rows], ["PASS", "PASS"])
        self.assertEqual([row["rows"] for row in rows], ["23", "23"])
        self.assertEqual(
            [row["sample_phases"] for row in rows], ["1", "1"]
        )
        self.assertEqual(
            [row["derived_metrics"] for row in rows], ["14", "14"]
        )
        with output.open("r", encoding="utf-8") as stream:
            header_count = sum(
                line.startswith("status\tstream_path\t") for line in stream
            )
        self.assertEqual(header_count, 1)

    def test_cli_writes_fail_validation_tsv(self):
        output = Path(self.tempdir.name) / "phase_stream_validation.tsv"
        with redirect_stderr(io.StringIO()):
            status = validator.main(
                [
                    str(FIXTURE),
                    "--maxinsts",
                    "100000",
                    "--warmup-insts",
                    "100000",
                    "--output",
                    str(output),
                ]
            )

        self.assertEqual(status, 2)
        with output.open("r", encoding="utf-8", newline="") as stream:
            rows = list(csv.DictReader(stream, delimiter="\t"))
        self.assertEqual(rows[0]["status"], "FAIL")
        self.assertIn("must exceed", rows[0]["error"])

    def test_cli_rejects_incompatible_existing_validation_header(self):
        output = Path(self.tempdir.name) / "phase_stream_validation.tsv"
        output.write_text("wrong\theader\n", encoding="utf-8")
        error_stream = io.StringIO()
        with redirect_stderr(error_stream):
            status = validator.main(
                [
                    str(FIXTURE),
                    "--maxinsts",
                    "100000",
                    "--warmup-insts",
                    "0",
                    "--output",
                    str(output),
                ]
            )

        self.assertEqual(status, 2)
        self.assertIn("incompatible header", error_stream.getvalue())

    def test_duplicate_column_header_is_rejected(self):
        header = list(validator.CSV_FIELDS)
        header[-1] = "resident_entries_end"
        with self.path.open("w", encoding="utf-8", newline="") as stream:
            csv.writer(stream).writerow(header)

        with self.assertRaisesRegex(
            validator.ValidationError, "duplicate CSV header"
        ):
            validator.validate_phase_stream(
                self.path, maxinsts=100_000, warmup_insts=0
            )

    def test_repeated_header_row_is_rejected(self):
        with FIXTURE.open("r", encoding="utf-8") as source:
            lines = source.readlines()
        with self.path.open("w", encoding="utf-8") as stream:
            stream.writelines([lines[0], lines[0], *lines[1:]])

        with self.assertRaisesRegex(
            validator.ValidationError, "repeated CSV header"
        ):
            validator.validate_phase_stream(
                self.path, maxinsts=100_000, warmup_insts=0
            )

    def test_duplicate_phase_scope_key_is_rejected(self):
        rows = fixture_rows()
        rows[1]["scope"] = rows[0]["scope"]
        self.assert_invalid(rows, "duplicate key")

    def test_missing_scope_is_rejected(self):
        rows = fixture_rows()
        rows.pop()
        self.assert_invalid(rows, "expected 23 unique rows")

    def test_non_applicable_field_must_be_empty(self):
        rows = fixture_rows()
        rows[0]["misses_delta"] = "0"
        self.assert_invalid(rows, "non-applicable misses_delta")

    def test_negative_delta_is_rejected(self):
        rows = fixture_rows()
        rows[0]["hits_delta"] = "-1"
        self.assert_invalid(rows, "must be non-negative")

    def test_delta_above_uint64_max_is_rejected(self):
        rows = fixture_rows()
        rows[0]["hits_delta"] = str(1 << 64)
        self.assert_invalid(rows, "exceeds UINT64_MAX")

    def test_sign_bit_delta_is_rejected_as_likely_wrapped(self):
        rows = fixture_rows()
        rows[0]["hits_delta"] = str(1 << 63)
        self.assert_invalid(rows, "likely a wrapped delta")

    def test_largest_non_wrapped_delta_is_accepted(self):
        rows = fixture_rows()
        rows[0]["hits_delta"] = str((1 << 63) - 1)
        write_rows(self.path, rows)

        result = validator.validate_phase_stream(
            self.path, maxinsts=100_000, warmup_insts=0
        )

        self.assertEqual(result.row_count, 23)

    def test_table_evicts_cannot_exceed_successful_allocations(self):
        for scope, evicts, allocs in (
            ("tage_t3", "5", "4"),
            ("ittage_t3", "4", "3"),
        ):
            with self.subTest(scope=scope):
                rows = fixture_rows()
                table = next(row for row in rows if row["scope"] == scope)
                table["evicts_delta"] = evicts
                self.assert_invalid(
                    rows,
                    f"evicts_delta={evicts} exceeds alloc_success_delta={allocs}",
                )

    def test_resident_above_capacity_is_rejected(self):
        rows = fixture_rows()
        rows[0]["resident_entries_end"] = "5"
        self.assert_invalid(rows, "exceeds capacity_entries")

    def test_aggregate_must_equal_child_sums(self):
        rows = fixture_rows()
        aggregate = next(
            row for row in rows if row["scope"] == "ittage_all"
        )
        aggregate["evicts_delta"] = "9"
        self.assert_invalid(rows, "expected child sum 10")

    def test_epoch_must_match_segment_and_warmup_boundary(self):
        rows = rows_for_run(200_000, 100_000)
        for row in rows:
            if row["phase_id"] == "2":
                row["stats_epoch_start"] = "7"
                row["stats_epoch_end"] = "7"
        self.assert_invalid(
            rows,
            "aligned warmup boundary requires one epoch increment",
            maxinsts=200_000,
            warmup_insts=100_000,
        )

    def test_multiple_internal_resets_are_valid_transitions(self):
        rows = rows_for_run(500_000, 0)
        for row in rows:
            phase_id = int(row["phase_id"])
            if phase_id == 2:
                row.update(
                    segment="transition",
                    replay_eligible="false",
                    stats_epoch_start="7",
                    stats_epoch_end="8",
                )
            elif phase_id == 3:
                row.update(stats_epoch_start="8", stats_epoch_end="8")
            elif phase_id == 4:
                row.update(
                    segment="transition",
                    replay_eligible="false",
                    stats_epoch_start="8",
                    stats_epoch_end="9",
                )
            elif phase_id == 5:
                row.update(stats_epoch_start="9", stats_epoch_end="9")
        write_rows(self.path, rows)

        result = validator.validate_phase_stream(
            self.path, maxinsts=500_000, warmup_insts=0
        )

        self.assertEqual(result.segment_count("transition"), 2)

    def test_arbitrary_boundary_reset_keeps_phase_replayable(self):
        rows = rows_for_run(300_000, 0)
        for row in rows:
            if int(row["phase_id"]) >= 2:
                row.update(stats_epoch_start="8", stats_epoch_end="8")
        write_rows(self.path, rows)

        result = validator.validate_phase_stream(
            self.path, maxinsts=300_000, warmup_insts=0
        )

        self.assertEqual(result.segment_count("transition"), 0)
        self.assertTrue(all(phase.replay_eligible for phase in result.phases))

    def test_epoch_decrease_is_rejected(self):
        rows = rows_for_run(200_000, 0)
        for row in rows:
            if row["phase_id"] == "2":
                row.update(stats_epoch_start="6", stats_epoch_end="6")
        self.assert_invalid(
            rows,
            "stats epoch decreases at boundary",
            maxinsts=200_000,
            warmup_insts=0,
        )

    def test_epoch_jump_greater_than_one_is_rejected(self):
        rows = rows_for_run(100_000, 0)
        for row in rows:
            row.update(
                segment="transition",
                replay_eligible="false",
                stats_epoch_start="7",
                stats_epoch_end="9",
            )
        self.assert_invalid(rows, "jumps by more than one within phase")

    def test_epoch_change_requires_transition_and_non_replayable(self):
        rows = rows_for_run(100_000, 0)
        for row in rows:
            row.update(stats_epoch_start="7", stats_epoch_end="8")
        self.assert_invalid(rows, "expected 'transition'")

    def test_unaligned_warmup_requires_epoch_jump(self):
        rows = rows_for_run(200_000, 150_000)
        for row in rows:
            if row["phase_id"] == "2":
                row.update(stats_epoch_start="7", stats_epoch_end="7")
        self.assert_invalid(
            rows,
            "unaligned warmup crossing requires an epoch jump",
            maxinsts=200_000,
            warmup_insts=150_000,
        )

    def test_partial_or_missing_complete_phase_is_rejected(self):
        rows = rows_for_run(200_000, 0)
        rows = [row for row in rows if row["phase_id"] == "1"]
        self.assert_invalid(
            rows,
            "phase ID coverage mismatch",
            maxinsts=200_000,
            warmup_insts=0,
        )


if __name__ == "__main__":
    unittest.main()
