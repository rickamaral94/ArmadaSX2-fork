// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkFrameGen.h"

#include "Fork/ForkConfig.h"
#include "GS/Renderers/Common/GSPresentationMetrics.h"
#include "PerformanceMetrics.h"

#include "fmt/format.h"

#include <algorithm>
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

	// 2. Carência. Vem logo depois do degrau de "sem quadro novo" — o único que jamais pode esperar
	// — e antes de todos os outros, porque o problema que ela resolve não é de nenhum degrau em
	// particular: é de TODOS. Histerese foi remendo três vezes (velocidade, orçamento, e faltava a
	// estabilidade), sempre depois de o aparelho mostrar o mesmo padrão de um limiar oscilando em
	// torno de si mesmo. O último caso: 18 trocas a cada 10 s durante uma corrida.
	//
	// Só atrasa o ENGATE, nunca segura a geração ligada — não há como cumprir carência roubando
	// tempo da emulação.
	if (inputs.frames_since_disengage < policy.reengage_cooldown_frames)
	{
		decision.state = State::Waiting;
		decision.reason = Reason::Cooldown;
		return decision;
	}

	// 3. Emulação abaixo da velocidade correta: 22 reais mostrando 44 não é sucesso, é maquiagem.
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

	// 4. Piso absoluto de FPS, contra LATÊNCIA e não contra lentidão. Interpolar segura o quadro
	// novo até produzir o do meio, então o atraso em milissegundos cresce à medida que a taxa cai:
	// a 60 FPS é ~17 ms, a 15 FPS é ~67 ms. Abaixo daqui o ganho de fluidez não paga o input lag.
	if (inputs.real_fps > 0.0f && inputs.real_fps < policy.min_real_fps)
	{
		decision.state = State::Waiting;
		decision.reason = Reason::BelowMinimumRealFps;
		return decision;
	}

	// 5. Ritmo irregular: interpolar sobre frametime que oscila piora a percepção em vez de
	// melhorar, porque o quadro sintético entra em um instante que não corresponde a nada.
	if (inputs.frametime_avg_ms > 0.0f &&
		inputs.frametime_p99_ms > (inputs.frametime_avg_ms * policy.max_p99_ratio))
	{
		decision.state = State::Waiting;
		decision.reason = Reason::Unstable;
		return decision;
	}

	// 6. Orçamento estourado: a geração passou a roubar tempo da emulação. Suspende — estado
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
		case Reason::Cooldown:
			return "Cooldown";
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
		case Reason::Cooldown:
			return "Em carência após desengatar — evitando ligar e desligar sem parar.";
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
	policy.budget_hysteresis = ForkConfig::GetFloat(ForkConfig::Option::FrameGenBudgetHysteresis);
	policy.reengage_cooldown_frames =
		static_cast<u32>(std::max(0, ForkConfig::GetInt(ForkConfig::Option::FrameGenCooldownFrames)));
	// Limitado a 16 dobras porque 30 << 16 já passa do teto do contador: aceitar mais só criaria
	// uma configuração que promete algo que a régua não pode cumprir.
	policy.max_cooldown_doublings = static_cast<u32>(
		std::clamp(ForkConfig::GetInt(ForkConfig::Option::FrameGenMaxCooldownDoublings), 0, 16));
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

ForkFrameGen::Decision ForkFrameGen::Advance(const Policy& policy, Governor& governor, Inputs inputs)
{
	// As entradas que dependem do PASSADO são preenchidas aqui, e não por quem chama. [Decide] é
	// pura e precisa continuar sendo — mas isso empurrava a memória para o chamador, e foi ali que
	// todos os osciladores desta fase nasceram. Com a memória em um só lugar, um teste consegue
	// reproduzir uma cena inteira sem GPU e sem reimplementar a lógica que pretende verificar.
	inputs.previously_engaged = (governor.last.state == State::Engaged);
	inputs.previously_over_budget = (governor.last.reason == Reason::OverBudget);
	inputs.frames_since_disengage = governor.frames_since_disengage;

	// A carência cresce numa CÓPIA da política: a régua recebe um número de quadros e o compara;
	// quem decide que esse número deve dobrar é quem tem a memória dos fracassos.
	Policy effective_policy = policy;
	const u32 doublings = std::min(governor.failed_engagements, policy.max_cooldown_doublings);
	// Saturada no teto do contador: uma carência que ninguém consegue cumprir seria "FG desligado"
	// disfarçado de "aguardando", e o log passaria a mentir sobre o motivo.
	effective_policy.reengage_cooldown_frames = static_cast<u32>(std::min<u64>(
		static_cast<u64>(policy.reengage_cooldown_frames) << doublings, DISENGAGE_COUNTER_CEILING));

	Decision decision = Decide(effective_policy, inputs);

	// Contrapeso da carência crescente. A carência supõe que a cena continua a mesma, e essa
	// suposição vence enquanto ela vale — mas cenas acabam. Sem uma saída, o castigo acumulado
	// numa corrida pesada seria cobrado do menu seguinte, que não fez nada para merecê-lo.
	//
	// A prova de que a cena mudou não pode ser um quadro bom: numa cena que oscila, metade dos
	// quadros é boa. Tem que ser CALMA SUSTENTADA — uma carência base inteira de quadros seguidos
	// que teriam engatado. Isso é exatamente o que a oscilação nunca consegue produzir, e o que
	// uma cena estável produz em meio segundo.
	if (decision.reason == Reason::Cooldown)
	{
		Inputs counterfactual = inputs;
		counterfactual.frames_since_disengage = DISENGAGE_COUNTER_CEILING;
		const Decision would_be = Decide(effective_policy, counterfactual);
		if (would_be.state == State::Engaged)
			governor.frames_ready_in_cooldown++;
		else
			governor.frames_ready_in_cooldown = 0;

		if (governor.frames_ready_in_cooldown >= policy.reengage_cooldown_frames)
		{
			governor.failed_engagements = 0;
			governor.frames_ready_in_cooldown = 0;
			governor.frames_since_disengage = DISENGAGE_COUNTER_CEILING;
			decision = would_be;
		}
	}
	else
	{
		governor.frames_ready_in_cooldown = 0;
	}

	// A carência conta a partir da TRANSIÇÃO de engatado para não-engatado, não de qualquer quadro
	// parado: senão, com FG desligado, o contador ficaria preso em zero para sempre e o primeiro
	// engate legítimo da sessão nasceria em carência.
	const bool was_engaged = inputs.previously_engaged;
	const bool is_engaged = (decision.state == State::Engaged);
	if (was_engaged && !is_engaged)
	{
		governor.frames_since_disengage = 0;
		// O julgamento do engate que acabou de morrer, com a própria carência de régua: um engate
		// mais curto que ela não chegou a virar fluidez para o usuário — foi uma piscada, e a
		// resposta certa é tentar de novo mais TARDE, não mais cedo.
		if (governor.frames_engaged < policy.reengage_cooldown_frames)
		{
			if (governor.failed_engagements < policy.max_cooldown_doublings)
				governor.failed_engagements++;
		}
		else
		{
			// Durou. A cena mudou, e a régua volta a responder rápido — sem isto, sair de uma
			// corrida pesada deixaria o menu seguinte esperando dezenas de segundos por nada.
			governor.failed_engagements = 0;
		}
		governor.frames_engaged = 0;
	}
	else if (governor.frames_since_disengage < DISENGAGE_COUNTER_CEILING)
	{
		governor.frames_since_disengage++;
	}

	if (is_engaged)
		governor.frames_engaged++;

	governor.last = decision;
	return decision;
}

namespace
{
	std::mutex s_governor_mutex;
	ForkFrameGen::Governor s_governor;
} // namespace

ForkFrameGen::Decision ForkFrameGen::EvaluateAtPresent(bool supported, bool has_new_frame)
{
	const Policy policy = PolicyFromConfig();

	Inputs inputs;
	inputs.supported = supported;
	inputs.has_new_frame = has_new_frame;

	if (policy.mode != Mode::Off && supported)
	{
		// Caminho rápido ao contrário: desligado não paga nem a leitura das métricas. FG desligado
		// é o padrão, e o custo dele tem que ser indistinguível de não existir. `!supported` entra
		// no mesmo caminho porque, sem backend capaz de apresentar, nenhuma política muda o
		// resultado. O motivo continua distinguindo os dois casos, porque [Decide] os distingue:
		// "desligado" é escolha do usuário, "incompatível" é do aparelho.
		const GSPresentationMetrics::Snapshot snapshot = GSPresentationMetrics::GetSnapshot();
		inputs.real_fps = snapshot.real_fps;
		// Velocidade vem do PCSX2, não das nossas métricas: ele a calcula contra a taxa alvo da
		// MÁQUINA (59,94 / 50), que é o único denominador que separa "o jogo renderiza a 30" de
		// "o aparelho só dá conta de metade".
		inputs.speed_percent = PerformanceMetrics::GetSpeed();
		inputs.frametime_avg_ms = snapshot.real_frametime_avg_ms;
		inputs.frametime_p99_ms = snapshot.real_frametime_low1_ms;
		inputs.last_generation_ms = snapshot.generation_avg_ms;
	}

	std::lock_guard lock(s_governor_mutex);
	return Advance(policy, s_governor, inputs);
}

ForkFrameGen::Decision ForkFrameGen::GetLastDecision()
{
	std::lock_guard lock(s_governor_mutex);
	return s_governor.last;
}

void ForkFrameGen::ResetGovernor()
{
	std::lock_guard lock(s_governor_mutex);
	s_governor = Governor{};
}
