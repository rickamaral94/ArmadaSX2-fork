#!/usr/bin/env python3
"""Compara dois relatórios JUnit/gtest (baseline x candidate) e reprova em REGRESSÃO.

O ponto: a suíte `recompiler_tests` do ARMSX2 pode ter falhas preexistentes que não são culpa
nossa. Um gate que exige "zero falhas" seria vermelho permanente e logo ignorado — o pior tipo
de gate. Este compara conjuntos:

  regressão  : falha no candidate que NÃO falha no baseline   -> reprova (exit 1)
  correção   : falha no baseline que passa no candidate       -> reporta, não reprova
  preexistente: falha nos dois                                -> reporta, não reprova
  sumiço     : teste no baseline ausente no candidate         -> reprova (exit 1)

O último caso existe porque apagar ou desabilitar um teste que incomoda é a forma mais fácil de
deixar um gate verde, e é exatamente o que o projeto não aceita.

Uso: compare-gtest-xml.py BASELINE.xml CANDIDATE.xml [--json-out resumo.json]
"""
import argparse
import json
import pathlib
import sys
import xml.etree.ElementTree as ET


def carregar(caminho: pathlib.Path) -> tuple[set[str], set[str]]:
	"""Devolve (todos_os_testes, testes_que_falharam) como conjuntos de 'Suite.Caso'."""
	if not caminho.exists():
		raise SystemExit(f"!! {caminho} não existe — a perna correspondente não produziu relatório")
	raiz = ET.parse(caminho).getroot()
	suites = [raiz] if raiz.tag == "testsuite" else list(raiz.iter("testsuite"))
	todos: set[str] = set()
	falhas: set[str] = set()
	for suite in suites:
		for caso in suite.findall("testcase"):
			nome = f"{caso.get('classname') or suite.get('name')}.{caso.get('name')}"
			todos.add(nome)
			# gtest emite <failure> e <error>; ambos contam como falha.
			if caso.find("failure") is not None or caso.find("error") is not None:
				falhas.add(nome)
	return todos, falhas


def bloco(titulo: str, itens: set[str], limite: int = 40) -> None:
	print(f"\n--- {titulo} ({len(itens)}) ---")
	for nome in sorted(itens)[:limite]:
		print(f"  {nome}")
	if len(itens) > limite:
		print(f"  … e mais {len(itens) - limite}")


def main() -> int:
	ap = argparse.ArgumentParser()
	ap.add_argument("baseline", type=pathlib.Path)
	ap.add_argument("candidate", type=pathlib.Path)
	ap.add_argument("--json-out", type=pathlib.Path)
	args = ap.parse_args()

	base_todos, base_falhas = carregar(args.baseline)
	cand_todos, cand_falhas = carregar(args.candidate)

	regressoes = cand_falhas - base_falhas
	correcoes = base_falhas - cand_falhas
	preexistentes = base_falhas & cand_falhas
	sumidos = base_todos - cand_todos
	novos = cand_todos - base_todos

	print("=== Fase 0.5 — comparação de correctness ARM64 ===")
	print(f"baseline : {len(base_todos)} testes, {len(base_falhas)} falhas")
	print(f"candidate: {len(cand_todos)} testes, {len(cand_falhas)} falhas")

	bloco("REGRESSÕES (reprovam)", regressoes)
	bloco("TESTES SUMIDOS do candidate (reprovam)", sumidos)
	bloco("Falhas preexistentes (não reprovam)", preexistentes)
	bloco("Corrigidos pelo candidate", correcoes)
	bloco("Testes novos no candidate", novos)

	if args.json_out:
		args.json_out.write_text(json.dumps({
			"baseline_total": len(base_todos), "baseline_failures": sorted(base_falhas),
			"candidate_total": len(cand_todos), "candidate_failures": sorted(cand_falhas),
			"regressions": sorted(regressoes), "missing": sorted(sumidos),
			"pre_existing": sorted(preexistentes), "fixed": sorted(correcoes),
			"new_tests": sorted(novos),
		}, indent=2, ensure_ascii=False), encoding="utf-8")

	if regressoes or sumidos:
		print(f"\nREPROVADO — {len(regressoes)} regressão(ões), {len(sumidos)} teste(s) sumido(s).")
		return 1
	print("\nAPROVADO — nenhuma regressão e nenhum teste sumido em relação ao baseline.")
	if preexistentes:
		print(f"({len(preexistentes)} falha(s) preexistente(s) herdada(s) do baseline — rastreadas, não bloqueiam.)")
	return 0


if __name__ == "__main__":
	sys.exit(main())
