// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkLsfgPackage.h"

#include "GS/Renderers/Vulkan/GSLsfgShaderTable.h"

#include "common/FileSystem.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <set>

namespace
{
	/// Tipo de recurso onde os shaders vivem. RT_RCDATA do Windows.
	constexpr u32 RT_RCDATA = 10;

	/// Leituras little-endian sem alinhamento. Nunca leem fora do span: quem chama já conferiu o
	/// limite, e cada caminho de parsing aqui confere antes de avançar.
	u16 ReadU16(std::span<const u8> data, size_t offset)
	{
		return static_cast<u16>(data[offset]) | static_cast<u16>(static_cast<u16>(data[offset + 1]) << 8);
	}

	u32 ReadU32(std::span<const u8> data, size_t offset)
	{
		return static_cast<u32>(data[offset]) | (static_cast<u32>(data[offset + 1]) << 8) |
		       (static_cast<u32>(data[offset + 2]) << 16) | (static_cast<u32>(data[offset + 3]) << 24);
	}

	bool Fits(std::span<const u8> data, size_t offset, size_t length)
	{
		return offset <= data.size() && length <= data.size() - offset;
	}

	/// Uma seção do PE, o suficiente para traduzir RVA -> offset no arquivo.
	struct Section
	{
		u32 virtual_address = 0;
		u32 virtual_size = 0;
		u32 raw_pointer = 0;
		u32 raw_size = 0;
	};

	/// Traduz um endereço virtual para um offset dentro do arquivo em disco. Devolve false quando
	/// o RVA não cai em nenhuma seção mapeada — um arquivo montado à mão, ou truncado depois da
	/// tabela de seções.
	bool RvaToOffset(const std::vector<Section>& sections, u32 rva, size_t& out_offset)
	{
		for (const Section& section : sections)
		{
			const u32 span = std::max(section.virtual_size, section.raw_size);
			if (rva >= section.virtual_address && rva < section.virtual_address + span)
			{
				const u32 delta = rva - section.virtual_address;
				if (delta >= section.raw_size)
					return false;
				out_offset = static_cast<size_t>(section.raw_pointer) + delta;
				return true;
			}
		}
		return false;
	}

	/// True quando a entrada de dados em `entry_offset` aponta para bytes que existem de verdade
	/// dentro do arquivo. A entrada tem 16 bytes: RVA, tamanho, code page e reservado.
	bool BlobIsPresent(std::span<const u8> file, const std::vector<Section>& sections, size_t entry_offset);

	/// Uma entrada de diretório de recursos: um nome (ou id) e para onde ele aponta.
	struct ResourceEntry
	{
		u32 name = 0;
		u32 offset = 0;
		bool is_directory = false;
		bool is_named = false;
	};

	/// Lê as entradas de um diretório de recursos. Vazio quando o diretório não cabe no arquivo —
	/// o caminhamento para ali em vez de seguir com lixo.
	std::vector<ResourceEntry> ReadDirectory(std::span<const u8> file, size_t directory_offset)
	{
		std::vector<ResourceEntry> entries;
		if (!Fits(file, directory_offset, 16))
			return entries;

		const u32 named_count = ReadU16(file, directory_offset + 12);
		const u32 id_count = ReadU16(file, directory_offset + 14);
		const u32 total = named_count + id_count;
		// Um diretório de recursos com dezenas de milhares de entradas é sinal de cabeçalho
		// corrompido, não de um DLL rico. O teto evita transformar bytes ruins em trabalho.
		if (total > 65535)
			return entries;

		size_t cursor = directory_offset + 16;
		for (u32 i = 0; i < total; i++)
		{
			if (!Fits(file, cursor, 8))
				break;
			ResourceEntry entry;
			const u32 name = ReadU32(file, cursor);
			const u32 offset = ReadU32(file, cursor + 4);
			entry.is_named = (name & 0x80000000u) != 0;
			entry.name = name & 0x7FFFFFFFu;
			entry.is_directory = (offset & 0x80000000u) != 0;
			entry.offset = offset & 0x7FFFFFFFu;
			entries.push_back(entry);
			cursor += 8;
		}
		return entries;
	}
	bool BlobIsPresent(std::span<const u8> file, const std::vector<Section>& sections, size_t entry_offset)
	{
		if (!Fits(file, entry_offset, 16))
			return false;
		const u32 data_rva = ReadU32(file, entry_offset);
		const u32 data_size = ReadU32(file, entry_offset + 4);
		if (data_size == 0)
			return false;
		size_t data_offset = 0;
		if (!RvaToOffset(sections, data_rva, data_offset))
			return false;
		return Fits(file, data_offset, data_size);
	}
} // namespace

std::vector<u32> ForkLsfgPackage::RequiredResourceIds(bool performance_family)
{
	std::set<u32> ids;
	for (const auto& [name, id] : GSLsfgShaderTable::Get())
	{
		if (GSLsfgShaderTable::IsPerformanceShader(name) == performance_family)
			ids.insert(id);
	}
	return std::vector<u32>(ids.begin(), ids.end());
}

ForkLsfgPackage::Inspection ForkLsfgPackage::InspectBytes(std::span<const u8> file)
{
	Inspection result;
	result.size_bytes = file.size();

	// Magic antes de tamanho, pela mesma lição do inspetor de drivers: um arquivo curto SEM magic
	// é "isto não é um DLL", não "seu download foi interrompido". Mandar o usuário rebaixar uma
	// página de erro HTML é o pior conselho possível.
	if (file.size() < 2 || file[0] != 'M' || file[1] != 'Z')
	{
		result.verdict = Verdict::NotAPortableExecutable;
		return result;
	}
	if (file.size() < 0x40)
	{
		result.verdict = Verdict::Truncated;
		return result;
	}

	const u32 pe_offset = ReadU32(file, 0x3C);
	if (!Fits(file, pe_offset, 24))
	{
		result.verdict = Verdict::Truncated;
		return result;
	}
	if (file[pe_offset] != 'P' || file[pe_offset + 1] != 'E' || file[pe_offset + 2] != 0 ||
		file[pe_offset + 3] != 0)
	{
		result.verdict = Verdict::NotAPortableExecutable;
		return result;
	}

	result.machine = ReadU16(file, pe_offset + 4);
	const u32 section_count = ReadU16(file, pe_offset + 6);
	const u32 optional_size = ReadU16(file, pe_offset + 20);

	const size_t optional_offset = static_cast<size_t>(pe_offset) + 24;
	if (!Fits(file, optional_offset, optional_size) || optional_size < 2)
	{
		result.verdict = Verdict::Truncated;
		return result;
	}

	const u16 optional_magic = ReadU16(file, optional_offset);
	result.is_pe32plus = (optional_magic == 0x20B);
	// O diretório de dados começa depois do cabeçalho opcional, cujo tamanho difere entre PE32 e
	// PE32+. Recursos são a entrada 2.
	const size_t data_directory = optional_offset + (result.is_pe32plus ? 112 : 96);
	const size_t resource_directory_entry = data_directory + 2 * 8;
	if (!Fits(file, resource_directory_entry, 8))
	{
		result.verdict = Verdict::NoResources;
		return result;
	}
	const u32 resource_rva = ReadU32(file, resource_directory_entry);
	if (resource_rva == 0)
	{
		result.verdict = Verdict::NoResources;
		return result;
	}

	std::vector<Section> sections;
	size_t cursor = optional_offset + optional_size;
	for (u32 i = 0; i < section_count; i++)
	{
		if (!Fits(file, cursor, 40))
			break;
		Section section;
		section.virtual_size = ReadU32(file, cursor + 8);
		section.virtual_address = ReadU32(file, cursor + 12);
		section.raw_size = ReadU32(file, cursor + 16);
		section.raw_pointer = ReadU32(file, cursor + 20);
		sections.push_back(section);
		cursor += 40;
	}

	size_t root = 0;
	if (sections.empty() || !RvaToOffset(sections, resource_rva, root))
	{
		result.verdict = Verdict::NoResources;
		return result;
	}

	// Três níveis: tipo -> nome/id -> idioma. Só o tipo RCDATA interessa, e do segundo nível só
	// os ids numéricos: os shaders são recursos numerados.
	std::set<u32> present;
	for (const ResourceEntry& type : ReadDirectory(file, root))
	{
		if (type.is_named || type.name != RT_RCDATA || !type.is_directory)
			continue;

		for (const ResourceEntry& resource : ReadDirectory(file, root + type.offset))
		{
			if (resource.is_named)
				continue;

			// Um recurso conta como presente só quando o BLOB dele está mesmo no arquivo — não
			// basta a entrada de dados existir.
			//
			// Isso não é zelo teórico: a árvore de recursos mora no começo do `.rsrc` e os blobs
			// vêm depois. Num Lossless.dll de 7,5 MB truncado em 400 KB, a árvore inteira e todas
			// as 300 entradas de dados cabem — e conferir só até aí aprovava o arquivo como
			// "as duas famílias completas" quando 95% dos shaders tinham ido embora. Medido no
			// arquivo real; era o engano exato que este módulo existe para evitar.
			bool has_data = false;
			if (resource.is_directory)
			{
				for (const ResourceEntry& language : ReadDirectory(file, root + resource.offset))
				{
					if (!language.is_directory && BlobIsPresent(file, sections, root + language.offset))
					{
						has_data = true;
						break;
					}
				}
			}
			else
			{
				has_data = BlobIsPresent(file, sections, root + resource.offset);
			}

			if (has_data)
				present.insert(resource.name);
		}
	}

	result.rcdata_count = static_cast<u32>(present.size());

	const auto HasAll = [&present](const std::vector<u32>& required) {
		return std::all_of(required.begin(), required.end(),
			[&present](u32 id) { return present.count(id) != 0; });
	};
	result.has_standard_family = HasAll(RequiredResourceIds(false));
	result.has_performance_family = HasAll(RequiredResourceIds(true));

	// Uma família basta. Uma versão do Lossless Scaling costuma trazer 3.1 OU 3.1p, e exigir as
	// duas recusaria arquivos que funcionam perfeitamente.
	result.verdict = (result.has_standard_family || result.has_performance_family) ? Verdict::Ok
																				  : Verdict::NoShaderFamily;
	return result;
}

ForkLsfgPackage::Inspection ForkLsfgPackage::Inspect(const std::string& path)
{
	Inspection result;
	if (path.empty() || !FileSystem::FileExists(path.c_str()))
	{
		result.verdict = Verdict::Missing;
		return result;
	}

	std::optional<std::vector<u8>> data = FileSystem::ReadBinaryFile(path.c_str());
	if (!data.has_value())
	{
		result.verdict = Verdict::Unreadable;
		return result;
	}

	return InspectBytes(std::span<const u8>(data->data(), data->size()));
}

const char* ForkLsfgPackage::VerdictToString(Verdict value)
{
	switch (value)
	{
		case Verdict::Ok:
			return "Ok";
		case Verdict::Missing:
			return "Missing";
		case Verdict::Unreadable:
			return "Unreadable";
		case Verdict::NotAPortableExecutable:
			return "NotAPortableExecutable";
		case Verdict::Truncated:
			return "Truncated";
		case Verdict::NoResources:
			return "NoResources";
		case Verdict::NoShaderFamily:
			return "NoShaderFamily";
	}
	return "Missing";
}

const char* ForkLsfgPackage::VerdictReason(Verdict value)
{
	switch (value)
	{
		case Verdict::Ok:
			return "Arquivo válido, com shaders de frame generation.";
		case Verdict::Missing:
			return "O arquivo escolhido não existe mais.";
		case Verdict::Unreadable:
			return "O arquivo existe mas não pôde ser lido.";
		case Verdict::NotAPortableExecutable:
			return "Este arquivo não é um Lossless.dll.";
		case Verdict::Truncated:
			return "O arquivo está incompleto — provavelmente uma cópia interrompida.";
		case Verdict::NoResources:
			return "É um DLL, mas não contém recursos. Não é o Lossless.dll.";
		case Verdict::NoShaderFamily:
			return "Este DLL não traz os shaders de frame generation. Verifique se é o Lossless.dll "
				   "do Lossless Scaling e se ele está atualizado.";
	}
	return "O arquivo escolhido não existe mais.";
}
