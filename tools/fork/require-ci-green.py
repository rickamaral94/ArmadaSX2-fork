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
# Chaveado pelo ARQUIVO do workflow, não pelo nome de exibição. O nome muda quando alguém edita
# o `name:` do YAML, e uma verificação obrigatória que para de casar por causa disso vira uma
# verificação que aprova tudo em silêncio — o pior modo de falha que um gate pode ter.
#
# `caminhos` espelha o filtro `paths:` do workflow correspondente. Divergir daqui torna esta
# verificação mentirosa, então os dois andam juntos.
OBRIGATORIOS = {
    "phase-0.5-arm64-correctness.yml": {
        "rotulo": "Phase 0.5 ARM64 Correctness",
        "caminhos": [
            "pcsx2/", "common/", "tests/", "3rdparty/", "cmake/", "CMakeLists.txt",
            "tools/fork/compare-gtest-xml.py", ".github/workflows/phase-0.5-arm64-correctness.yml",
        ],
    },
    "fork-android-arm64.yml": {
        "rotulo": "Fork · Android arm64 (debug APK)",
        "caminhos": [
            "pcsx2/", "common/", "platforms/", "3rdparty/", "cmake/", "CMakeLists.txt",
            ".github/workflows/fork-android-arm64.yml",
        ],
    },
}


class ApiIndisponivel(Exception):
    """Não deu para PERGUNTAR à API — distinto de perguntar e a resposta ser 'não existe'.

    A distinção não é preciosismo: na primeira execução real deste gate, um erro de parse foi
    reportado como "nunca executou com sucesso neste SHA nem em ancestral". O veredicto (vermelho)
    estava certo; o MOTIVO estava errado, e um motivo errado manda quem lê o log procurar no lugar
    errado. Ausência de evidência não é evidência de ausência, e o log tem de dizer qual das duas é.
    """


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True).stdout.strip()


def conclusao_no_sha(runs, nome):
    for r in runs.get("workflow_runs", []):
        if r.get("name") == nome:
            return r.get("conclusion")
    return None


def gh_json(caminho):
    """Uma página, um objeto JSON, erros que aparecem.

    A versão anterior usava `gh api --paginate` e fazia `json.loads` LINHA A LINHA, engolindo
    JSONDecodeError com `continue`. Com múltiplas páginas o gh não emite um objeto por linha, então
    toda linha falhava o parse e a função devolvia "nada encontrado" — indistinguível de "não há
    execução verde". Foi assim que a alpha 26 foi bloqueada por um motivo que não era o real.

    Sem `--paginate`: a consulta é por workflow específico e ordenada do mais recente para o mais
    antigo, então o ancestral procurado está nas primeiras posições. Cem cabem de sobra.
    """
    saida = subprocess.run(["gh", "api", caminho], capture_output=True, text=True)
    if saida.returncode != 0:
        raise ApiIndisponivel(f"gh api falhou ({saida.returncode}): {saida.stderr.strip()[:300]}")
    try:
        return json.loads(saida.stdout)
    except json.JSONDecodeError as e:
        raise ApiIndisponivel(f"resposta da API nao e JSON valido: {e}") from e


def ultimo_ancestral_verde(arquivo, sha):
    """SHA do commit mais recente, ancestral de `sha`, onde aquele workflow concluiu com sucesso.

    Levanta ApiIndisponivel quando não deu para perguntar. Devolve None quando perguntou e não há.
    """
    repo = os.environ.get("REPO")
    if not repo:
        raise ApiIndisponivel("variavel REPO nao definida")

    dados = gh_json(f"repos/{repo}/actions/workflows/{arquivo}/runs?status=success&per_page=100")
    execucoes = dados.get("workflow_runs", [])
    print(f"             ({len(execucoes)} execucao(oes) verde(s) de {arquivo} consultada(s))")

    for r in execucoes:
        if r.get("conclusion") != "success":
            continue
        cand = r.get("head_sha", "")
        # Ancestral de verdade: um sucesso num branch paralelo nao diz nada sobre este commit.
        if cand and subprocess.run(
                ["git", "merge-base", "--is-ancestor", cand, sha],
                capture_output=True).returncode == 0:
            return cand
    return None


def main():
    runs = json.load(open(sys.argv[1], encoding="utf-8"))
    sha = sys.argv[2]
    problemas = []

    for arquivo, spec in OBRIGATORIOS.items():
        rotulo, caminhos = spec["rotulo"], spec["caminhos"]
        conclusao = conclusao_no_sha(runs, rotulo)

        if conclusao == "success":
            print(f"  OK       {rotulo} — passou neste SHA")
            continue
        if conclusao is not None:
            problemas.append(f"{rotulo}: concluiu como '{conclusao}' neste SHA")
            print(f"  FALHA    {rotulo} — '{conclusao}' neste SHA")
            continue

        # Nao executou aqui. Um ancestral verde vale se nada relevante mudou desde ele.
        try:
            ancestral = ultimo_ancestral_verde(arquivo, sha)
        except ApiIndisponivel as e:
            # NAO e "nao existe execucao verde" — e "nao consegui perguntar". Continua vermelho,
            # porque sem saber nao da para autorizar, mas o log diz a verdade sobre o motivo.
            problemas.append(f"{rotulo}: NAO FOI POSSIVEL VERIFICAR — {e}")
            print(f"  ERRO     {rotulo} — nao consegui consultar a API: {e}")
            continue

        if not ancestral:
            problemas.append(f"{rotulo}: nunca executou com sucesso neste SHA nem em ancestral")
            print(f"  FALHA    {rotulo} — sem execucao verde neste SHA nem em ancestral")
            continue

        mudados = git("diff", "--name-only", f"{ancestral}..{sha}").splitlines()
        relevantes = [m for m in mudados if any(m.startswith(c) for c in caminhos)]
        if relevantes:
            problemas.append(
                f"{rotulo}: nao executou neste SHA, e {len(relevantes)} arquivo(s) que ela observa "
                f"mudaram desde {ancestral[:10]}")
            print(f"  FALHA    {rotulo} — nao executou aqui e o codigo que ela cobre mudou:")
            for m in relevantes[:10]:
                print(f"             {m}")
        else:
            print(f"  OK       {rotulo} — verde em {ancestral[:10]}; nada que ela observa mudou desde entao")

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
