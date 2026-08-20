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
		/// Desengatou há pouco e está em carência. Existe porque ligar e desligar duas vezes por
		/// segundo é PIOR que ficar desligado: o usuário vê a fluidez pulsar sem entender por quê.
		Cooldown,
		Engaged,
	};

	struct Policy
	{
		Mode mode = Mode::Off;
		/// Quanto do INTERVALO REAL entre quadros a geração pode consumir.
		///
		/// O orçamento era um número absoluto de milissegundos, e isso estava errado por um motivo
		/// que só apareceu com três jogos medidos lado a lado: 8 ms dentro de um quadro de 60 Hz
		/// (16,7 ms) é metade do tempo disponível; os MESMOS 8 ms dentro de um quadro de 30 Hz
		/// (33,3 ms) são um quarto. Um teto absoluto trata os dois casos como iguais e acaba
		/// proibindo, no jogo de 30, exatamente o caso em que FG mais rende.
		///
		/// O aparelho mostrou a conta: NFS Underground 2 a 2.00x, 30 fps travados, geração custando
		/// ~6,5 ms quando aquecida — e recusada por 11 minutos seguidos contra um teto de 8 ms que
		/// o custo A FRIO (13 ms) estourava. God of War II, mesmo aparelho, mesmo upscale, mesmos
		/// 30 fps, custo 5,7 ms: engatou e ficou em `engaged=100% transitions=0`. A diferença não
		/// era o jogo, era qual dos dois conseguiu passar dos primeiros quadros.
		///
		/// 0,5 preserva o comportamento medido a 60 Hz (0,5 × 16,7 = 8,35 ms ≈ o teto antigo) e
		/// abre 16,7 ms a 30 Hz, que admite os 13 ms frios e continua recusando os 20 ms que o
		/// upscale de 3.00x custava.
		float budget_fraction = 0.5f;
		/// Teto absoluto, que só morde quando o intervalo real é muito longo.
		///
		/// A fração sozinha ficaria permissiva demais num jogo lento: a 15 fps ela autorizaria 33 ms
		/// de geração. Os degraus de velocidade e de FPS mínimo já barram esse caso antes, mas o
		/// teto existe para que a régua não dependa da ordem dos degraus para estar correta.
		float budget_ms = 20.0f;
		/// Histerese do orçamento, como FRAÇÃO do teto: uma vez suspenso por custo, só volta a
		/// engatar quando a média cair abaixo de `budget_ms * (1 - budget_hysteresis)`.
		///
		/// Sem isto o degrau oscila sozinho, e o aparelho mostrou o mecanismo: suspenso, nenhum
		/// custo novo é registrado, a média da janela de 1 s decai até passar no teto, engata,
		/// registra o custo alto de novo e suspende. Um oscilador com período de ~0,5 s — 20
		/// transições a cada 10 s no log.
		float budget_hysteresis = 0.25f;
		/// Quadros de carência depois de desengatar, antes de poder engatar de novo.
		///
		/// Substitui o remendo por degrau. Histerese foi acrescentada três vezes — velocidade,
		/// orçamento e faltava a estabilidade — sempre depois de o aparelho mostrar o mesmo
		/// padrão: um degrau oscilando em torno do próprio limiar. A carência age DEPOIS de
		/// qualquer degrau e resolve todos de uma vez, inclusive os que ainda não existem.
		///
		/// Só atrasa o ENGATE, nunca segura a geração ligada — então não há risco de roubar tempo
		/// da emulação para cumprir uma carência.
		u32 reengage_cooldown_frames = 30;
		/// Quantas vezes a carência pode DOBRAR quando o engate seguinte também fracassa.
		///
		/// Carência fixa não resolve sozinha, e o teste com a corrida medida no aparelho mostrou
		/// isso sem margem: com 30 quadros fixos, as 18 transições em 10 s viraram 8 — o pulso
		/// ficou mais espaçado, não sumiu. A causa é que, numa cena que simplesmente não cabe no
		/// orçamento, cada engate é uma tentativa condenada, e uma carência constante garante um
		/// número constante de tentativas por segundo.
		///
		/// Dobrar a cada fracasso transforma isso: a régua tenta, tenta de novo mais tarde, e vai
		/// desistindo sozinha até parar de piscar. Cinco dobras levam 30 quadros a 960 — cerca de
		/// 16 s a 60 Hz — que é o suficiente para a cena pesada passar sem mais nenhuma piscada.
		/// Um engate que DURA zera o contador, então sair da cena pesada devolve a resposta rápida.
		u32 max_cooldown_doublings = 5;
		/// Abaixo de quantos quadros um engate conta como PISCADA em vez de tentativa.
		///
		/// A distinção decide se a "calma sustentada" pode soltar a carência. Uma piscada é curta
		/// demais para que a régua tenha visto a cena com a geração ligada: o que ela mede depois,
		/// já desengatada, é a cena SEM geração — e essa cena está calma justamente porque a
		/// geração não está lá.
		u32 blink_frames = 5;
		/// Quanto o intervalo real precisa mudar para contar como cena diferente.
		///
		/// É o teste que a oscilação não consegue falsificar. Um jogo travado em 30 fps que pisca
		/// a cada segundo mede 33,4 ms antes e depois — nada mudou. Sair da corrida para um menu
		/// de 60 mede 33,4 e depois 16,7: mudou, e a régua volta a responder na hora.
		float regime_change_fraction = 0.15f;
		/// Velocidade mínima da emulação, em porcentagem da taxa alvo da máquina. Abaixo disto FG
		/// não engata: o projeto define que suavizar uma emulação lenta é mascarar, não melhorar.
		float min_speed_percent = 90.0f;
		/// Piso absoluto de FPS real, contra o custo de LATÊNCIA da interpolação — não contra
		/// lentidão, que é papel de [min_speed_percent].
		float min_real_fps = 15.0f;
		/// Histerese do degrau de velocidade, em pontos percentuais. Uma vez ENGATADO, FG só
		/// desengata abaixo de `min_speed_percent - speed_hysteresis_percent`.
		///
		/// Não é enfeite: medido no aparelho, com um limiar único de 90%, o modo `auto` trocou de
		/// estado **20 vezes em 10 segundos** durante uma cena pesada — a velocidade oscilava em
		/// torno do limiar e cada oscilação ligava e desligava a geração. Piscar é pior que ficar
		/// desligado: o usuário vê a fluidez aparecer e sumir sem entender por quê, e nenhuma
		/// medição feita nesse estado significa alguma coisa.
		float speed_hysteresis_percent = 5.0f;
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
		/// O quadro anterior estava engatado? Entra por parâmetro para que [Decide] continue PURA
		/// — a histerese precisa de memória, e a memória mora em quem chama.
		bool previously_engaged = false;
		/// O quadro anterior estava SUSPENSO por custo? Governa a histerese do orçamento, que é
		/// separada da de velocidade porque os dois degraus oscilam por motivos diferentes.
		bool previously_over_budget = false;
		/// Quadros desde a última vez que a régua desengatou. Grande quando faz tempo.
		u32 frames_since_disengage = 1000;
		float frametime_avg_ms = 0.0f;
		float frametime_p99_ms = 0.0f;
		/// Custo de regime da geração — a média das gerações AQUECIDAS, não de todas.
		///
		/// Zero quando ainda não houve nenhuma aquecida, e isso quer dizer "sem evidência", não
		/// "de graça". Sem evidência o orçamento não barra: a alternativa, medida no aparelho, é
		/// julgar o recurso pelo custo do próprio aquecimento e nunca deixá-lo aquecer.
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

	/// Teto do contador de quadros desde o desengate. Só existe para o número não crescer sem
	/// limite numa sessão longa; fica CONFORTAVELMENTE acima da maior carência possível (30 << 5
	/// = 960 no padrão, e a carência base é configurável), senão o contador saturaria abaixo do
	/// limiar e a régua nunca mais engataria.
	inline constexpr u32 DISENGAGE_COUNTER_CEILING = 1u << 20;

	/// A memória entre quadros que a régua precisa e [Decide] não pode ter.
	///
	/// Ela mora aqui, exposta, e não escondida em variáveis estáticas do .cpp, por um motivo
	/// prático: todo defeito desta fase — o oscilador de velocidade, o de orçamento, o pulso da
	/// corrida — apareceu num teste sem GPU, e nenhum deles estava em [Decide]. Estavam na
	/// memória. Uma memória que não dá para instanciar num teste é uma memória que só o aparelho
	/// do usuário depura.
	struct Governor
	{
		Decision last;
		u32 frames_since_disengage = DISENGAGE_COUNTER_CEILING;
		/// Há quantos quadros seguidos a régua está engatada. Serve para julgar se o engate VALEU:
		/// um engate mais curto que a própria carência não virou fluidez, foi uma piscada.
		u32 frames_engaged = 0;
		/// Quantos engates seguidos fracassaram por esse critério. É o expoente da carência.
		u32 failed_engagements = 0;
		/// Quantos quadros durou o último engate. Distingue tentativa de piscada.
		u32 last_engagement_frames = 0;
		/// Intervalo real medido no instante do último desengate, para comparar regimes.
		float frametime_at_disengage = 0.0f;
		/// Há quantos quadros seguidos, DENTRO da carência, o quadro teria engatado se a carência
		/// não existisse. É o contrapeso da carência crescente: sem ele, uma corrida pesada que
		/// dobrou a espera cinco vezes deixaria o menu seguinte esperando 16 s sem motivo.
		u32 frames_ready_in_cooldown = 0;
	};

	/// Decide o quadro e avança a memória: preenche as entradas que dependem do passado, aplica a
	/// carência crescente e registra o resultado. É o ponto onde a política vira comportamento.
	Decision Advance(const Policy& policy, Governor& governor, Inputs inputs);

	Mode ParseMode(std::string_view value);
	/// Nome curto e estável do motivo, para log e parsing. Diferente de [ReasonText], que é a
	/// frase para o usuário — um log com frases inteiras é caro de filtrar e fácil de quebrar.
	const char* ReasonToString(Reason reason);
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

	/// Esquece a memória entre quadros. Chamado quando um renderer sobe — ou seja, uma vez por
	/// jogo. Sem isto, uma corrida pesada que terminou com a carência dobrada cinco vezes entrega
	/// esse castigo ao PRÓXIMO título, que não fez nada para merecê-lo.
	void ResetGovernor();
} // namespace ForkFrameGen
