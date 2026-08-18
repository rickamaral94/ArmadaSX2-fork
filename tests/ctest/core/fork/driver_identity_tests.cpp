// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkDriverIdentity.h"

#include <gtest/gtest.h>

using ForkDriverIdentity::LoadOutcome;
using ForkDriverIdentity::MesaVersion;

namespace
{
	constexpr bool REQUESTED = true;
	constexpr bool NOT_REQUESTED = false;
	constexpr bool OPENED = true;
	constexpr bool NOT_OPENED = false;
	constexpr bool PROPS = true;
	constexpr bool NO_PROPS = false;
} // namespace

// O veredito que motivou o módulo. O fallback do VKLoader é silencioso por desenho: o usuário
// selecionou Turnip, o handle não abriu, o driver do sistema assumiu e o jogo rodou. Sem isto,
// um A/B "System vs Turnip" pode comparar o driver do sistema com ele mesmo.
TEST(ForkDriverIdentity, SilentFallbackIsDetected)
{
	EXPECT_EQ(ForkDriverIdentity::EvaluateOutcome(REQUESTED, NOT_OPENED, MobileGpuDriver::QualcommProprietary, PROPS),
		LoadOutcome::FellBackToSystem);
	EXPECT_TRUE(ForkDriverIdentity::IsUnexpected(LoadOutcome::FellBackToSystem));
}

// Abriu, mas quem respondeu não é o Turnip: pacote repackado, renomeado, ou simplesmente outra
// coisa. Também é divergência, e por um motivo diferente do anterior.
TEST(ForkDriverIdentity, OpenedButNotTurnipIsAlsoUnexpected)
{
	EXPECT_EQ(ForkDriverIdentity::EvaluateOutcome(REQUESTED, OPENED, MobileGpuDriver::QualcommProprietary, PROPS),
		LoadOutcome::CustomOpenedButNotTurnip);
	EXPECT_TRUE(ForkDriverIdentity::IsUnexpected(LoadOutcome::CustomOpenedButNotTurnip));
}

TEST(ForkDriverIdentity, TurnipLoadedIsTheHappyPath)
{
	EXPECT_EQ(ForkDriverIdentity::EvaluateOutcome(REQUESTED, OPENED, MobileGpuDriver::MesaTurnip, PROPS),
		LoadOutcome::CustomDriverActive);
	EXPECT_FALSE(ForkDriverIdentity::IsUnexpected(LoadOutcome::CustomDriverActive));
}

TEST(ForkDriverIdentity, NoSelectionMeansSystemDriverIsExpected)
{
	EXPECT_EQ(ForkDriverIdentity::EvaluateOutcome(NOT_REQUESTED, NOT_OPENED, MobileGpuDriver::QualcommProprietary, PROPS),
		LoadOutcome::SystemDriverByChoice);
	EXPECT_FALSE(ForkDriverIdentity::IsUnexpected(LoadOutcome::SystemDriverByChoice));
}

// Sem VK_KHR_driver_properties não há como saber QUEM abriu. Acusar divergência aqui seria gritar
// lobo em aparelhos cujo driver não reporta identidade — e um aviso falso ensina o usuário a
// ignorar os verdadeiros.
TEST(ForkDriverIdentity, WithoutDriverPropertiesWeDoNotCryWolf)
{
	EXPECT_EQ(ForkDriverIdentity::EvaluateOutcome(REQUESTED, OPENED, MobileGpuDriver::Unknown, NO_PROPS),
		LoadOutcome::CustomDriverActive);
	EXPECT_EQ(ForkDriverIdentity::EvaluateOutcome(REQUESTED, OPENED, MobileGpuDriver::QualcommProprietary, NO_PROPS),
		LoadOutcome::CustomDriverActive);

	// Mas a falha de ABRIR não depende de driverProperties: ela é fato do carregador.
	EXPECT_EQ(ForkDriverIdentity::EvaluateOutcome(REQUESTED, NOT_OPENED, MobileGpuDriver::Unknown, NO_PROPS),
		LoadOutcome::FellBackToSystem);
}

TEST(ForkDriverIdentity, ParsesTheFreedrenoVersionString)
{
	const MesaVersion v = ForkDriverIdentity::ParseMesaVersion("Mesa 25.2.0-devel (git-1a2b3c4)");
	EXPECT_TRUE(v.known);
	EXPECT_EQ(v.major, 25u);
	EXPECT_EQ(v.minor, 2u);
	EXPECT_EQ(v.patch, 0u);
	// O texto original é preservado: é o "-devel (git-...)" que distingue duas builds do mesmo
	// release, que é exatamente o que um relatório de compatibilidade precisa.
	EXPECT_EQ(v.raw, "Mesa 25.2.0-devel (git-1a2b3c4)");
}

TEST(ForkDriverIdentity, ParsesVariantsAndPrefixes)
{
	EXPECT_TRUE(ForkDriverIdentity::ParseMesaVersion("Mesa 24.1.3").known);
	EXPECT_EQ(ForkDriverIdentity::ParseMesaVersion("Mesa 24.1.3").minor, 1u);

	// Sem patch.
	const MesaVersion two = ForkDriverIdentity::ParseMesaVersion("Mesa 26.0");
	EXPECT_TRUE(two.known);
	EXPECT_EQ(two.major, 26u);
	EXPECT_EQ(two.minor, 0u);

	// Empacotadores às vezes prefixam o texto; por isso procuramos a etiqueta em vez de exigir
	// que ela abra a string.
	const MesaVersion prefixed = ForkDriverIdentity::ParseMesaVersion("turnip build / Mesa 25.3.1");
	EXPECT_TRUE(prefixed.known);
	EXPECT_EQ(prefixed.major, 25u);
	EXPECT_EQ(prefixed.patch, 1u);

	// Caixa não importa.
	EXPECT_TRUE(ForkDriverIdentity::ParseMesaVersion("MESA 25.0.0").known);
}

TEST(ForkDriverIdentity, NonMesaDriverInfoYieldsNoVersion)
{
	// O blob da Qualcomm não publica versão de Mesa.
	EXPECT_FALSE(ForkDriverIdentity::ParseMesaVersion("Qualcomm driver 512.780.0").known);
	EXPECT_FALSE(ForkDriverIdentity::ParseMesaVersion("").known);
	// "Mesa" solto, sem número, não vira versão inventada.
	EXPECT_FALSE(ForkDriverIdentity::ParseMesaVersion("Mesa").known);
	EXPECT_FALSE(ForkDriverIdentity::ParseMesaVersion("Mesa sem numero").known);
}

TEST(ForkDriverIdentity, EveryOutcomeHasANameAndAReason)
{
	const LoadOutcome all[] = {
		LoadOutcome::Unknown,
		LoadOutcome::SystemDriverByChoice,
		LoadOutcome::CustomDriverActive,
		LoadOutcome::FellBackToSystem,
		LoadOutcome::CustomOpenedButNotTurnip,
	};
	for (const LoadOutcome outcome : all)
	{
		EXPECT_STRNE(ForkDriverIdentity::OutcomeToString(outcome), "");
		EXPECT_STRNE(ForkDriverIdentity::OutcomeReason(outcome), "");
	}
}

TEST(ForkDriverIdentity, PublishRecordsTheVerdictAndTheEvidence)
{
	ForkDriverIdentity::NoteSelectedPackageSha256("deadbeef");

	ForkDriverIdentity::PublishInput input;
	input.requested = true;
	input.opened = true;
	input.requested_driver = "libvulkan_freedreno.so";
	input.active_driver = MobileGpuDriver::MesaTurnip;
	input.driver_name = "turnip";
	input.driver_info = "Mesa 25.2.0-devel (git-1a2b3c4)";
	input.gpu_name = "Adreno (TM) 750";
	input.vulkan_api_version = (1u << 22) | (3u << 12) | 281u;
	input.driver_properties_available = true;
	ForkDriverIdentity::Publish(input);

	const ForkDriverIdentity::Identity identity = ForkDriverIdentity::Get();
	EXPECT_TRUE(identity.probed);
	EXPECT_EQ(identity.outcome, LoadOutcome::CustomDriverActive);
	EXPECT_TRUE(identity.mesa.known);
	EXPECT_EQ(identity.mesa.major, 25u);
	// O SHA-256 informado antes do renderer subir tem que sobreviver à publicação.
	EXPECT_EQ(identity.package_sha256, "deadbeef");

	const std::string line = ForkDriverIdentity::DescribeForLog();
	EXPECT_NE(line.find("Mesa 25.2.0"), std::string::npos);
	EXPECT_NE(line.find("1.3.281"), std::string::npos);
	EXPECT_NE(line.find("deadbeef"), std::string::npos);
}
