// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkGpuCapabilities.h"

#include "common/Console.h"

#include "fmt/format.h"

#include <mutex>

#if defined(__ANDROID__)
#include <array>
#include <cstdlib>
#include <sys/system_properties.h>
#endif

namespace
{
	std::mutex s_mutex;
	ForkGpuCapabilities::Capabilities s_capabilities;

	/// Adreno das séries em que builds de Turnip existem e são exercitadas. AdrenoX (Snapdragon X,
	/// notebooks) fica de fora aqui de propósito: o veredito é sobre carregar driver no Android, e
	/// aquele silício não roda este caminho.
	bool IsTurnipEraAdreno(MobileGpuArchitecture architecture)
	{
		switch (architecture)
		{
			case MobileGpuArchitecture::Adreno6xx:
			case MobileGpuArchitecture::Adreno7xx:
			case MobileGpuArchitecture::Adreno8xx:
				return true;
			default:
				return false;
		}
	}

	u32 ReadAndroidSdk()
	{
#if defined(__ANDROID__)
		std::array<char, PROP_VALUE_MAX> value = {};
		const int length = __system_property_get("ro.build.version.sdk", value.data());
		if (length <= 0)
			return 0;
		return static_cast<u32>(std::atoi(value.data()));
#else
		return 0;
#endif
	}

	constexpr bool IsAndroidBuild()
	{
#if defined(__ANDROID__)
		return true;
#else
		return false;
#endif
	}
} // namespace

ForkGpuCapabilities::TurnipSupport ForkGpuCapabilities::EvaluateTurnipSupport(
	RuntimeGpuProfile vendor, MobileGpuArchitecture architecture, u32 android_sdk, bool is_android)
{
	if (!is_android)
		return TurnipSupport::NotAndroid;

	// A ordem importa: o fabricante é checado ANTES da versão de Android, para que uma Mali em
	// Android 15 seja recusada por ser Mali — e não por um motivo que sugira que atualizar o
	// sistema resolveria. Turnip é freedreno; em Mali/PowerVR/Xclipse não existe pergunta a fazer.
	if (vendor != RuntimeGpuProfile::Adreno)
		return TurnipSupport::UnsupportedVendor;

	// Adreno reconhecida, mas de geração anterior às builds de Turnip que circulam.
	if (!IsTurnipEraAdreno(architecture))
		return TurnipSupport::UnsupportedAdrenoGeneration;

	if (android_sdk != 0 && android_sdk < MIN_ANDROID_SDK_FOR_CUSTOM_DRIVER)
		return TurnipSupport::UnsupportedAndroidVersion;

	// android_sdk == 0 significa "não consegui ler a propriedade", não "versão 0". Recusar aí
	// esconderia a funcionalidade de aparelhos perfeitamente capazes por causa de uma leitura que
	// falhou; o carregador ainda tem o próprio fallback para o driver do sistema.
	return TurnipSupport::Supported;
}

void ForkGpuCapabilities::Publish(const GpuProfileSelection& profile, u32 vulkan_api_version)
{
	Capabilities caps;
	caps.probed = true;
	caps.vendor = profile.runtime_profile;
	caps.architecture = profile.gpu.architecture;
	caps.model_number = profile.gpu.model_number;
	caps.gpu_name = profile.gpu.name;
	caps.active_driver = profile.driver.driver;
	caps.vulkan_api_version = vulkan_api_version;
	caps.android_sdk = ReadAndroidSdk();
	caps.turnip = EvaluateTurnipSupport(caps.vendor, caps.architecture, caps.android_sdk, IsAndroidBuild());

	{
		std::lock_guard lock(s_mutex);
		s_capabilities = std::move(caps);
	}

	Console.WriteLn("ForkGpuCapabilities: %s", DescribeForLog().c_str());
}

ForkGpuCapabilities::Capabilities ForkGpuCapabilities::Get()
{
	std::lock_guard lock(s_mutex);
	return s_capabilities;
}

bool ForkGpuCapabilities::IsTurnipCapable()
{
	std::lock_guard lock(s_mutex);
	return s_capabilities.turnip == TurnipSupport::Supported;
}

const char* ForkGpuCapabilities::TurnipSupportToString(TurnipSupport value)
{
	switch (value)
	{
		case TurnipSupport::Unknown:
			return "Unknown";
		case TurnipSupport::Supported:
			return "Supported";
		case TurnipSupport::NotAndroid:
			return "NotAndroid";
		case TurnipSupport::UnsupportedVendor:
			return "UnsupportedVendor";
		case TurnipSupport::UnsupportedAdrenoGeneration:
			return "UnsupportedAdrenoGeneration";
		case TurnipSupport::UnsupportedAndroidVersion:
			return "UnsupportedAndroidVersion";
	}
	return "Unknown";
}

const char* ForkGpuCapabilities::TurnipSupportReason(TurnipSupport value)
{
	// Frases para o usuário. Uma seção que some sem explicação vira relato de bug; um motivo
	// escrito encerra a dúvida.
	switch (value)
	{
		case TurnipSupport::Unknown:
			return "Ainda não detectado — inicie um jogo uma vez para sondar a GPU.";
		case TurnipSupport::Supported:
			return "Disponível.";
		case TurnipSupport::NotAndroid:
			return "Troca de driver Vulkan só existe no Android.";
		case TurnipSupport::UnsupportedVendor:
			return "Turnip é um driver para GPUs Adreno (Qualcomm). Esta GPU usa outra arquitetura.";
		case TurnipSupport::UnsupportedAdrenoGeneration:
			return "Esta Adreno é anterior à série 6xx, para a qual não há builds de Turnip.";
		case TurnipSupport::UnsupportedAndroidVersion:
			return "Carregar driver customizado exige Android 10 ou mais recente.";
	}
	return "Indisponível.";
}

std::string ForkGpuCapabilities::FormatVulkanVersion(u32 packed)
{
	if (packed == 0)
		return "desconhecida";

	// Mesmo empacotamento do VK_MAKE_API_VERSION, desmontado à mão para não arrastar os headers do
	// Vulkan para dentro de um módulo que o resto do fork inclui livremente.
	const u32 major = (packed >> 22) & 0x7Fu;
	const u32 minor = (packed >> 12) & 0x3FFu;
	const u32 patch = packed & 0xFFFu;
	return fmt::format("{}.{}.{}", major, minor, patch);
}

std::string ForkGpuCapabilities::DescribeForLog()
{
	const Capabilities caps = Get();
	if (!caps.probed)
		return "GPU ainda não sondada";

	return fmt::format("GPU {} ({}, {}) | Vulkan {} | Android SDK {} | driver ativo {} | Turnip: {}",
		caps.gpu_name, GpuProfileDetector::RuntimeProfileToString(caps.vendor),
		GpuProfileDetector::ArchitectureToString(caps.architecture),
		FormatVulkanVersion(caps.vulkan_api_version),
		caps.android_sdk != 0 ? fmt::format("{}", caps.android_sdk) : std::string("desconhecido"),
		GpuProfileDetector::DriverToString(caps.active_driver), TurnipSupportToString(caps.turnip));
}
