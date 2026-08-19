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
		/// Dimensionado pelo ENUM, não por um literal. Já quebrou uma vez: `Reason` cresceu com
		/// `Cooldown`, o array ficou com um lugar a menos, e `Engaged` — o último valor — caía fora
		/// do limite e era descartado em silêncio pela guarda de índice. O bloco passou a reportar
		/// `engaged=0.0%` num quadro perfeitamente engatado.
		static constexpr size_t REASON_COUNT = static_cast<size_t>(ForkFrameGen::Reason::Engaged) + 1;
		std::array<u32, REASON_COUNT> frames_by_reason{};
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

	/// Onde o tempo está indo. Responde a PRIMEIRA pergunta de qualquer relato de lentidão —
	/// "é CPU ou GPU?" — que o bloco anterior não respondia de jeito nenhum. Sem isso, "está
	/// lento" gera um palpite; com isso, gera um alvo.
	struct Load
	{
		float speed_percent = 0.0f;
		float vps = 0.0f;
		float internal_fps = 0.0f;
		bool internal_fps_valid = false;
		double cpu_thread_usage = 0.0;
		float gs_thread_usage = 0.0f;
		float gs_back_thread_usage = 0.0f;
		bool has_gs_back_thread = false;
		float vu_thread_usage = 0.0f;
		float gpu_usage = 0.0f;
		/// Shaders compilados NESTE intervalo. Um pico de frametime explicado por compilação não é
		/// o mesmo problema que um pico explicado por carga, e sem este número os dois são
		/// indistinguíveis no log.
		u32 shader_compiles = 0;
	};

	std::string FormatLoadLine(const Load& load);

	/// Configurações que INVALIDAM uma medição, não que a deixam "insegura" — é uma lista mais
	/// estreita e mais útil que o aviso de configuração insegura do PCSX2.
	///
	/// Existe porque custou uma rodada inteira: a primeira sessão medida no aparelho estava com o
	/// despejo de texturas ligado, gravando 246 arquivos em disco no meio do caminho de render, e
	/// isso só apareceu porque alguém foi procurar. Um número medido assim não vale nada, e o log
	/// tem de dizer isso sozinho.
	/// Lida do config pelo chamador e passada aqui, em vez de a função ler o estado global: é o
	/// mesmo motivo das outras — uma função pura se exercita sem VM, sem GPU e sem config viva.
	struct Hygiene
	{
		bool dumping_textures = false;
		bool loading_texture_pack = false;
		bool ee_cycle_rate_changed = false;
		bool ee_cycle_skip_changed = false;
		float upscale_multiplier = 1.0f;
		int blending_level = 0;
	};

	std::string FormatHygieneLine(const Hygiene& hygiene);

	/// Cabeçalho de identidade, uma vez por sessão: GPU, driver pedido, driver ATIVO e hash do
	/// pacote. Sem isso duas medições não são comparáveis — e é a linha que revela um fallback
	/// silencioso antes de qualquer número de desempenho ser levado a sério.
	std::string FormatIdentityLine();

	/// Chamado a cada quadro apresentado. Barato quando desligado e quando o intervalo não fechou.
	///
	/// A higiene entra por parâmetro em vez de ser lida aqui: manter este módulo sem dependência
	/// do config global é o que permite exercitá-lo inteiro sem VM. Custa meia dúzia de leituras
	/// de campo no chamador, que já vive nesse mundo de qualquer forma.
	void NotePresent(const ForkFrameGen::Decision& decision, const Hygiene& hygiene);

	/// Zera o acumulado (troca de jogo, recriação de swapchain).
	void Reset();

	namespace Detail
	{
		/// Só para teste: substitui o relógio.
		void SetClockForTesting(u64 (*clock)());
	}
} // namespace ForkDiagnostics
