// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkDiagnostics.h"

#include "Fork/ForkGpuCapabilities.h"

#include <gtest/gtest.h>

#include <string>

using ForkDiagnostics::Accumulator;
using ForkFrameGen::Reason;
using ForkFrameGen::State;

namespace
{
	bool Contains(const std::string& haystack, std::string_view needle)
	{
		return haystack.find(needle) != std::string::npos;
	}

	ForkFrameGen::Decision Make(State state, Reason reason)
	{
		ForkFrameGen::Decision decision;
		decision.state = state;
		decision.reason = reason;
		decision.frames_to_generate = (state == State::Engaged) ? 1 : 0;
		return decision;
	}
} // namespace

// A regra do projeto vale ainda mais no log que na tela: ele é lido fora de contexto, meses
// depois, por alguém que não estava nesta conversa. Duas colunas vizinhas viram um número só na
// memória de quem lê.
TEST(ForkDiagnostics, RealAndPresentedNeverShareALine)
{
	GSPresentationMetrics::Snapshot snapshot{};
	snapshot.real_fps = 30.0f;
	snapshot.presented_fps = 60.0f;

	const std::string real = ForkDiagnostics::FormatRealLine(snapshot);
	const std::string presented = ForkDiagnostics::FormatPresentedLine(snapshot);

	EXPECT_TRUE(Contains(real, "30.00"));
	EXPECT_FALSE(Contains(real, "60.00")) << real;
	EXPECT_TRUE(Contains(presented, "60.00"));
	EXPECT_FALSE(Contains(presented, "30.00")) << presented;
}

// O prefixo é a interface com o script de captura: no Android o Console sai por stdout e vira
// tag `STDOUT` no logcat, então filtrar por tag não acha nada — o que acha é o texto.
TEST(ForkDiagnostics, EveryLineCarriesTheGreppablePrefix)
{
	GSPresentationMetrics::Snapshot snapshot{};
	ForkFrameGen::Policy policy;
	Accumulator accumulator;

	EXPECT_TRUE(Contains(ForkDiagnostics::FormatRealLine(snapshot), "@@FORK@@"));
	EXPECT_TRUE(Contains(ForkDiagnostics::FormatPresentedLine(snapshot), "@@FORK@@"));
	EXPECT_TRUE(Contains(ForkDiagnostics::FormatFrameGenLine(accumulator, policy, 0.0f, 0.0f, 16.6f), "@@FORK@@"));
}

// A pergunta que o log tem de responder: "o Auto engata firme ou fica piscando?". Contagem de
// transições responde; despejar cada mudança afogaria o log justamente no caso patológico.
TEST(ForkDiagnostics, TransitionsCountStateChangesNotFrames)
{
	Accumulator accumulator;
	for (int i = 0; i < 100; i++)
		accumulator.Note(Make(State::Engaged, Reason::Engaged), 2.0f);

	EXPECT_EQ(accumulator.frames, 100u);
	EXPECT_EQ(accumulator.transitions, 0u) << "estado constante não é transição";

	accumulator.Note(Make(State::Suspended, Reason::OverBudget), 9.0f);
	EXPECT_EQ(accumulator.transitions, 1u);
	accumulator.Note(Make(State::Engaged, Reason::Engaged), 2.0f);
	EXPECT_EQ(accumulator.transitions, 2u);
}

// Piscar é exatamente isto, e o número tem de mostrar sem ambiguidade.
TEST(ForkDiagnostics, FlickerShowsUpAsAHighTransitionCount)
{
	Accumulator accumulator;
	for (int i = 0; i < 20; i++)
	{
		accumulator.Note(Make(State::Engaged, Reason::Engaged), 1.0f);
		accumulator.Note(Make(State::Waiting, Reason::Unstable), 1.0f);
	}
	EXPECT_EQ(accumulator.transitions, 39u);
}

// A média esconde o pico, e é o pico que estoura o orçamento e suspende a geração.
TEST(ForkDiagnostics, WorstGenerationCostSurvivesTheAverage)
{
	Accumulator accumulator;
	accumulator.Note(Make(State::Engaged, Reason::Engaged), 1.0f);
	accumulator.Note(Make(State::Engaged, Reason::Engaged), 11.5f);
	accumulator.Note(Make(State::Engaged, Reason::Engaged), 1.2f);
	EXPECT_FLOAT_EQ(accumulator.worst_generation_ms, 11.5f);

	ForkFrameGen::Policy policy;
	policy.budget_ms = 6.0f;
	const std::string line = ForkDiagnostics::FormatFrameGenLine(accumulator, policy, 1.4f, 1.2f, 16.6f);
	EXPECT_TRUE(Contains(line, "gen_worst=11.50ms")) << line;
	EXPECT_TRUE(Contains(line, "budget=6.00ms")) << line;
}

// O motivo dominante é o que cabe numa frase; sem ele a linha diria só "não engatou".
TEST(ForkDiagnostics, DominantReasonIsTheMostFrequentOne)
{
	Accumulator accumulator;
	for (int i = 0; i < 3; i++)
		accumulator.Note(Make(State::Engaged, Reason::Engaged), 1.0f);
	for (int i = 0; i < 10; i++)
		accumulator.Note(Make(State::Waiting, Reason::BelowMinimumRealFps), 0.0f);

	EXPECT_EQ(accumulator.DominantReason(), Reason::BelowMinimumRealFps);

	ForkFrameGen::Policy policy;
	const std::string line = ForkDiagnostics::FormatFrameGenLine(accumulator, policy, 0.0f, 0.0f, 16.6f);
	EXPECT_TRUE(Contains(line, "engaged=23.1%")) << line;
}

// Reset zera o intervalo mas NÃO o último estado: a transição que cai exatamente na virada de um
// bloco para o outro é uma transição de verdade, e esquecê-la a perderia.
TEST(ForkDiagnostics, ResetKeepsTheLastStateSoBoundaryTransitionsSurvive)
{
	Accumulator accumulator;
	accumulator.Note(Make(State::Engaged, Reason::Engaged), 1.0f);
	accumulator.Reset();
	EXPECT_EQ(accumulator.frames, 0u);
	EXPECT_EQ(accumulator.transitions, 0u);

	accumulator.Note(Make(State::Waiting, Reason::Unstable), 0.0f);
	EXPECT_EQ(accumulator.transitions, 1u) << "a mudança atravessou a fronteira do bloco";
}

// A pergunta que o bloco anterior não respondia: CPU ou GPU? Sem ela, "está lento" vira palpite.
TEST(ForkDiagnostics, LoadLineNamesWhereTheTimeGoes)
{
	ForkDiagnostics::Load load;
	load.speed_percent = 87.5f;
	load.vps = 52.4f;
	load.cpu_thread_usage = 96.0;
	load.gs_thread_usage = 41.0f;
	load.gpu_usage = 38.0f;
	load.shader_compiles = 7;

	const std::string line = ForkDiagnostics::FormatLoadLine(load);
	EXPECT_TRUE(Contains(line, "@@FORK@@"));
	EXPECT_TRUE(Contains(line, "speed=87.5%"));
	EXPECT_TRUE(Contains(line, "cpu=96%"));
	EXPECT_TRUE(Contains(line, "gs=41%"));
	EXPECT_TRUE(Contains(line, "gpu=38%"));
	EXPECT_TRUE(Contains(line, "shader_compiles=7"));
}

// Um "0.00" indistinguível de "não sei" é pior que a ausência do campo: o leitor conclui que o
// jogo renderiza a zero.
TEST(ForkDiagnostics, InternalFpsIsOmittedWhenTheMethodCannotMeasureIt)
{
	ForkDiagnostics::Load load;
	load.internal_fps = 0.0f;
	load.internal_fps_valid = false;
	EXPECT_FALSE(Contains(ForkDiagnostics::FormatLoadLine(load), "internal_fps"));

	load.internal_fps = 29.97f;
	load.internal_fps_valid = true;
	EXPECT_TRUE(Contains(ForkDiagnostics::FormatLoadLine(load), "internal_fps=29.97"));
}

// A linha de carga fala de CUSTO. Repetir aqui o número que o usuário vê na tela seria começar a
// confundir os dois de novo, que é o erro que este projeto inteiro evita.
TEST(ForkDiagnostics, LoadLineDoesNotRepeatThePresentedNumber)
{
	ForkDiagnostics::Load load;
	load.speed_percent = 100.0f;
	load.vps = 59.94f;
	const std::string line = ForkDiagnostics::FormatLoadLine(load);
	EXPECT_FALSE(Contains(line, "presented"));
	EXPECT_FALSE(Contains(line, "generated"));
}

// Custou uma rodada inteira de teste: a primeira sessão medida no aparelho estava despejando
// texturas em disco — 246 arquivos durante a partida — e isso só apareceu porque alguém foi
// procurar. O log tem de gritar sozinho, senão a próxima medição contaminada passa igual.
TEST(ForkDiagnostics, ContaminatedMeasurementsAnnounceThemselves)
{
	ForkDiagnostics::Hygiene clean;
	clean.upscale_multiplier = 2.75f;
	clean.blending_level = 1;
	const std::string ok = ForkDiagnostics::FormatHygieneLine(clean);
	EXPECT_TRUE(Contains(ok, "limpo"));
	EXPECT_FALSE(Contains(ok, "CONTAMINADA"));
	// O contexto sai nos dois casos: um número sem saber o upscale é ininterpretável.
	EXPECT_TRUE(Contains(ok, "upscale=2.75x"));
	EXPECT_TRUE(Contains(ok, "atr=off"));

	clean.atr_enabled = true;
	const std::string atr = ForkDiagnostics::FormatHygieneLine(clean);
	EXPECT_TRUE(Contains(atr, "limpo"));
	EXPECT_TRUE(Contains(atr, "atr=on"));
	EXPECT_FALSE(Contains(atr, "CONTAMINADA"));

	ForkDiagnostics::Hygiene dirty = clean;
	dirty.dumping_textures = true;
	const std::string bad = ForkDiagnostics::FormatHygieneLine(dirty);
	EXPECT_TRUE(Contains(bad, "CONTAMINADA"));
	EXPECT_TRUE(Contains(bad, "despejo-de-texturas"));
	EXPECT_TRUE(Contains(bad, "upscale=2.75x"));
}

// Vários problemas ao mesmo tempo saem todos, não só o primeiro: quem for limpar o ambiente
// precisa da lista inteira de uma vez.
TEST(ForkDiagnostics, EveryContaminantIsListed)
{
	ForkDiagnostics::Hygiene dirty;
	dirty.dumping_textures = true;
	dirty.loading_texture_pack = true;
	dirty.ee_cycle_rate_changed = true;
	dirty.ee_cycle_skip_changed = true;

	const std::string line = ForkDiagnostics::FormatHygieneLine(dirty);
	EXPECT_TRUE(Contains(line, "despejo-de-texturas"));
	EXPECT_TRUE(Contains(line, "pacote-de-texturas"));
	EXPECT_TRUE(Contains(line, "EECycleRate"));
	EXPECT_TRUE(Contains(line, "EECycleSkip"));
}

// ---------------------------------------------------------------------------------------------
// Etapa 2: o bloco de identidade tem que carregar o CUSTO do driver, não só o nome dele.
//
// A pergunta "o Turnip está ajudando?" ficou sem resposta por onze alphas, e não por falta de
// dado: o emulador já resolvia defeitos e desvios por driver e já os imprimia — numa linha do
// upstream, longe do bloco que existe para descrever exatamente isso. Responder exigiu decodificar
// dois bitmasks à mão a partir de sessões diferentes.
//
// Medido no Odin 2, mesma GPU: o blob da Qualcomm expõe Vulkan 1.3.128 com 9 defeitos e 4 desvios;
// o Turnip, 1.4.359 com 2 e 1. Com estes campos na linha, um A/B de driver vira a diferença entre
// duas linhas de log.
// ---------------------------------------------------------------------------------------------

TEST(ForkDiagnostics, TheIdentityLineCarriesTheDriverCost)
{
    GpuProfileSelection profile;
    profile.runtime_profile = RuntimeGpuProfile::Adreno;
    profile.gpu.architecture = MobileGpuArchitecture::Adreno7xx;
    profile.gpu.name = "Turnip Adreno (TM) 740";
    profile.driver.driver = MobileGpuDriver::MesaTurnip;
    // Os números reais do aparelho: os dois defeitos de auto-leitura de anexo e o desvio caro.
    profile.driver.bugs = 0x300;
    profile.driver.workarounds = 0x40;
    profile.driver.matched_rule_count = 1;

    // Vulkan 1.4.359, empacotado como a spec manda (major<<22 | minor<<12 | patch).
    ForkGpuCapabilities::Publish(profile, (1u << 22) | (4u << 12) | 359u);

    const std::string line = ForkDiagnostics::FormatIdentityLine();
    EXPECT_TRUE(Contains(line, "vk=1.4.359")) << line;
    EXPECT_TRUE(Contains(line, "bugs=0x300")) << line;
    EXPECT_TRUE(Contains(line, "workarounds=0x40")) << line;
    EXPECT_TRUE(Contains(line, "rules=1")) << line;
}

// ---------------------------------------------------------------------------------------------
// Etapa 3: `shader_compiles` sozinho não distingue as duas situações que mais importam.
//
// O contador somava compilação de FONTE (shaderc, dezenas de ms, trava quadro) com criação de
// PIPELINE (microssegundos quando o cache do driver está quente). Medido nas alphas: os 2 primeiros
// minutos do mesmo jogo caíram de 556 para 305 conforme o cache de SPIR-V esquentou, e pararam
// ali — os 305 restantes são pipeline, não compilação. Lendo só a contagem, eu atribuí engasgos à
// compilação em janelas onde ela não custava nada.
// ---------------------------------------------------------------------------------------------

TEST(ForkDiagnostics, TheLoadLineSeparatesShaderKindAndCost)
{
    ForkDiagnostics::Load warm{};
    warm.speed_percent = 100.0f;
    warm.vps = 59.94f;
    warm.shader_compiles = 305;
    warm.shader_source_compiles = 0;
    warm.shader_ms = 1.2f;

    ForkDiagnostics::Load cold = warm;
    cold.shader_source_compiles = 140;
    cold.shader_ms = 890.0f;

    const std::string warm_line = ForkDiagnostics::FormatLoadLine(warm);
    const std::string cold_line = ForkDiagnostics::FormatLoadLine(cold);

    // A contagem total é IDÊNTICA nos dois — que é exatamente por que ela sozinha não servia.
    EXPECT_TRUE(Contains(warm_line, "shader_compiles=305")) << warm_line;
    EXPECT_TRUE(Contains(cold_line, "shader_compiles=305")) << cold_line;

    // O que separa: quantas foram de fonte, e quanto tempo custaram.
    EXPECT_TRUE(Contains(warm_line, "shader_src=0")) << warm_line;
    EXPECT_TRUE(Contains(warm_line, "shader_ms=1.2")) << warm_line;
    EXPECT_TRUE(Contains(cold_line, "shader_src=140")) << cold_line;
    EXPECT_TRUE(Contains(cold_line, "shader_ms=890.0")) << cold_line;
}
