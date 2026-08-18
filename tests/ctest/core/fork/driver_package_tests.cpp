// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkDriverPackage.h"

#include <gtest/gtest.h>

#include <array>
#include <string_view>
#include <vector>

using ForkDriverPackage::Verdict;

namespace
{
	/// Monta um cabeçalho ELF64 mínimo com os campos que a validação olha. Tudo o mais fica zero:
	/// o validador não deve depender de nada além do que ele declara olhar.
	std::vector<u8> MakeElfHeader(u8 elf_class = 2, u8 endianness = 1,
		u16 machine = ForkDriverPackage::EM_AARCH64, u16 type = ForkDriverPackage::ET_DYN,
		bool valid_magic = true, size_t size = ForkDriverPackage::ELF_HEADER_SIZE)
	{
		std::vector<u8> header(size, 0);
		if (size >= 4 && valid_magic)
		{
			header[0] = 0x7F;
			header[1] = 'E';
			header[2] = 'L';
			header[3] = 'F';
		}
		if (size > 5)
		{
			header[4] = elf_class;
			header[5] = endianness;
		}
		if (size >= 20)
		{
			header[16] = static_cast<u8>(type & 0xFF);
			header[17] = static_cast<u8>(type >> 8);
			header[18] = static_cast<u8>(machine & 0xFF);
			header[19] = static_cast<u8>(machine >> 8);
		}
		return header;
	}

	std::span<const u8> AsBytes(std::string_view text)
	{
		return std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size());
	}
} // namespace

TEST(ForkDriverPackage, AcceptsAnAarch64SharedObject)
{
	EXPECT_EQ(ForkDriverPackage::ValidateElfHeader(MakeElfHeader()), Verdict::Ok);
}

// O caso perigoso: um ELF perfeitamente válido, com nome de driver, compilado para outra
// arquitetura. Sem esta checagem ele só falha lá adiante, dentro do dlopen, no meio do boot.
TEST(ForkDriverPackage, RejectsOtherArchitectures)
{
	constexpr u16 EM_X86_64 = 62;
	constexpr u16 EM_ARM32 = 40;
	constexpr u16 EM_RISCV = 243;

	for (const u16 machine : {EM_X86_64, EM_ARM32, EM_RISCV})
		EXPECT_EQ(ForkDriverPackage::ValidateElfHeader(MakeElfHeader(2, 1, machine)), Verdict::NotAarch64)
			<< "machine=" << machine;
}

TEST(ForkDriverPackage, Rejects32BitAndBigEndian)
{
	EXPECT_EQ(ForkDriverPackage::ValidateElfHeader(MakeElfHeader(1)), Verdict::NotElf64);
	EXPECT_EQ(ForkDriverPackage::ValidateElfHeader(MakeElfHeader(2, 2)), Verdict::NotLittleEndian);
}

// Um ELF big-endian tem e_machine em outra ordem de bytes, então reprová-lo por "arquitetura
// errada" reportaria a causa errada ao usuário. A ordem das checagens garante isso.
TEST(ForkDriverPackage, EndiannessIsCheckedBeforeArchitecture)
{
	const std::vector<u8> big_endian_aarch64 = MakeElfHeader(2, 2, ForkDriverPackage::EM_AARCH64);
	EXPECT_EQ(ForkDriverPackage::ValidateElfHeader(big_endian_aarch64), Verdict::NotLittleEndian);
}

TEST(ForkDriverPackage, RejectsExecutablesAndNonElfFiles)
{
	constexpr u16 ET_EXEC = 2;
	EXPECT_EQ(ForkDriverPackage::ValidateElfHeader(MakeElfHeader(2, 1, ForkDriverPackage::EM_AARCH64, ET_EXEC)),
		Verdict::NotSharedObject);

	const std::vector<u8> not_elf = MakeElfHeader(2, 1, ForkDriverPackage::EM_AARCH64,
		ForkDriverPackage::ET_DYN, /*valid_magic=*/false);
	EXPECT_EQ(ForkDriverPackage::ValidateElfHeader(not_elf), Verdict::NotAnElf);
}

// "Truncado" é só para arquivo que COMEÇA como ELF e acaba cedo. Um arquivo curto sem magic é
// outra coisa — e a distinção é a diferença entre mandar o usuário baixar de novo (certo, quando
// o download caiu) e mandá-lo baixar de novo um arquivo que nunca foi um driver (errado).
TEST(ForkDriverPackage, TruncationOnlyAppliesToRealElfFiles)
{
	EXPECT_EQ(ForkDriverPackage::ValidateElfHeader(MakeElfHeader(2, 1, ForkDriverPackage::EM_AARCH64,
					  ForkDriverPackage::ET_DYN, true, /*size=*/32)),
		Verdict::Truncated);

	// Curto demais até para conter o magic: não dá para afirmar mais nada.
	EXPECT_EQ(ForkDriverPackage::ValidateElfHeader({}), Verdict::Truncated);
	EXPECT_EQ(ForkDriverPackage::ValidateElfHeader(AsBytes("no")), Verdict::Truncated);
}

// O caso real: um link de download expirado devolve uma página de erro, o navegador a salva com
// nome de .so, e o usuário a importa.
TEST(ForkDriverPackage, ShortNonElfFilesAreNotCalledTruncated)
{
	EXPECT_EQ(ForkDriverPackage::ValidateElfHeader(AsBytes("<!DOCTYPE html><html>404 not found")),
		Verdict::NotAnElf);
	EXPECT_EQ(ForkDriverPackage::ValidateElfHeader(AsBytes("nada disso")), Verdict::NotAnElf);
}

// Vetores do FIPS 180-2. O hash é IDENTIDADE do binário: um relatório de compatibilidade que
// diga "Turnip 25.2" não distingue duas builds com o mesmo nome, e um A/B entre drivers sem hash
// compara rótulos em vez de binários.
TEST(ForkDriverPackage, Sha256MatchesKnownVectors)
{
	EXPECT_EQ(ForkDriverPackage::Sha256Bytes(AsBytes("")),
		"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	EXPECT_EQ(ForkDriverPackage::Sha256Bytes(AsBytes("abc")),
		"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	EXPECT_EQ(ForkDriverPackage::Sha256Bytes(
				  AsBytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
		"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(ForkDriverPackage, Sha256IsSensitiveToASingleBit)
{
	const std::string a = ForkDriverPackage::Sha256Bytes(AsBytes("turnip-25.2.0"));
	const std::string b = ForkDriverPackage::Sha256Bytes(AsBytes("turnip-25.2.1"));
	EXPECT_NE(a, b);
	EXPECT_EQ(a.size(), 64u);
}

TEST(ForkDriverPackage, MissingFileIsReportedAsMissing)
{
	const ForkDriverPackage::Inspection inspection =
		ForkDriverPackage::InspectLibrary("/nao/existe/libvulkan_freedreno.so");
	EXPECT_EQ(inspection.verdict, Verdict::MissingLibrary);
	EXPECT_FALSE(inspection.IsUsable());
	EXPECT_TRUE(inspection.sha256.empty());
	EXPECT_EQ(ForkDriverPackage::Sha256File("/nao/existe/coisa"), "");
}

// Todo veredito precisa de nome e de frase apresentável: um import recusado sem motivo legível
// vira relato de bug.
TEST(ForkDriverPackage, EveryVerdictHasANameAndAReason)
{
	const Verdict all[] = {
		Verdict::Ok,
		Verdict::MissingLibrary,
		Verdict::LibraryUnreadable,
		Verdict::Truncated,
		Verdict::NotAnElf,
		Verdict::NotElf64,
		Verdict::NotLittleEndian,
		Verdict::NotAarch64,
		Verdict::NotSharedObject,
	};
	for (const Verdict verdict : all)
	{
		EXPECT_STRNE(ForkDriverPackage::VerdictToString(verdict), "");
		EXPECT_STRNE(ForkDriverPackage::VerdictReason(verdict), "");
		EXPECT_STRNE(ForkDriverPackage::VerdictToString(verdict), "Unknown");
	}
}
