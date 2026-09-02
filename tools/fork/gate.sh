#!/usr/bin/env bash
#
# Gate obrigatório de pré-release. Roda tudo o que precisa estar verde ANTES de marcar uma tag,
# na ordem em que cada etapa é capaz de reprovar, e para na primeira falha.
#
# Por que isto existe: as alphas 21, 22 e 23 quebraram no runner do GitHub, cada uma num estrato
# diferente do mesmo merge de 214 commits — link do C++, sintaxe do Kotlin, ambiguidade do
# Kotlin. Cada ciclo custou uma release inteira para descobrir um erro que o compilador local
# acha em minutos. A ordem aqui é essa lição: o que falha mais rápido vem primeiro.
#
# Uso:
#   tools/fork/gate.sh              # completo, é o que uma tag exige (~30 min em 4 núcleos)
#   tools/fork/gate.sh --quick      # ambiente + estrutura + Kotlin (~2 min), para o ciclo curto
#
# O modo --quick NÃO autoriza uma release: ele não compila nada de nativo, e link foi
# exatamente o que derrubou a alpha 21.
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ANDROID_DIR="${REPO}/platforms/android"
QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1

# Versões que o CI usa. Divergência aqui não reprova por si só, mas é reportada: um "verde local"
# produzido noutro toolchain é uma garantia mais fraca do que parece.
CI_JDK_MAJOR=17
NDK_VERSION="28.2.13676358"
CMAKE_VERSION="3.31.6"

STEP_NAMES=(); STEP_RESULTS=(); STEP_SECONDS=(); FAILED=""

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

# Executa uma etapa e registra o resultado. A primeira falha encerra o gate: continuar depois de
# uma etapa vermelha só produz erro em cascata, que foi como a alpha 23 escondeu a causa real.
step() {
	local name="$1"; shift
	local start; start=$(date +%s)
	say "${name}"
	if "$@"; then
		STEP_NAMES+=("${name}"); STEP_RESULTS+=("OK"); STEP_SECONDS+=($(( $(date +%s) - start )))
	else
		STEP_NAMES+=("${name}"); STEP_RESULTS+=("FALHA"); STEP_SECONDS+=($(( $(date +%s) - start )))
		FAILED="${name}"
		report
		exit 1
	fi
}

skip() { STEP_NAMES+=("$1"); STEP_RESULTS+=("PULADA"); STEP_SECONDS+=(0); printf '    (pulada: %s)\n' "$2"; }

gradle() { ( cd "${ANDROID_DIR}" && ANDROID_HOME="${ANDROID_HOME}" ./gradlew --no-daemon "$@" ); }

# --- 1. Ambiente -------------------------------------------------------------------------------
check_env() {
	local ok=0

	if [ -z "${ANDROID_HOME:-}" ] && [ -f "${ANDROID_DIR}/local.properties" ]; then
		ANDROID_HOME="$(sed -n 's/^sdk\.dir=//p' "${ANDROID_DIR}/local.properties")"
	fi
	export ANDROID_HOME
	if [ -z "${ANDROID_HOME:-}" ] || [ ! -d "${ANDROID_HOME}" ]; then
		echo "    ERRO: SDK não encontrado. Rode tools/fork/setup-android-sdk.sh"; ok=1
	else
		echo "    SDK          ${ANDROID_HOME}"
	fi

	if ! command -v java >/dev/null; then
		echo "    ERRO: java não encontrado"; ok=1
	else
		local jdk; jdk="$(java -version 2>&1 | sed -n '1s/.*"\([0-9]*\).*/\1/p')"
		if [ "${jdk}" = "${CI_JDK_MAJOR}" ]; then
			echo "    JDK          ${jdk}"
		else
			# Não reprova: passou nos dois na prática. Mas quem lê o relatório precisa saber que
			# o verde não foi produzido no mesmo JDK que produz o APK publicado.
			echo "    JDK          ${jdk}  (AVISO: o CI usa ${CI_JDK_MAJOR})"
		fi
	fi

	if [ -d "${ANDROID_HOME:-/nao-existe}/ndk/${NDK_VERSION}" ]; then
		echo "    NDK          ${NDK_VERSION}"
	else
		echo "    ERRO: NDK ${NDK_VERSION} ausente (é pinado; outro NDK invalida qualquer A/B)"; ok=1
	fi

	if [ -d "${ANDROID_HOME:-/nao-existe}/cmake/${CMAKE_VERSION}" ]; then
		echo "    CMake        ${CMAKE_VERSION}"
	else
		echo "    ERRO: CMake ${CMAKE_VERSION} ausente"; ok=1
	fi

	if [ -d "${REPO}/platforms/android/app/src/main/cpp/3rdparty/shaderc/third_party/spirv-tools" ]; then
		echo "    shaderc      dependências presentes"
	else
		echo "    ERRO: rode git-sync-deps do shaderc (SPIRV-Tools não é submódulo)"; ok=1
	fi

	if command -v rustup >/dev/null && rustup target list --installed 2>/dev/null | grep -q aarch64-linux-android; then
		echo "    Rust         alvo aarch64-linux-android presente"
	else
		echo "    ERRO: falta o alvo Rust aarch64-linux-android (librashader)"; ok=1
	fi

	return ${ok}
}

# --- 5. Link -----------------------------------------------------------------------------------
# A pergunta "há símbolo indefinido?" quem responde é o próprio link, na etapa 4 — um .so com
# pendência não chega a existir. Aqui se confirma que o artefato saiu, e que saiu para a
# arquitetura certa: um .so de arquitetura errada linka e só falha no aparelho.
check_link() {
	local so
	so="$(find "${ANDROID_DIR}/app/build/intermediates" -name 'libemucore*.so' -path '*arm64-v8a*' 2>/dev/null | head -1)"
	if [ -z "${so}" ]; then
		echo "    ERRO: nenhuma libemucore*.so para arm64-v8a foi produzida"; return 1
	fi
	local desc; desc="$(file -b "${so}" 2>/dev/null || echo desconhecido)"
	echo "    ${so#"${REPO}/"}"
	echo "    $(du -h "${so}" | cut -f1)  ${desc:0:60}"
	case "${desc}" in
		*aarch64*|*ARM\ aarch64*) : ;;
		*) echo "    ERRO: a .so não é aarch64"; return 1 ;;
	esac
	return 0
}

check_apk() {
	local apk; apk="$(find "${ANDROID_DIR}/app/build/outputs/apk/github/$1" -name '*.apk' 2>/dev/null | head -1)"
	[ -n "${apk}" ] || { echo "    ERRO: APK de $1 não foi produzido"; return 1; }
	echo "    ${apk#"${REPO}/"}  ($(du -h "${apk}" | cut -f1))"
}

report() {
	printf '\n\033[1m─── Relatório do gate ───\033[0m\n'
	printf '    commit   %s\n' "$(git -C "${REPO}" rev-parse --short HEAD)"
	printf '    branch   %s\n' "$(git -C "${REPO}" rev-parse --abbrev-ref HEAD)"
	local dirty; dirty="$(git -C "${REPO}" status --porcelain | wc -l)"
	[ "${dirty}" -gt 0 ] && printf '    SUJA     %s arquivo(s) nao commitados\n' "${dirty}"
	printf '\n'
	local i
	for i in "${!STEP_NAMES[@]}"; do
		printf '    %-7s %4ss  %s\n' "${STEP_RESULTS[$i]}" "${STEP_SECONDS[$i]}" "${STEP_NAMES[$i]}"
	done
	if [ -n "${FAILED}" ]; then
		printf '\n\033[1mGATE VERMELHO — reprovou em: %s\033[0m\n' "${FAILED}"
		printf 'Nao marque tag nem publique release enquanto isto nao estiver verde.\n'
	elif [ "${QUICK}" = "1" ]; then
		printf '\n\033[1mVERDE (modo --quick)\033[0m\n'
		printf 'NAO autoriza release: nada de nativo foi compilado, e link foi o que derrubou a alpha 21.\n'
	elif [ "${dirty}" -gt 0 ]; then
		# Arvore suja e verdicto verde sao afirmacoes contraditorias: o que passou foi a ARVORE,
		# e uma tag aponta para um COMMIT. Pior ainda, uma etapa que ficou "up-to-date" pode ter
		# rodado ANTES da edicao nao commitada — foi o que aconteceu na primeira execucao deste
		# gate. Entao aqui ele se recusa a autorizar, em vez de avisar e autorizar assim mesmo.
		printf '\n\033[1mVERDE PARA A ARVORE, NAO PARA UM COMMIT\033[0m\n'
		printf 'Ha %s arquivo(s) nao commitados. Commite e rode de novo antes de marcar a tag:\n' "${dirty}"
		git -C "${REPO}" status --porcelain | sed 's/^/    /'
	else
		printf '\n\033[1mGATE VERDE — pode marcar a tag\033[0m\n'
	fi
}

step "1. Ambiente"                     check_env
step "2. Consistencia de toolchain"    python3 "${REPO}/tools/fork/check-toolchain.py"
step "3. Estrutura (residuos de merge)" python3 "${REPO}/tools/fork/scan-merge-damage.py"
step "4. Kotlin"                       gradle :app:compileGithubDebugKotlin

if [ "${QUICK}" = "1" ]; then
	skip "5. C/C++ ARM64"    "--quick"
	skip "6. Link"           "--quick"
	skip "7. Testes host"    "--quick"
	skip "8. APK debug"      "--quick"
	skip "9. APK release"    "--quick"
	report
	exit 0
fi

step "5. C/C++ ARM64"                  gradle :app:externalNativeBuildGithubDebug
step "6. Link"                         check_link
# A suíte host (tests/ctest) exige a cadeia ~/deps — Qt, ffmpeg — que o build Android não usa,
# e que hoje não cabe neste laço. Fica DECLARADA como delegada ao CI em vez de omitida: um gate
# que finge cobrir o que não cobre é pior que um que diz o que deixou de fora.
skip "7. Testes host"    "delegados ao CI (phase-0.5 e fork tests exigem a cadeia ~/deps)"
step "8. APK debug"                    gradle :app:assembleGithubDebug
step "9. APK debug presente"           check_apk debug
step "10. APK release (LTO + R8)"      gradle :app:assembleGithubRelease
step "11. APK release presente"        check_apk release

report
