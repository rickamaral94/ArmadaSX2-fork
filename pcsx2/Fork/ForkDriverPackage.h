// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <span>
#include <string>

/// Validação e identificação de pacotes de driver Vulkan (Turnip e afins).
///
/// Hoje um pacote importado é aceito pelo que o `meta.json` DIZ ser. O `meta.json` vem dentro do
/// próprio zip, é escrito por quem o empacotou e não é verificado contra nada — então o emulador
/// pode entregar ao `adrenotools_open_libvulkan` um arquivo que não é um driver, é de outra
/// arquitetura, ou está truncado por um download interrompido. O resultado disso é um `dlopen`
/// que falha no meio do boot, ou pior, um carregamento que dá errado só quando o jogo abre.
///
/// Este módulo responde duas perguntas ANTES de qualquer carregamento:
///
///   1. isto é mesmo uma biblioteca compartilhada AArch64? (leitura do cabeçalho ELF)
///   2. qual é o SHA-256 exato deste arquivo?
///
/// O hash não é segurança — é IDENTIDADE. Sem ele, um relatório de compatibilidade que diz
/// "Turnip 25.2" não distingue duas builds diferentes com o mesmo nome, e um A/B entre drivers
/// compara rótulos em vez de binários. É o mesmo motivo pelo qual registramos o SHA-256 do APK no
/// registro de execuções do CI.
namespace ForkDriverPackage
{
	/// Ordenado do mais específico para o mais genérico: o primeiro problema encontrado é o
	/// reportado, e ele deve ser o mais informativo possível para o usuário.
	enum class Verdict : u8
	{
		Ok,
		/// O arquivo apontado pelo pacote não existe.
		MissingLibrary,
		/// Existe mas não pôde ser lido (permissão, IO).
		LibraryUnreadable,
		/// Começa com \x7fELF mas o cabeçalho não está completo — download interrompido.
		/// Reportado só quando o magic confere; um arquivo curto SEM magic é NotAnElf, porque o
		/// caso real mais comum é uma página de erro HTML salva com nome de .so.
		Truncated,
		/// Não começa com \x7fELF. Alguém importou um zip errado, um README, uma página de erro.
		NotAnElf,
		/// ELF de 32 bits. Não roda no nosso processo arm64-v8a.
		NotElf64,
		/// ELF big-endian.
		NotLittleEndian,
		/// ELF válido, mas de OUTRA arquitetura (x86-64, ARM 32, RISC-V...). O caso perigoso:
		/// parece um driver, tem nome de driver, e só falha no dlopen.
		NotAarch64,
		/// Não é objeto compartilhado (ET_DYN) — é executável ou relocatable.
		NotSharedObject,
	};

	/// O que o cabeçalho ELF declara. Exposto para log e diagnóstico.
	struct ElfIdentity
	{
		bool has_elf_magic = false;
		bool is_64bit = false;
		bool is_little_endian = false;
		u16 machine = 0; ///< e_machine (EM_AARCH64 = 183)
		u16 type = 0; ///< e_type (ET_DYN = 3)
	};

	struct Inspection
	{
		Verdict verdict = Verdict::MissingLibrary;
		ElfIdentity elf;
		/// SHA-256 em hexadecimal minúsculo, vazio quando o arquivo não pôde ser lido. Calculado
		/// mesmo quando o veredito reprova: saber o hash do arquivo errado ajuda a rastrear de
		/// onde ele veio.
		std::string sha256;
		u64 size_bytes = 0;

		bool IsUsable() const { return verdict == Verdict::Ok; }
	};

	/// Cabeçalho ELF64 completo. Menos que isso já é motivo de reprovação.
	inline constexpr size_t ELF_HEADER_SIZE = 64;
	inline constexpr u16 EM_AARCH64 = 183;
	inline constexpr u16 ET_DYN = 3;

	/// Pura, sem IO: decide sobre os bytes do cabeçalho. Toda a lógica de aceitação vive aqui,
	/// para poder ser exercitada sem um driver de verdade em disco.
	Verdict ValidateElfHeader(std::span<const u8> header);
	ElfIdentity ReadElfIdentity(std::span<const u8> header);

	/// Lê o arquivo, valida o cabeçalho e calcula o hash em uma passada.
	Inspection InspectLibrary(const std::string& path);

	/// SHA-256 em hexadecimal minúsculo. Vazio se o arquivo não puder ser lido.
	std::string Sha256File(const std::string& path);
	std::string Sha256Bytes(std::span<const u8> data);

	const char* VerdictToString(Verdict value);
	/// Frase para a UI. Um import recusado sem motivo legível vira relato de bug.
	const char* VerdictReason(Verdict value);
} // namespace ForkDriverPackage
