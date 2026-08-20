#!/usr/bin/env bash
#
# Gera a chave de assinatura do fork e imprime os quatro segredos que o workflow de release espera.
#
# Por que isto existe: sem uma chave estável, o Gradle assina com a `debug.keystore` do runner do
# GitHub, que é descartável — cada release sai com assinatura diferente, o Android recusa instalar
# por cima ("conflito com um pacote já existente"), e desinstalar apaga
# `Android/data/com.armsx2/files/`: memory cards, configurações, cache de shaders e drivers.
#
# A chave FICA COM VOCÊ. Este script não a envia para lugar nenhum; ele só imprime o que colar em
# Settings → Secrets and variables → Actions do repositório. Guarde o .jks em backup: perdê-la
# significa que nenhuma release futura instala por cima das antigas.
set -euo pipefail

OUT="${1:-armada-release.jks}"
ALIAS="${ARMADA_KEY_ALIAS:-armada}"

if [ -e "${OUT}" ]; then
	echo "ERRO: '${OUT}' já existe. Sobrescrever apagaria a chave das releases anteriores." >&2
	echo "Se a intenção é começar de novo, mova o arquivo antigo para outro lugar antes." >&2
	exit 1
fi

command -v keytool >/dev/null || { echo "ERRO: keytool não encontrado (instale um JDK)." >&2; exit 1; }

read -r -s -p "Senha da keystore (mínimo 6 caracteres): " STORE_PASS; echo
[ "${#STORE_PASS}" -ge 6 ] || { echo "ERRO: senha curta demais." >&2; exit 1; }

# Uma senha só para keystore e chave: duas senhas diferentes não acrescentam segurança aqui e
# multiplicam a chance de a release quebrar por engano de digitação.
keytool -genkeypair -v \
	-keystore "${OUT}" \
	-alias "${ALIAS}" \
	-keyalg RSA -keysize 4096 \
	-validity 10950 \
	-storepass "${STORE_PASS}" -keypass "${STORE_PASS}" \
	-dname "CN=ArmadaSX2, OU=Fork, O=ArmadaSX2, C=BR" >/dev/null

echo
echo "Chave criada em '${OUT}'. GUARDE ESSE ARQUIVO — sem ele, nenhuma release futura instala"
echo "por cima das anteriores."
echo
echo "Cole estes quatro segredos em Settings > Secrets and variables > Actions:"
echo
echo "  ARMADA_KEYSTORE_BASE64  = (conteúdo abaixo, uma linha só)"
echo "  ARMADA_KEYSTORE_PASSWORD = a senha que você acabou de digitar"
echo "  ARMADA_KEY_ALIAS         = ${ALIAS}"
echo "  ARMADA_KEY_PASSWORD      = a mesma senha"
echo
echo "----- ARMADA_KEYSTORE_BASE64 -----"
base64 -w 0 "${OUT}" 2>/dev/null || base64 "${OUT}" | tr -d '\n'
echo
echo "----------------------------------"
