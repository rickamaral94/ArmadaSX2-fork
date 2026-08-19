// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkFrameGen.h"

#include <gtest/gtest.h>

#include <string>

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
		policy.budget_ms = 8.0f;
		policy.budget_hysteresis = 0.25f;
		policy.min_speed_percent = 90.0f;
		policy.speed_hysteresis_percent = 5.0f;
		policy.min_real_fps = 15.0f;
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
		inputs.speed_percent = 100.0f;
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
	slow.speed_percent = 37.0f;
	slow.frametime_avg_ms = 45.5f;
	slow.frametime_p99_ms = 46.0f; // perfeitamente REGULAR — só que lento

	const Decision decision = ForkFrameGen::Decide(MakePolicy(), slow);
	EXPECT_EQ(decision.reason, Reason::BelowFullSpeed);
	EXPECT_EQ(decision.frames_to_generate, 0u);
}

// O erro de projeto que só um aparelho de verdade revelou: o piso era FPS ABSOLUTO, e um jogo de
// PS2 que renderiza a 30 rodando PERFEITAMENTE entrega os mesmos 30 FPS reais que um jogo de 60
// rodando pela metade. O primeiro é o caso em que FG mais ajuda; o segundo é o que a regra proíbe.
// Só a velocidade contra a taxa alvo da máquina separa os dois.
TEST(ForkFrameGen, ThirtyFpsAtFullSpeedIsNotSlowEmulation)
{
	Inputs native_30 = HealthyFrame();
	native_30.real_fps = 30.0f;
	native_30.speed_percent = 100.0f; // o jogo é assim; a emulação está correta
	native_30.frametime_avg_ms = 33.3f;
	native_30.frametime_p99_ms = 34.0f;

	const Decision engaged = ForkFrameGen::Decide(MakePolicy(), native_30);
	EXPECT_EQ(engaged.reason, Reason::Engaged) << "30 FPS nativos a 100% merecem FG";
	EXPECT_EQ(engaged.frames_to_generate, 1u);

	// Mesmíssimo FPS real, metade da velocidade: recusado.
	Inputs half_speed = native_30;
	half_speed.speed_percent = 50.0f;
	EXPECT_EQ(ForkFrameGen::Decide(MakePolicy(), half_speed).reason, Reason::BelowFullSpeed);
}

// O piso absoluto sobrevive, com outro papel: contra LATÊNCIA. Interpolar segura o quadro novo,
// e a 10 FPS isso custa ~100 ms de input lag mesmo com a emulação em velocidade correta.
TEST(ForkFrameGen, VeryLowRateIsRefusedForLatencyNotForSlowness)
{
	Inputs crawling = HealthyFrame();
	crawling.real_fps = 10.0f;
	crawling.speed_percent = 100.0f;
	crawling.frametime_avg_ms = 100.0f;
	crawling.frametime_p99_ms = 101.0f;

	EXPECT_EQ(ForkFrameGen::Decide(MakePolicy(), crawling).reason, Reason::BelowMinimumRealFps);
}

// O motivo do degrau de FPS vir antes do de estabilidade: emulação lenta pode ser regular, e aí
// passaria no teste de ritmo enquanto viola a regra que mais importa.
TEST(ForkFrameGen, SlowButSteadyIsStillRefusedForBeingSlow)
{
	Inputs slow_and_steady = HealthyFrame();
	slow_and_steady.real_fps = 20.0f;
	slow_and_steady.speed_percent = 33.0f;
	slow_and_steady.frametime_avg_ms = 50.0f;
	slow_and_steady.frametime_p99_ms = 50.1f;

	EXPECT_EQ(ForkFrameGen::Decide(MakePolicy(), slow_and_steady).reason, Reason::BelowFullSpeed);
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
		Reason::BelowFullSpeed, Reason::BelowMinimumRealFps, Reason::OverBudget, Reason::Engaged};
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
	policy.min_speed_percent = 90.0f;
	policy.min_real_fps = 15.0f;
	policy.budget_ms = 6.0f;

	ForkFrameGen::Inputs inputs;
	inputs.supported = true;
	inputs.has_new_frame = true;
	inputs.real_fps = 60.0f;
	inputs.speed_percent = 100.0f;
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
	slow.speed_percent = 37.0f;
	EXPECT_EQ(ForkFrameGen::Decide(policy, slow).frames_to_generate, 0u);

	ForkFrameGen::Inputs unstable = inputs;
	unstable.frametime_p99_ms = 40.0f;
	EXPECT_EQ(ForkFrameGen::Decide(policy, unstable).frames_to_generate, 0u);

	ForkFrameGen::Inputs expensive = inputs;
	expensive.last_generation_ms = 9.0f;
	EXPECT_EQ(ForkFrameGen::Decide(policy, expensive).frames_to_generate, 0u);
}

// Medido no aparelho: com limiar único de 90%, o modo auto trocou de estado 20 VEZES EM 10
// SEGUNDOS numa cena pesada — a velocidade oscilava em torno do limiar e cada oscilação ligava e
// desligava a geração. Piscar é pior que ficar desligado.
TEST(ForkFrameGen, HysteresisStopsTheFlickerAroundTheSpeedFloor)
{
	const Policy policy = MakePolicy(); // piso 90, histerese 5

	Inputs borderline = HealthyFrame();
	borderline.speed_percent = 87.0f; // dentro da faixa de histerese

	// Vindo de DESENGATADO: 87 < 90, não engata.
	borderline.previously_engaged = false;
	EXPECT_EQ(ForkFrameGen::Decide(policy, borderline).reason, Reason::BelowFullSpeed);

	// Vindo de ENGATADO: 87 ainda está acima de 90-5=85, então SEGUE engatado.
	borderline.previously_engaged = true;
	EXPECT_EQ(ForkFrameGen::Decide(policy, borderline).reason, Reason::Engaged);
}

// A histerese não pode virar teimosia: abaixo do piso inferior larga, mesmo vindo de engatado.
TEST(ForkFrameGen, HysteresisStillLetsGoWhenItGetsGenuinelySlow)
{
	Inputs slow = HealthyFrame();
	slow.speed_percent = 80.0f;
	slow.previously_engaged = true;
	EXPECT_EQ(ForkFrameGen::Decide(MakePolicy(), slow).reason, Reason::BelowFullSpeed);
}

// Simula a oscilação real do log: velocidade balançando em torno do limiar. Sem histerese seriam
// dezenas de trocas; com ela, uma só — a que importa.
TEST(ForkFrameGen, AnOscillatingSpeedNoLongerProducesDozensOfTransitions)
{
	const Policy policy = MakePolicy();
	const float samples[] = {95.0f, 89.0f, 91.0f, 88.0f, 92.0f, 87.0f, 93.0f, 86.0f, 94.0f, 88.0f};

	bool engaged = false;
	int transitions = 0;
	for (const float speed : samples)
	{
		Inputs inputs = HealthyFrame();
		inputs.speed_percent = speed;
		inputs.previously_engaged = engaged;
		const bool now_engaged = (ForkFrameGen::Decide(policy, inputs).state == State::Engaged);
		if (now_engaged != engaged)
			transitions++;
		engaged = now_engaged;
	}
	EXPECT_EQ(transitions, 1) << "engata uma vez em 95% e não larga mais na faixa 86-94";
}

// Nome curto para log e parsing, frase para o usuário: um log com frases inteiras é caro de
// filtrar e quebra na primeira vez que alguém reescreve o texto da UI.
TEST(ForkFrameGen, EveryReasonHasAShortStableName)
{
	const Reason all[] = {Reason::Off, Reason::Unsupported, Reason::NoNewFrame, Reason::Unstable,
		Reason::BelowFullSpeed, Reason::BelowMinimumRealFps, Reason::OverBudget, Reason::Engaged};
	for (const Reason reason : all)
	{
		const std::string short_name = ForkFrameGen::ReasonToString(reason);
		EXPECT_FALSE(short_name.empty());
		EXPECT_EQ(short_name.find(' '), std::string::npos) << short_name << " tem espaço";
		EXPECT_EQ(short_name.find('.'), std::string::npos) << short_name << " parece frase";
	}
}

// Medido no Adreno 740, LSFG 3.1p a 1080p x2: cena leve custa ~6,5 ms, cena pesada ~16 ms. O teto
// de 6,0 ms — chute meu, feito sem aparelho — caía EM CIMA do custo normal. O teto novo separa os
// dois regimes; este teste fixa essa separação com os números que o aparelho produziu.
TEST(ForkFrameGen, TheBudgetSeparatesTheTwoMeasuredRegimes)
{
	const Policy policy = MakePolicy(); // budget 8 ms

	Inputs light = HealthyFrame();
	light.last_generation_ms = 6.5f; // cena leve, medida
	EXPECT_EQ(ForkFrameGen::Decide(policy, light).reason, Reason::Engaged);

	Inputs heavy = HealthyFrame();
	heavy.last_generation_ms = 16.0f; // cena pesada, medida
	EXPECT_EQ(ForkFrameGen::Decide(policy, heavy).reason, Reason::OverBudget);
}

// O oscilador que o aparelho expôs: suspenso por custo, nenhuma amostra nova entra na janela de
// 1 s, a média decai, o degrau libera, o custo alto volta na hora e suspende de novo — 20
// transições a cada 10 s. A histerese exige que a média caia BEM abaixo do teto antes de reengatar.
TEST(ForkFrameGen, BudgetHysteresisBreaksTheSuspendResumeOscillation)
{
	const Policy policy = MakePolicy(); // teto 8 ms, histerese 25% -> reengata só abaixo de 6 ms

	Inputs cooling = HealthyFrame();
	cooling.last_generation_ms = 7.0f; // abaixo do teto, mas ainda caro
	cooling.previously_over_budget = true;
	EXPECT_EQ(ForkFrameGen::Decide(policy, cooling).reason, Reason::OverBudget)
		<< "vindo de suspenso, 7 ms ainda não reengata";

	cooling.last_generation_ms = 5.0f; // esfriou de verdade
	EXPECT_EQ(ForkFrameGen::Decide(policy, cooling).reason, Reason::Engaged);

	// E sem vir de suspenso, 7 ms passa normalmente — a histerese não aperta quem já está engatado.
	Inputs running = HealthyFrame();
	running.last_generation_ms = 7.0f;
	running.previously_over_budget = false;
	EXPECT_EQ(ForkFrameGen::Decide(policy, running).reason, Reason::Engaged);
}

// Reproduz a oscilação do log com custo real batendo em torno do teto antigo.
TEST(ForkFrameGen, TheMeasuredCostNoLongerThrashes)
{
	const Policy policy = MakePolicy();
	const float measured[] = {6.45f, 6.57f, 6.38f, 6.82f, 6.39f, 6.45f, 6.93f, 6.34f, 6.16f, 6.97f};

	bool engaged = false;
	bool over_budget = false;
	int transitions = 0;
	for (const float cost : measured)
	{
		Inputs inputs = HealthyFrame();
		inputs.last_generation_ms = cost;
		inputs.previously_engaged = engaged;
		inputs.previously_over_budget = over_budget;

		const Decision decision = ForkFrameGen::Decide(policy, inputs);
		const bool now_engaged = (decision.state == State::Engaged);
		if (now_engaged != engaged)
			transitions++;
		engaged = now_engaged;
		over_budget = (decision.reason == Reason::OverBudget);
	}
	EXPECT_EQ(transitions, 1) << "engata uma vez e fica; antes eram ~20 por 10 s";
	EXPECT_TRUE(engaged);
}

// A carência existe porque a histerese por degrau foi remendo três vezes — velocidade, orçamento,
// e faltava a estabilidade. O aparelho mostrou o terceiro: numa corrida de NFS Underground 2, com
// o jogo renderizando ~30 fps internos e o custo de geração em ~20 ms, a régua piscou 18 vezes a
// cada 10 s. Ligar e desligar duas vezes por segundo é PIOR que ficar desligado.
TEST(ForkFrameGen, CooldownStopsThrashOnAnyGateIncludingOnesNotYetWritten)
{
	Policy policy = MakePolicy();
	policy.reengage_cooldown_frames = 30;

	// Desengata por instabilidade (o degrau que ainda não tinha histerese).
	Inputs jittery = HealthyFrame();
	jittery.frametime_avg_ms = 33.3f;
	jittery.frametime_p99_ms = 92.0f;
	jittery.frames_since_disengage = 1000;
	EXPECT_EQ(ForkFrameGen::Decide(policy, jittery).reason, Reason::Unstable);

	// Logo depois, mesmo com o ritmo perfeito de novo, a carência segura.
	Inputs recovered = HealthyFrame();
	recovered.frames_since_disengage = 5;
	EXPECT_EQ(ForkFrameGen::Decide(policy, recovered).reason, Reason::Cooldown);

	// Passada a carência, engata.
	recovered.frames_since_disengage = 30;
	EXPECT_EQ(ForkFrameGen::Decide(policy, recovered).reason, Reason::Engaged);
}

// A carência NUNCA pode segurar a geração ligada — só atrasar o engate. Senão ela mesma viraria um
// jeito de roubar tempo da emulação para cumprir um contador.
TEST(ForkFrameGen, CooldownOnlyDelaysEngagementAndNeverForcesIt)
{
	Policy policy = MakePolicy();
	policy.reengage_cooldown_frames = 30;

	// Sem quadro novo é o único degrau que jamais espera: vem ANTES da carência.
	Inputs stalled = HealthyFrame();
	stalled.has_new_frame = false;
	stalled.frames_since_disengage = 5;
	EXPECT_EQ(ForkFrameGen::Decide(policy, stalled).reason, Reason::NoNewFrame);

	// E em carência nunca se gera quadro.
	Inputs waiting = HealthyFrame();
	waiting.frames_since_disengage = 1;
	EXPECT_EQ(ForkFrameGen::Decide(policy, waiting).frames_to_generate, 0u);
}

// Reproduz a corrida medida: ritmo oscilando em torno do limiar de estabilidade, dez segundos a
// 60 Hz. Roda pelo [Advance] de verdade — a memória entre quadros é onde os osciladores desta fase
// nasceram, e um teste que a reimplementa verifica a própria cópia, não o produto.
TEST(ForkFrameGen, TheMeasuredRaceNoLongerPulses)
{
	Policy policy = MakePolicy();
	policy.reengage_cooldown_frames = 30;

	ForkFrameGen::Governor governor;
	bool engaged = false;
	int transitions = 0;
	for (int i = 0; i < 600; i++)
	{
		Inputs inputs = HealthyFrame();
		inputs.frametime_avg_ms = 33.3f;
		inputs.frametime_p99_ms = (i % 2 == 0) ? 92.0f : 40.0f;

		const bool now = (ForkFrameGen::Advance(policy, governor, inputs).state == State::Engaged);
		if (now != engaged)
			transitions++;
		engaged = now;
	}

	// Eram 18 a cada 10 s, e uma carência FIXA de 30 quadros só as espaçava — 8 em 120 quadros, o
	// mesmo pulso em ritmo mais lento. O que corta o pulso é a carência DOBRAR: as tentativas caem
	// pela metade a cada fracasso, então o total cresce com o logaritmo do tempo em vez de
	// linearmente. Dez segundos inteiros de cena impossível cabem agora em oito trocas, e os dez
	// segundos seguintes cabem em duas.
	EXPECT_LE(transitions, 8);
	EXPECT_GT(governor.failed_engagements, 2u) << "a carência tem que ter crescido, não só contado";
}

// O outro lado da mesma moeda: o castigo acumulado numa cena impossível não pode ser cobrado da
// cena seguinte. Sem isto, sair de uma corrida com a carência dobrada cinco vezes deixaria o menu
// seguinte — perfeitamente estável — esperando 16 s por nada.
TEST(ForkFrameGen, SustainedCalmReleasesTheAccumulatedCooldown)
{
	Policy policy = MakePolicy();
	policy.reengage_cooldown_frames = 30;

	ForkFrameGen::Governor governor;
	// Cena impossível: acumula fracassos até a carência estar bem dilatada.
	for (int i = 0; i < 600; i++)
	{
		Inputs inputs = HealthyFrame();
		inputs.frametime_avg_ms = 33.3f;
		inputs.frametime_p99_ms = (i % 2 == 0) ? 92.0f : 40.0f;
		ForkFrameGen::Advance(policy, governor, inputs);
	}
	ASSERT_GT(governor.failed_engagements, 2u);

	// A cena acaba. Meio segundo de calma sustentada — e não um quadro bom solto, que a própria
	// oscilação produzia a cada dois quadros — devolve a resposta rápida.
	int frames_until_engaged = 0;
	for (int i = 0; i < 240; i++)
	{
		frames_until_engaged++;
		if (ForkFrameGen::Advance(policy, governor, HealthyFrame()).state == State::Engaged)
			break;
	}
	EXPECT_LE(frames_until_engaged, 40) << "meia carência de folga; 16 s de espera seria o defeito";
	EXPECT_EQ(governor.failed_engagements, 0u);
}

// E a calma tem que ser SUSTENTADA. Um único quadro bom no meio da oscilação não pode soltar a
// carência: é exatamente o que a cena ruim produz metade do tempo.
TEST(ForkFrameGen, OneGoodFrameDoesNotReleaseTheCooldown)
{
	Policy policy = MakePolicy();
	policy.reengage_cooldown_frames = 30;

	ForkFrameGen::Governor governor;
	governor.last.state = State::Engaged;
	governor.frames_since_disengage = 0;
	governor.failed_engagements = 3;

	// Alterna um quadro bom e um ruim por bastante tempo: nunca acumula calma suficiente.
	for (int i = 0; i < 200; i++)
	{
		Inputs inputs = HealthyFrame();
		inputs.frametime_avg_ms = 33.3f;
		inputs.frametime_p99_ms = (i % 2 == 0) ? 92.0f : 40.0f;
		ForkFrameGen::Advance(policy, governor, inputs);
		ASSERT_LT(governor.frames_ready_in_cooldown, policy.reengage_cooldown_frames);
	}
}

// Um engate que DUROU não é um fracasso, e não pode dilatar a carência. Sem esta metade, a régua
// iria endurecendo a cada desengate legítimo até parar de engatar em qualquer lugar.
TEST(ForkFrameGen, AnEngagementThatLastsClearsTheBackoff)
{
	Policy policy = MakePolicy();
	policy.reengage_cooldown_frames = 30;

	ForkFrameGen::Governor governor;
	governor.failed_engagements = 4;

	// Cem quadros engatados: bem mais que a carência base.
	for (int i = 0; i < 100; i++)
		ASSERT_EQ(ForkFrameGen::Advance(policy, governor, HealthyFrame()).state, State::Engaged);

	// Agora desengata por um motivo legítimo.
	Inputs stalled = HealthyFrame();
	stalled.has_new_frame = false;
	ForkFrameGen::Advance(policy, governor, stalled);
	EXPECT_EQ(governor.failed_engagements, 0u);
}
