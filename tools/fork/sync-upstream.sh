#!/usr/bin/env bash
# Configura os remotos do fork e traz o upstream para o nosso branch.
#
#   ./tools/fork/sync-upstream.sh --setup    # cria/corrige os remotos armsx2 e pcsx2 e faz fetch
#   ./tools/fork/sync-upstream.sh --status   # quanto estamos atrás do upstream, sem tocar em nada
#   ./tools/fork/sync-upstream.sh --merge    # traz armsx2/master para o branch atual (merge)
#
# MERGE, não rebase: reescrever 25k commits de base compartilhada destruiria o `git blame` e
# tornaria cada conflito futuro anônimo. Ver docs/fase0-setup.md.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

ARMSX2_URL="https://github.com/ARMSX2/ARMSX2.git"
PCSX2_URL="https://github.com/PCSX2/pcsx2.git"

garantir_remoto() {
	local nome="$1" url="$2"
	if git remote get-url "$nome" >/dev/null 2>&1; then
		git remote set-url "$nome" "$url"
	else
		git remote add "$nome" "$url"
	fi
}

setup() {
	garantir_remoto armsx2 "$ARMSX2_URL"
	garantir_remoto pcsx2 "$PCSX2_URL"
	echo ">> fetch armsx2/master"
	git fetch armsx2 master
	echo ">> remotos:"
	git remote -v
}

status() {
	git remote get-url armsx2 >/dev/null 2>&1 || { echo "remoto 'armsx2' ausente — rode --setup"; exit 1; }
	git fetch armsx2 master
	local base atras
	base="$(tr -d '[:space:]' < tools/fork/UPSTREAM_BASE)"
	atras="$(git rev-list --count "HEAD..armsx2/master")"
	echo "base registrada : $base"
	echo "upstream atual  : $(git rev-parse --short armsx2/master)  ($(git log -1 --format=%ad --date=short armsx2/master))"
	echo "commits atrás   : $atras"
	echo
	echo "Nossa superfície de contato hoje:"
	./tools/fork/fork-diff.sh
}

merge() {
	git remote get-url armsx2 >/dev/null 2>&1 || { echo "remoto 'armsx2' ausente — rode --setup"; exit 1; }
	git diff --quiet && git diff --cached --quiet || { echo "árvore suja — commite ou guarde antes"; exit 1; }
	git fetch armsx2 master
	echo ">> merge armsx2/master em $(git rev-parse --abbrev-ref HEAD)"
	if git merge --no-edit armsx2/master; then
		git rev-parse armsx2/master > tools/fork/UPSTREAM_BASE
		git add tools/fork/UPSTREAM_BASE
		git commit -m "fork: atualiza UPSTREAM_BASE para $(git rev-parse --short armsx2/master)" || true
		echo
		echo ">> merge limpo. AGORA, antes de qualquer push:"
	else
		echo
		echo ">> CONFLITOS. Resolva-os e depois atualize tools/fork/UPSTREAM_BASE manualmente."
	fi
	echo "   1. CI verde (build Android arm64)"
	echo "   2. lista de jogos-canário reexecutada (docs/jogos-canario.md)"
}

case "${1:-}" in
	--setup)  setup ;;
	--status) status ;;
	--merge)  merge ;;
	*) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
