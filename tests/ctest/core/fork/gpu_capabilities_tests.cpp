// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkGpuCapabilities.h"

#include "GS/Renderers/Common/GSGPUProfile.h"

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

// MatchedRulesString: a Etapa 1 do desenho em docs/regras-driver-com-validade.md. A contagem
// sozinha nunca respondeu "qual regra"; estes testes prendem o formato que passa a responder.
TEST(MobileDriverProfileRules, NothingMatchedReadsAsADash)
{
	MobileDriverProfile p;
	EXPECT_EQ(p.MatchedRulesString(), "-");
}

TEST(MobileDriverProfileRules, OneRuleIsNamed)
{
	MobileDriverProfile p;
	p.matched_rule_count = 1;
	p.matched_rule_ids[0] = "vk-turnip-attachment-self-read";
	EXPECT_EQ(p.MatchedRulesString(), "vk-turnip-attachment-self-read");
}

TEST(MobileDriverProfileRules, SeveralRulesKeepTableOrder)
{
	MobileDriverProfile p;
	p.matched_rule_count = 3;
	p.matched_rule_ids[0] = "a";
	p.matched_rule_ids[1] = "b";
	p.matched_rule_ids[2] = "c";
	EXPECT_EQ(p.MatchedRulesString(), "a,b,c");
}

TEST(MobileDriverProfileRules, OverflowKeepsTheCountExactAndSaysItTruncated)
{
	// O caso que importa nao e a lista, e a HONESTIDADE dela: se um dia a tabela casar mais
	// regras do que o vetor guarda, a contagem tem de continuar certa e o leitor tem de saber
	// que esta vendo uma lista parcial, em vez de concluir que o resto nao existe.
	MobileDriverProfile p;
	p.matched_rule_count = MobileDriverProfile::MAX_TRACKED_RULES + 3;
	for (u32 i = 0; i < MobileDriverProfile::MAX_TRACKED_RULES; i++)
		p.matched_rule_ids[i] = "r";

	const std::string out = p.MatchedRulesString();
	EXPECT_NE(out.find("+3"), std::string::npos) << out;
	EXPECT_EQ(p.TrackedRuleCount(), MobileDriverProfile::MAX_TRACKED_RULES);
	EXPECT_EQ(p.matched_rule_count, MobileDriverProfile::MAX_TRACKED_RULES + 3);
}

TEST(MobileDriverProfileRules, ExactlyAtTheLimitHasNoTruncationSuffix)
{
	// A fronteira entre "lista completa" e "lista parcial". Um erro de um no `>` faria a lista
	// cheia anunciar "+0", e um leitor que confia no sufixo concluiria que ha regra escondida
	// que nao existe.
	MobileDriverProfile p;
	p.matched_rule_count = MobileDriverProfile::MAX_TRACKED_RULES;
	for (u32 i = 0; i < MobileDriverProfile::MAX_TRACKED_RULES; i++)
		p.matched_rule_ids[i] = "r";

	const std::string out = p.MatchedRulesString();
	EXPECT_EQ(out.find('+'), std::string::npos) << out;
	EXPECT_EQ(p.TrackedRuleCount(), p.matched_rule_count);
}

TEST(MobileDriverProfileRules, NullIdIsSkippedWithoutLeavingASeparator)
{
	// O vetor e zero-inicializado, entao um id nulo no meio e o estado natural de um registro
	// parcial. Pular sem cuidado produziria "a,,c" ou uma virgula sobrando na ponta, e uma lista
	// com buraco de pontuacao e lida como lista corrompida.
	MobileDriverProfile p;
	p.matched_rule_count = 3;
	p.matched_rule_ids[0] = "a";
	p.matched_rule_ids[1] = nullptr;
	p.matched_rule_ids[2] = "c";
	EXPECT_EQ(p.MatchedRulesString(), "a,c");
}

TEST(MobileDriverProfileRules, AllIdsNullReadsAsADashRatherThanEmpty)
{
	// Contagem maior que zero e nenhum id utilizavel. Uma string vazia faria a linha de log virar
	// `rules=` sem valor, que se le como campo faltando; o traco e o mesmo vocabulario que a
	// contagem zero ja usa para ausencia.
	MobileDriverProfile p;
	p.matched_rule_count = 2;
	EXPECT_EQ(p.MatchedRulesString(), "-");
}
