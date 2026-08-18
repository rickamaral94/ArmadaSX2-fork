// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkConfig.h"

#include "common/MemorySettingsInterface.h"

#include <gtest/gtest.h>

using ForkConfig::Option;
using ForkConfig::Type;

namespace
{
	class ForkConfigTest : public ::testing::Test
	{
	protected:
		void SetUp() override { ForkConfig::ResetToDefaults(); }
		void TearDown() override { ForkConfig::ResetToDefaults(); }
	};

	int s_callback_hits = 0;
	void CountCallback()
	{
		s_callback_hits++;
	}
} // namespace

// A tabela e o enum precisam andar juntos. O static_assert em ForkConfig.cpp já garante isso em
// compilação; aqui a verificação é de que a tabela exposta à UI descreve as MESMAS opções — uma UI
// construída a partir de uma tabela desalinhada editaria a opção errada.
TEST_F(ForkConfigTest, TableMatchesEnum)
{
	const std::span<const ForkConfig::OptionDesc> options = ForkConfig::GetOptions();
	ASSERT_EQ(options.size(), static_cast<size_t>(Option::Count));

	for (size_t i = 0; i < options.size(); i++)
	{
		const Option option = static_cast<Option>(i);
		EXPECT_EQ(options[i].option, option);
		EXPECT_EQ(&ForkConfig::GetOption(option), &options[i]);
		ASSERT_NE(options[i].key, nullptr);
		EXPECT_NE(options[i].description, nullptr);
		// Toda opção tem que ser encontrável pela chave: é assim que a JNI genérica e o INI a
		// acham, e uma chave que não resolve é uma opção que a UI não consegue tocar.
		EXPECT_EQ(ForkConfig::FindOption(options[i].key), &options[i]);
	}
}

TEST_F(ForkConfigTest, UnknownKeyResolvesToNothing)
{
	// Acontece de verdade: INI escrito por um binário mais novo, aberto por um mais velho.
	// Ignorar é a resposta certa; travar ou adivinhar não é.
	EXPECT_EQ(ForkConfig::FindOption("NaoExiste.Coisa"), nullptr);
	EXPECT_EQ(ForkConfig::FindOption(""), nullptr);
}

TEST_F(ForkConfigTest, DefaultsApplyWhenSettingsAreEmpty)
{
	MemorySettingsInterface si;
	ForkConfig::LoadSettings(si);

	for (const ForkConfig::OptionDesc& desc : ForkConfig::GetOptions())
	{
		if (desc.type == Type::Bool)
			EXPECT_EQ(ForkConfig::GetBool(desc.option), desc.default_bool) << desc.key;
	}
}

TEST_F(ForkConfigTest, ReadsValuesFromTheForkSection)
{
	MemorySettingsInterface si;
	si.SetStringValue(ForkConfig::SECTION, "PresentationMetrics.Enabled", "true");
	si.SetStringValue(ForkConfig::SECTION, "PresentationMetrics.Overlay", "true");

	ForkConfig::LoadSettings(si);

	EXPECT_TRUE(ForkConfig::GetBool(Option::PresentationMetricsEnabled));
	EXPECT_TRUE(ForkConfig::GetBool(Option::PresentationMetricsOverlay));
}

// Cada carregamento começa do padrão. Sem isso, uma opção ligada em um jogo continuaria ligada no
// próximo — que é exatamente o bug que um override por jogo mal feito produz.
TEST_F(ForkConfigTest, EachLoadStartsFromDefaults)
{
	MemorySettingsInterface with_value;
	with_value.SetStringValue(ForkConfig::SECTION, "PresentationMetrics.Enabled", "true");
	ForkConfig::LoadSettings(with_value);
	ASSERT_TRUE(ForkConfig::GetBool(Option::PresentationMetricsEnabled));

	MemorySettingsInterface empty;
	ForkConfig::LoadSettings(empty);
	EXPECT_FALSE(ForkConfig::GetBool(Option::PresentationMetricsEnabled));
}

// Valor que não faz sentido para o tipo cai no PADRÃO, não em "false". Um INI editado à mão com
// "sim" não deve significar silenciosamente "desligado".
TEST_F(ForkConfigTest, InvalidValueFallsBackToDefaultNotZero)
{
	MemorySettingsInterface si;
	si.SetStringValue(ForkConfig::SECTION, "PresentationMetrics.Enabled", "talvez");
	ForkConfig::LoadSettings(si);

	const ForkConfig::OptionDesc& desc = ForkConfig::GetOption(Option::PresentationMetricsEnabled);
	EXPECT_EQ(ForkConfig::GetBool(Option::PresentationMetricsEnabled), desc.default_bool);
}

TEST_F(ForkConfigTest, ValueAsStringRoundTrips)
{
	MemorySettingsInterface si;
	si.SetStringValue(ForkConfig::SECTION, "PresentationMetrics.Enabled", "true");
	ForkConfig::LoadSettings(si);

	EXPECT_EQ(ForkConfig::GetValueAsString(Option::PresentationMetricsEnabled), "true");
	EXPECT_EQ(ForkConfig::GetValueAsString(Option::PresentationMetricsOverlay), "false");
}

// O observador é o que faz uma opção valer na hora — inclusive vinda da UI Android por JNI — sem
// reiniciar a máquina virtual.
TEST_F(ForkConfigTest, ChangeCallbackFiresOnLoad)
{
	ForkConfig::RegisterChangeCallback(&CountCallback);
	const int before = s_callback_hits;

	MemorySettingsInterface si;
	ForkConfig::LoadSettings(si);

	EXPECT_GT(s_callback_hits, before);
}

TEST_F(ForkConfigTest, SetAndSaveRejectsInvalidValues)
{
	EXPECT_FALSE(ForkConfig::SetAndSave(Option::PresentationMetricsEnabled, "talvez"));
	// Recusado significa recusado: o valor em memória não pode ter sido mexido no caminho.
	EXPECT_FALSE(ForkConfig::GetBool(Option::PresentationMetricsEnabled));

	EXPECT_TRUE(ForkConfig::SetAndSave(Option::PresentationMetricsEnabled, "true"));
	EXPECT_TRUE(ForkConfig::GetBool(Option::PresentationMetricsEnabled));
}
