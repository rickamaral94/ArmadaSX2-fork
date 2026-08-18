// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkDiagnostics.h"

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
	EXPECT_TRUE(Contains(ForkDiagnostics::FormatFrameGenLine(accumulator, policy, 0.0f), "@@FORK@@"));
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
	const std::string line = ForkDiagnostics::FormatFrameGenLine(accumulator, policy, 1.4f);
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
	const std::string line = ForkDiagnostics::FormatFrameGenLine(accumulator, policy, 0.0f);
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
