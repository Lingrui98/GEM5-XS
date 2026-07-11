import csv
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import prepare_stage3_runs as prepare


class PrepareStage3RunsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)
        self.spec_root = self.root / "spec"
        self.cbp_root = self.root / "cbp"
        self.allowed_runs = self.root / "runs"
        self.run_root = self.allowed_runs / "stage3_cal"
        self.spec_root.mkdir()
        self.cbp_root.mkdir()
        self.allowed_runs.mkdir()
        self.manifest = self.root / "source.tsv"
        self.manifest.write_text(
            "# ignored metadata\n"
            "family\tid\tsource_path\tinst_count\tformat\n"
            f"spec17\tomnetpp_21279\t{self.spec_root}/a.zstd\t20000000\tcpt\n"
            f"spec17\tother_spec\t{self.spec_root}/b.zstd\t20000000\tcpt\n"
            f"cbp_int\tint_12_trace\t{self.cbp_root}/int/int_12_trace.gz\t59999603\tcbp2025\n"
            f"cbp_int\tint_13_trace\t{self.cbp_root}/int/int_13_trace.gz\t40000052\tcbp2025\n"
            f"cbp_int\tint_14_trace\t{self.cbp_root}/int/int_14_trace.gz\t39999640\tcbp2025\n"
            f"cbp_int\tint_15_trace\t{self.cbp_root}/int/int_15_trace.gz\t70000157\tcbp2025\n"
            f"champsim_btb\tignored\t{self.root}/ignored.gz\t99999999\tchampsim\n",
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def build_plan(self) -> prepare.PreparationPlan:
        return prepare.build_plan(
            self.manifest,
            self.run_root,
            allowed_run_root=self.allowed_runs,
            spec_root=self.spec_root,
            cbp_root=self.cbp_root,
            enforce_production_set=False,
        )

    def test_filters_counts_and_emits_contract_fields(self) -> None:
        plan = self.build_plan()
        self.assertEqual(len(plan.spec_rows), 2)
        self.assertEqual(
            [row.workload_id for row in plan.cbp_rows],
            ["int_12_trace", "int_13_trace", "int_15_trace"],
        )
        contents = {str(item.relative_path): item.content for item in plan.files}
        cbp_lines = contents["worklists/cbp_cal.lst"].splitlines()
        self.assertEqual(
            cbp_lines[1],
            "int_12_trace\tint/int_12_trace\t0\t20000000\t0\t20000000",
        )
        self.assertNotIn("int_14_trace", contents["worklists/cbp_cal.lst"])
        self.assertNotIn("champsim", contents["workload_manifest.tsv"])

        reader = csv.DictReader(
            io.StringIO(contents["workload_manifest.tsv"]), delimiter="\t"
        )
        rows = list(reader)
        self.assertEqual(tuple(reader.fieldnames or ()), prepare.MANIFEST_FIELDS)
        by_id = {row["id"]: row for row in rows}
        self.assertEqual(by_id["omnetpp_21279"]["legacy_v2_seed"], "true")
        self.assertEqual(by_id["other_spec"]["legacy_v2_seed"], "false")
        self.assertEqual(by_id["int_13_trace"]["total_insts"], "40000000")
        self.assertEqual(
            by_id["int_13_trace"]["phase_stream_path"],
            str(
                self.run_root.resolve()
                / "cbp_int/int_13_trace/sway_phase_bc_raw.csv"
            ),
        )

    def test_configs_leave_instruction_windows_to_scheduler(self) -> None:
        plan = self.build_plan()
        contents = {str(item.relative_path): item.content for item in plan.files}
        for name in ("configs/spec17_cpt.json", "configs/cbp_trace.json"):
            config = json.loads(contents[name])
            self.assertNotIn("XS_MAX_INSTS", config["env"])
            self.assertNotIn("XS_WARMUP_INSTS_NO_SWITCH", config["env"])

    def test_write_plan_creates_all_files(self) -> None:
        plan = self.build_plan()
        prepare.write_plan(plan)
        for output_file in plan.files:
            destination = self.run_root / output_file.relative_path
            self.assertEqual(destination.read_text(encoding="utf-8"), output_file.content)
        self.assertEqual(
            list(self.run_root.rglob("*.tmp")), [], "atomic temp files leaked"
        )

    def test_rejects_output_and_source_path_escapes(self) -> None:
        with self.assertRaisesRegex(ValueError, "run_root is outside"):
            prepare.build_plan(
                self.manifest,
                self.root / "elsewhere",
                allowed_run_root=self.allowed_runs,
                spec_root=self.spec_root,
                cbp_root=self.cbp_root,
                enforce_production_set=False,
            )

        escaped = self.manifest.read_text(encoding="utf-8").replace(
            f"{self.spec_root}/a.zstd", f"{self.root}/outside.zstd"
        )
        self.manifest.write_text(escaped, encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "source_path.*outside"):
            self.build_plan()


if __name__ == "__main__":
    unittest.main()
