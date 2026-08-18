#!/usr/bin/env bash
# Mostra a superfície de contato deste fork com o upstream: quais arquivos do ARMSX2 nós
# modificamos, e quais arquivos são inteiramente nossos.
#
# A comparação é feita contra o commit gravado em tools/fork/UPSTREAM_BASE — que é ancestral
# da nossa história, então NÃO exige rede nem o remoto `armsx2` configurado.
#
# Uso:
#   ./tools/fork/fork-diff.sh          # relatório completo
#   ./tools/fork/fork-diff.sh --ci     # mesmo relatório, formatado para log de CI
#
# Sai com 0 sempre: isto informa, não reprova. O julgamento é humano — mas fica visível.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"
BASE_FILE="tools/fork/UPSTREAM_BASE"
[ -f "$BASE_FILE" ] || { echo "!! $BASE_FILE ausente"; exit 0; }
BASE="$(tr -d '[:space:]' < "$BASE_FILE")"

if ! git cat-file -e "$BASE^{commit}" 2>/dev/null; then
	echo "!! commit-base $BASE não está neste clone (clone raso?). Pulando relatório."
	exit 0
fi

# Caminhos onde uma mudança nossa é, por política, um erro até prova em contrário:
# núcleo de emulação, recompiladores e temporização. Ver FORK.md, regra 1.
PROTEGIDOS='^(pcsx2/(x86|arm64)/|pcsx2/(R5900|R3000A|VU|VU0|VU1|VUmicro|VUops|VUflags|Iop|Counters|Cache|COP0|COP2|FPU|Gif|Vif|Dmac|SPU2|IPU)|common/(emitter|arm64)/)'

echo "=== Superfície de contato com o upstream ==="
echo "base: $BASE"
echo "HEAD: $(git rev-parse --short HEAD)"
echo

modificados="$(git diff --name-only --diff-filter=MDR "$BASE"..HEAD || true)"
novos="$(git diff --name-only --diff-filter=A "$BASE"..HEAD || true)"

echo "--- Arquivos do upstream MODIFICADOS/REMOVIDOS ($(printf '%s' "$modificados" | grep -c . || true)) ---"
if [ -n "$modificados" ]; then
	printf '%s\n' "$modificados" | sed 's/^/  /'
else
	echo "  (nenhum)"
fi
echo

echo "--- Arquivos NOVOS, só nossos ($(printf '%s' "$novos" | grep -c . || true)) ---"
if [ -n "$novos" ]; then
	printf '%s\n' "$novos" | sed 's/^/  /'
else
	echo "  (nenhum)"
fi
echo

alertas="$(printf '%s\n%s\n' "$modificados" "$novos" | grep -E "$PROTEGIDOS" || true)"
if [ -n "$alertas" ]; then
	echo "!!! ALERTA — mudanças em caminhos do núcleo de emulação (FORK.md, regra 1):"
	printf '%s\n' "$alertas" | sed 's/^/  !! /'
	echo
	echo "    Cada uma precisa de justificativa explícita no commit, ou deve ser revertida"
	echo "    e reportada ao upstream."
else
	echo "OK — nenhuma mudança em caminhos do núcleo de emulação."
fi
