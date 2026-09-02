#!/usr/bin/env python3
"""Recusa a publicação quando as validações obrigatórias não passaram NESTE commit.

O problema que resolve: "o CI roda isso" não é evidência de que rodou neste SHA. A alpha 25 foi
publicada de 02aaf475a5, um commit que mexeu só em `.github/workflows/`; a Phase 0.5 tem filtro
de paths e por isso não disparou, e a suíte host — que é quem executa `fork_config_tests` — nunca
rodou naquele SHA. Ali o risco era nulo, porque YAML não afeta teste de C++. Nada no processo,
porém, distinguia esse caso de um em que o risco seria real.

Alargar o filtro da Phase 0.5 resolveria pelo lado errado: faria um job de ~40 minutos rodar a
cada edição de YAML ou de documentação. A regra aqui é mais precisa — a execução de um ANCESTRAL
vale, desde que nenhum arquivo que aquela suíte observa tenha mudado entre os dois commits.

Uso:  require-ci-green.py <runs.json> <sha>
      runs.json = saída de `gh api repos/OWNER/REPO/actions/runs?head_sha=SHA`
"""
import json
import os
import subprocess
import sys

# Cada entrada: nome do workflow -> caminhos cuja mudança EXIGE nova execução. Tem de espelhar o
# filtro `paths:` do workflow correspondente; divergir aqui torna esta verificação mentirosa.
OBRIGATORIOS = {
    "Phase 0.5 ARM64 Correctness": [
        "pcsx2/", "common/", "tests/", "3rdparty/", "cmake/", "CMakeLists.txt",
        "tools/fork/compare-gtest-xml.py", ".github/workflows/phase-0.5-arm64-correctness.yml",
    ],
    "Fork · Android arm64 (debug APK)": [
        "pcsx2/", "common/", "platforms/", "3rdparty/", "cmake/", "CMakeLists.txt",
        ".github/workflows/fork-android-arm64.yml",
    ],
}


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True).stdout.strip()


def conclusao_no_sha(runs, nome):
    for r in runs.get("workflow_runs", []):
        if r.get("name") == nome:
            return r.get("conclusion")
    return None


def ultimo_ancestral_verde(nome, sha):
    """SHA do commit mais recente, ancestral de `sha`, onde `nome` concluiu com sucesso."""
    repo = os.environ.get("REPO")
    if not repo:
        return None
    saida = subprocess.run(
        ["gh", "api", "--paginate",
         f"repos/{repo}/actions/runs?per_page=100&status=success"],
        capture_output=True, text=True)
    if saida.returncode != 0:
        # Sem resposta da API nao da para afirmar que houve execucao verde. Falha fechada: o
        # custo de bloquear uma publicacao legitima e um novo disparo; o de liberar uma sem
        # validacao e publicar codigo que ninguem testou.
        return None
    for linha in saida.stdout.splitlines():
        try:
            dados = json.loads(linha)
        except json.JSONDecodeError:
            continue
        for r in dados.get("workflow_runs", []):
            if r.get("name") != nome or r.get("conclusion") != "success":
                continue
            cand = r.get("head_sha", "")
            # Ancestral de verdade: um sucesso num branch paralelo nao diz nada sobre este commit.
            if cand and subprocess.run(["git", "merge-base", "--is-ancestor", cand, sha]).returncode == 0:
                return cand
    return None


def main():
    runs = json.load(open(sys.argv[1], encoding="utf-8"))
    sha = sys.argv[2]
    problemas = []

    for nome, caminhos in OBRIGATORIOS.items():
        conclusao = conclusao_no_sha(runs, nome)
        if conclusao == "success":
            print(f"  OK       {nome} — passou neste SHA")
            continue
        if conclusao is not None:
            problemas.append(f"{nome}: concluiu como '{conclusao}' neste SHA")
            print(f"  FALHA    {nome} — '{conclusao}' neste SHA")
            continue

        # Nao executou aqui. Um ancestral verde vale se nada relevante mudou desde ele.
        ancestral = ultimo_ancestral_verde(nome, sha)
        if not ancestral:
            problemas.append(f"{nome}: nunca executou com sucesso neste SHA nem em ancestral")
            print(f"  FALHA    {nome} — sem execucao verde neste SHA nem em ancestral")
            continue

        mudados = git("diff", "--name-only", f"{ancestral}..{sha}").splitlines()
        relevantes = [m for m in mudados if any(m.startswith(c) for c in caminhos)]
        if relevantes:
            problemas.append(
                f"{nome}: nao executou neste SHA, e {len(relevantes)} arquivo(s) que ela observa "
                f"mudaram desde {ancestral[:10]}")
            print(f"  FALHA    {nome} — nao executou aqui e o codigo que ela cobre mudou:")
            for m in relevantes[:10]:
                print(f"             {m}")
        else:
            print(f"  OK       {nome} — verde em {ancestral[:10]}; nada que ela observa mudou desde entao")

    if problemas:
        print("\nGATE DE RELEASE VERMELHO. Publicar aqui seria afirmar uma validacao que nao houve:")
        for p in problemas:
            print(f"  · {p}")
        print("\nRode os workflows faltantes neste SHA antes de publicar.")
        return 1

    print("\nGATE DE RELEASE VERDE — as validacoes obrigatorias cobrem este commit.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
