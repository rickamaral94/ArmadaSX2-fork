#!/usr/bin/env python3
"""Structural gate for fork-owned RetroArch shader presets.

This deliberately does not pretend to replace glslang/librashader. It catches the failures that
otherwise survive an APK build and only appear on the device: missing relative stages, a wrong pass
count, path traversal, a stage-less .slang file, duplicate descriptor bindings, and feedback names
whose producing pass has no alias.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SHADER_ROOT = ROOT / "platforms/android/app/src/main/assets/shaders"
COUNT_RE = re.compile(r'^\s*shaders\s*=\s*"?(\d+)"?\s*$', re.IGNORECASE | re.MULTILINE)
SHADER_RE = re.compile(r'^\s*shader(\d+)\s*=\s*"([^"]+)"\s*$', re.IGNORECASE | re.MULTILINE)
ALIAS_RE = re.compile(r'^\s*alias(\d+)\s*=\s*"?([A-Za-z_][A-Za-z0-9_]*)"?\s*$', re.IGNORECASE | re.MULTILINE)
BINDING_RE = re.compile(r'\bbinding\s*=\s*(\d+)\b')
FEEDBACK_RE = re.compile(r'\buniform\s+sampler2D\s+([A-Za-z_][A-Za-z0-9_]*Feedback)\b')
STAGE_RE = re.compile(r'^\s*#pragma\s+stage\s+(vertex|fragment)\s*$', re.MULTILINE)


def fail(errors: list[str], path: Path, message: str) -> None:
    errors.append(f"{path.relative_to(ROOT)}: {message}")


def compile_stages(errors: list[str], path: Path, source: str, compiler: str) -> None:
    markers = list(STAGE_RE.finditer(source))
    stages: dict[str, str] = {}
    if markers:
        # librashader metadata pragmas are intentionally omitted from the plain glslang check.
        preamble = "\n".join(
            line for line in source[: markers[0].start()].splitlines()
            if not line.lstrip().startswith(("#pragma name", "#pragma parameter"))
        )
        for index, marker in enumerate(markers):
            end = markers[index + 1].start() if index + 1 < len(markers) else len(source)
            stages[marker.group(1)] = preamble + "\n" + source[marker.end() : end]

    with tempfile.TemporaryDirectory(prefix="armada-shader-") as temp_dir:
        for stage, extension in (("vertex", "vert"), ("fragment", "frag")):
            stage_source = stages.get(stage)
            if stage_source is None:
                continue
            input_path = Path(temp_dir) / f"stage.{extension}"
            output_path = Path(temp_dir) / f"stage.{extension}.spv"
            input_path.write_text(stage_source, encoding="utf-8")
            process = subprocess.run(
                [compiler, "-V", "--target-env", "vulkan1.1", "-S", extension,
                 "-o", str(output_path), str(input_path)],
                capture_output=True,
                text=True,
                check=False,
            )
            if process.returncode != 0:
                detail = (process.stderr or process.stdout).strip().replace("\n", " | ")
                fail(errors, path, f"glslang {stage} compilation failed: {detail}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--require-compiler", action="store_true")
    args = parser.parse_args()

    errors: list[str] = []
    compiler = shutil.which("glslangValidator")
    if args.require_compiler and compiler is None:
        print("glslangValidator is required but was not found", file=sys.stderr)
        return 1

    presets = sorted(SHADER_ROOT.rglob("*.slangp"))
    if not presets:
        print("No bundled .slangp presets found", file=sys.stderr)
        return 1

    for preset in presets:
        text = preset.read_text(encoding="utf-8")
        count_match = COUNT_RE.search(text)
        references = {int(index): relative for index, relative in SHADER_RE.findall(text)}
        aliases = {int(index): alias for index, alias in ALIAS_RE.findall(text)}

        if count_match is None:
            fail(errors, preset, "missing `shaders = N`")
            continue

        count = int(count_match.group(1))
        expected = set(range(count))
        if set(references) != expected:
            fail(errors, preset, f"declares {count} passes but references {sorted(references)}")

        for index, relative in references.items():
            relative_path = Path(relative)
            if relative_path.is_absolute() or ".." in relative_path.parts:
                fail(errors, preset, f"shader{index} escapes its preset directory: {relative}")
                continue

            stage = (preset.parent / relative_path).resolve()
            try:
                stage.relative_to(preset.parent.resolve())
            except ValueError:
                fail(errors, preset, f"shader{index} resolves outside its preset directory")
                continue

            if not stage.is_file():
                fail(errors, preset, f"shader{index} does not exist: {relative}")
                continue

            source = stage.read_text(encoding="utf-8")
            for required in ("#pragma stage vertex", "#pragma stage fragment"):
                if required not in source:
                    fail(errors, stage, f"missing `{required}`")

            bindings = [int(value) for value in BINDING_RE.findall(source)]
            if len(bindings) != len(set(bindings)):
                fail(errors, stage, f"descriptor bindings are not unique: {bindings}")

            known_aliases = {alias for pass_index, alias in aliases.items() if pass_index < index}
            for feedback in FEEDBACK_RE.findall(source):
                producer = feedback.removesuffix("Feedback")
                if producer not in known_aliases:
                    fail(errors, stage, f"feedback `{feedback}` has no earlier aliased pass")

            if compiler is not None:
                compile_stages(errors, stage, source, compiler)

    if errors:
        print("Bundled shader validation failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    suffix = " + glslang" if compiler is not None else " (structural; glslang unavailable)"
    print(f"Bundled shader validation passed: {len(presets)} preset(s){suffix}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
