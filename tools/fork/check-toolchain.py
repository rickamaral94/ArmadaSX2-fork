#!/usr/bin/env python3
"""Confere se as versões de toolchain batem entre projeto, CI, script de setup e documentação.

Por que isto existe: o NDK e o CMake são pinados de propósito — um A/B de driver ou de frame
generation não significa nada se o toolchain mudou entre as duas medições. Mas o pino está
escrito em CINCO lugares (build.gradle.kts, dois workflows, o script de setup e a doc), e nada
impedia que um subisse e os outros ficassem para trás em silêncio.

Falha com código 1 quando encontra divergência. Só lê arquivos; não altera nada.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(rel):
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.exists() else None


def find(text, pattern, label, source, out):
    """Registra TODAS as ocorrências: duas linhas discordando dentro do mesmo arquivo é
    exatamente o tipo de coisa que este verificador existe para achar."""
    if text is None:
        out.setdefault(label, []).append((source, None))
        return
    for m in re.finditer(pattern, text):
        out.setdefault(label, []).append((source, m.group(1)))


def main():
    found = {}

    gradle = read("platforms/android/app/build.gradle.kts")
    find(gradle, r'armsx2NdkVersion\s*=.*?orElse\("([\d.]+)"\)', "ndk", "build.gradle.kts", found)
    find(gradle, r'compileSdk\s*=\s*(\d+)', "sdk", "build.gradle.kts", found)

    for wf in ("fork-release.yml", "fork-android-arm64.yml"):
        text = read(f".github/workflows/{wf}")
        find(text, r'"ndk;([\d.]+)"', "ndk", wf, found)
        find(text, r'"cmake;([\d.]+)"', "cmake", wf, found)
        find(text, r'java-version:\s*"?(\d+)"?', "jdk", wf, found)

    setup = read("tools/fork/setup-android-sdk.sh")
    find(setup, r'NDK_VERSION="([\d.]+)"', "ndk", "setup-android-sdk.sh", found)
    find(setup, r'CMAKE_VERSION="([\d.]+)"', "cmake", "setup-android-sdk.sh", found)
    find(setup, r'PLATFORM="platforms;android-(\d+)', "sdk", "setup-android-sdk.sh", found)

    doc = read("docs/fase0-setup.md")
    find(doc, r'NDK `([\d.]+)`', "ndk", "fase0-setup.md", found)
    find(doc, r'CMake `([\d.]+)`', "cmake", "fase0-setup.md", found)
    find(doc, r'JDK (\d+)', "jdk", "fase0-setup.md", found)

    problems = 0
    for label in ("ndk", "cmake", "sdk", "jdk"):
        entries = found.get(label, [])
        if not entries:
            print(f"  {label:6s} NAO ENCONTRADO em lugar nenhum")
            problems += 1
            continue
        values = {v for _, v in entries if v is not None}
        missing = [s for s, v in entries if v is None]
        where = ", ".join(f"{s}={v}" for s, v in entries if v is not None)
        if len(values) == 1 and not missing:
            print(f"  {label:6s} OK  {values.pop():<16s} ({where})")
        else:
            print(f"  {label:6s} DIVERGE  {where}")
            for s in missing:
                print(f"         arquivo ausente: {s}")
            problems += 1

    if problems:
        print(f"\n{problems} divergencia(s) de toolchain.")
        print("As versoes tem de ser identicas: um binario compilado com toolchain diferente do")
        print("pinado invalida qualquer comparacao A/B de driver ou de frame generation.")
        return 1
    print("\nToolchain consistente entre projeto, CI, script de setup e documentacao.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
