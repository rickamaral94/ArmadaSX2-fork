#!/usr/bin/env bash
# Captura de log de sessão para análise do fork — roda na SUA máquina, com o aparelho ligado
# por USB. Produz dois arquivos: o log cru e um resumo filtrado, pronto para colar.
#
# Uso:
#   ./tools/fork/capture-logs.sh                 # captura até Ctrl-C
#   ./tools/fork/capture-logs.sh -o minha-sessao # prefixo dos arquivos
#
# Por que um script e não "roda adb logcat":
#
#   O Console do PCSX2 escreve em stdout, e o native-lib redireciona stdout/stderr para o
#   logcat sob a tag `STDOUT` (ver redirect_stdout_to_logcat). Ou seja, "ForkDriverIdentity:"
#   é o TEXTO da mensagem, não a tag — um `adb logcat -s ForkDriverIdentity` volta VAZIO e dá
#   a impressão de que o recurso não existe. E não há emulog.txt no Android: o app nunca chama
#   SetFileOutputLevel, então o logcat é o único canal que existe.
set -euo pipefail

PREFIX="armsx2-log-$(date +%Y%m%d-%H%M%S)"
while [ $# -gt 0 ]; do
	case "$1" in
		-o|--output) PREFIX="$2"; shift 2 ;;
		-h|--help) sed -n '2,16p' "$0"; exit 0 ;;
		*) echo "argumento desconhecido: $1" >&2; exit 2 ;;
	esac
done

command -v adb >/dev/null || { echo "adb não encontrado no PATH." >&2; exit 1; }

# Um aparelho, e só um: com dois conectados o adb escolhe sozinho e a captura sai do errado.
DEVICES="$(adb devices | awk 'NR>1 && $2=="device" {print $1}')"
COUNT="$(printf '%s\n' "${DEVICES}" | grep -c . || true)"
if [ "${COUNT}" -eq 0 ]; then
	echo "Nenhum aparelho autorizado. Ligue a depuração USB e aceite o pedido na tela." >&2
	adb devices >&2
	exit 1
elif [ "${COUNT}" -gt 1 ]; then
	echo "Mais de um aparelho conectado; desconecte os outros ou use ANDROID_SERIAL." >&2
	adb devices >&2
	exit 1
fi

RAW="${PREFIX}.raw.log"
DIGEST="${PREFIX}.digest.log"

# Cabeçalho de identidade. Sem isto, uma medição não é comparável com outra: driver próprio não
# tem rótulo que outra pessoa reproduza, e "alpha 1" não distingue dois APKs.
{
	echo "# captura: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo "# aparelho : $(adb shell getprop ro.product.manufacturer | tr -d '\r') $(adb shell getprop ro.product.model | tr -d '\r')"
	echo "# android  : $(adb shell getprop ro.build.version.release | tr -d '\r') (SDK $(adb shell getprop ro.build.version.sdk | tr -d '\r'))"
	echo "# soc      : $(adb shell getprop ro.soc.model | tr -d '\r') / $(adb shell getprop ro.board.platform | tr -d '\r')"
	for PKG in $(adb shell pm list packages | tr -d '\r' | sed 's/^package://' | grep -iE 'armsx2|pcsx2'); do
		VER="$(adb shell dumpsys package "${PKG}" | tr -d '\r' | awk '/versionName=/{print $1; exit}')"
		echo "# apk      : ${PKG} ${VER}"
	done
} | tee "${DIGEST}"
cp "${DIGEST}" "${RAW}"

echo
echo "Capturando. Rode o jogo, ligue e desligue a Frame Generation, e pressione Ctrl-C ao terminar."
echo

# Buffer grande e limpo: o padrão do logcat é pequeno demais para uma sessão de jogo e o começo
# — que é onde o driver é escolhido — seria o primeiro a ser descartado.
adb logcat -G 16M >/dev/null 2>&1 || true
adb logcat -c || true

finish() {
	echo
	echo "Resumindo..."
	# Grep no TEXTO da mensagem, não na tag. Veja o comentário no topo.
	grep -aE 'ForkDriverIdentity|ForkGpuCapabilities|ForkRuntime:|ForkConfig:|@@ANDROID_LSFG@@|adrenotools|libvulkan|turnip|Turnip|VK_ERROR|FATAL EXCEPTION|beginning of crash' \
		"${RAW}" >> "${DIGEST}" || true
	echo
	echo "  log cru : ${RAW}"
	echo "  resumo  : ${DIGEST}"
	echo
	echo "Mande o RESUMO. Se ele não tiver nenhuma linha 'ForkDriverIdentity', mande o cru:"
	echo "significa que o driver nunca foi sondado, e isso por si só já é o achado."
}
trap finish EXIT

adb logcat -v time >> "${RAW}"
