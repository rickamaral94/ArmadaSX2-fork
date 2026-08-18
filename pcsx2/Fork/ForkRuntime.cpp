// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkRuntime.h"

#include "Fork/ForkConfig.h"
#include "GS/Renderers/Common/GSPresentationMetrics.h"

#if defined(__ANDROID__) && defined(ENABLE_VULKAN)
#include "GS/Renderers/Vulkan/VKLoader.h"
#endif

#include "common/Console.h"

#include <mutex>

namespace
{
	/// Empurra a configuração para os módulos. Roda tanto no carregamento quanto em qualquer
	/// SetAndSave (inclusive vindo da UI Android por JNI), porque está registrada como observador
	/// — é isto que faz uma opção nova valer imediatamente sem reiniciar o VM.
	/// Fase 5: aplica a seleção de driver resolvida das camadas (global + jogo).
	///
	/// Roda a partir de VMManager::LoadSettings, que acontece na inicialização da VM ANTES do
	/// primeiro MTGS::Open — e é esse primeiro Open que dispara o LoadVulkanLibrary onde o
	/// VKLoader lê o driver. Ou seja: o override por jogo chega a tempo sem ninguém coordenar
	/// ordens de chamada, porque o ponto de aplicação é o mesmo em que o upstream reconstrói
	/// toda a configuração.
	void ApplyDriverSelection()
	{
#if defined(__ANDROID__) && defined(ENABLE_VULKAN)
		const std::string mode = ForkConfig::GetString(ForkConfig::Option::DriverMode);
		const std::string dir = ForkConfig::GetString(ForkConfig::Option::DriverDir);
		const std::string name = ForkConfig::GetString(ForkConfig::Option::DriverName);
		const std::string redirect = ForkConfig::GetString(ForkConfig::Option::DriverRedirectDir);
		const std::string hook = ForkConfig::GetString(ForkConfig::Option::DriverHookLibDir);

		switch (ForkConfig::ResolveDriverSelection(mode, dir, name, hook))
		{
			case ForkConfig::DriverSelection::Inherit:
				// Ninguém opinou: mantém o que o frontend já configurou. Sobrescrever aqui
				// desfaria a seleção global feita antes da VM subir.
				break;

			case ForkConfig::DriverSelection::System:
				Console.WriteLn("ForkRuntime: driver do sistema (por configuração).");
				Vulkan::SetCustomDriverPath("", "", "", "");
				break;

			case ForkConfig::DriverSelection::Custom:
				Console.WriteLn("ForkRuntime: driver customizado '%s' (por configuração).", name.c_str());
				Vulkan::SetCustomDriverPath(dir.c_str(), name.c_str(), redirect.c_str(), hook.c_str());
				break;
		}
#endif
	}

	void ApplyToModules()
	{
		GSPresentationMetrics::SetEnabled(
			ForkConfig::GetBool(ForkConfig::Option::PresentationMetricsEnabled));
		ApplyDriverSelection();
	}

	std::once_flag s_registered;
} // namespace

void ForkRuntime::LoadSettings(const SettingsInterface& si)
{
	std::call_once(s_registered, []() { ForkConfig::RegisterChangeCallback(&ApplyToModules); });

	// LoadSettings notifica os observadores no fim, então ApplyToModules roda a partir daqui.
	ForkConfig::LoadSettings(si);
}
