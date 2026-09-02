#!/usr/bin/env bash
#
# Gate de pré-release. Roda o que precisa estar verde antes de marcar uma tag, na ordem em que
# cada etapa é capaz de reprovar, e para na primeira falha.
#
# Por que existe: as alphas 21, 22 e 23 quebraram no runner, cada uma num estrato diferente do
# mesmo merge de 214 commits — link do C++, sintaxe do Kotlin, ambiguidade do Kotlin. Cada ciclo
# custou uma release para achar erro que o compilador local acha em minutos.
#
# O QUE ESTE GATE NÃO PODE AFIRMAR. Ele roda na máquina de quem o chama e NÃO executa a suíte
# host (tests/ctest, phase-0.5), que linka o PCSX2 inteiro e exige a cadeia ~/deps. Por isso o
# veredicto máximo daqui é GATE LOCAL VERDE. "Pronto para publicar" exige as duas metades no
# MESMO SHA, e a outra metade só o CI produz.
#
# Uso:
#   tools/fork/gate.sh              # completo — o que uma tag exige
#   tools/fork/gate.sh --quick      # ambiente + estrutura + Kotlin, para o ciclo curto
#
# Códigos de saída:
#   0  todas as etapas executadas passaram (no modo completo, com a árvore limpa)
#   1  alguma etapa reprovou
#   2  modo completo com árvore suja — o veredicto é RETIDO, porque o que passou foi a árvore e
#      uma tag aponta para um commit
#
# No modo --quick, 0 significa apenas "o subconjunto rápido passou". NÃO significa pronto para
# release: nada de nativo foi compilado, e link foi o que derrubou a alpha 21.
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ANDROID_DIR="${REPO}/platforms/android"
TOOLS="${REPO}/tools/fork"
QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1

# Instante de início. Todo artefato aceito adiante tem de ser MAIS NOVO que isto — é o que separa
# "produzido por esta execução" de "encontrado no diretório de build". A alternativa seria apagar
# a árvore de saída, e apagar em massa num repositório de trabalho é pior do que o problema.
RUN_START="$(date +%s)"

CI_JDK_MAJOR=17
NDK_VERSION="28.2.13676358"
CMAKE_VERSION="3.31.6"

STEP_NAMES=(); STEP_RESULTS=(); STEP_SECONDS=(); FAILED=""
JDK_LOCAL=""; APK_FACTS=(); SO_FACTS=""

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

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

# --- Ambiente ----------------------------------------------------------------------------------
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
		# NAO ancorar na linha 1: com JAVA_TOOL_OPTIONS definido (proxy, truststore) a JVM
		# imprime "Picked up ..." ANTES da versao, e o parse devolvia string vazia. Com a
		# politica antiga, que so avisava, uma versao vazia passava batido — o bug so apareceu
		# quando o modo completo passou a reprovar.
		JDK_LOCAL="$(java -version 2>&1 | sed -n 's/.*version "\([0-9]*\).*/\1/p' | head -1)"
		if [ -z "${JDK_LOCAL}" ]; then
			echo "    ERRO: nao consegui ler a versao do java (saida inesperada de 'java -version')"
			ok=1
		elif [ "${JDK_LOCAL}" = "${CI_JDK_MAJOR}" ]; then
			echo "    JDK          ${JDK_LOCAL}"
		elif [ "${QUICK}" = "1" ]; then
			# No ciclo curto o desvio é aviso: o objetivo ali é achar erro de código depressa, e
			# trocar de JDK para isso custaria mais do que rende.
			echo "    JDK          ${JDK_LOCAL}  (AVISO: o CI usa ${CI_JDK_MAJOR})"
		else
			# No modo completo, reprova. Este e o modo que autoriza uma tag, e um verde produzido
			# noutra major nao e o mesmo verde que produz o APK publicado — bytecode, desugaring
			# e o proprio R8 mudam entre majors. Chamar isso de garantia seria mentir sobre o
			# que foi verificado.
			echo "    ERRO: JDK ${JDK_LOCAL} local, mas o CI compila o APK publicado com ${CI_JDK_MAJOR}."
			echo "          Use o mesmo JDK, ou rode --quick se a intencao e so o ciclo curto."
			ok=1
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
		echo "    Rust         $(rustc --version 2>/dev/null | awk '{print $2}') · alvo aarch64-linux-android"
	else
		echo "    ERRO: falta o alvo Rust aarch64-linux-android (librashader)"; ok=1
	fi

	return ${ok}
}

check_tree() { "${TOOLS}/check-tree-state.sh" "${REPO}"; }

# --- Link --------------------------------------------------------------------------------------
# "Há símbolo indefinido?" quem responde é o próprio link — um .so com pendência não chega a
# existir. Aqui se confirma que o artefato saiu, saiu AGORA, e saiu para a arquitetura certa: um
# .so de arquitetura errada linka e só falha no aparelho.
check_link() {
	local so
	so="$(find "${ANDROID_DIR}/app/build/intermediates" -name 'libemucore*.so' -path '*arm64-v8a*' \
		-newermt "@${RUN_START}" 2>/dev/null | head -1)"
	if [ -z "${so}" ]; then
		echo "    ERRO: nenhuma libemucore*.so para arm64-v8a produzida por ESTA execucao."
		echo "          (uma .so antiga no diretorio de build nao conta como link desta rodada)"
		return 1
	fi
	local desc; desc="$(file -b "${so}" 2>/dev/null || echo desconhecido)"
	case "${desc}" in
		*aarch64*|*ARM\ aarch64*) : ;;
		*) echo "    ERRO: a .so nao e aarch64: ${desc}"; return 1 ;;
	esac
	SO_FACTS="${so#"${REPO}/"} · $(du -h "${so}" | cut -f1) · aarch64"
	echo "    ${SO_FACTS}"
	return 0
}

# --- APK ---------------------------------------------------------------------------------------
# Caminho EXATO da variante, não uma busca genérica: um `find` largo aceita o APK de outra
# variante ou de outra execução, e um artefato antigo aprovado como novo e o modo de falha mais
# perigoso que um gate pode ter — ele diz verde sobre codigo que nunca compilou.
check_apk() {
	local variante="$1"
	local dir="${ANDROID_DIR}/app/build/outputs/apk/github/${variante}"
	local apks=(); mapfile -t apks < <(find "${dir}" -maxdepth 1 -name '*.apk' 2>/dev/null)

	if [ "${#apks[@]}" -eq 0 ]; then
		echo "    ERRO: nenhum APK em ${dir#"${REPO}/"}"; return 1
	fi
	if [ "${#apks[@]}" -gt 1 ]; then
		echo "    ERRO: ${#apks[@]} APKs em ${dir#"${REPO}/"} — ambiguo, nao da para dizer qual e o desta rodada"
		printf '           %s\n' "${apks[@]#"${REPO}/"}"
		return 1
	fi

	local apk="${apks[0]}" mtime
	mtime="$(stat -c %Y "${apk}")"
	if [ "${mtime}" -lt "${RUN_START}" ]; then
		echo "    ERRO: o APK e ANTERIOR a esta execucao (mtime $(date -d "@${mtime}" '+%F %T'))."
		echo "          Artefato antigo nao prova que o codigo atual compila."
		return 1
	fi

	local sha; sha="$(sha256sum "${apk}" | cut -d' ' -f1)"
	APK_FACTS+=("${variante}  $(du -h "${apk}" | cut -f1)  ${sha}")
	echo "    ${apk#"${REPO}/"}  ($(du -h "${apk}" | cut -f1))"
	echo "    sha256 ${sha}"
	return 0
}

# --- Relatório ---------------------------------------------------------------------------------
report() {
	local sha branch dirty
	sha="$(git -C "${REPO}" rev-parse HEAD)"
	branch="$(git -C "${REPO}" rev-parse --abbrev-ref HEAD)"
	dirty="$(git -C "${REPO}" status --porcelain | wc -l)"

	printf '\n\033[1m─── Relatório do gate ───\033[0m\n'
	printf '    SHA        %s\n' "${sha}"
	printf '    branch     %s\n' "${branch}"
	printf '    arvore     %s\n' "$([ "${dirty}" -eq 0 ] && echo limpa || echo "SUJA (${dirty} arquivo(s))")"
	printf '    toolchain  JDK %s · NDK %s · CMake %s · Rust %s\n' \
		"${JDK_LOCAL:-?}" "${NDK_VERSION}" "${CMAKE_VERSION}" \
		"$(rustc --version 2>/dev/null | awk '{print $2}' || echo '?')"
	[ -n "${SO_FACTS}" ] && printf '    nativo     %s\n' "${SO_FACTS}"
	local a; for a in "${APK_FACTS[@]:-}"; do [ -n "${a}" ] && printf '    apk        %s\n' "${a}"; done

	printf '\n'
	local i
	for i in "${!STEP_NAMES[@]}"; do
		printf '    %-7s %4ss  %s\n' "${STEP_RESULTS[$i]}" "${STEP_SECONDS[$i]}" "${STEP_NAMES[$i]}"
	done

	if [ -n "${FAILED}" ]; then
		printf '\n\033[1mGATE VERMELHO — reprovou em: %s\033[0m\n' "${FAILED}"
		printf 'Nao marque tag nem publique release enquanto isto nao estiver verde.\n'
		return 1
	fi

	if [ "${QUICK}" = "1" ]; then
		printf '\n\033[1mSUBCONJUNTO RAPIDO VERDE\033[0m\n'
		printf 'Codigo 0 aqui significa apenas: as etapas rapidas passaram.\n'
		printf 'NAO autoriza release — nada de nativo foi compilado, e link derrubou a alpha 21.\n'
		[ "${dirty}" -gt 0 ] && printf 'AVISO: arvore suja; o que passou foi a arvore, nao o commit %s.\n' "${sha:0:10}"
		return 0
	fi

	if [ "${dirty}" -gt 0 ]; then
		printf '\n\033[1mVEREDICTO RETIDO — arvore suja\033[0m\n'
		printf 'O que passou foi a ARVORE; uma tag aponta para um COMMIT. Pior: uma etapa que ficou\n'
		printf 'UP-TO-DATE pode ter rodado ANTES da edicao nao commitada. Pendentes:\n'
		git -C "${REPO}" status --porcelain | sed 's/^/    /'
		return 2
	fi

	printf '\n\033[1mGATE LOCAL VERDE — %s\033[0m\n' "${sha:0:10}"
	printf 'Isto NAO e "GATE DE RELEASE VERDE". Falta a metade que so o CI produz, no MESMO SHA:\n'
	printf '  · suite host (tests/ctest, phase-0.5) — linka o PCSX2 inteiro, exige a cadeia ~/deps\n'
	printf '  · o build de release do runner, que e o binario efetivamente publicado\n'
	printf 'Confira os workflows neste SHA antes de marcar a tag.\n'
	return 0
}

# --- Execução ----------------------------------------------------------------------------------
step "1. Ambiente"                      check_env
step "2. Consistencia de toolchain"     python3 "${TOOLS}/check-toolchain.py"
step "3. Estrutura (residuos de merge)" python3 "${TOOLS}/scan-merge-damage.py"
step "4. Kotlin"                        gradle :app:compileGithubDebugKotlin

if [ "${QUICK}" = "1" ]; then
	skip "5. C/C++ ARM64"        "--quick"
	skip "6. Link"               "--quick"
	skip "7. Testes de unidade"  "--quick"
	skip "8. APK debug"          "--quick"
	skip "9. APK release"        "--quick"
	report; exit $?
fi

step "5. C/C++ ARM64"                   gradle :app:externalNativeBuildGithubDebug
step "6. Link"                          check_link
step "7. Testes de unidade (JVM)"       gradle :app:testGithubDebugUnitTest
# A suíte host (tests/ctest, phase-0.5) linka o PCSX2 inteiro e exige a cadeia ~/deps, que o
# build Android não usa. Fica DECLARADA como delegada em vez de omitida — e é por causa dela que
# o veredicto local nunca pode ser "pronto para publicar".
skip "7b. Testes host (ctest)"    "delegados ao CI — exigem a cadeia ~/deps"
step "8. APK debug"                     gradle :app:assembleGithubDebug
step "9. APK debug desta execucao"      check_apk debug
step "10. APK release (LTO + R8)"       gradle :app:assembleGithubRelease
step "11. APK release desta execucao"   check_apk release

report; exit $?
