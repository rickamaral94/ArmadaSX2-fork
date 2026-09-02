#!/usr/bin/env bash
#
# Testa a lógica de estado de árvore do gate contra um repositório TEMPORÁRIO.
#
# Por que num repositório descartável: a única forma honesta de verificar "o gate reprova com
# árvore suja?" é sujar uma árvore de propósito, e fazer isso no repositório de trabalho arrisca
# perder alteração real. Aqui cada cenário é montado do zero em /tmp e apagado no fim.
#
# O defeito que motivou este teste: o gate imprimia o aviso de árvore suja E terminava com
# código 0. Um relatório vermelho que sai com sucesso é invisível para qualquer automação — e
# quem lê exit code é justamente quem decide se marca a tag.
set -uo pipefail

CHECK="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/check-tree-state.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

falhas=0
cenario() {
	local nome="$1" esperado="$2"
	local obtido; "${CHECK}" "${TMP}" >/dev/null 2>&1; obtido=$?
	if [ "${obtido}" = "${esperado}" ]; then
		printf '  ok    %-42s exit=%s\n' "${nome}" "${obtido}"
	else
		printf '  FALHA %-42s exit=%s (esperado %s)\n' "${nome}" "${obtido}" "${esperado}"
		falhas=$((falhas + 1))
	fi
}

git -C "${TMP}" init -q
git -C "${TMP}" config user.email teste@exemplo
git -C "${TMP}" config user.name Teste
echo "conteudo original" > "${TMP}/rastreado.txt"
git -C "${TMP}" add rastreado.txt
git -C "${TMP}" commit -qm "commit inicial"

echo "Estado de arvore — cinco cenarios:"

cenario "1. arvore limpa" 0

echo "modificado" >> "${TMP}/rastreado.txt"
cenario "2. rastreado modificado" 1
git -C "${TMP}" checkout -- rastreado.txt

echo "novo" > "${TMP}/nao-rastreado.txt"
cenario "3. arquivo novo nao rastreado" 1

# O caso que um `git diff --quiet` sozinho erra: já no stage, o diff da worktree fica limpo, mas
# o conteúdo ainda NÃO está no commit que a tag apontaria.
git -C "${TMP}" add nao-rastreado.txt
cenario "4. alteracao no stage, ainda sem commit" 1
git -C "${TMP}" commit -qm "segundo commit"

cenario "5. arvore limpa de novo" 0

# Fora dos cinco pedidos, mas do mesmo tipo e igualmente silencioso: um arquivo APAGADO deixa a
# worktree diferente do commit sem criar nada que salte aos olhos.
git -C "${TMP}" rm -q rastreado.txt
cenario "6. arquivo rastreado removido" 1
git -C "${TMP}" checkout -- rastreado.txt 2>/dev/null || git -C "${TMP}" reset -q --hard

echo
if [ "${falhas}" -gt 0 ]; then
	echo "${falhas} cenario(s) reprovaram."
	exit 1
fi
echo "Todos os cenarios passaram."
