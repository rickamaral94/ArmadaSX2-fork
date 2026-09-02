#!/usr/bin/env bash
#
# Responde "o que o gate validou é o que está no commit?".
#
# Existe como script próprio, e não como função dentro do gate, por dois motivos: o gate roda no
# repositório onde ele mesmo vive, e um teste honesto desta lógica precisa de um repositório
# DESCARTÁVEL onde dá para sujar a árvore de propósito. Ver tools/fork/gate-selftest.sh.
#
# Uso:  tools/fork/check-tree-state.sh [caminho-do-repo]
# Saída: 0 = limpa · 1 = suja (arquivos listados) · 2 = não é um repositório git
set -uo pipefail

REPO="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

git -C "${REPO}" rev-parse --git-dir >/dev/null 2>&1 || {
	echo "ERRO: '${REPO}' nao e um repositorio git." >&2
	exit 2
}

# --porcelain cobre os três casos que importam e que se confundem facilmente: rastreado e
# modificado, rastreado e removido, e não rastreado. Um `git diff --quiet` sozinho perde o
# terceiro, que é justamente como um arquivo novo e esquecido passa despercebido.
sujos="$(git -C "${REPO}" status --porcelain)"
if [ -z "${sujos}" ]; then
	exit 0
fi

echo "${sujos}"
exit 1
