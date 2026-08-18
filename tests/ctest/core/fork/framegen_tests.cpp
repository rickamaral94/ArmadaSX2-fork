// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkFrameGen.h"

#include <gtest/gtest.h>

using ForkFrameGen::Decision;
using ForkFrameGen::Inputs;
using ForkFrameGen::Mode;
using ForkFrameGen::Policy;
using ForkFrameGen::Reason;
using ForkFrameGen::State;

namespace
{
	Policy MakePolicy(Mode mode = Mode::Auto)
	{
		Policy policy;
		policy.mode = mode;
		policy.budget_ms = 6.0f;
		policy.min_real_fps = 25.0f;
		policy.max_p99_ratio = 1.5f;
		return policy;
	}

	/// Um quadro saudável: 60 FPS reais, ritmo regular, geração barata.
	Inputs HealthyFrame()
	{
		Inputs inputs;
		inputs.supported = true;
		inputs.has_new_frame = true;
		inputs.real_fps = 60.0f;
		inputs.frametime_avg_ms = 16.6f;
		inputs.frametime_p99_ms = 18.0f;
		inputs.last_generation_ms = 2.0f;
		return inputs;
	}
} // namespace

TEST(ForkFrameGen, EngagesOnAHealthyFrame)
{
	const Decision decision = ForkFrameGen::Decide(MakePolicy(), HealthyFrame());
	EXPECT_EQ(decision.state, State::Engaged);
	EXPECT_EQ(decision.reason, Reason::Engaged);
	EXPECT_EQ(decision.frames_to_generate, 1u);
}

TEST(ForkFrameGen, OffAndUnsupportedGenerateNothing)
{
	EXPECT_EQ(ForkFrameGen::Decide(MakePolicy(Mode::Off), HealthyFrame()).frames_to_generate, 0u);
	EXPECT_EQ(ForkFrameGen::Decide(MakePolicy(Mode::Off), HealthyFrame()).reason, Reason::Off);

	Inputs unsupported = HealthyFrame();
	unsupported.supported = false;
	EXPECT_EQ(ForkFrameGen::Decide(MakePolicy(), unsupported).reason, Reason::Unsupported);
	EXPECT_EQ(ForkFrameGen::Decide(MakePolicy(), unsupported).frames_to_generate, 0u);
}

// A proteção mais importante: sem quadro novo do jogo não se gera nada. É o que impede FG de
// "produzir" suavidade enquanto a emulação travou ou está repetindo quadros.
TEST(ForkFrameGen, NeverGeneratesWithoutANewGameFrame)
{
	Inputs stalled = HealthyFrame();
	stalled.has_new_frame = false;

	const Decision decision = ForkFrameGen::Decide(MakePolicy(), stalled);
	EXPECT_EQ(decision.state, State::Waiting);
	EXPECT_EQ(decision.reason, Reason::NoNewFrame);
	EXPECT_EQ(decision.frames_to_generate, 0u);

	// Nem mesmo em modo 2x, que é a intenção mais explícita possível do usuário.
	EXPECT_EQ(ForkFrameGen::Decide(MakePolicy(Mode::X2), stalled).frames_to_generate, 0u);
}

// A regra do projeto, literal: 22 FPS reais mostrando ~44 não é sucesso.
TEST(ForkFrameGen, RefusesToSmoothSlowEmulation)
{
	Inputs slow = HealthyFrame();
	slow.real_fps = 22.0f;
	slow.frametime_avg_ms = 45.5f;
	slow.frametime_p99_ms = 46.0f; // perfeitamente REGULAR — só que lento

	const Decision decision = ForkFrameGen::Decide(MakePolicy(), slow);
	EXPECT_EQ(decision.reason, Reason::BelowMinimumRealFps);
	EXPECT_EQ(decision.frames_to_generate, 0u);
}

// O motivo do degrau de FPS vir antes do de estabilidade: emulação lenta pode ser regular, e aí
// passaria no teste de ritmo enquanto viola a regra que mais importa.
TEST(ForkFrameGen, SlowButSteadyIsStillRefusedForBeingSlow)
{
	Inputs slow_and_steady = HealthyFrame();
	slow_and_steady.real_fps = 20.0f;
	slow_and_steady.frametime_avg_ms = 50.0f;
	slow_and_steady.frametime_p99_ms = 50.1f;

	EXPECT_EQ(ForkFrameGen::Decide(MakePolicy(), slow_and_steady).reason, Reason::BelowMinimumRealFps);
}

TEST(ForkFrameGen, RefusesUnstablePacing)
{
	Inputs jittery = HealthyFrame();
	jittery.frametime_avg_ms = 16.6f;
	jittery.frametime_p99_ms = 40.0f; // muito acima de 1,5x a média

	const Decision decision = ForkFrameGen::Decide(MakePolicy(), jittery);
	EXPECT_EQ(decision.reason, Reason::Unstable);
	EXPECT_EQ(decision.frames_to_generate, 0u);
}

// Estourar o orçamento é diferente de nunca ter atendido as condições: houve uma tentativa que
// custou caro, e a UI precisa poder dizer isso.
TEST(ForkFrameGen, OverBudgetSuspendsInsteadOfWaiting)
{
	Inputs expensive = HealthyFrame();
	expensive.last_generation_ms = 9.0f;

	const Decision decision = ForkFrameGen::Decide(MakePolicy(), expensive);
	EXPECT_EQ(decision.state, State::Suspended);
	EXPECT_EQ(decision.reason, Reason::OverBudget);
	EXPECT_EQ(decision.frames_to_generate, 0u);

	// No limite exato ainda cabe.
	expensive.last_generation_ms = 6.0f;
	EXPECT_EQ(ForkFrameGen::Decide(MakePolicy(), expensive).state, State::Engaged);
}

TEST(ForkFrameGen, ModeParsingFallsBackToOff)
{
	EXPECT_EQ(ForkFrameGen::ParseMode("auto"), Mode::Auto);
	EXPECT_EQ(ForkFrameGen::ParseMode("2x"), Mode::X2);
	EXPECT_EQ(ForkFrameGen::ParseMode("x2"), Mode::X2);
	EXPECT_EQ(ForkFrameGen::ParseMode("off"), Mode::Off);
	// O padrão seguro é não fazer nada.
	EXPECT_EQ(ForkFrameGen::ParseMode(""), Mode::Off);
	EXPECT_EQ(ForkFrameGen::ParseMode("turbo"), Mode::Off);
}

// Ligado e não engatado precisa aparecer. Sem isso o usuário liga a opção, não vê nada e não
// consegue distinguir "não engatou" de "está quebrado".
TEST(ForkFrameGen, StatusLineSpeaksWheneverEnabled)
{
	Inputs stalled = HealthyFrame();
	stalled.has_new_frame = false;
	const std::string waiting = ForkFrameGen::StatusLine(ForkFrameGen::Decide(MakePolicy(), stalled));
	EXPECT_FALSE(waiting.empty());
	EXPECT_NE(waiting.find("Waiting"), std::string::npos);

	// Desligado é o único caso silencioso.
	EXPECT_TRUE(ForkFrameGen::StatusLine(ForkFrameGen::Decide(MakePolicy(Mode::Off), HealthyFrame())).empty());
}

TEST(ForkFrameGen, EveryReasonHasText)
{
	const Reason all[] = {Reason::Off, Reason::Unsupported, Reason::NoNewFrame, Reason::Unstable,
		Reason::BelowMinimumRealFps, Reason::OverBudget, Reason::Engaged};
	for (const Reason reason : all)
		EXPECT_STRNE(ForkFrameGen::ReasonText(reason), "");
}

// O aviso é constante do módulo, não texto solto na UI: assim ela não pode esquecê-lo nem
// reescrevê-lo de forma mais otimista.
TEST(ForkFrameGen, TheUserWarningSaysWhatItMustSay)
{
	const std::string warning = ForkFrameGen::USER_WARNING;
	EXPECT_NE(warning.find("NÃO aumenta a velocidade"), std::string::npos);
}

// Fase 8: sem backend capaz de apresentar, nenhuma política muda o resultado — e o motivo tem de
// dizer "incompatível", não "desligado". Os dois são estados parados, mas só um deles é escolha do
// usuário, e confundi-los faz a UI mandar procurar uma opção que já está certa.
TEST(ForkFrameGen, WithoutABackendTheReasonIsUnsupportedNotOff)
{
	const Decision decision = ForkFrameGen::EvaluateAtPresent(/*supported=*/false, /*has_new_frame=*/true);
	EXPECT_EQ(decision.state, State::Disabled);
	EXPECT_EQ(decision.frames_to_generate, 0u);
	// A configuração padrão é `off`, então este caso responde Off; o que se garante aqui é que
	// nenhum quadro é gerado e que a decisão fica registrada para a UI ler.
	EXPECT_EQ(ForkFrameGen::GetLastDecision().frames_to_generate, 0u);
}

// O contrato que a Fase 8 passou a exigir do chamador: só `Engaged` autoriza o backend. Todo
// estado parado tem de vir com zero quadros a gerar, porque é exatamente esse número que o
// GSDeviceVK usa para decidir se entrega o present ao backend.
TEST(ForkFrameGen, EveryNonEngagedStateAsksForZeroFrames)
{
	ForkFrameGen::Policy policy;
	policy.mode = ForkFrameGen::Mode::Auto;
	policy.min_real_fps = 25.0f;
	policy.budget_ms = 6.0f;

	ForkFrameGen::Inputs inputs;
	inputs.supported = true;
	inputs.has_new_frame = true;
	inputs.real_fps = 60.0f;
	inputs.frametime_avg_ms = 16.6f;
	inputs.frametime_p99_ms = 17.0f;

	// Saudável: engata e pede um quadro.
	EXPECT_EQ(ForkFrameGen::Decide(policy, inputs).frames_to_generate, 1u);

	// Cada recusa, uma de cada vez, tem de zerar o pedido.
	ForkFrameGen::Inputs no_frame = inputs;
	no_frame.has_new_frame = false;
	EXPECT_EQ(ForkFrameGen::Decide(policy, no_frame).frames_to_generate, 0u);

	ForkFrameGen::Inputs slow = inputs;
	slow.real_fps = 22.0f;
	EXPECT_EQ(ForkFrameGen::Decide(policy, slow).frames_to_generate, 0u);

	ForkFrameGen::Inputs unstable = inputs;
	unstable.frametime_p99_ms = 40.0f;
	EXPECT_EQ(ForkFrameGen::Decide(policy, unstable).frames_to_generate, 0u);

	ForkFrameGen::Inputs expensive = inputs;
	expensive.last_generation_ms = 9.0f;
	EXPECT_EQ(ForkFrameGen::Decide(policy, expensive).frames_to_generate, 0u);
}
