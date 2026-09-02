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

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

command -v java >/dev/null || { echo "ERRO: java não encontrado (instale um JDK 17+)." >&2; exit 1; }
command -v unzip >/dev/null || { echo "ERRO: unzip não encontrado." >&2; exit 1; }
command -v python3 >/dev/null || { echo "ERRO: python3 não encontrado (o git-sync-deps do shaderc precisa)." >&2; exit 1; }

# 1. Ferramentas de linha de comando do SDK.
if [ ! -x "${SDK}/cmdline-tools/latest/bin/sdkmanager" ]; then
	echo "==> Baixando as command-line tools do Android SDK"
	mkdir -p "${SDK}/cmdline-tools"
	tmp="$(mktemp -d)"
	curl -sSL -o "${tmp}/cmdline-tools.zip" "${CMDLINE_TOOLS_URL}"
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
	echo "==> Garantindo o alvo Rust aarch64-linux-android (librashader)"
	rustup target add aarch64-linux-android >/dev/null
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
