// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkFrameGen.h"

#include "Fork/ForkConfig.h"
#include "GS/Renderers/Common/GSPresentationMetrics.h"

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

	// 2. FPS real abaixo do mínimo: 22 reais mostrando 44 não é sucesso, é maquiagem. Vem antes da
	// checagem de estabilidade porque uma emulação lenta pode ser perfeitamente REGULAR — e aí
	// passaria no teste de ritmo enquanto viola a regra que mais importa.
	if (inputs.real_fps > 0.0f && inputs.real_fps < policy.min_real_fps)
	{
		decision.state = State::Waiting;
		decision.reason = Reason::BelowMinimumRealFps;
		return decision;
	}

	// 3. Ritmo irregular: interpolar sobre frametime que oscila piora a percepção em vez de
	// melhorar, porque o quadro sintético entra em um instante que não corresponde a nada.
	if (inputs.frametime_avg_ms > 0.0f &&
		inputs.frametime_p99_ms > (inputs.frametime_avg_ms * policy.max_p99_ratio))
	{
		decision.state = State::Waiting;
		decision.reason = Reason::Unstable;
		return decision;
	}

	// 4. Orçamento estourado: a geração passou a roubar tempo da emulação. Suspende — estado
	// distinto de Waiting, porque aqui houve uma tentativa que custou caro, e a UI precisa poder
	// dizer isso em vez de sugerir que as condições nunca foram atendidas.
	if (inputs.last_generation_ms > policy.budget_ms)
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
		case Reason::BelowMinimumRealFps:
			return "FPS real abaixo do mínimo; suavizar aqui esconderia a lentidão.";
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
