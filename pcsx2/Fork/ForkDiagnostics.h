// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Fork/ForkFrameGen.h"
#include "GS/Renderers/Common/GSPresentationMetrics.h"

#include "common/Pcsx2Defs.h"

#include <array>
#include <string>
#include <vector>

/// Diagnóstico de sessão: escreve no LOG o que até agora só existia no overlay.
///
/// ## Por que isto existe
///
/// A Fase 2 mediu a apresentação e a Fase 7 registrou a decisão de frame generation — e as duas
/// mostram o resultado na tela. Mas o teste que importa acontece no aparelho do usuário, e o que
/// volta de lá é um log. Pedir "mande o log" para responder "a geração cabe no orçamento?" era
/// pedir um dado que o binário nunca escreveu: as métricas não passavam por `Console`, e o
/// benchmark da Fase 6 não tem UI que o acione. O log tinha tudo, menos o que se queria saber.
///
/// ## O que ele registra, e por que assim
///
/// Um bloco a cada N segundos, com prefixo fixo `@@FORK@@` para que um `grep` simples o encontre
/// (o `Console` do PCSX2 sai por stdout e o Android o redireciona para o logcat sob a tag
/// `STDOUT`, então filtrar por tag não funciona — o que funciona é o texto).
///
/// **FPS real e FPS apresentado saem em LINHAS SEPARADAS.** É a mesma regra do overlay, e ela vale
/// ainda mais aqui: um log é lido fora de contexto, meses depois, por alguém que não participou
/// desta conversa. Duas colunas vizinhas viram um número só na memória de quem lê.
///
/// Transições de estado são CONTADAS, não despejadas. "Auto engata e desengata sem parar" é a
/// pergunta real, e registrar cada mudança produziria centenas de linhas por segundo justamente
/// no caso patológico — afogando o log no momento em que ele precisa ser legível. Uma contagem
/// por intervalo responde a pergunta: 1 transição é estável, 40 é piscando.
namespace ForkDiagnostics
{
	/// Acumula o que aconteceu entre dois blocos. Puro e sem estado global, para ser exercitado
	/// sem GPU, sem VM e sem relógio de verdade.
	struct Accumulator
	{
		/// Quadros observados em cada motivo, na ordem do enum `ForkFrameGen::Reason`.
		std::array<u32, 7> frames_by_reason{};
		/// Quantas vezes o ESTADO mudou. É o número que responde "está piscando?".
		u32 transitions = 0;
		u32 frames = 0;
		/// Pior custo de geração visto no intervalo. A média esconde o pico, e é o pico que estoura
		/// o orçamento e suspende a geração.
		float worst_generation_ms = 0.0f;
		bool has_previous = false;
		ForkFrameGen::State previous_state = ForkFrameGen::State::Disabled;

		void Note(const ForkFrameGen::Decision& decision, float generation_ms);
		void Reset();
		/// Motivo que mais apareceu no intervalo, para a linha caber em uma frase.
		ForkFrameGen::Reason DominantReason() const;
	};

	/// As três linhas do bloco, puras. Recebem tudo de que precisam para que o teste não dependa do
	/// estado global das métricas.
	std::string FormatRealLine(const GSPresentationMetrics::Snapshot& snapshot);
	std::string FormatPresentedLine(const GSPresentationMetrics::Snapshot& snapshot);
	std::string FormatFrameGenLine(
		const Accumulator& accumulator, const ForkFrameGen::Policy& policy, float generation_avg_ms);

	/// Cabeçalho de identidade, uma vez por sessão: GPU, driver pedido, driver ATIVO e hash do
	/// pacote. Sem isso duas medições não são comparáveis — e é a linha que revela um fallback
	/// silencioso antes de qualquer número de desempenho ser levado a sério.
	std::string FormatIdentityLine();

	/// Chamado a cada quadro apresentado. Barato quando desligado e quando o intervalo não fechou.
	void NotePresent(const ForkFrameGen::Decision& decision);

	/// Zera o acumulado (troca de jogo, recriação de swapchain).
	void Reset();

	namespace Detail
	{
		/// Só para teste: substitui o relógio.
		void SetClockForTesting(u64 (*clock)());
	}
} // namespace ForkDiagnostics
