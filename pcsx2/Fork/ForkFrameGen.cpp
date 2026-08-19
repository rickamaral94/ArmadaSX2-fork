// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkFrameGen.h"

#include "Fork/ForkConfig.h"
#include "GS/Renderers/Common/GSPresentationMetrics.h"
#include "PerformanceMetrics.h"

#include "fmt/format.h"

#include <atomic>
#include <mutex>

ForkFrameGen::Decision ForkFrameGen::Decide(const Policy& policy, const Inputs& inputs)
{
	Decision decision;

	if (policy.mode == Mode::Off)
	{
		decision.state = State::Disabled;
		decision.reason = Reason::Off;
		return decision;
	}

	if (!inputs.supported)
	{
		decision.state = State::Disabled;
		decision.reason = Reason::Unsupported;
		return decision;
	}

	// A ordem daqui para baixo é deliberada, e cada degrau é uma regra do projeto.

	// 1. Sem quadro novo não há o que gerar — e é este degrau que impede FG de "produzir"
	// suavidade quando a emulação travou ou está repetindo quadros. Vem antes de tudo porque é a
	// condição que protege contra o pior cenário: números subindo enquanto o jogo congela.
	if (!inputs.has_new_frame)
	{
		decision.state = State::Waiting;
		decision.reason = Reason::NoNewFrame;
		return decision;
	}

	// 2. Emulação abaixo da velocidade correta: 22 reais mostrando 44 não é sucesso, é maquiagem.
	// Vem antes da checagem de estabilidade porque uma emulação lenta pode ser perfeitamente
	// REGULAR — e aí passaria no teste de ritmo enquanto viola a regra que mais importa.
	//
	// Medido em VELOCIDADE, não em FPS absoluto. O piso em FPS era um erro de projeto que só um
	// aparelho de verdade revelou: um jogo de PS2 que renderiza a 30 e roda perfeitamente entrega
	// 30 FPS reais, enquanto um jogo de 60 rodando pela METADE entrega os mesmos 30. O primeiro
	// merece FG mais do que qualquer outro caso; o segundo é exatamente o que a regra proíbe. Um
	// número não distingue os dois; a razão contra a taxa alvo da máquina distingue.
	// A histerese existe porque o aparelho provou que ela faz falta: com limiar único, 20 trocas
	// de estado em 10 s numa cena pesada. Engata em `min_speed_percent`, mas só larga abaixo de
	// `min_speed_percent - histerese` — a faixa entre os dois é onde a oscilação vivia.
	const float speed_floor = inputs.previously_engaged
								  ? (policy.min_speed_percent - policy.speed_hysteresis_percent)
								  : policy.min_speed_percent;
	if (inputs.speed_percent > 0.0f && inputs.speed_percent < speed_floor)
	{
		decision.state = State::Waiting;
		decision.reason = Reason::BelowFullSpeed;
		return decision;
	}

	// 3. Piso absoluto de FPS, contra LATÊNCIA e não contra lentidão. Interpolar segura o quadro
	// novo até produzir o do meio, então o atraso em milissegundos cresce à medida que a taxa cai:
	// a 60 FPS é ~17 ms, a 15 FPS é ~67 ms. Abaixo daqui o ganho de fluidez não paga o input lag.
	if (inputs.real_fps > 0.0f && inputs.real_fps < policy.min_real_fps)
	{
		decision.state = State::Waiting;
		decision.reason = Reason::BelowMinimumRealFps;
		return decision;
	}

	// 4. Ritmo irregular: interpolar sobre frametime que oscila piora a percepção em vez de
	// melhorar, porque o quadro sintético entra em um instante que não corresponde a nada.
	if (inputs.frametime_avg_ms > 0.0f &&
		inputs.frametime_p99_ms > (inputs.frametime_avg_ms * policy.max_p99_ratio))
	{
		decision.state = State::Waiting;
		decision.reason = Reason::Unstable;
		return decision;
	}

	// 5. Orçamento estourado: a geração passou a roubar tempo da emulação. Suspende — estado
	// distinto de Waiting, porque aqui houve uma tentativa que custou caro, e a UI precisa poder
	// dizer isso em vez de sugerir que as condições nunca foram atendidas.
	// Histerese também aqui, e pelo mesmo motivo medido no aparelho: suspenso, nenhum custo novo
	// entra na janela, a média decai, o degrau libera, o custo alto volta na hora e suspende de
	// novo. Exigir que a média caia BEM abaixo do teto antes de reengatar quebra o ciclo.
	const float budget_ceiling =
		inputs.previously_over_budget ? (policy.budget_ms * (1.0f - policy.budget_hysteresis)) : policy.budget_ms;
	if (inputs.last_generation_ms > budget_ceiling)
	{
		decision.state = State::Suspended;
		decision.reason = Reason::OverBudget;
		return decision;
	}

	decision.state = State::Engaged;
	decision.reason = Reason::Engaged;
	// Auto e 2x geram um quadro sintético por quadro real. Multiplicadores maiores ficam para
	// quando houver backend real e evidência de que cabem no orçamento.
	decision.frames_to_generate = 1;
	return decision;
}

ForkFrameGen::Mode ForkFrameGen::ParseMode(std::string_view value)
{
	if (value == "auto")
		return Mode::Auto;
	if (value == "2x" || value == "x2")
		return Mode::X2;
	// Vazio, lixo ou "off" caem em Off: o padrão seguro é não fazer nada.
	return Mode::Off;
}

const char* ForkFrameGen::ModeToString(Mode mode)
{
	switch (mode)
	{
		case Mode::Off:
			return "off";
		case Mode::Auto:
			return "auto";
		case Mode::X2:
			return "2x";
	}
	return "off";
}

const char* ForkFrameGen::StateToString(State state)
{
	switch (state)
	{
		case State::Disabled:
			return "Disabled";
		case State::Waiting:
			return "Waiting";
		case State::Engaged:
			return "Engaged";
		case State::Suspended:
			return "Suspended";
	}
	return "Disabled";
}

const char* ForkFrameGen::ReasonToString(Reason reason)
{
	switch (reason)
	{
		case Reason::Off:
			return "Off";
		case Reason::Unsupported:
			return "Unsupported";
		case Reason::NoNewFrame:
			return "NoNewFrame";
		case Reason::Unstable:
			return "Unstable";
		case Reason::BelowFullSpeed:
			return "BelowFullSpeed";
		case Reason::BelowMinimumRealFps:
			return "BelowMinimumRealFps";
		case Reason::OverBudget:
			return "OverBudget";
		case Reason::Engaged:
			return "Engaged";
	}
	return "Off";
}

const char* ForkFrameGen::ReasonText(Reason reason)
{
	switch (reason)
	{
		case Reason::Off:
			return "Desligado.";
		case Reason::Unsupported:
			return "Este renderer ou GPU não suporta frame generation.";
		case Reason::NoNewFrame:
			return "Aguardando quadros novos do jogo.";
		case Reason::Unstable:
			return "Ritmo instável — gerar agora pioraria a fluidez.";
		case Reason::BelowFullSpeed:
			return "Emulação abaixo da velocidade correta; suavizar aqui esconderia a lentidão.";
		case Reason::BelowMinimumRealFps:
			return "Taxa muito baixa; o atraso da interpolação não compensaria.";
		case Reason::OverBudget:
			return "A geração não coube no orçamento de tempo e foi suspensa.";
		case Reason::Engaged:
			return "Ativo.";
	}
	return "Desligado.";
}

ForkFrameGen::Policy ForkFrameGen::PolicyFromConfig()
{
	Policy policy;
	policy.mode = ParseMode(ForkConfig::GetString(ForkConfig::Option::FrameGenMode));
	policy.budget_ms = ForkConfig::GetFloat(ForkConfig::Option::FrameGenBudgetMs);
	policy.min_speed_percent = ForkConfig::GetFloat(ForkConfig::Option::FrameGenMinSpeedPercent);
	policy.speed_hysteresis_percent = ForkConfig::GetFloat(ForkConfig::Option::FrameGenSpeedHysteresis);
	policy.min_real_fps = ForkConfig::GetFloat(ForkConfig::Option::FrameGenMinRealFps);
	return policy;
}

std::string ForkFrameGen::StatusLine(const Decision& decision)
{
	if (decision.state == State::Disabled && decision.reason == Reason::Off)
		return {};

	// O estado aparece SEMPRE que FG está ligado, mesmo (principalmente) quando não está gerando.
	// A alternativa é o usuário ligar a opção, não ver nada acontecer e não ter como distinguir
	// "não engatou" de "está quebrado".
	return fmt::format("FG: {} — {}", StateToString(decision.state), ReasonText(decision.reason));
}

namespace
{
	std::mutex s_decision_mutex;
	ForkFrameGen::Decision s_last_decision;
} // namespace

ForkFrameGen::Decision ForkFrameGen::EvaluateAtPresent(bool supported, bool has_new_frame)
{
	const Policy policy = PolicyFromConfig();

	Decision decision;
	if (policy.mode == Mode::Off || !supported)
	{
		// Caminho rápido: desligado não paga nem a leitura das métricas. FG desligado é o padrão,
		// e o custo dele tem que ser indistinguível de não existir.
		//
		// `!supported` entra no mesmo caminho porque, sem backend capaz de apresentar, nenhuma
		// política muda o resultado — e ler as métricas a cada quadro para concluir isso seria
		// cobrar de todo mundo por um recurso que ninguém ligou. O motivo continua distinguindo os
		// dois casos: "desligado" é escolha do usuário, "incompatível" é do aparelho.
		decision.state = State::Disabled;
		decision.reason = (policy.mode == Mode::Off) ? Reason::Off : Reason::Unsupported;
	}
	else
	{
		const GSPresentationMetrics::Snapshot snapshot = GSPresentationMetrics::GetSnapshot();

		Inputs inputs;
		inputs.supported = supported;
		inputs.has_new_frame = has_new_frame;
		inputs.real_fps = snapshot.real_fps;
		// Velocidade vem do PCSX2, não das nossas métricas: ele a calcula contra a taxa alvo da
		// MÁQUINA (59,94 / 50), que é o único denominador que separa "o jogo renderiza a 30" de
		// "o aparelho só dá conta de metade".
		inputs.speed_percent = PerformanceMetrics::GetSpeed();
		{
			std::lock_guard lock(s_decision_mutex);
			inputs.previously_engaged = (s_last_decision.state == State::Engaged);
			inputs.previously_over_budget = (s_last_decision.reason == Reason::OverBudget);
		}
		inputs.frametime_avg_ms = snapshot.real_frametime_avg_ms;
		inputs.frametime_p99_ms = snapshot.real_frametime_low1_ms;
		inputs.last_generation_ms = snapshot.generation_avg_ms;
		decision = Decide(policy, inputs);
	}

	{
		std::lock_guard lock(s_decision_mutex);
		s_last_decision = decision;
	}
	return decision;
}

ForkFrameGen::Decision ForkFrameGen::GetLastDecision()
{
	std::lock_guard lock(s_decision_mutex);
	return s_last_decision;
}
