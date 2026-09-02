#!/usr/bin/env python3
"""Procura os estragos que uma resolução de merge produz e o compilador não pega.

Três alphas seguidas quebraram pelo mesmo merge de 214 commits, em estratos diferentes:

  * alpha 21 — "pegar todo o lado do upstream" apagou duas funções do fork em GSDeviceVK.cpp;
    as declarações e oito chamadas sobreviveram, então só o LINKER reclamou;
  * alpha 22 — a resolução por UNIÃO cortou o meio de um bloco KDoc, deixando ` *` órfão;
  * alpha 23 — sob a cascata da 22 havia `lsfgFlowScale` declarado DUAS vezes na mesma data
    class; o Kotlin aceita a declaração repetida e rejeita todo USO dela.

Depois disso ainda apareceu um quarto caso, que NENHUM compilador pega: `hwAat` e
`hwAccurateAlphaTest` eram a mesma opção sob dois nomes, e por os nomes diferirem tudo compilava.
Foi um invariante semântico que achou — e é o terceiro teste daqui.

Falha com código 1 quando encontra qualquer um. Só lê arquivos; não altera nada.

NÃO procura declaração C++ sem definição: a heurística possível aqui dá 12 falsos positivos no
próprio upstream (constexpr no cabeçalho, templates, virtual puro), e um verificador que grita
sem motivo é pior que verificador nenhum. Quem responde essa pergunta é o link, que o gate roda.
"""
import collections
import io
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CODE_DIRS = ("pcsx2", "common", "platforms", "tests")
CODE_EXT = (".cpp", ".h", ".inl", ".kt", ".kts", ".c", ".hpp")

# Propriedades que podem, de propósito, não ir para o JSON. Uma entrada aqui é uma DECISÃO, e
# quem a acrescentar deve dizer por quê — o valor é o motivo, e ele aparece no relatório.
SERIALIZATION_EXEMPT = {}


def code_files():
    for d in CODE_DIRS:
        base = ROOT / d
        if not base.exists():
            continue
        for p in base.rglob("*"):
            if p.suffix in CODE_EXT and ".cxx" not in p.parts and "build" not in p.parts:
                yield p


def check_conflict_markers():
    bad = []
    marker = re.compile(r"^(<{7} |={7}$|>{7} )")
    for p in code_files():
        try:
            for n, line in enumerate(io.open(p, encoding="utf-8", errors="replace"), 1):
                if marker.match(line):
                    bad.append(f"{p.relative_to(ROOT)}:{n}: {line.rstrip()[:60]}")
        except OSError:
            continue
    return bad


def primary_ctor(src, start):
    """Devolve o texto do construtor primário que começa no primeiro '(' após `start`."""
    i = src.index("(", start)
    depth = 0
    j = i
    while j < len(src):
        if src[j] == "(":
            depth += 1
        elif src[j] == ")":
            depth -= 1
            if depth == 0:
                return src[i:j], j
        j += 1
    return None, len(src)


def check_duplicate_properties():
    bad = []
    for p in (ROOT / "platforms").rglob("*.kt"):
        src = io.open(p, encoding="utf-8", errors="replace").read()
        for m in re.finditer(r"\b(?:data\s+)?class\s+([A-Za-z0-9_]+)\s*(?:@\w+\s*)?\(", src):
            ctor, _ = primary_ctor(src, m.end() - 1)
            if ctor is None:
                continue
            names = re.findall(r"\n\s*(?:val|var)\s+([A-Za-z_]\w*)\s*:", ctor)
            for name, count in collections.Counter(names).items():
                if count > 1:
                    bad.append(f"{p.relative_to(ROOT)}: {m.group(1)}.{name} declarada {count}x")
    return bad


def check_settings_serialization():
    """Toda propriedade da data class Settings tem de ser gravada no JSON.

    Uma que não é ou (a) não persiste, e o usuário perde a escolha ao reiniciar, ou (b) está
    morta. `hwAat` e os quatro `useMac*` eram o caso (b) e só apareceram por aqui.
    """
    p = ROOT / "platforms/android/app/src/main/java/com/armsx2/config/Settings.kt"
    if not p.exists():
        return [f"{p} nao encontrado"]
    src = io.open(p, encoding="utf-8", errors="replace").read()

    best = None
    for m in re.finditer(r"\bdata\s+class\s+([A-Za-z0-9_]+)\s*\(", src):
        ctor, _ = primary_ctor(src, m.end() - 1)
        if ctor is None:
            continue
        names = re.findall(r"\n\s*(?:val|var)\s+([A-Za-z_]\w*)\s*:", ctor)
        if best is None or len(names) > len(best[1]):
            best = (m.group(1), names)
    if best is None:
        return ["nenhuma data class encontrada em Settings.kt"]

    written = set(re.findall(r'\bput\("([A-Za-z_]\w*)"\s*,', src))
    bad = []
    for name in best[1]:
        if name in written or name in SERIALIZATION_EXEMPT:
            continue
        bad.append(f"{best[0]}.{name} nunca aparece em put(\"{name}\", ...) — nao vai para o JSON")
    return bad


def main():
    checks = (
        ("marcadores de conflito residuais", check_conflict_markers),
        ("propriedades duplicadas em construtor Kotlin", check_duplicate_properties),
        ("propriedades de Settings fora da serializacao", check_settings_serialization),
    )
    total = 0
    for label, fn in checks:
        problems = fn()
        total += len(problems)
        print(f"  {'FALHA' if problems else 'OK   '}  {label}" + (f" ({len(problems)})" if problems else ""))
        for line in problems[:20]:
            print(f"           {line}")
        if len(problems) > 20:
            print(f"           ... e mais {len(problems) - 20}")
    if SERIALIZATION_EXEMPT:
        print("\n  Excecoes deliberadas de serializacao:")
        for k, why in SERIALIZATION_EXEMPT.items():
            print(f"    {k}: {why}")
    if total:
        print(f"\n{total} problema(s) estrutural(is).")
        return 1
    print("\nNenhum residuo de merge encontrado.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
