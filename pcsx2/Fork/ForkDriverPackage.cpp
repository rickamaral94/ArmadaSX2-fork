// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkDriverPackage.h"

#include "common/FileSystem.h"

#include "fmt/format.h"

extern "C" {
#include "Sha256.h"
}

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

namespace
{
	/// Blocos de 64 KiB. Um driver Turnip passa de 10 MB; ler tudo para a memória só para hashear
	/// é desperdício em um aparelho que já está com pouca RAM por causa do emulador.
	constexpr size_t READ_CHUNK = 64 * 1024;

	std::string ToHex(const std::array<u8, SHA256_DIGEST_SIZE>& digest)
	{
		std::string out;
		out.reserve(digest.size() * 2);
		for (const u8 byte : digest)
			fmt::format_to(std::back_inserter(out), "{:02x}", byte);
		return out;
	}

	u16 ReadU16LE(std::span<const u8> data, size_t offset)
	{
		return static_cast<u16>(data[offset]) | (static_cast<u16>(data[offset + 1]) << 8);
	}
} // namespace

ForkDriverPackage::ElfIdentity ForkDriverPackage::ReadElfIdentity(std::span<const u8> header)
{
	ElfIdentity identity;
	if (header.size() < ELF_HEADER_SIZE)
		return identity;

	identity.has_elf_magic =
		(header[0] == 0x7F && header[1] == 'E' && header[2] == 'L' && header[3] == 'F');
	identity.is_64bit = (header[4] == 2); // EI_CLASS: 1 = 32 bits, 2 = 64 bits
	identity.is_little_endian = (header[5] == 1); // EI_DATA: 1 = LSB, 2 = MSB
	identity.type = ReadU16LE(header, 16); // e_type
	identity.machine = ReadU16LE(header, 18); // e_machine
	return identity;
}

ForkDriverPackage::Verdict ForkDriverPackage::ValidateElfHeader(std::span<const u8> header)
{
	// A ordem aqui é sobre a QUALIDADE DA MENSAGEM, não sobre economia de checagens.
	//
	// O magic vem antes do tamanho porque o caso real mais comum de import errado é uma página de
	// erro HTML salva com nome de .so (um link de download que expirou). Ela tem poucas dezenas de
	// bytes, então checar tamanho primeiro a classificaria como "download interrompido" — mandando
	// o usuário tentar baixar de novo um arquivo que nunca foi um driver.
	if (header.size() < 4)
		return Verdict::Truncated; // curto demais até para o magic; não há o que afirmar

	const bool has_magic = (header[0] == 0x7F && header[1] == 'E' && header[2] == 'L' && header[3] == 'F');
	if (!has_magic)
		return Verdict::NotAnElf;

	// Daqui em diante É um ELF, e um cabeçalho incompleto significa mesmo arquivo truncado.
	if (header.size() < ELF_HEADER_SIZE)
		return Verdict::Truncated;

	const ElfIdentity identity = ReadElfIdentity(header);

	if (!identity.is_64bit)
		return Verdict::NotElf64;
	// A ordem importa: os campos e_type/e_machine só têm significado depois de saber a ordem de
	// bytes. Reprovar por arquitetura um ELF big-endian seria reportar a causa errada.
	if (!identity.is_little_endian)
		return Verdict::NotLittleEndian;
	if (identity.machine != EM_AARCH64)
		return Verdict::NotAarch64;
	if (identity.type != ET_DYN)
		return Verdict::NotSharedObject;

	return Verdict::Ok;
}

std::string ForkDriverPackage::Sha256Bytes(std::span<const u8> data)
{
	CSha256 state;
	Sha256_Init(&state);
	Sha256_Update(&state, data.data(), data.size());

	std::array<u8, SHA256_DIGEST_SIZE> digest = {};
	Sha256_Final(&state, digest.data());
	return ToHex(digest);
}

std::string ForkDriverPackage::Sha256File(const std::string& path)
{
	std::FILE* fp = FileSystem::OpenCFile(path.c_str(), "rb");
	if (!fp)
		return {};

	CSha256 state;
	Sha256_Init(&state);

	std::vector<u8> buffer(READ_CHUNK);
	size_t read;
	while ((read = std::fread(buffer.data(), 1, buffer.size(), fp)) > 0)
		Sha256_Update(&state, buffer.data(), read);

	const bool failed = (std::ferror(fp) != 0);
	std::fclose(fp);
	if (failed)
		return {};

	std::array<u8, SHA256_DIGEST_SIZE> digest = {};
	Sha256_Final(&state, digest.data());
	return ToHex(digest);
}

ForkDriverPackage::Inspection ForkDriverPackage::InspectLibrary(const std::string& path)
{
	Inspection out;

	std::FILE* fp = FileSystem::OpenCFile(path.c_str(), "rb");
	if (!fp)
	{
		// Não existe e não pôde ser aberto são coisas diferentes para quem lê a mensagem: uma é
		// "o pacote não trouxe o arquivo", a outra é "o armazenamento recusou".
		out.verdict = FileSystem::FileExists(path.c_str()) ? Verdict::LibraryUnreadable : Verdict::MissingLibrary;
		return out;
	}

	CSha256 state;
	Sha256_Init(&state);

	std::array<u8, ELF_HEADER_SIZE> header = {};
	size_t header_filled = 0;

	std::vector<u8> buffer(READ_CHUNK);
	size_t read;
	while ((read = std::fread(buffer.data(), 1, buffer.size(), fp)) > 0)
	{
		if (header_filled < header.size())
		{
			const size_t take = std::min(header.size() - header_filled, read);
			std::memcpy(header.data() + header_filled, buffer.data(), take);
			header_filled += take;
		}
		Sha256_Update(&state, buffer.data(), read);
		out.size_bytes += read;
	}

	const bool failed = (std::ferror(fp) != 0);
	std::fclose(fp);
	if (failed)
	{
		out.verdict = Verdict::LibraryUnreadable;
		return out;
	}

	// O hash sai mesmo quando o veredito reprova: saber o SHA-256 do arquivo errado é o que
	// permite rastrear de onde ele veio.
	std::array<u8, SHA256_DIGEST_SIZE> digest = {};
	Sha256_Final(&state, digest.data());
	out.sha256 = ToHex(digest);

	out.elf = ReadElfIdentity(std::span<const u8>(header.data(), header_filled));
	out.verdict = ValidateElfHeader(std::span<const u8>(header.data(), header_filled));
	return out;
}

const char* ForkDriverPackage::VerdictToString(Verdict value)
{
	switch (value)
	{
		case Verdict::Ok:
			return "Ok";
		case Verdict::MissingLibrary:
			return "MissingLibrary";
		case Verdict::LibraryUnreadable:
			return "LibraryUnreadable";
		case Verdict::Truncated:
			return "Truncated";
		case Verdict::NotAnElf:
			return "NotAnElf";
		case Verdict::NotElf64:
			return "NotElf64";
		case Verdict::NotLittleEndian:
			return "NotLittleEndian";
		case Verdict::NotAarch64:
			return "NotAarch64";
		case Verdict::NotSharedObject:
			return "NotSharedObject";
	}
	return "Unknown";
}

const char* ForkDriverPackage::VerdictReason(Verdict value)
{
	switch (value)
	{
		case Verdict::Ok:
			return "Pacote válido.";
		case Verdict::MissingLibrary:
			return "O pacote não contém a biblioteca do driver.";
		case Verdict::LibraryUnreadable:
			return "A biblioteca do driver não pôde ser lida.";
		case Verdict::Truncated:
			return "Arquivo incompleto — o download provavelmente foi interrompido.";
		case Verdict::NotAnElf:
			return "Este arquivo não é uma biblioteca nativa.";
		case Verdict::NotElf64:
			return "Driver de 32 bits; este aparelho executa apenas bibliotecas de 64 bits.";
		case Verdict::NotLittleEndian:
			return "Driver compilado para uma ordem de bytes incompatível.";
		case Verdict::NotAarch64:
			return "Driver compilado para outra arquitetura de processador, não ARM64.";
		case Verdict::NotSharedObject:
			return "O arquivo é um executável, não uma biblioteca carregável.";
	}
	return "Pacote inválido.";
}
