import json
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY / "tools" / "fork"))

import android_validation as validation  # noqa: E402


def scenario_raw(repetitions=5):
    return {
        "schema_version": 1,
        "scenario_id": "phase-a",
        "package": "com.armsx2",
        "launch_uri": "file:///storage/emulated/0/test.gs",
        "warmup_seconds": 1,
        "measure_seconds": 30,
        "cooldown_seconds": 0,
        "repetitions": repetitions,
        "max_temperature_delta_c": 3.0,
        "expected_resolution": "1920x1080",
        "arms": [
            {"id": "A", "settings": {"PipelineCompiler.Mode": "off"}},
            {"id": "B", "settings": {"PipelineCompiler.Mode": "experimental"}},
        ],
    }


def measured_run(run_id, arm, temperature=40.0, config_hash=None):
    return {
        "run_id": run_id,
        "arm": arm,
        "static": {"resolution": "Physical size: 1920x1080", "runner_commit": "a" * 40},
        "before": {
            "app": {
                "configSha256": config_hash or f"config-{arm}",
                "buildCommit": "a" * 40,
                "driver": {
                    "unexpected": False,
                    "activeDriver": "QualcommProprietary",
                    "driverName": "Qualcomm",
                    "driverID": 8,
                    "driverVersionRaw": 51267653,
                    "pipelineCacheUUID": "00112233445566778899aabbccddeeff",
                },
            },
            "thermal": {"max_c": temperature},
        },
        "metrics": {
            "durationSeconds": 30.0,
            "realFps": 30.0,
            "presentedFps": 30.0,
            "low1Fps": 28.0,
            "frametimeP95Ms": 34.0,
            "frametimeP99Ms": 40.0,
            "stutters": 1,
            "shaderCompiles": 0,
            "presentErrors": 0,
            "driverAsRequested": True,
        },
        "visual": {"reference_hamming_distance": 2},
        "issues": [],
    }


class ScenarioTests(unittest.TestCase):
    def test_requires_five_runs_per_arm(self):
        with self.assertRaises(validation.ValidationError):
            validation.parse_scenario(scenario_raw(repetitions=4))

    def test_rejects_measurement_shorter_than_thirty_seconds(self):
        raw = scenario_raw()
        raw["measure_seconds"] = 29
        with self.assertRaises(validation.ValidationError):
            validation.parse_scenario(raw)

    def test_order_alternates_ab_ba_and_keeps_counts(self):
        scenario = validation.parse_scenario(scenario_raw())
        order = [arm.id for arm in validation.alternating_order(scenario)]
        self.assertEqual(order, ["A", "B", "B", "A", "A", "B", "B", "A", "A", "B"])
        self.assertEqual(order.count("A"), 5)
        self.assertEqual(order.count("B"), 5)

    def test_setting_types_do_not_treat_bool_as_int(self):
        self.assertEqual(validation.setting_type(True), ("bool", "true"))
        self.assertEqual(validation.setting_type(2), ("int", "2"))
        self.assertEqual(validation.setting_type(0.5), ("float", "0.5"))


class ProtocolTests(unittest.TestCase):
    def test_parses_android_ordered_broadcast_result(self):
        output = 'Broadcasting: Intent { act=com.armsx2.action.VALIDATION }\n' \
                 'Broadcast completed: result=-1, data="{\\"ok\\":true,\\"activeVm\\":false}"\n'
        self.assertEqual(validation.parse_broadcast_result(output), {"ok": True, "activeVm": False})

    def test_rejects_missing_receiver_data(self):
        with self.assertRaises(validation.ValidationError):
            validation.parse_broadcast_result("Broadcast completed: result=0")

    def test_parses_android_thermal_status(self):
        self.assertEqual(validation.parse_thermal_status("Thermal Status: 4\n"), 4)
        self.assertIsNone(validation.parse_thermal_status("permission denied"))


class VisualTests(unittest.TestCase):
    def test_difference_hash_changes_with_geometry(self):
        try:
            from PIL import Image, ImageDraw
        except ImportError:
            self.skipTest("Pillow not installed")
        with tempfile.TemporaryDirectory() as directory:
            first = Path(directory) / "first.png"
            second = Path(directory) / "second.png"
            a = Image.new("RGB", (64, 64), "black")
            b = Image.new("RGB", (64, 64), "black")
            ImageDraw.Draw(a).rectangle((0, 0, 20, 63), fill="white")
            ImageDraw.Draw(b).rectangle((43, 0, 63, 63), fill="white")
            a.save(first)
            b.save(second)
            ahash = validation.difference_hash(first)
            bhash = validation.difference_hash(second)
            self.assertGreater(validation.hamming_distance(ahash, bhash), 0)
            self.assertEqual(validation.hamming_distance(ahash, ahash), 0)


class GateTests(unittest.TestCase):
    def test_shader_compile_and_visual_difference_invalidate_run(self):
        scenario = validation.parse_scenario(scenario_raw())
        run = measured_run("01-A-1", "A")
        run["metrics"]["shaderCompiles"] = 1
        run["visual"]["reference_hamming_distance"] = 9
        issues = validation.validate_run(run, scenario, scenario.arms[0])
        self.assertTrue(any("shader compilation" in issue for issue in issues))
        self.assertTrue(any("visual hash" in issue for issue in issues))

    def test_apk_and_runner_must_identify_the_same_commit(self):
        scenario = validation.parse_scenario(scenario_raw())
        run = measured_run("01-A-1", "A")
        run["before"]["app"]["buildCommit"] = "b" * 40
        issues = validation.validate_run(run, scenario, scenario.arms[0])
        self.assertTrue(any("differs from runner commit" in issue for issue in issues))

    def test_missing_visual_comparison_is_not_publishable(self):
        scenario = validation.parse_scenario(scenario_raw())
        run = measured_run("01-A-1", "A")
        run["visual"].pop("reference_hamming_distance")
        issues = validation.validate_run(run, scenario, scenario.arms[0])
        self.assertTrue(any("visual reference" in issue for issue in issues))

    def test_thermal_imbalance_blocks_publication(self):
        scenario = validation.parse_scenario(scenario_raw())
        runs = []
        for index in range(5):
            runs.append(measured_run(f"A-{index}", "A", temperature=35.0))
            runs.append(measured_run(f"B-{index}", "B", temperature=42.0))
        summary = validation.summarize(scenario, runs)
        self.assertFalse(summary["publishable"])
        self.assertTrue(any("thermal start delta" in issue for issue in summary["issues"]))

    def test_five_clean_runs_per_arm_are_publishable(self):
        scenario = validation.parse_scenario(scenario_raw())
        runs = []
        for index in range(5):
            runs.append(measured_run(f"A-{index}", "A", temperature=39.0))
            runs.append(measured_run(f"B-{index}", "B", temperature=40.0))
        summary = validation.summarize(scenario, runs)
        self.assertTrue(summary["publishable"], json.dumps(summary["issues"], indent=2))
        self.assertEqual(summary["aggregate"]["A"]["valid_runs"], 5)
        self.assertEqual(summary["aggregate"]["B"]["valid_runs"], 5)


if __name__ == "__main__":
    unittest.main()
