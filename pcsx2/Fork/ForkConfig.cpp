// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkConfig.h"

#include "Host.h"

#include "common/Console.h"
#include "common/SettingsInterface.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <vector>

namespace
{
	using namespace ForkConfig;

	constexpr size_t OPTION_COUNT = static_cast<size_t>(Option::Count);

	/// A tabela. Acrescentar uma opção do fork é acrescentar UMA linha aqui e uma entrada no enum.
	/// Nenhum arquivo do upstream muda, nenhuma função de JNI nova é escrita, e a UI se constrói a
	/// partir desta tabela.
	constexpr std::array<OptionDesc, OPTION_COUNT> OPTIONS = {{
		{Option::PresentationMetricsEnabled, "PresentationMetrics.Enabled", Type::Bool, false, 0, 0.0f, "",
			"Mede FPS real e FPS apresentado, frametime e 1% low na camada de apresentação."},
		{Option::PresentationMetricsOverlay, "PresentationMetrics.Overlay", Type::Bool, false, 0, 0.0f, "",
			"Mostra a linha de cadência da apresentação no OSD."},
		{Option::DriverMode, "Driver.Mode", Type::String, false, 0, 0.0f, "inherit",
			"Como escolher o driver Vulkan: inherit, system ou custom."},
		{Option::DriverDir, "Driver.Dir", Type::String, false, 0, 0.0f, "",
			"Diretório do pacote de driver customizado."},
		{Option::DriverName, "Driver.Name", Type::String, false, 0, 0.0f, "",
			"Soname da biblioteca do driver."},
		{Option::DriverRedirectDir, "Driver.RedirectDir", Type::String, false, 0, 0.0f, "",
			"Diretório de cache do driver."},
		{Option::DriverHookLibDir, "Driver.HookLibDir", Type::String, false, 0, 0.0f, "",
			"Diretório das bibliotecas de hook do adrenotools."},
		{Option::DriverId, "Driver.Id", Type::String, false, 0, 0.0f, "",
			"Id do pacote de driver selecionado."},
	}};

	// Um desalinhamento entre o enum e a tabela faria uma opção ler o valor da vizinha — sem erro,
	// sem log, só o número errado. Barato de impedir aqui.
	static_assert(OPTIONS.size() == OPTION_COUNT, "tabela e enum Option divergem");
	constexpr bool OptionsAreInOrder()
	{
		for (size_t i = 0; i < OPTIONS.size(); i++)
		{
			if (static_cast<size_t>(OPTIONS[i].option) != i)
				return false;
		}
		return true;
	}
	static_assert(OptionsAreInOrder(), "a ordem da tabela tem que seguir a ordem do enum Option");

	/// Escalares em atômicos para que o caminho de apresentação leia sem lock; strings sob mutex,
	/// porque são lidas em decisões (qual driver, qual perfil) e não por quadro.
	struct Storage
	{
		std::array<std::atomic<s32>, OPTION_COUNT> scalars{};
		std::array<std::atomic<float>, OPTION_COUNT> floats{};
		std::mutex string_mutex;
		std::array<std::string, OPTION_COUNT> strings;
	};

	Storage& GetStorage()
	{
		static Storage storage;
		return storage;
	}

	std::mutex s_callback_mutex;
	std::vector<ChangeCallback> s_callbacks;

	void ApplyDefault(const OptionDesc& desc)
	{
		Storage& storage = GetStorage();
		const size_t index = static_cast<size_t>(desc.option);
		switch (desc.type)
		{
			case Type::Bool:
				storage.scalars[index].store(desc.default_bool ? 1 : 0, std::memory_order_relaxed);
				break;
			case Type::Int:
				storage.scalars[index].store(desc.default_int, std::memory_order_relaxed);
				break;
			case Type::Float:
				storage.floats[index].store(desc.default_float, std::memory_order_relaxed);
				break;
			case Type::String:
			{
				std::lock_guard lock(storage.string_mutex);
				storage.strings[index] = desc.default_string ? desc.default_string : "";
				break;
			}
		}
	}

	void NotifyChanged()
	{
		std::vector<ChangeCallback> callbacks;
		{
			std::lock_guard lock(s_callback_mutex);
			callbacks = s_callbacks;
		}
		for (const ChangeCallback callback : callbacks)
			callback();
	}
} // namespace

std::span<const OptionDesc> ForkConfig::GetOptions()
{
	return OPTIONS;
}

const ForkConfig::OptionDesc& ForkConfig::GetOption(Option option)
{
	return OPTIONS[static_cast<size_t>(option)];
}

const ForkConfig::OptionDesc* ForkConfig::FindOption(std::string_view key)
{
	const auto it = std::find_if(OPTIONS.begin(), OPTIONS.end(),
		[key](const OptionDesc& desc) { return key == desc.key; });
	return (it != OPTIONS.end()) ? &(*it) : nullptr;
}

bool ForkConfig::GetBool(Option option)
{
	return GetStorage().scalars[static_cast<size_t>(option)].load(std::memory_order_relaxed) != 0;
}

s32 ForkConfig::GetInt(Option option)
{
	return GetStorage().scalars[static_cast<size_t>(option)].load(std::memory_order_relaxed);
}

float ForkConfig::GetFloat(Option option)
{
	return GetStorage().floats[static_cast<size_t>(option)].load(std::memory_order_relaxed);
}

std::string ForkConfig::GetString(Option option)
{
	Storage& storage = GetStorage();
	std::lock_guard lock(storage.string_mutex);
	return storage.strings[static_cast<size_t>(option)];
}

std::string ForkConfig::GetValueAsString(Option option)
{
	const OptionDesc& desc = GetOption(option);
	switch (desc.type)
	{
		case Type::Bool:
			return GetBool(option) ? "true" : "false";
		case Type::Int:
			return fmt::format("{}", GetInt(option));
		case Type::Float:
			return fmt::format("{}", GetFloat(option));
		case Type::String:
			return GetString(option);
	}
	return {};
}

void ForkConfig::SetValueFromString(Option option, std::string_view value)
{
	const OptionDesc& desc = GetOption(option);
	Storage& storage = GetStorage();
	const size_t index = static_cast<size_t>(option);

	switch (desc.type)
	{
		case Type::Bool:
		{
			// Valor inválido cai no padrão em vez de virar `false`: um INI editado à mão com
			// "sim" não deve significar silenciosamente "desligado".
			const std::optional<bool> parsed = StringUtil::FromChars<bool>(value);
			storage.scalars[index].store(parsed.value_or(desc.default_bool) ? 1 : 0, std::memory_order_relaxed);
			break;
		}
		case Type::Int:
		{
			const std::optional<s32> parsed = StringUtil::FromChars<s32>(value);
			storage.scalars[index].store(parsed.value_or(desc.default_int), std::memory_order_relaxed);
			break;
		}
		case Type::Float:
		{
			const std::optional<float> parsed = StringUtil::FromChars<float>(value);
			storage.floats[index].store(parsed.value_or(desc.default_float), std::memory_order_relaxed);
			break;
		}
		case Type::String:
		{
			std::lock_guard lock(storage.string_mutex);
			storage.strings[index] = value;
			break;
		}
	}
}

void ForkConfig::ResetToDefaults()
{
	for (const OptionDesc& desc : OPTIONS)
		ApplyDefault(desc);
}

void ForkConfig::LoadSettings(const SettingsInterface& si)
{
	for (const OptionDesc& desc : OPTIONS)
	{
		// A ausência da chave é o caso NORMAL — um INI de usuário só tem o que ele mudou —, então
		// cada opção começa no padrão e só é sobrescrita se a camada (base ou jogo) a define.
		ApplyDefault(desc);

		std::string value;
		if (!si.GetStringValue(SECTION, desc.key, &value))
			continue;

		SetValueFromString(desc.option, value);
	}

	NotifyChanged();
}

bool ForkConfig::SetAndSave(Option option, std::string_view value)
{
	const OptionDesc& desc = GetOption(option);

	// Valida ANTES de escrever: gravar lixo no INI do usuário e só descobrir no próximo boot é
	// pior que recusar agora.
	switch (desc.type)
	{
		case Type::Bool:
			if (!StringUtil::FromChars<bool>(value).has_value())
				return false;
			break;
		case Type::Int:
			if (!StringUtil::FromChars<s32>(value).has_value())
				return false;
			break;
		case Type::Float:
			if (!StringUtil::FromChars<float>(value).has_value())
				return false;
			break;
		case Type::String:
			break;
	}

	SetValueFromString(option, value);

	// A escrita é sempre na camada BASE. Persistir por jogo é do frontend, que é quem sabe onde o
	// INI do jogo vive; a leitura em camadas acima respeita o que ele escrever.
	{
		std::unique_lock lock = Host::GetSettingsLock();
		if (SettingsInterface* base = Host::Internal::GetBaseSettingsLayer())
			base->SetStringValue(SECTION, desc.key, std::string(value).c_str());
		else
			Console.Warning("ForkConfig: sem camada base de settings; '%s' aplicado só em memória.", desc.key);
	}
	Host::CommitBaseSettingChanges();

	NotifyChanged();
	return true;
}

void ForkConfig::RegisterChangeCallback(ChangeCallback callback)
{
	if (!callback)
		return;
	std::lock_guard lock(s_callback_mutex);
	s_callbacks.push_back(callback);
}

ForkConfig::DriverSelection ForkConfig::ResolveDriverSelection(
	std::string_view mode, std::string_view dir, std::string_view name, std::string_view hook_lib_dir)
{
	if (mode == "system")
		return DriverSelection::System;

	if (mode == "custom")
	{
		// Modo custom sem os caminhos é configuração quebrada — INI editado à mão, pacote apagado
		// por fora, migração incompleta. Cair em System deixa o jogo rodando de forma previsível e
		// visível; cair em Inherit o deixaria com o driver de outro jogo, que é pior porque
		// parece que funcionou.
		if (dir.empty() || name.empty() || hook_lib_dir.empty())
			return DriverSelection::System;
		return DriverSelection::Custom;
	}

	// Qualquer outra coisa (inclusive vazio e lixo) é "não opino".
	return DriverSelection::Inherit;
}
