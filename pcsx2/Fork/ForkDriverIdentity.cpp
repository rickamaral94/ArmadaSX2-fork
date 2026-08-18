// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkDriverIdentity.h"

#include "Fork/ForkGpuCapabilities.h"

#include "common/Console.h"

#include "fmt/format.h"

#include <cctype>
#include <mutex>

namespace
{
	std::mutex s_mutex;
	ForkDriverIdentity::Identity s_identity;
	/// Guardado fora de `s_identity` porque é informado ANTES do renderer subir e precisa
	/// sobreviver à publicação, que reconstrói o resto.
	std::string s_selected_package_sha256;

	bool StartsWithMesaTag(std::string_view text, size_t position)
	{
		static constexpr std::string_view TAG = "mesa";
		if (position + TAG.size() > text.size())
			return false;
		for (size_t i = 0; i < TAG.size(); i++)
		{
			if (std::tolower(static_cast<unsigned char>(text[position + i])) != TAG[i])
				return false;
		}
		return true;
	}

	/// Lê um inteiro decimal a partir de `position`, avançando-a. Devolve false se não houver
	/// dígito algum ali.
	bool ReadNumber(std::string_view text, size_t& position, u32& out)
	{
		const size_t start = position;
		u32 value = 0;
		while (position < text.size() && std::isdigit(static_cast<unsigned char>(text[position])))
		{
			value = (value * 10) + static_cast<u32>(text[position] - '0');
			position++;
		}
		if (position == start)
			return false;
		out = value;
		return true;
	}
} // namespace

ForkDriverIdentity::MesaVersion ForkDriverIdentity::ParseMesaVersion(std::string_view driver_info)
{
	MesaVersion version;
	version.raw = std::string(driver_info);

	// O formato do freedreno é "Mesa 25.2.0-devel (git-1a2b3c4)". Procuramos a etiqueta em vez de
	// assumir que ela abre a string: alguns empacotadores prefixam o texto com o nome do build.
	for (size_t i = 0; i < driver_info.size(); i++)
	{
		if (!StartsWithMesaTag(driver_info, i))
			continue;

		size_t position = i + 4; // após "Mesa"
		while (position < driver_info.size() &&
			   std::isspace(static_cast<unsigned char>(driver_info[position])))
			position++;

		u32 major = 0;
		if (!ReadNumber(driver_info, position, major))
			continue; // "Mesa" solto, sem número: segue procurando

		u32 minor = 0;
		u32 patch = 0;
		if (position < driver_info.size() && driver_info[position] == '.')
		{
			position++;
			ReadNumber(driver_info, position, minor);
			if (position < driver_info.size() && driver_info[position] == '.')
			{
				position++;
				ReadNumber(driver_info, position, patch);
			}
		}

		version.known = true;
		version.major = major;
		version.minor = minor;
		version.patch = patch;
		return version;
	}

	return version;
}

ForkDriverIdentity::LoadOutcome ForkDriverIdentity::EvaluateOutcome(
	bool requested, bool opened, MobileGpuDriver active_driver, bool driver_properties_available)
{
	if (!requested)
		return LoadOutcome::SystemDriverByChoice;

	if (!opened)
		return LoadOutcome::FellBackToSystem;

	// Handle aberto, mas sem VK_KHR_driver_properties não há como saber QUEM abriu. Acusar
	// divergência aqui seria gritar lobo em aparelhos antigos cujo driver simplesmente não
	// reporta identidade — e um aviso falso ensina o usuário a ignorar os verdadeiros.
	if (!driver_properties_available)
		return LoadOutcome::CustomDriverActive;

	if (active_driver == MobileGpuDriver::MesaTurnip)
		return LoadOutcome::CustomDriverActive;

	return LoadOutcome::CustomOpenedButNotTurnip;
}

const char* ForkDriverIdentity::OutcomeToString(LoadOutcome value)
{
	switch (value)
	{
		case LoadOutcome::Unknown:
			return "Unknown";
		case LoadOutcome::SystemDriverByChoice:
			return "SystemDriverByChoice";
		case LoadOutcome::CustomDriverActive:
			return "CustomDriverActive";
		case LoadOutcome::FellBackToSystem:
			return "FellBackToSystem";
		case LoadOutcome::CustomOpenedButNotTurnip:
			return "CustomOpenedButNotTurnip";
	}
	return "Unknown";
}

const char* ForkDriverIdentity::OutcomeReason(LoadOutcome value)
{
	switch (value)
	{
		case LoadOutcome::Unknown:
			return "Ainda não detectado — inicie um jogo uma vez.";
		case LoadOutcome::SystemDriverByChoice:
			return "Usando o driver Vulkan do sistema.";
		case LoadOutcome::CustomDriverActive:
			return "Driver customizado ativo.";
		case LoadOutcome::FellBackToSystem:
			return "O driver selecionado não pôde ser carregado; o do sistema está em uso.";
		case LoadOutcome::CustomOpenedButNotTurnip:
			return "O driver carregou, mas não se identifica como Turnip.";
	}
	return "Estado desconhecido.";
}

bool ForkDriverIdentity::IsUnexpected(LoadOutcome value)
{
	return value == LoadOutcome::FellBackToSystem || value == LoadOutcome::CustomOpenedButNotTurnip;
}

void ForkDriverIdentity::Publish(const PublishInput& input)
{
	Identity identity;
	identity.probed = true;
	identity.requested_driver = input.requested_driver;
	identity.load_error = input.load_error;
	identity.active_driver = input.active_driver;
	identity.driver_name = input.driver_name;
	identity.driver_info = input.driver_info;
	identity.gpu_name = input.gpu_name;
	identity.vulkan_api_version = input.vulkan_api_version;
	identity.driver_version_raw = input.driver_version_raw;
	identity.driver_properties_available = input.driver_properties_available;
	identity.mesa = ParseMesaVersion(input.driver_info);
	identity.outcome = EvaluateOutcome(
		input.requested, input.opened, input.active_driver, input.driver_properties_available);

	{
		std::lock_guard lock(s_mutex);
		identity.package_sha256 = s_selected_package_sha256;
		s_identity = std::move(identity);
	}

	const std::string description = DescribeForLog();
	// Divergência sobe como Warning: é a única pista que o usuário tem de que está medindo o
	// driver errado, e ela precisa aparecer em um log que alguém vai colar em um relatório.
	if (IsUnexpected(Get().outcome))
		Console.Warning("ForkDriverIdentity: %s", description.c_str());
	else
		Console.WriteLn("ForkDriverIdentity: %s", description.c_str());
}

ForkDriverIdentity::Identity ForkDriverIdentity::Get()
{
	std::lock_guard lock(s_mutex);
	return s_identity;
}

void ForkDriverIdentity::NoteSelectedPackageSha256(std::string sha256)
{
	std::lock_guard lock(s_mutex);
	s_selected_package_sha256 = std::move(sha256);
	s_identity.package_sha256 = s_selected_package_sha256;
}

std::string ForkDriverIdentity::DescribeForLog()
{
	const Identity identity = Get();
	if (!identity.probed)
		return "driver ainda não sondado";

	std::string line = fmt::format("{} | driver {} ({})", OutcomeToString(identity.outcome),
		GpuProfileDetector::DriverToString(identity.active_driver),
		identity.driver_name.empty() ? "sem nome reportado" : identity.driver_name);

	if (identity.mesa.known)
		line += fmt::format(" | Mesa {}.{}.{}", identity.mesa.major, identity.mesa.minor, identity.mesa.patch);
	if (!identity.driver_info.empty())
		line += fmt::format(" | info \"{}\"", identity.driver_info);

	line += fmt::format(" | Vulkan {}", ForkGpuCapabilities::FormatVulkanVersion(identity.vulkan_api_version));

	if (!identity.gpu_name.empty())
		line += fmt::format(" | GPU {}", identity.gpu_name);
	if (!identity.requested_driver.empty())
		line += fmt::format(" | pedido {}", identity.requested_driver);
	if (!identity.package_sha256.empty())
		line += fmt::format(" | sha256 {}", identity.package_sha256);
	if (!identity.load_error.empty())
		line += fmt::format(" | erro: {}", identity.load_error);
	if (!identity.driver_properties_available)
		line += " | (driverProperties indisponível — identidade não confirmada)";

	return line;
}
