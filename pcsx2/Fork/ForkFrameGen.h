// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>
#include <string_view>

/// Frame generation — **política e segurança**. Esta fase não sintetiza um pixel sequer.
///
/// O contrato é o que precisa ser provado primeiro: FG ligado **não pode** alterar a velocidade
/// lógica do PS2, nem áudio, nem input, nem os timers. Um backend que interpola de verdade
/// esconderia qualquer erro aqui — se o FPS real caísse por causa do custo da interpolação, o
/// número apresentado subiria junto e daria a impressão de sucesso. Por isso o backend desta fase
/// **duplica o quadro**: mede-se o caminho inteiro sem nenhuma imagem inventada.
///
/// Referência de projeto: o FG do GameHub e do LSFG-Android são a mesma tecnologia (Lossless
/// Scaling em Vulkan), e a consistência que se atribui a eles vem, em boa parte, dos controles de
/// PACING — bypass, alinhamento com vsync, presets de ritmo, teto de FPS, profundidade de fila e
/// suavização de jitter — e não do algoritmo de interpolação. Daí esta fase existir antes da 8:
/// ritmo primeiro, pixels depois.
namespace ForkFrameGen
{
	/// Aviso obrigatório na UI. Escrito aqui para que a UI não possa "esquecer" de mostrá-lo nem
	/// reescrevê-lo de forma mais otimista.
	inline constexpr const char* USER_WARNING =
		"Frame Generation melhora a fluidez percebida, mas NÃO aumenta a velocidade da emulação.";

	enum class Mode : u8
	{
		Off,
		/// Engata sozinho quando o ritmo está estável e desengata quando não está.
		Auto,
		/// Sempre 2x quando as condições de segurança permitirem. As condições continuam valendo:
		/// "2x" é a intenção do usuário, não uma ordem para ignorar a régua.
		X2,
	};

	enum class State : u8
	{
		Disabled,
		/// Ligado, mas ainda não engatado — condições não atendidas.
		Waiting,
		Engaged,
		/// Estava engatado e se desengatou sozinho por segurança.
		Suspended,
	};

	/// Por que o estado é esse. Existe para a UI e o log dizerem o motivo, em vez de o usuário
	/// concluir que "FG não funciona neste aparelho".
	enum class Reason : u8
	{
		Off,
		/// GPU/renderer incompatível.
		Unsupported,
		/// Nenhum quadro novo do jogo — nada a partir do que gerar. É também o mecanismo que
		/// impede FG de inventar suavidade quando a emulação travou.
		NoNewFrame,
		/// Ritmo instável: gerar em cima de frametime irregular piora a percepção em vez de
		/// melhorar.
		Unstable,
		/// A emulação não está rodando na velocidade correta. **Este é o degrau principal**, e o
		/// que a regra do projeto realmente quer dizer: 22 FPS reais mostrando 44 não é sucesso.
		///
		/// Medido como VELOCIDADE (`fps / taxa alvo da máquina`), não como FPS absoluto. A
		/// diferença não é acadêmica: um jogo de PS2 que renderiza a 30 rodando perfeitamente dá
		/// 30 FPS reais, e um piso em FPS o confundiria com um jogo de 60 rodando pela metade —
		/// exatamente ao contrário. Velocidade separa "o jogo é assim" de "o aparelho não dá conta".
		BelowFullSpeed,
		/// FPS real absoluto baixo demais. Guarda secundária, e por outro motivo: interpolar
		/// segura o quadro novo até gerar o do meio, então quanto menor a taxa, maior o atraso em
		/// MILISSEGUNDOS que isso custa. A 20 FPS um quadro é 50 ms de input lag.
		BelowMinimumRealFps,
		/// A geração não coube no orçamento de tempo.
		OverBudget,
		Engaged,
	};

	struct Policy
	{
		Mode mode = Mode::Off;
		/// Teto de tempo para produzir o quadro gerado. Estourar significa roubar tempo da
		/// emulação, que é exatamente o que FG não pode fazer.
		float budget_ms = 6.0f;
		/// Velocidade mínima da emulação, em porcentagem da taxa alvo da máquina. Abaixo disto FG
		/// não engata: o projeto define que suavizar uma emulação lenta é mascarar, não melhorar.
		float min_speed_percent = 90.0f;
		/// Piso absoluto de FPS real, contra o custo de LATÊNCIA da interpolação — não contra
		/// lentidão, que é papel de [min_speed_percent].
		float min_real_fps = 15.0f;
		/// Quão irregular o frametime pode ser e ainda contar como estável, medido como razão
		/// entre p99 e a média.
		float max_p99_ratio = 1.5f;
	};

	struct Inputs
	{
		bool supported = false;
		/// O quadro que acabou de ser submetido carrega conteúdo novo do jogo?
		bool has_new_frame = false;
		float real_fps = 0.0f;
		/// Velocidade da emulação em %, como o PCSX2 a calcula (`fps / taxa alvo`). 100 = correta.
		float speed_percent = 100.0f;
		float frametime_avg_ms = 0.0f;
		float frametime_p99_ms = 0.0f;
		/// Custo da última geração, 0 quando ainda não houve.
		float last_generation_ms = 0.0f;
	};

	struct Decision
	{
		State state = State::Disabled;
		Reason reason = Reason::Off;
		/// Quantos quadros sintéticos apresentar antes do real. Zero em qualquer estado que não
		/// seja Engaged.
		u32 frames_to_generate = 0;
	};

	/// A régua inteira, pura e sem estado, para poder ser exercitada exaustivamente sem GPU.
	Decision Decide(const Policy& policy, const Inputs& inputs);

	Mode ParseMode(std::string_view value);
	const char* ModeToString(Mode mode);
	const char* StateToString(State state);
	/// Frase para a UI e para o overlay.
	const char* ReasonText(Reason reason);

	/// Lê a política da configuração do fork (com override por jogo, via camadas).
	Policy PolicyFromConfig();

	/// Linha do overlay: estado, motivo e o que está sendo apresentado. Vazia quando desligado.
	std::string StatusLine(const Decision& decision);

	/// Avalia no ponto de apresentação, montando as entradas a partir das métricas ao vivo, e
	/// guarda a decisão para a UI e o log.
	///
	/// **Esta fase não apresenta quadro nenhum a mais.** Nem sintetizado, nem duplicado. A decisão
	/// é calculada, registrada e exibida — e nada mais. Apresentar um quadro extra é mudança real
	/// no caminho de present (aquisição de imagem, semáforos, ritmo) e pertence ao backend da Fase
	/// 8, junto com os pixels. Contar como "gerado" um quadro que não foi apresentado corromperia
	/// justamente a métrica que a Fase 2 construiu para não deixar ninguém se enganar.
	Decision EvaluateAtPresent(bool supported, bool has_new_frame);
	Decision GetLastDecision();
} // namespace ForkFrameGen
