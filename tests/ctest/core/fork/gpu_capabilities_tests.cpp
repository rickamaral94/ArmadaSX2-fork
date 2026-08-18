// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkGpuCapabilities.h"

#include <gtest/gtest.h>

using ForkGpuCapabilities::EvaluateTurnipSupport;
using ForkGpuCapabilities::TurnipSupport;

namespace
{
	constexpr u32 ANDROID_10 = 29;
	constexpr u32 ANDROID_9 = 28;
	constexpr u32 ANDROID_15 = 35;

	TurnipSupport OnAndroid(RuntimeGpuProfile vendor, MobileGpuArchitecture arch, u32 sdk = ANDROID_15)
	{
		return EvaluateTurnipSupport(vendor, arch, sdk, true);
	}
} // namespace

TEST(ForkGpuCapabilities, AdrenoGenerationsWithTurnipBuildsAreSupported)
{
	EXPECT_EQ(OnAndroid(RuntimeGpuProfile::Adreno, MobileGpuArchitecture::Adreno6xx), TurnipSupport::Supported);
	EXPECT_EQ(OnAndroid(RuntimeGpuProfile::Adreno, MobileGpuArchitecture::Adreno7xx), TurnipSupport::Supported);
	EXPECT_EQ(OnAndroid(RuntimeGpuProfile::Adreno, MobileGpuArchitecture::Adreno8xx), TurnipSupport::Supported);
}

// A regra que não se negocia. Turnip é freedreno; tentar carregá-lo em uma Mali ou PowerVR não é
// "arriscado", é carregar o driver de outra arquitetura — e é exatamente o que o projeto proíbe.
// Testado com Android novo de propósito: nenhuma versão de sistema torna isto aceitável.
TEST(ForkGpuCapabilities, NonAdrenoIsAlwaysRefused)
{
	const MobileGpuArchitecture architectures[] = {
		MobileGpuArchitecture::MaliValhall3,
		MobileGpuArchitecture::MaliG1,
		MobileGpuArchitecture::PowerVR,
		MobileGpuArchitecture::Unknown,
	};
	const RuntimeGpuProfile vendors[] = {
		RuntimeGpuProfile::Mali,
		RuntimeGpuProfile::PowerVR,
		RuntimeGpuProfile::Xclipse,
		RuntimeGpuProfile::Apple,
		RuntimeGpuProfile::Unknown,
	};

	for (const RuntimeGpuProfile vendor : vendors)
	{
		for (const MobileGpuArchitecture arch : architectures)
			EXPECT_EQ(OnAndroid(vendor, arch), TurnipSupport::UnsupportedVendor);
	}
}

// Xclipse tem pacotes em formato AdrenoTools, mas não são Turnip. O veredito é sobre Turnip, e
// confundir os dois levaria a oferecer freedreno para uma GPU RDNA.
TEST(ForkGpuCapabilities, XclipseIsNotTurnipTerritory)
{
	EXPECT_EQ(OnAndroid(RuntimeGpuProfile::Xclipse, MobileGpuArchitecture::Unknown), TurnipSupport::UnsupportedVendor);
}

TEST(ForkGpuCapabilities, OlderAdrenoGenerationsAreRefusedForGeneration)
{
	const MobileGpuArchitecture old_ones[] = {
		MobileGpuArchitecture::Adreno2xx,
		MobileGpuArchitecture::Adreno3xx,
		MobileGpuArchitecture::Adreno4xx,
		MobileGpuArchitecture::Adreno5xx,
	};
	for (const MobileGpuArchitecture arch : old_ones)
		EXPECT_EQ(OnAndroid(RuntimeGpuProfile::Adreno, arch), TurnipSupport::UnsupportedAdrenoGeneration);
}

// O motivo tem que ser o do fabricante, não o da versão do Android: dizer "atualize o sistema"
// para o dono de uma Mali é mandá-lo perseguir algo que nunca vai funcionar.
TEST(ForkGpuCapabilities, VendorIsCheckedBeforeAndroidVersion)
{
	EXPECT_EQ(EvaluateTurnipSupport(RuntimeGpuProfile::Mali, MobileGpuArchitecture::MaliValhall3, ANDROID_9, true),
		TurnipSupport::UnsupportedVendor);
}

TEST(ForkGpuCapabilities, AndroidTooOldForTheLoader)
{
	EXPECT_EQ(EvaluateTurnipSupport(RuntimeGpuProfile::Adreno, MobileGpuArchitecture::Adreno7xx, ANDROID_9, true),
		TurnipSupport::UnsupportedAndroidVersion);
	EXPECT_EQ(EvaluateTurnipSupport(RuntimeGpuProfile::Adreno, MobileGpuArchitecture::Adreno7xx, ANDROID_10, true),
		TurnipSupport::Supported);
}

// SDK 0 é "não consegui ler a propriedade", não "versão 0". Recusar aí esconderia a
// funcionalidade de aparelhos capazes por causa de uma leitura falha — e o carregador ainda tem o
// próprio fallback para o driver do sistema.
TEST(ForkGpuCapabilities, UnknownAndroidVersionDoesNotHideTheFeature)
{
	EXPECT_EQ(EvaluateTurnipSupport(RuntimeGpuProfile::Adreno, MobileGpuArchitecture::Adreno7xx, 0, true),
		TurnipSupport::Supported);
}

TEST(ForkGpuCapabilities, NonAndroidPlatformsHaveNoLoader)
{
	EXPECT_EQ(EvaluateTurnipSupport(RuntimeGpuProfile::Adreno, MobileGpuArchitecture::Adreno7xx, ANDROID_15, false),
		TurnipSupport::NotAndroid);
}

// Todo veredito precisa de um motivo apresentável: uma seção que some sem explicação vira relato
// de bug.
TEST(ForkGpuCapabilities, EverySupportStateHasAReason)
{
	const TurnipSupport states[] = {
		TurnipSupport::Unknown,
		TurnipSupport::Supported,
		TurnipSupport::NotAndroid,
		TurnipSupport::UnsupportedVendor,
		TurnipSupport::UnsupportedAdrenoGeneration,
		TurnipSupport::UnsupportedAndroidVersion,
	};
	for (const TurnipSupport state : states)
	{
		EXPECT_STRNE(ForkGpuCapabilities::TurnipSupportToString(state), "");
		EXPECT_STRNE(ForkGpuCapabilities::TurnipSupportReason(state), "");
	}
}

TEST(ForkGpuCapabilities, VulkanVersionFormatting)
{
	// VK_MAKE_API_VERSION(0, 1, 3, 281)
	EXPECT_EQ(ForkGpuCapabilities::FormatVulkanVersion((1u << 22) | (3u << 12) | 281u), "1.3.281");
	EXPECT_EQ(ForkGpuCapabilities::FormatVulkanVersion((1u << 22) | (1u << 12) | 0u), "1.1.0");
	EXPECT_EQ(ForkGpuCapabilities::FormatVulkanVersion(0), "desconhecida");
}

// Antes de qualquer renderer subir, a resposta é "não sei" — e não "não suportado". A UI precisa
// dessa diferença para não afirmar ao usuário algo que ainda não olhou.
TEST(ForkGpuCapabilities, UnprobedIsNotTheSameAsUnsupported)
{
	const ForkGpuCapabilities::Capabilities caps = ForkGpuCapabilities::Get();
	if (!caps.probed)
	{
		EXPECT_EQ(caps.turnip, TurnipSupport::Unknown);
		EXPECT_FALSE(ForkGpuCapabilities::IsTurnipCapable());
	}
}
