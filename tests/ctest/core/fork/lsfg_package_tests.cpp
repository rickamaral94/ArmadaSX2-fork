// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkLsfgPackage.h"

#include "GS/Renderers/Vulkan/GSLsfgShaderTable.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace
{
	// Um PE mínimo, montado byte a byte, com exatamente os recursos que o teste pedir.
	//
	// Montado aqui e não lido de um arquivo de apoio de propósito: o único Lossless.dll que
	// existe é o que o usuário comprou, ele não pode ser versionado, e um teste que depende de um
	// arquivo que o CI não tem é um teste que não roda. O que se exercita é a lógica de
	// caminhamento — que é onde os bugs moram.
	class PeBuilder
	{
	public:
		explicit PeBuilder(std::vector<u32> rcdata_ids)
			: m_ids(std::move(rcdata_ids))
		{
		}

		void SetPe32Plus(bool value) { m_pe32plus = value; }
		void SetResourceRva(u32 value) { m_resource_rva_override = value; }
		void SetEmptyBlobs(bool value) { m_empty_blobs = value; }

		std::vector<u8> Build() const
		{
			// Layout: [DOS 0x40][PE headers][section table][.rsrc]
			const size_t pe_offset = 0x40;
			const size_t optional_size = m_pe32plus ? 240 : 224;
			const size_t section_table = pe_offset + 24 + optional_size;
			const size_t rsrc_raw = section_table + 40;
			const u32 rsrc_rva = 0x1000;
			std::vector<u8> rsrc = BuildResourceSection(rsrc_rva);

			std::vector<u8> file(rsrc_raw + rsrc.size(), 0);
			file[0] = 'M';
			file[1] = 'Z';
			WriteU32(file, 0x3C, static_cast<u32>(pe_offset));

			file[pe_offset] = 'P';
			file[pe_offset + 1] = 'E';
			WriteU16(file, pe_offset + 4, 0x8664); // machine
			WriteU16(file, pe_offset + 6, 1); // one section
			WriteU16(file, pe_offset + 20, static_cast<u16>(optional_size));

			const size_t optional_offset = pe_offset + 24;
			WriteU16(file, optional_offset, m_pe32plus ? 0x20B : 0x10B);
			const size_t data_directory = optional_offset + (m_pe32plus ? 112 : 96);
			WriteU32(file, data_directory + 2 * 8,
				m_resource_rva_override != 0 ? m_resource_rva_override : rsrc_rva);
			WriteU32(file, data_directory + 2 * 8 + 4, static_cast<u32>(rsrc.size()));

			std::memcpy(file.data() + section_table, ".rsrc\0\0\0", 8);
			WriteU32(file, section_table + 8, static_cast<u32>(rsrc.size())); // virtual size
			WriteU32(file, section_table + 12, rsrc_rva);
			WriteU32(file, section_table + 16, static_cast<u32>(rsrc.size())); // raw size
			WriteU32(file, section_table + 20, static_cast<u32>(rsrc_raw));

			std::copy(rsrc.begin(), rsrc.end(), file.begin() + static_cast<ptrdiff_t>(rsrc_raw));
			return file;
		}

	private:
		static void WriteU16(std::vector<u8>& out, size_t offset, u16 value)
		{
			out[offset] = static_cast<u8>(value & 0xFF);
			out[offset + 1] = static_cast<u8>((value >> 8) & 0xFF);
		}

		static void WriteU32(std::vector<u8>& out, size_t offset, u32 value)
		{
			for (size_t i = 0; i < 4; i++)
				out[offset + i] = static_cast<u8>((value >> (8 * i)) & 0xFF);
		}

		// tipo (RCDATA) -> id -> idioma -> entrada de dados -> blob. Os offsets internos da árvore
		// são relativos ao início da seção; o da entrada de dados é um RVA, que é como o formato
		// define — e é justamente essa diferença que faz um arquivo truncado ainda ter a árvore
		// inteira e nenhum blob.
		std::vector<u8> BuildResourceSection(u32 section_rva) const
		{
			const size_t type_dir = 0;
			const size_t id_dir = 16 + 8; // um tipo
			const size_t lang_dirs = id_dir + 16 + 8 * m_ids.size();
			const size_t data_entries = lang_dirs + m_ids.size() * (16 + 8);
			const size_t blobs = data_entries + m_ids.size() * 16;

			std::vector<u8> out(blobs + m_ids.size() * BLOB_SIZE, 0xAB);

			std::fill(out.begin(), out.begin() + static_cast<ptrdiff_t>(blobs), u8{0});
			WriteU16(out, type_dir + 12, 0); // nomeados
			WriteU16(out, type_dir + 14, 1); // por id
			WriteU32(out, type_dir + 16, 10); // RT_RCDATA
			WriteU32(out, type_dir + 20, static_cast<u32>(id_dir) | 0x80000000u);

			WriteU16(out, id_dir + 12, 0);
			WriteU16(out, id_dir + 14, static_cast<u16>(m_ids.size()));
			for (size_t i = 0; i < m_ids.size(); i++)
			{
				const size_t entry = id_dir + 16 + i * 8;
				const size_t lang_dir = lang_dirs + i * (16 + 8);
				WriteU32(out, entry, m_ids[i]);
				WriteU32(out, entry + 4, static_cast<u32>(lang_dir) | 0x80000000u);

				WriteU16(out, lang_dir + 12, 0);
				WriteU16(out, lang_dir + 14, 1);
				WriteU32(out, lang_dir + 16, 0x0409); // en-US
				const size_t data_entry = data_entries + i * 16;
				WriteU32(out, lang_dir + 20, static_cast<u32>(data_entry));

				WriteU32(out, data_entry, section_rva + static_cast<u32>(blobs + i * BLOB_SIZE));
				WriteU32(out, data_entry + 4, m_empty_blobs ? 0u : BLOB_SIZE);
			}
			return out;
		}

		static constexpr u32 BLOB_SIZE = 8;

		std::vector<u32> m_ids;
		bool m_pe32plus = true;
		u32 m_resource_rva_override = 0;
		bool m_empty_blobs = false;
	};

	std::vector<u32> IdsFor(bool performance) { return ForkLsfgPackage::RequiredResourceIds(performance); }

	ForkLsfgPackage::Inspection InspectIds(const std::vector<u32>& ids)
	{
		const std::vector<u8> file = PeBuilder(ids).Build();
		return ForkLsfgPackage::InspectBytes(file);
	}
} // namespace

// A tabela de ids é a única fonte da verdade, e as duas famílias existem nela. Se alguém apagar
// metade do `GSLsfgShaderTable`, é aqui que aparece — antes de a UI passar a aprovar tudo.
TEST(ForkLsfgPackage, BothFamiliesAreDerivedFromTheSharedTable)
{
	const std::vector<u32> standard = IdsFor(false);
	const std::vector<u32> performance = IdsFor(true);
	EXPECT_FALSE(standard.empty());
	EXPECT_FALSE(performance.empty());
	// Ordenados e sem repetição: são um conjunto, não uma lista de leitura. A tabela repete ids
	// de propósito (delta[0] e gamma[0] são ambos 257), e uma lista com repetição faria a
	// verificação de família conferir o mesmo recurso duas vezes.
	EXPECT_TRUE(std::is_sorted(standard.begin(), standard.end()));
	EXPECT_TRUE(std::is_sorted(performance.begin(), performance.end()));
	std::vector<u32> deduplicated = standard;
	deduplicated.erase(std::unique(deduplicated.begin(), deduplicated.end()), deduplicated.end());
	EXPECT_EQ(deduplicated.size(), standard.size());

	// 255 e 256 são comuns às duas famílias — o mipmaps e o generate. Se essa sobreposição sumir,
	// a derivação por prefixo parou de refletir a tabela.
	EXPECT_NE(std::find(standard.begin(), standard.end(), 255u), standard.end());
	EXPECT_NE(std::find(performance.begin(), performance.end(), 255u), performance.end());
}

TEST(ForkLsfgPackage, AcceptsAFileCarryingTheStandardFamily)
{
	const ForkLsfgPackage::Inspection inspection = InspectIds(IdsFor(false));
	EXPECT_TRUE(inspection.IsUsable());
	EXPECT_TRUE(inspection.has_standard_family);
	EXPECT_FALSE(inspection.has_performance_family);
}

// Uma versão do Lossless Scaling traz uma família OU a outra. Exigir as duas recusaria arquivos
// que funcionam — e é por isso que o pedido de 3.1p sabe cair para 3.1 em vez de falhar.
TEST(ForkLsfgPackage, OneFamilyIsEnough)
{
	const ForkLsfgPackage::Inspection inspection = InspectIds(IdsFor(true));
	EXPECT_TRUE(inspection.IsUsable());
	EXPECT_TRUE(inspection.has_performance_family);
}

TEST(ForkLsfgPackage, ReportsBothFamiliesWhenBothArePresent)
{
	std::vector<u32> both = IdsFor(false);
	const std::vector<u32> performance = IdsFor(true);
	both.insert(both.end(), performance.begin(), performance.end());
	const ForkLsfgPackage::Inspection inspection = InspectIds(both);
	EXPECT_TRUE(inspection.IsUsable());
	EXPECT_TRUE(inspection.has_standard_family);
	EXPECT_TRUE(inspection.has_performance_family);
}

// O caso que motivou o módulo: um DLL de verdade, com recursos de verdade, que simplesmente não é
// este. Recusado com um motivo que diz o que fazer, em vez de "falha ao inicializar" no meio de um
// quadro, muito depois.
TEST(ForkLsfgPackage, RejectsARealDllThatIsNotLosslessScaling)
{
	const ForkLsfgPackage::Inspection inspection = InspectIds({101, 102, 103});
	EXPECT_FALSE(inspection.IsUsable());
	EXPECT_EQ(inspection.verdict, ForkLsfgPackage::Verdict::NoShaderFamily);
	EXPECT_EQ(inspection.rcdata_count, 3u);
}

// Uma família a que falta UM shader não serve: o extrator pede todos, e um "quase" aqui vira uma
// falha dentro do primeiro quadro.
TEST(ForkLsfgPackage, AFamilyMissingASingleResourceIsNotAFamily)
{
	std::vector<u32> almost = IdsFor(false);
	almost.pop_back();
	const ForkLsfgPackage::Inspection inspection = InspectIds(almost);
	EXPECT_FALSE(inspection.IsUsable());
	EXPECT_FALSE(inspection.has_standard_family);
}

// A lição que o inspetor de drivers já tinha aprendido, repetida aqui porque o erro é o mesmo: uma
// página de erro HTML salva com nome de .dll é "isto não é um DLL", nunca "seu download foi
// interrompido" — mandar rebaixar algo que nunca foi o arquivo certo não ajuda ninguém.
TEST(ForkLsfgPackage, ShortFilesWithoutMagicAreNotTruncated)
{
	const char* html = "<html><body>404 Not Found</body></html>";
	const ForkLsfgPackage::Inspection inspection = ForkLsfgPackage::InspectBytes(
		std::span<const u8>(reinterpret_cast<const u8*>(html), std::strlen(html)));
	EXPECT_EQ(inspection.verdict, ForkLsfgPackage::Verdict::NotAPortableExecutable);
}

TEST(ForkLsfgPackage, EmptyFileIsNotAPortableExecutable)
{
	const ForkLsfgPackage::Inspection inspection = ForkLsfgPackage::InspectBytes({});
	EXPECT_EQ(inspection.verdict, ForkLsfgPackage::Verdict::NotAPortableExecutable);
	EXPECT_EQ(inspection.size_bytes, 0u);
}

// Com magic e sem cabeçalho, aí sim: começou a ser um DLL e não terminou.
TEST(ForkLsfgPackage, MagicWithoutAHeaderIsTruncated)
{
	const std::vector<u8> stub = {'M', 'Z', 0x90, 0x00};
	EXPECT_EQ(ForkLsfgPackage::InspectBytes(stub).verdict, ForkLsfgPackage::Verdict::Truncated);
}

// O caso que só apareceu medindo o arquivo de verdade: um Lossless.dll de 7,5 MB truncado em
// 400 KB mantém a ÁRVORE de recursos inteira — ela mora no começo do `.rsrc` — e todas as 300
// entradas de dados. Só os blobs, que vêm depois, se perdem. Conferir a árvore aprovava esse
// arquivo como "as duas famílias completas" com 95% dos shaders faltando; a presença tem que ser
// medida no blob, não na entrada que aponta para ele.
TEST(ForkLsfgPackage, ATruncatedFileKeepsItsResourceTreeButNotItsShaders)
{
	const std::vector<u32> ids = IdsFor(false);
	std::vector<u8> file = PeBuilder(ids).Build();
	ASSERT_TRUE(ForkLsfgPackage::InspectBytes(file).IsUsable());

	// Corta só os blobs, deixando a árvore e as entradas de dados intactas — exatamente o que um
	// download interrompido deixa para trás.
	file.resize(file.size() - 8 * ids.size());
	const ForkLsfgPackage::Inspection inspection = ForkLsfgPackage::InspectBytes(file);
	EXPECT_FALSE(inspection.IsUsable());
	EXPECT_EQ(inspection.verdict, ForkLsfgPackage::Verdict::NoShaderFamily);
	EXPECT_LT(inspection.rcdata_count, static_cast<u32>(ids.size()));
}

// Uma entrada de dados com tamanho zero é uma entrada, não um shader.
TEST(ForkLsfgPackage, EmptyResourcesDoNotCountAsPresent)
{
	PeBuilder builder(IdsFor(false));
	builder.SetEmptyBlobs(true);
	const ForkLsfgPackage::Inspection inspection = ForkLsfgPackage::InspectBytes(builder.Build());
	EXPECT_FALSE(inspection.IsUsable());
	EXPECT_EQ(inspection.rcdata_count, 0u);
}

// PE32 (32 bits) não é motivo de recusa: o extrator lê RECURSOS, e recurso não tem arquitetura.
// Reprovar aqui inventaria uma restrição que o código que consome o arquivo não tem.
TEST(ForkLsfgPackage, Pe32IsNotRejectedForBeing32Bit)
{
	PeBuilder builder(IdsFor(false));
	builder.SetPe32Plus(false);
	const std::vector<u8> file = builder.Build();
	const ForkLsfgPackage::Inspection inspection = ForkLsfgPackage::InspectBytes(file);
	EXPECT_TRUE(inspection.IsUsable());
	EXPECT_FALSE(inspection.is_pe32plus);
}

// Um RVA de recursos que não cai em nenhuma seção é cabeçalho mentindo. Parar ali é o certo:
// seguir o ponteiro leria bytes arbitrários do arquivo como se fossem uma árvore de recursos.
TEST(ForkLsfgPackage, AResourceRvaOutsideEverySectionIsNoResources)
{
	PeBuilder builder(IdsFor(false));
	builder.SetResourceRva(0x900000);
	const std::vector<u8> file = builder.Build();
	EXPECT_EQ(ForkLsfgPackage::InspectBytes(file).verdict, ForkLsfgPackage::Verdict::NoResources);
}

TEST(ForkLsfgPackage, MissingFileIsReportedAsMissingNotUnreadable)
{
	const ForkLsfgPackage::Inspection inspection = ForkLsfgPackage::Inspect("/nao/existe/Lossless.dll");
	EXPECT_EQ(inspection.verdict, ForkLsfgPackage::Verdict::Missing);
}

// Todo veredito precisa de nome e de frase: um import recusado sem motivo legível vira relato de
// bug, e um veredito novo sem texto passaria despercebido até chegar na tela de alguém.
TEST(ForkLsfgPackage, EveryVerdictHasANameAndAReason)
{
	using V = ForkLsfgPackage::Verdict;
	for (const V verdict : {V::Ok, V::Missing, V::Unreadable, V::NotAPortableExecutable, V::Truncated,
			 V::NoResources, V::NoShaderFamily})
	{
		EXPECT_STRNE(ForkLsfgPackage::VerdictToString(verdict), "");
		EXPECT_GT(std::strlen(ForkLsfgPackage::VerdictReason(verdict)), 10u);
	}
}
