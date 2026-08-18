// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkRuntime.h"

#include "Fork/ForkConfig.h"
#include "GS/Renderers/Common/GSPresentationMetrics.h"

#include <mutex>

namespace
{
	/// Empurra a configuração para os módulos. Roda tanto no carregamento quanto em qualquer
	/// SetAndSave (inclusive vindo da UI Android por JNI), porque está registrada como observador
	/// — é isto que faz uma opção nova valer imediatamente sem reiniciar o VM.
	void ApplyToModules()
	{
		GSPresentationMetrics::SetEnabled(
			ForkConfig::GetBool(ForkConfig::Option::PresentationMetricsEnabled));
	}

	std::once_flag s_registered;
} // namespace

void ForkRuntime::LoadSettings(const SettingsInterface& si)
{
	std::call_once(s_registered, []() { ForkConfig::RegisterChangeCallback(&ApplyToModules); });

	// LoadSettings notifica os observadores no fim, então ApplyToModules roda a partir daqui.
	ForkConfig::LoadSettings(si);
}
