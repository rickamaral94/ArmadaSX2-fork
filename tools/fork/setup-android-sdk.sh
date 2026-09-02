#!/usr/bin/env bash
#
# Prepara uma máquina Linux limpa para COMPILAR o fork sem depender do CI.
#
# Por que isto existe: três alphas seguidas (21, 22, 23) quebraram no runner do GitHub, cada uma
# num estrato diferente do mesmo merge — link do C++, sintaxe do Kotlin, ambiguidade do Kotlin.
# Cada ciclo custou uma release inteira para descobrir um erro que um compilador local acha em
# noventa segundos. Compilar antes de marcar a tag é mais barato que marcar a tag para descobrir.
#
# Instala EXATAMENTE o que .github/workflows/fork-release.yml usa, e mais dois passos que o
# workflow faz mas um clone comum não traz.
#
# Uso:   tools/fork/setup-android-sdk.sh [diretório do SDK]     (padrão: /opt/android-sdk)
# Depois: cd platforms/android && ./gradlew :app:assembleGithubDebug
set -euo pipefail

SDK="${1:-/opt/android-sdk}"
NDK_VERSION="28.2.13676358"
CMAKE_VERSION="3.31.6"
# O nome do pacote da plataforma passou a carregar versão MENOR: "platforms;android-37" não existe
# no repositório, é "platforms;android-37.0". compileSdk = 37 no build.gradle.kts resolve para ele.
PLATFORM="platforms;android-37.0"
BUILD_TOOLS="build-tools;37.0.0"
CMDLINE_TOOLS_URL="https://dl.google.com/android/repository/commandlinetools-linux-13114758_latest.zip"
# SHA-1 publicado pelo proprio Google em dl.google.com/android/repository/repository2-3.xml, que
# e o manifesto que o sdkmanager usa. E SHA-1 porque e o que eles publicam; o SHA-256 abaixo foi
# derivado de um download que passou nesse SHA-1, e existe para que uma colisao de SHA-1 nao
# baste para trocar o arquivo por baixo.
CMDLINE_TOOLS_SHA1="5fdcc763663eefb86a5b8879697aa6088b041e70"
CMDLINE_TOOLS_SHA256="7ec965280a073311c339e571cd5de778b9975026cfcbe79f2b1cdcb1e15317ee"
CMDLINE_TOOLS_SIZE=164760899

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

command -v java >/dev/null || { echo "ERRO: java não encontrado (instale um JDK 17)." >&2; exit 1; }
# O CI compila o APK publicado com JDK 17. Um "verde local" produzido noutra major e uma garantia
# mais fraca do que parece, entao o desvio e dito em voz alta em vez de passar batido.
JDK_LOCAL="$(java -version 2>&1 | sed -n '1s/.*"\([0-9]*\).*/\1/p')"
[ "${JDK_LOCAL}" = "17" ] || echo "AVISO: JDK ${JDK_LOCAL} local; o CI usa 17." >&2
command -v unzip >/dev/null || { echo "ERRO: unzip não encontrado." >&2; exit 1; }
command -v python3 >/dev/null || { echo "ERRO: python3 não encontrado (o git-sync-deps do shaderc precisa)." >&2; exit 1; }

# 1. Ferramentas de linha de comando do SDK.
if [ ! -x "${SDK}/cmdline-tools/latest/bin/sdkmanager" ]; then
	echo "==> Baixando as command-line tools do Android SDK"
	mkdir -p "${SDK}/cmdline-tools"
	tmp="$(mktemp -d)"
	curl -sSL -o "${tmp}/cmdline-tools.zip" "${CMDLINE_TOOLS_URL}"

	# Conferir ANTES de descompactar: um zip corrompido ou trocado nao pode virar o toolchain que
	# produz o APK publicado. Sem isto, o unico arquivo do setup sem verificacao de integridade
	# era justamente este — o Gradle wrapper do mesmo projeto ja confere o dele por
	# distributionSha256Sum.
	verify() {
		local algo="$1" esperado="$2" obtido
		obtido="$(${algo}sum "${tmp}/cmdline-tools.zip" | cut -d" " -f1)"
		[ "${obtido}" = "${esperado}" ] && return 0
		echo "ERRO: ${algo} das command-line tools nao confere." >&2
		echo "  esperado: ${esperado}" >&2
		echo "  obtido:   ${obtido}" >&2
		rm -rf "${tmp}"
		exit 1
	}
	tamanho="$(stat -c%s "${tmp}/cmdline-tools.zip")"
	if [ "${tamanho}" != "${CMDLINE_TOOLS_SIZE}" ]; then
		echo "ERRO: tamanho inesperado (${tamanho}, esperado ${CMDLINE_TOOLS_SIZE})." >&2
		rm -rf "${tmp}"; exit 1
	fi
	verify sha1 "${CMDLINE_TOOLS_SHA1}"
	verify sha256 "${CMDLINE_TOOLS_SHA256}"
	echo "    integridade conferida (sha1 + sha256)"

	unzip -q "${tmp}/cmdline-tools.zip" -d "${tmp}/x"
	mv "${tmp}/x/cmdline-tools" "${SDK}/cmdline-tools/latest"
	rm -rf "${tmp}"
fi

export ANDROID_HOME="${SDK}" ANDROID_SDK_ROOT="${SDK}"
SDKMANAGER="${SDK}/cmdline-tools/latest/bin/sdkmanager"

echo "==> Aceitando as licenças do SDK"
yes | "${SDKMANAGER}" --licenses >/dev/null 2>&1 || true

echo "==> Instalando plataforma, build-tools, NDK ${NDK_VERSION} e CMake ${CMAKE_VERSION}"
"${SDKMANAGER}" "platform-tools" "${PLATFORM}" "${BUILD_TOOLS}" \
	"ndk;${NDK_VERSION}" "cmake;${CMAKE_VERSION}" >/dev/null

# 2. Dependências do shaderc. NÃO são vendorizadas nem submódulo: o shaderc as busca sob demanda,
#    e sem elas o CMake para em "SPIRV-Tools was not found - required for compilation".
echo "==> Buscando as dependências de terceiros do shaderc"
python3 "${REPO_ROOT}/platforms/android/app/src/main/cpp/3rdparty/shaderc/utils/git-sync-deps"

# 3. Alvo Rust do librashader. Sem ele o cargo compila para o host e morre em
#    "can't find crate for `core`" no meio da configuração do CMake.
if command -v rustup >/dev/null; then
	# `rustup show` materializa o que o rust-toolchain.toml na raiz pede — versao E alvo —
	# em vez de instalar o alvo por cima de um rustc qualquer.
	echo "==> Instalando a toolchain Rust pinada em rust-toolchain.toml"
	( cd "${REPO_ROOT}" && rustup show >/dev/null )
else
	echo "AVISO: rustup não encontrado. O librashader precisa do alvo aarch64-linux-android;" >&2
	echo "       instale a toolchain Rust ou a compilação nativa vai falhar." >&2
fi

# 4. Onde o Gradle procura o SDK. local.properties é ignorado pelo git de propósito: o caminho é
#    da máquina, não do projeto.
echo "sdk.dir=${SDK}" > "${REPO_ROOT}/platforms/android/local.properties"

cat <<EOF

Pronto. SDK em ${SDK}.

  cd ${REPO_ROOT}/platforms/android
  ./gradlew :app:assembleGithubDebug          # nativo + Kotlin + APK (~10 min em 4 núcleos)
  ./gradlew :app:compileGithubDebugKotlin     # só o Kotlin (~1 min), pega erro de merge

Para conferir UM arquivo nativo sem recompilar tudo, o ninja aceita o objeto direto:

  cd app/.cxx/Debug/*/arm64-v8a
  ${SDK}/cmake/${CMAKE_VERSION}/bin/ninja CMakeFiles/emucore_4k.dir/native-lib.cpp.o

O Maven Central às vezes devolve 429 atrás de proxy; nesse caso é só repetir o comando, o que já
baixou fica em cache.
EOF
