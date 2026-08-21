#!/usr/bin/env python3
"""Reproducible Android A/B runner for ArmadaSX2 Phase B.

The runner never supplies games, BIOS, savestates or GS dumps. It only launches a URI already
present on the tester's device and records enough evidence to reject biased measurements.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable


SCHEMA_VERSION = 1
MIN_REPETITIONS = 5
MIN_MEASURE_SECONDS = 30
DEFAULT_TIMEOUT_SECONDS = 60


class ValidationError(RuntimeError):
    pass


@dataclass(frozen=True)
class Arm:
    id: str
    settings: dict[str, Any]
    expected_driver: dict[str, str]
    visual_reference: str | None


@dataclass(frozen=True)
class Scenario:
    scenario_id: str
    package: str
    launch_uri: str
    save_slot: int | None
    warmup_seconds: int
    measure_seconds: int
    cooldown_seconds: int
    repetitions: int
    max_temperature_delta_c: float
    max_visual_hamming_distance: int
    expected_resolution: str | None
    arms: tuple[Arm, Arm]


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValidationError(message)


def load_scenario(path: Path) -> Scenario:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValidationError(f"cannot read scenario: {exc}") from exc
    return parse_scenario(raw)


def parse_scenario(raw: dict[str, Any]) -> Scenario:
    _require(isinstance(raw, dict), "scenario root must be an object")
    _require(raw.get("schema_version") == SCHEMA_VERSION, f"schema_version must be {SCHEMA_VERSION}")
    for key in ("scenario_id", "package", "launch_uri"):
        _require(isinstance(raw.get(key), str) and raw[key].strip(), f"{key} must be a non-empty string")

    repetitions = raw.get("repetitions", MIN_REPETITIONS)
    warmup = raw.get("warmup_seconds", 120)
    measure = raw.get("measure_seconds", 60)
    cooldown = raw.get("cooldown_seconds", 300)
    _require(isinstance(repetitions, int) and repetitions >= MIN_REPETITIONS,
             f"repetitions must be at least {MIN_REPETITIONS} per arm")
    _require(isinstance(warmup, int) and warmup >= 1, "warmup_seconds must be positive")
    _require(isinstance(measure, int) and measure >= MIN_MEASURE_SECONDS,
             f"measure_seconds must be at least {MIN_MEASURE_SECONDS}")
    _require(isinstance(cooldown, int) and cooldown >= 0, "cooldown_seconds cannot be negative")

    save_slot = raw.get("save_slot")
    _require(save_slot is None or (isinstance(save_slot, int) and 0 <= save_slot <= 9),
             "save_slot must be null or 0..9")

    arm_data = raw.get("arms")
    _require(isinstance(arm_data, list) and len(arm_data) == 2, "exactly two arms are required")
    arms: list[Arm] = []
    for item in arm_data:
        _require(isinstance(item, dict), "each arm must be an object")
        arm_id = item.get("id")
        settings = item.get("settings", {})
        expected = item.get("expected_driver", {})
        reference = item.get("visual_reference")
        _require(isinstance(arm_id, str) and arm_id.strip(), "arm id must be non-empty")
        _require(isinstance(settings, dict), f"{arm_id}.settings must be an object")
        _require(isinstance(expected, dict), f"{arm_id}.expected_driver must be an object")
        _require(all(isinstance(key, str) and key for key in settings),
                 f"{arm_id}.settings keys must be non-empty strings")
        _require(all(isinstance(key, str) and isinstance(value, str)
                     for key, value in expected.items()),
                 f"{arm_id}.expected_driver must contain string patterns")
        _require(reference is None or isinstance(reference, str), f"{arm_id}.visual_reference must be a path")
        arms.append(Arm(arm_id, settings, expected, reference))
    _require(arms[0].id != arms[1].id, "arm ids must differ")

    max_temperature_delta_c = raw.get("max_temperature_delta_c", 3.0)
    max_visual_hamming_distance = raw.get("max_visual_hamming_distance", 8)
    expected_resolution = raw.get("expected_resolution")
    _require(isinstance(max_temperature_delta_c, (int, float)) and max_temperature_delta_c >= 0,
             "max_temperature_delta_c must be non-negative")
    _require(isinstance(max_visual_hamming_distance, int) and 0 <= max_visual_hamming_distance <= 64,
             "max_visual_hamming_distance must be 0..64")
    _require(expected_resolution is None or isinstance(expected_resolution, str),
             "expected_resolution must be a string")

    return Scenario(
        scenario_id=raw["scenario_id"],
        package=raw["package"],
        launch_uri=raw["launch_uri"],
        save_slot=save_slot,
        warmup_seconds=warmup,
        measure_seconds=measure,
        cooldown_seconds=cooldown,
        repetitions=repetitions,
        max_temperature_delta_c=float(max_temperature_delta_c),
        max_visual_hamming_distance=max_visual_hamming_distance,
        expected_resolution=expected_resolution,
        arms=(arms[0], arms[1]),
    )


def alternating_order(scenario: Scenario) -> list[Arm]:
    """AB, BA, AB...; every arm still receives exactly repetitions executions."""
    order: list[Arm] = []
    for pair in range(scenario.repetitions):
        first, second = scenario.arms if pair % 2 == 0 else tuple(reversed(scenario.arms))
        order.extend((first, second))
    return order


def setting_type(value: Any) -> tuple[str, str]:
    if isinstance(value, bool):
        return "bool", "true" if value else "false"
    if isinstance(value, int):
        return "int", str(value)
    if isinstance(value, float):
        return "float", format(value, ".9g")
    if isinstance(value, str):
        return "string", value
    raise ValidationError(f"unsupported setting value type: {type(value).__name__}")


class Adb:
    def __init__(self, executable: str = "adb", serial: str | None = None,
                 recorder: Callable[[list[str]], None] | None = None) -> None:
        self.prefix = [executable] + (["-s", serial] if serial else [])
        self.recorder = recorder

    def run(self, args: list[str], timeout: int = DEFAULT_TIMEOUT_SECONDS,
            binary: bool = False) -> str | bytes:
        command = self.prefix + args
        if self.recorder:
            self.recorder(command)
        proc = subprocess.run(command, capture_output=True, timeout=timeout, check=False,
                              text=not binary)
        if proc.returncode != 0:
            stderr = proc.stderr.decode(errors="replace") if binary else proc.stderr
            raise ValidationError(f"adb failed ({proc.returncode}): {' '.join(command)}\n{stderr.strip()}")
        return proc.stdout

    def shell(self, *args: str, timeout: int = DEFAULT_TIMEOUT_SECONDS) -> str:
        return str(self.run(["shell", *args], timeout=timeout)).strip()

    def broadcast(self, package: str, command: str, **extras: Any) -> dict[str, Any]:
        args = ["shell", "am", "broadcast", "--receiver-foreground", "-a",
                f"{package}.action.VALIDATION", "--es", "command", command]
        for key, value in extras.items():
            flag = "--ei" if isinstance(value, int) else "--es"
            args.extend((flag, key, str(value)))
        output = str(self.run(args, timeout=DEFAULT_TIMEOUT_SECONDS))
        return parse_broadcast_result(output)


def parse_broadcast_result(output: str) -> dict[str, Any]:
    matches = re.findall(r"data=(.+)$", output, flags=re.MULTILINE)
    if not matches:
        raise ValidationError(f"validation receiver returned no data: {output.strip()}")
    token = matches[-1].strip()
    try:
        decoded: Any = json.loads(token)
        if isinstance(decoded, str):
            decoded = json.loads(decoded)
        _require(isinstance(decoded, dict), "receiver data is not a JSON object")
        return decoded
    except (json.JSONDecodeError, ValidationError) as exc:
        raise ValidationError(f"invalid receiver JSON: {token}") from exc


def safe_id(value: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "-", value).strip("-.")
    return cleaned or "scenario"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def difference_hash(path: Path) -> str:
    try:
        from PIL import Image
    except ImportError as exc:
        raise ValidationError("Pillow is required for visual references") from exc
    with Image.open(path) as image:
        resized = image.convert("L").resize((9, 8))
        pixels = list(resized.get_flattened_data() if hasattr(resized, "get_flattened_data")
                      else resized.getdata())
    bits = 0
    for row in range(8):
        for column in range(8):
            bits = (bits << 1) | int(pixels[row * 9 + column] > pixels[row * 9 + column + 1])
    return f"{bits:016x}"


def hamming_distance(left: str, right: str) -> int:
    _require(len(left) == len(right), "visual hashes must have equal length")
    return (int(left, 16) ^ int(right, 16)).bit_count()


def parse_key_value_lines(text: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in text.splitlines():
        key, separator, value = line.partition("\t")
        if separator and key:
            values[key] = value
    return values


def normalize_temperature(raw: str) -> float | None:
    try:
        value = float(raw)
    except ValueError:
        return None
    while abs(value) > 200.0:
        value /= 1000.0
    return value if -40.0 <= value <= 200.0 else None


def parse_thermal_status(raw: str) -> int | None:
    for pattern in (r"(?im)^\s*Thermal Status:\s*(\d+)",
                    r"(?im)^\s*mStatus:\s*(\d+)",
                    r"(?im)^\s*Status:\s*(\d+)"):
        match = re.search(pattern, raw)
        if match:
            return int(match.group(1))
    return None


def collect_thermal(adb: Adb) -> dict[str, Any]:
    raw = adb.shell("sh", "-c", "for z in /sys/class/thermal/thermal_zone*; do "
                    "[ -r \"$z/type\" ] && [ -r \"$z/temp\" ] && "
                    "printf '%s\\t%s\\n' \"$(cat \"$z/type\" 2>/dev/null)\" "
                    "\"$(cat \"$z/temp\" 2>/dev/null)\"; done")
    zones = parse_key_value_lines(raw)
    normalized = {name: value for name, text in zones.items()
                  if (value := normalize_temperature(text)) is not None}
    status = adb.shell("dumpsys", "thermalservice")
    return {
        "zones_c": normalized,
        "max_c": max(normalized.values()) if normalized else None,
        "status": parse_thermal_status(status),
        "thermalservice": status[:12000] if status else "unavailable",
    }


def collect_clocks(adb: Adb) -> dict[str, Any]:
    cpu_raw = adb.shell("sh", "-c", "for p in /sys/devices/system/cpu/cpufreq/policy*; do "
                        "[ -r \"$p/scaling_cur_freq\" ] && printf '%s\\t%s\\n' "
                        "\"${p##*/}\" \"$(cat \"$p/scaling_cur_freq\" 2>/dev/null)\"; done")
    gpu_raw = adb.shell("sh", "-c", "for d in /sys/class/devfreq/*; do "
                        "[ -r \"$d/cur_freq\" ] && printf '%s\\t%s\\n' "
                        "\"${d##*/}\" \"$(cat \"$d/cur_freq\" 2>/dev/null)\"; done")
    return {"cpu_khz": parse_key_value_lines(cpu_raw), "devfreq_hz": parse_key_value_lines(gpu_raw)}


def getprop(adb: Adb, key: str) -> str:
    return adb.shell("getprop", key) or "unavailable"


def collect_static_environment(adb: Adb) -> dict[str, Any]:
    return {
        "adb_serial": adb.shell("getprop", "ro.serialno"),
        "manufacturer": getprop(adb, "ro.product.manufacturer"),
        "model": getprop(adb, "ro.product.model"),
        "soc_manufacturer": getprop(adb, "ro.soc.manufacturer"),
        "soc_model": getprop(adb, "ro.soc.model"),
        "android_release": getprop(adb, "ro.build.version.release"),
        "android_sdk": getprop(adb, "ro.build.version.sdk"),
        "build_fingerprint": getprop(adb, "ro.build.fingerprint"),
        "kernel": adb.shell("uname", "-a") or "unavailable",
        "page_size": adb.shell("getconf", "PAGESIZE") or "unavailable",
        "abi": getprop(adb, "ro.product.cpu.abi"),
        "resolution": adb.shell("wm", "size") or "unavailable",
    }


def wait_for(adb: Adb, package: str, active: bool, timeout: int = 90) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        try:
            last = adb.broadcast(package, "status")
            if last.get("ok") and bool(last.get("activeVm")) is active:
                return last
        except ValidationError:
            pass
        time.sleep(2)
    raise ValidationError(f"timed out waiting for activeVm={active}; last={last}")


def apply_arm_settings(adb: Adb, package: str, arm: Arm) -> None:
    for key, raw_value in arm.settings.items():
        value_type, value = setting_type(raw_value)
        result = adb.broadcast(package, "setting", key=key, type=value_type, value=value)
        _require(result.get("ok") is True, f"setting {key} failed: {result}")


def capture_screen(adb: Adb, path: Path) -> dict[str, Any]:
    data = adb.run(["exec-out", "screencap", "-p"], timeout=30, binary=True)
    assert isinstance(data, bytes)
    _require(data.startswith(b"\x89PNG\r\n\x1a\n"), "adb screencap did not return PNG")
    path.write_bytes(data)
    return {"path": path.name, "sha256": sha256_file(path), "dhash": difference_hash(path)}


def driver_matches(expected: dict[str, str], actual: dict[str, Any]) -> list[str]:
    issues: list[str] = []
    for field in ("activeDriver", "driverName", "driverInfo"):
        wanted = expected.get(field)
        if wanted is not None and str(actual.get(field, "")) != wanted:
            issues.append(f"driver {field} expected {wanted!r}, got {actual.get(field)!r}")
        pattern = expected.get(f"{field}Regex")
        if pattern is not None and re.search(pattern, str(actual.get(field, ""))) is None:
            issues.append(f"driver {field} does not match /{pattern}/")
    return issues


def validate_run(run: dict[str, Any], scenario: Scenario, arm: Arm) -> list[str]:
    issues: list[str] = []
    metrics = run.get("metrics", {})
    before = run.get("before", {})
    driver = before.get("app", {}).get("driver") or {}
    if driver.get("unexpected") or not metrics.get("driverAsRequested", True):
        issues.append("requested driver was not active")
    if not driver.get("driverID"):
        issues.append("Vulkan driverID unavailable")
    if not driver.get("driverName"):
        issues.append("Vulkan driverName unavailable")
    if not driver.get("driverVersionRaw"):
        issues.append("Vulkan driver version unavailable")
    app_commit = str(before.get("app", {}).get("buildCommit", "unknown"))
    runner_commit = str(run.get("static", {}).get("runner_commit", "unknown"))
    if run.get("static", {}).get("runner_dirty"):
        issues.append("runner repository has uncommitted changes")
    if app_commit == "unknown":
        issues.append("APK source commit is unknown")
    elif runner_commit != "unknown" and app_commit != runner_commit:
        issues.append(f"APK commit {app_commit} differs from runner commit {runner_commit}")
    if not driver.get("pipelineCacheUUID"):
        issues.append("pipeline cache UUID unavailable")
    issues.extend(driver_matches(arm.expected_driver, driver))
    if int(metrics.get("shaderCompiles", 0)) > 0:
        issues.append("shader compilation occurred during measurement")
    if int(metrics.get("presentErrors", 0)) > 0:
        issues.append("present errors occurred")
    duration = float(metrics.get("durationSeconds", 0.0))
    if duration < scenario.measure_seconds * 0.9 or duration > scenario.measure_seconds * 1.25:
        issues.append(f"measurement duration outside tolerance: {duration:.1f}s")
    for edge in ("before", "after"):
        thermal_status = run.get(edge, {}).get("thermal", {}).get("status")
        if thermal_status is not None and int(thermal_status) >= 4:
            issues.append(f"Android thermal status is severe at {edge} ({thermal_status})")
    if scenario.expected_resolution:
        resolution = run.get("static", {}).get("resolution", "")
        if scenario.expected_resolution not in resolution:
            issues.append(f"unexpected resolution: {resolution}")
    visual = run.get("visual", {})
    distance = visual.get("reference_hamming_distance")
    if distance is None:
        issues.append("visual reference was not compared")
    elif distance > scenario.max_visual_hamming_distance:
        issues.append(f"visual hash distance {distance} exceeds {scenario.max_visual_hamming_distance}")
    return issues


def median_field(runs: list[dict[str, Any]], key: str) -> float | None:
    values = [float(run["metrics"][key]) for run in runs if key in run.get("metrics", {})]
    return statistics.median(values) if values else None


def summarize(scenario: Scenario, runs: list[dict[str, Any]]) -> dict[str, Any]:
    issues: list[str] = []
    by_arm = {arm.id: [run for run in runs if run.get("arm") == arm.id] for arm in scenario.arms}
    aggregate: dict[str, Any] = {}
    for arm in scenario.arms:
        arm_runs = by_arm[arm.id]
        valid_runs = [run for run in arm_runs if not run.get("issues")]
        if len(valid_runs) < scenario.repetitions:
            issues.append(f"{arm.id}: only {len(valid_runs)}/{scenario.repetitions} valid runs")
        hashes = {run.get("before", {}).get("app", {}).get("configSha256") for run in arm_runs}
        hashes.discard(None)
        if len(hashes) > 1:
            issues.append(f"{arm.id}: resolved configuration changed between runs")
        aggregate[arm.id] = {
            "runs": len(arm_runs),
            "valid_runs": len(valid_runs),
            "median_real_fps": median_field(valid_runs, "realFps"),
            "median_presented_fps": median_field(valid_runs, "presentedFps"),
            "median_low1_fps": median_field(valid_runs, "low1Fps"),
            "median_frametime_p95_ms": median_field(valid_runs, "frametimeP95Ms"),
            "median_frametime_p99_ms": median_field(valid_runs, "frametimeP99Ms"),
            "median_stutters": median_field(valid_runs, "stutters"),
        }

    start_temperatures: dict[str, float] = {}
    for arm in scenario.arms:
        values = [run["before"]["thermal"]["max_c"] for run in by_arm[arm.id]
                  if run.get("before", {}).get("thermal", {}).get("max_c") is not None]
        if values:
            start_temperatures[arm.id] = statistics.median(values)
        else:
            issues.append(f"{arm.id}: temperature unavailable")
    if len(start_temperatures) == 2:
        delta = abs(start_temperatures[scenario.arms[0].id] - start_temperatures[scenario.arms[1].id])
        if delta > scenario.max_temperature_delta_c:
            issues.append(f"thermal start delta {delta:.1f}C exceeds {scenario.max_temperature_delta_c:.1f}C")

    for run in runs:
        issues.extend(f"{run.get('run_id')}: {issue}" for issue in run.get("issues", []))

    return {
        "publishable": not issues,
        "issues": sorted(set(issues)),
        "start_temperature_median_c": start_temperatures,
        "aggregate": aggregate,
        "note": "presented FPS is recorded but never used as emulation-performance evidence",
    }


def write_outputs(root: Path, scenario: Scenario, manifest: dict[str, Any],
                  runs: list[dict[str, Any]], summary: dict[str, Any]) -> None:
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    (root / "runs.json").write_text(json.dumps({"runs": runs}, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    (root / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    with (root / "summary.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(("run_id", "arm", "real_fps", "presented_fps", "low1_fps",
                         "frametime_p95_ms", "frametime_p99_ms", "stutters", "valid"))
        for run in runs:
            m = run.get("metrics", {})
            writer.writerow((run.get("run_id"), run.get("arm"), m.get("realFps"),
                             m.get("presentedFps"), m.get("low1Fps"), m.get("frametimeP95Ms"),
                             m.get("frametimeP99Ms"), m.get("stutters"), not run.get("issues")))
    lines = [f"# Android validation — {scenario.scenario_id}", "",
             f"**Publishable:** {'yes' if summary['publishable'] else 'no'}", "",
             "Presented FPS is diagnostic only and is not emulation-performance evidence.", ""]
    if summary["issues"]:
        lines.extend(("## Blocking issues", ""))
        lines.extend(f"- {issue}" for issue in summary["issues"])
        lines.append("")
    lines.extend(("## Raw medians", "",
                  "| Arm | Valid | Real FPS | Presented FPS | 1% low | p95 ms | p99 ms | Stutters |",
                  "|---|---:|---:|---:|---:|---:|---:|---:|"))
    for arm in scenario.arms:
        a = summary["aggregate"][arm.id]
        def f(key: str) -> str:
            value = a.get(key)
            return "n/a" if value is None else f"{value:.3f}"
        lines.append(f"| {arm.id} | {a['valid_runs']}/{scenario.repetitions} | "
                     f"{f('median_real_fps')} | {f('median_presented_fps')} | {f('median_low1_fps')} | "
                     f"{f('median_frametime_p95_ms')} | {f('median_frametime_p99_ms')} | "
                     f"{f('median_stutters')} |")
    (root / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_session(scenario: Scenario, adb: Adb, output_base: Path, scenario_path: Path) -> Path:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    root = output_base / f"{safe_id(scenario.scenario_id)}-{stamp}"
    suffix = 1
    while root.exists():
        root = output_base / f"{safe_id(scenario.scenario_id)}-{stamp}-{suffix}"
        suffix += 1
    root.mkdir(parents=True)
    command_log = root / "adb-commands.txt"
    def record_command(command: list[str]) -> None:
        with command_log.open("a", encoding="utf-8") as handle:
            handle.write(" ".join(command) + "\n")
    adb.recorder = record_command

    static = collect_static_environment(adb)
    if scenario.expected_resolution and scenario.expected_resolution not in static["resolution"]:
        raise ValidationError(f"device resolution {static['resolution']!r} does not match "
                              f"{scenario.expected_resolution!r}")

    adb.shell("am", "start", "-W", "-n", f"{scenario.package}/.BootSplashActivity", timeout=90)
    initial = wait_for(adb, scenario.package, active=False)
    original_options = {item["key"]: item["value"] for item in
                        initial.get("forkConfig", {}).get("options", [])}
    changed_keys = {key for arm in scenario.arms for key in arm.settings}
    invalid_keys = sorted(changed_keys - original_options.keys())
    _require(not invalid_keys, f"scenario uses unknown Fork settings: {invalid_keys}")

    try:
        repository = Path(__file__).resolve().parents[2]
        commit = subprocess.run(("git", "-C", str(repository), "rev-parse", "HEAD"),
                                capture_output=True, text=True, check=False).stdout.strip() or "unknown"
        static["runner_commit"] = commit
        static["runner_dirty"] = bool(subprocess.run(
            ("git", "-C", str(repository), "status", "--porcelain"), capture_output=True,
            text=True, check=False).stdout.strip())
        manifest = {
            "schema_version": SCHEMA_VERSION,
            "created_at": datetime.now(timezone.utc).isoformat(),
            "repository_commit": commit,
            "scenario_sha256": sha256_file(scenario_path),
            "scenario": json.loads(scenario_path.read_text(encoding="utf-8")),
            "device": static,
            "initial_app_environment": initial,
        }
        adb.broadcast(scenario.package, "query", request="benchmark.clear")
        runs: list[dict[str, Any]] = []
        occurrence = {arm.id: 0 for arm in scenario.arms}
        order = alternating_order(scenario)
        for index, arm in enumerate(order):
            occurrence[arm.id] += 1
            run_id = f"{index + 1:02d}-{arm.id}-{occurrence[arm.id]}"
            adb.broadcast(scenario.package, "stop")
            wait_for(adb, scenario.package, active=False)
            apply_arm_settings(adb, scenario.package, arm)
            adb.shell("am", "start", "-W", "-a", "android.intent.action.VIEW", "-d",
                      scenario.launch_uri, "-n", f"{scenario.package}/.Main", timeout=90)
            wait_for(adb, scenario.package, active=True)
            if scenario.save_slot is not None:
                time.sleep(2)
                loaded = adb.broadcast(scenario.package, "load-state", slot=scenario.save_slot)
                _require(loaded.get("ok") is True, f"{run_id}: savestate failed: {loaded}")

            time.sleep(scenario.warmup_seconds)
            before = {
                "app": adb.broadcast(scenario.package, "status"),
                "thermal": collect_thermal(adb),
                "clocks": collect_clocks(adb),
            }
            label = f"{scenario.scenario_id}:{run_id}"
            started = adb.broadcast(scenario.package, "query", request=f"benchmark.begin:{label}")
            _require(started.get("ok") is True, f"{run_id}: benchmark begin failed: {started}")
            time.sleep(scenario.measure_seconds)
            ended = adb.broadcast(scenario.package, "query", request="benchmark.end")
            _require(ended.get("ok") is True, f"{run_id}: benchmark end failed: {ended}")
            all_runs = adb.broadcast(scenario.package, "query", request="benchmark.runs")
            measured_runs = all_runs.get("runs", [])
            _require(isinstance(measured_runs, list) and measured_runs,
                     f"{run_id}: benchmark returned no measured run")
            metrics = measured_runs[-1]
            after = {
                "app": adb.broadcast(scenario.package, "status"),
                "thermal": collect_thermal(adb),
                "clocks": collect_clocks(adb),
            }
            adb.broadcast(scenario.package, "pause")
            time.sleep(0.5)
            screenshot = capture_screen(adb, root / f"{run_id}.png")
            if arm.visual_reference:
                reference = Path(arm.visual_reference)
                if not reference.is_absolute():
                    reference = scenario_path.parent / reference
                _require(reference.is_file(), f"visual reference not found: {reference}")
                reference_hash = difference_hash(reference)
                screenshot["reference"] = str(reference)
                screenshot["reference_dhash"] = reference_hash
                screenshot["reference_hamming_distance"] = hamming_distance(screenshot["dhash"], reference_hash)

            record = {"run_id": run_id, "arm": arm.id, "static": static, "before": before,
                      "metrics": metrics, "after": after, "visual": screenshot}
            record["issues"] = validate_run(record, scenario, arm)
            runs.append(record)
            (root / "runs.partial.json").write_text(json.dumps({"runs": runs}, indent=2,
                                                    ensure_ascii=False) + "\n", encoding="utf-8")
            adb.broadcast(scenario.package, "stop")
            wait_for(adb, scenario.package, active=False)
            if index + 1 < len(order) and scenario.cooldown_seconds:
                time.sleep(scenario.cooldown_seconds)

        summary = summarize(scenario, runs)
        write_outputs(root, scenario, manifest, runs, summary)
        partial = root / "runs.partial.json"
        if partial.exists():
            partial.unlink()
        logcat = str(adb.run(["logcat", "-d", "-v", "threadtime"], timeout=60))
        (root / "logcat.txt").write_text(logcat, encoding="utf-8")
        return root
    finally:
        try:
            adb.broadcast(scenario.package, "query", request="benchmark.end")
        except ValidationError:
            pass
        try:
            adb.broadcast(scenario.package, "stop")
            wait_for(adb, scenario.package, active=False)
            for key in changed_keys:
                value_type, value = setting_type_from_declared(initial, key, original_options[key])
                adb.broadcast(scenario.package, "setting", key=key, type=value_type, value=value)
        except (ValidationError, KeyError):
            pass


def setting_type_from_declared(environment: dict[str, Any], key: str, value: str) -> tuple[str, str]:
    for item in environment.get("forkConfig", {}).get("options", []):
        if item.get("key") == key:
            return str(item.get("type")), value
    raise ValidationError(f"original type unavailable for {key}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scenario", type=Path)
    parser.add_argument("--adb", default="adb", help="adb executable")
    parser.add_argument("--serial", help="adb device serial")
    parser.add_argument("--output", type=Path, default=Path("validation-results"))
    parser.add_argument("--validate-only", action="store_true")
    args = parser.parse_args(argv)
    try:
        scenario = load_scenario(args.scenario)
        if args.validate_only:
            print(json.dumps({"ok": True, "runs_per_arm": scenario.repetitions,
                              "order": [arm.id for arm in alternating_order(scenario)]}))
            return 0
        root = run_session(scenario, Adb(args.adb, args.serial), args.output, args.scenario)
        print(root)
        summary = json.loads((root / "summary.json").read_text(encoding="utf-8"))
        if not summary.get("publishable", False):
            print("RESULT: not publishable; inspect summary.json and report.md", file=sys.stderr)
            return 3
        return 0
    except (ValidationError, subprocess.TimeoutExpired, KeyboardInterrupt) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
