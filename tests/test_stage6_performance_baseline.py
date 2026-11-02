import statistics
import json
import sys
import tempfile
import unittest
from decimal import Decimal
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import stage6_performance_baseline as baseline  # noqa: E402

VALUES = {
    "audio_device_callback_budget": [0.001, 0.001, 0.001, 0.0009, 0.001],
    "file_watcher_delivery": [0.1935, 0.246, 0.2016, 0.2112, 0.2117],
    "playback_load_seek": [2.4399, 2.4208, 2.5149, 2.3654, 2.0723],
    "scan_terminal_throughput": [98.04058056, 122.2632144, 125.6292297, 121.4049859, 123.0751811],
    "main_thread_frame_budget": [3.4459, 3.2092, 3.3413, 3.4424, 3.5685],
}
BOUNDS = dict(zip(VALUES, map(Decimal, ("0.001120", "0.264932", "2.879398", "91.903101", "3.802742"))))


def make_metric(name):
    spec, values = baseline.METRIC_SPECS[name], VALUES[name]
    ordered, sigma = sorted(values), statistics.pstdev(values)
    metric = {"status": "measured", "name": spec["name"], "metric": spec["metric"], "unit": spec["unit"],
        "rounds": [{"value": value, "raw_evidence": f"round-{i}.txt"} for i, value in enumerate(values, 1)],
        "statistics": {"median": ordered[2], "p95": ordered[-1], "max": ordered[-1]},
        "noise_budget": {"derived_from_rounds": 5, "estimator": "population", "standard_deviation": sigma,
                         "coefficient_of_variation": sigma / ordered[2]}}
    if name == "main_thread_frame_budget":
        metric.update(product_gate={"direction": "upper", "value": 16.0},
                      empirical_baseline_limit={"direction": "upper", "value": float(BOUNDS[name])})
    else: metric["limit"] = {"direction": spec["direction"], "value": float(BOUNDS[name])}
    return metric


def make_manifest():
    return {"schema": 1, "entry_commit": "0" * 40,
        "environment": {"machine": "test-machine", "compiler": "MSVC 19.44",
                        "build_configuration": "Public diagnostics Debug x64"},
        "metrics": {name: make_metric(name) for name in baseline.REQUIRED_METRICS}}


def make_run_index(values=None):
    values = values or VALUES
    probe_names = {
        "audio_device_callback_budget": "TestStage6AudioDeviceProbe",
        "file_watcher_delivery": "TestStage6FileWatcherProbe",
        "playback_load_seek": "TestStage6PlaybackLoadSeekProbe",
        "scan_terminal_throughput": "TestStage6ScanTerminalThroughputProbe",
        "main_thread_frame_budget": "TestStage6MainThreadFrameBudgetProbe",
    }
    payloads = {
        "audio_device_callback_budget": lambda value: {"status": "measured", "callback_execution": {"p95_ms": value}},
        "file_watcher_delivery": lambda value: {"status": "measured", "delivery": {"p95_ms": value}},
        "playback_load_seek": lambda value: {"status": "measured", "value_ms": value},
        "scan_terminal_throughput": lambda value: {"status": "measured", "value_files_per_second": value},
        "main_thread_frame_budget": lambda value: {"status": "measured", "value_ms": value},
    }
    return {"schema": 1, "kind": "stage6-performance-run-index", "status": "complete",
            "rounds_requested": 5, "probes": {
                probe_names[name]: {"rounds": [
                    {"round": index, "exit_code": 0, "status": "measured", "payload": payloads[name](value)}
                    for index, value in enumerate(metric_values, 1)]}
                for name, metric_values in values.items()}}


class Stage6PerformanceBaselineTests(unittest.TestCase):
    def assertInvalid(self, manifest, text):
        self.assertIn(text, "\n".join(baseline.validate_manifest(manifest)))

    def test_accepts_valid_minimal_manifest(self):
        self.assertEqual([], baseline.validate_manifest(make_manifest()))

    def test_population_sigma_and_directed_six_decimal_bounds(self):
        for name, values in VALUES.items():
            result = baseline.derive_bound(values, baseline.METRIC_SPECS[name]["direction"])
            self.assertEqual(BOUNDS[name], result); self.assertEqual(6, -result.as_tuple().exponent)

    def test_rejects_missing_required_or_unknown_metric(self):
        manifest = make_manifest(); del manifest["metrics"]["main_thread_frame_budget"]
        self.assertInvalid(manifest, "missing required metric `main_thread_frame_budget`")
        manifest = make_manifest(); manifest["metrics"]["unknown_probe"] = make_metric("playback_load_seek")
        self.assertInvalid(manifest, "unknown metric `unknown_probe`")

    def test_rejects_fewer_than_five_rounds_or_noise_samples(self):
        manifest = make_manifest(); metric = manifest["metrics"]["file_watcher_delivery"]
        metric["rounds"].pop(); metric["noise_budget"]["derived_from_rounds"] = 4
        self.assertInvalid(manifest, "requires exactly 5 measured rounds")
        self.assertInvalid(manifest, "noise budget must derive from exactly 5 rounds")

    def test_rejects_non_finite_measurement_or_statistic(self):
        manifest = make_manifest(); metric = manifest["metrics"]["main_thread_frame_budget"]
        metric["rounds"][0]["value"] = float("nan"); metric["statistics"]["p95"] = float("inf")
        self.assertInvalid(manifest, "round 1 value must be finite"); self.assertInvalid(manifest, "statistic `p95` must be finite")

    def test_direction_aware_bounds_and_frame_dual_gates(self):
        manifest = make_manifest(); manifest["metrics"]["audio_device_callback_budget"]["statistics"]["max"] = 1.0
        self.assertInvalid(manifest, "maximum 1.0 exceeds upper bound 0.00112")
        manifest = make_manifest(); manifest["metrics"]["scan_terminal_throughput"]["statistics"]["median"] = 89.0
        self.assertInvalid(manifest, "median 89.0 is below lower bound 91.903101")
        manifest = make_manifest(); frame = manifest["metrics"]["main_thread_frame_budget"]
        frame["statistics"]["max"] = 4.0; errors = "\n".join(baseline.validate_manifest(manifest))
        self.assertIn("frame empirical baseline maximum 4.0 exceeds 3.802742", errors)
        self.assertNotIn("frame product maximum", errors)
        frame["statistics"]["max"] = 17.0; errors = "\n".join(baseline.validate_manifest(manifest))
        self.assertIn("frame product maximum 17.0 exceeds 16.0", errors)
        self.assertIn("frame empirical baseline maximum 17.0 exceeds 3.802742", errors)

    def test_rejects_identity_direction_types_extra_and_stale_values(self):
        manifest = make_manifest(); metric = manifest["metrics"]["playback_load_seek"]
        metric["unit"] = "items"; metric["statistics"]["median"] += .1; metric["extra"] = True
        errors = "\n".join(baseline.validate_manifest(manifest))
        self.assertIn("unit must be", errors); self.assertIn("statistics do not match frozen rounds", errors)
        self.assertIn("unexpected fields", errors)
        manifest = make_manifest(); metric = manifest["metrics"]["audio_device_callback_budget"]
        metric["rounds"][0]["value"] = True; metric["noise_budget"]["derived_from_rounds"] = 5.0
        metric["limit"]["value"] = True; errors = "\n".join(baseline.validate_manifest(manifest))
        self.assertIn("round 1 value must be finite", errors); self.assertIn("noise budget must derive", errors)
        self.assertIn("limit value must be finite", errors)
        manifest = make_manifest(); manifest["metrics"]["scan_terminal_throughput"]["limit"]["direction"] = "upper"
        self.assertInvalid(manifest, "limit direction must be `lower`")

    def test_rejects_missing_entry_environment_or_evidence(self):
        manifest = make_manifest(); del manifest["entry_commit"]
        self.assertInvalid(manifest, "missing valid entry commit")
        manifest = make_manifest(); del manifest["environment"]["machine"]
        self.assertInvalid(manifest, "missing environment field `machine`")
        manifest = make_manifest(); manifest["metrics"]["playback_load_seek"]["rounds"][0]["raw_evidence"] = "C:/user/evidence.json"
        self.assertInvalid(manifest, "raw evidence must be a portable relative path")

    def test_cli_only_validates_an_existing_manifest(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "baseline.json"
            path.write_text(__import__("json").dumps(make_manifest()), encoding="utf-8")
            self.assertEqual(0, baseline.main(["--manifest", str(path)]))

    def test_current_five_probe_run_is_compared_to_frozen_limits(self):
        manifest = make_manifest()
        self.assertEqual([], baseline.validate_run_index(manifest, make_run_index()))

        regressed = {name: list(values) for name, values in VALUES.items()}
        regressed["playback_load_seek"] = [16.5027] * 5
        regressed["scan_terminal_throughput"] = [60.8264] * 5
        errors = "\n".join(baseline.validate_run_index(manifest, make_run_index(regressed)))
        self.assertIn("playback_load_seek maximum 16.5027 exceeds upper bound 2.879398", errors)
        self.assertIn("scan_terminal_throughput median 60.8264 is below lower bound 91.903101", errors)

    def test_cmake_registers_current_run_gate_only_with_explicit_evidence(self):
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("ARIS_STAGE6_PERFORMANCE_RUN_INDEX", cmake)
        self.assertIn("TestStage6PerformanceCurrentRun", cmake)
        self.assertIn("--run-index", cmake)

    def test_canonical_manifest_freezes_only_approved_r3_values(self):
        path = ROOT / "docs/plans/refactor/stage-6-performance-baseline.json"
        self.assertTrue(path.is_file())
        text = path.read_text(encoding="utf-8"); manifest = json.loads(text)
        self.assertEqual([], baseline.validate_manifest(manifest))
        self.assertEqual("anonymized-reference-host", manifest["environment"]["machine"])
        self.assertNotIn(".local/", text)
        for name, values in VALUES.items():
            self.assertEqual(values, [item["value"] for item in manifest["metrics"][name]["rounds"]])
            self.assertTrue(all(item["raw_evidence"].startswith("historical-evidence/five-probe-r3/")
                                for item in manifest["metrics"][name]["rounds"]))
        self.assertNotIn("five-probe-r1", text); self.assertNotIn("five-probe-r2", text)
        for literal in ("0.001120", "0.264932", "2.879398", "91.903101", "3.802742", "16.000000"):
            self.assertIn(f'"value": {literal}', text)


if __name__ == "__main__": unittest.main()
