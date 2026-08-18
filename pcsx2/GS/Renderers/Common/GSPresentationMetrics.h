// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "common/Timer.h"

#include <string>
#include <vector>

/// Medição da CAMADA DE APRESENTAÇÃO — quantos quadros saem para a tela, com que cadência, e
/// quanto custa entregá-los. Deliberadamente separado de PerformanceMetrics, que mede a
/// VELOCIDADE DA EMULAÇÃO (FPS interno, VPS, uso de thread).
///
/// A distinção é a razão de existir deste módulo. Frame generation muda quantos quadros chegam ao
/// display e não muda a velocidade do PS2; um único número de "FPS" não consegue dizer as duas
/// coisas, e misturá-las é exatamente como se mascara emulação lenta. Aqui:
///
///   FPS real      = quadros com conteúdo NOVO do jogo que foram apresentados
///   FPS aparente  = tudo que chegou ao display (real + repetido + gerado)
///
/// Os dois são contados separadamente e nunca somados no mesmo campo.
///
/// Thread-safety: as chamadas de registro vêm da thread de apresentação (MTGS) e os leitores
/// podem ser outra thread (log, frontend), então o estado é protegido por um mutex. Ele só é
/// tomado quando a medição está LIGADA — desligada, cada Note* custa uma carga atômica relaxed e
/// um branch, e nada mais.
namespace GSPresentationMetrics
{
	/// O que o quadro apresentado carregava. `Generated` ainda não é produzido por ninguém: existe
	/// desde já para que a interpolação (fases 7-8) só precise chamar, sem redesenhar a métrica —
	/// e para que a separação real/aparente esteja pronta antes do primeiro quadro sintético.
	enum class FrameKind : u8
	{
		Real, ///< conteúdo novo do GS
		Duplicate, ///< reapresentação do quadro anterior (o jogo não produziu nada novo)
		Generated, ///< sintetizado pela interpolação
	};

	/// Fotografia da janela deslizante de 1 s. Todos os campos são zero antes da primeira janela.
	struct Snapshot
	{
		float real_fps; ///< só quadros Real
		float presented_fps; ///< Real + Duplicate + Generated
		float real_frametime_avg_ms;
		float real_frametime_min_ms;
		float real_frametime_max_ms;
		/// "1% low" no sentido usual: média do 1% PIOR dos intervalos entre quadros reais, em ms e
		/// convertida em FPS. Com poucas amostras vira o pior intervalo da janela.
		float real_frametime_low1_ms;
		float real_low1_fps;
		/// Intervalos acima de 2x a mediana da janela — o que se sente como engasgo, e o que uma
		/// média esconde.
		u32 stutter_count;
		u64 real_frames; ///< na janela
		u64 duplicated_frames;
		u64 generated_frames;
		u64 skipped_presents; ///< present pulado inteiro (frame skip / throttle)
		float present_call_avg_ms; ///< custo da chamada de present ao WSI
		float present_call_max_ms;
		float generation_avg_ms; ///< custo da interpolação, quando houver
		u64 present_errors;
	};

	/// Desligado por padrão. Ligar não realoca nada: os buffers são fixos e alocados uma vez.
	void SetEnabled(bool enabled);
	bool IsEnabled();

	/// Zera janela e contadores. Chamado ao (re)criar swapchain: cadência medida através de uma
	/// recriação é ruído, não dado.
	void Reset();

	// --- registro (caminho quente) ---

	/// Um quadro chegou ao display.
	void NotePresented(FrameKind kind);
	/// O present foi pulado inteiro — nada chegou ao display neste ciclo.
	void NoteSkippedPresent();
	/// Duração da chamada de present ao WSI, e se ela foi aceita.
	void NotePresentCall(double ms, bool ok);
	/// Custo de GPU/CPU para produzir os quadros interpolados deste ciclo (fases 7-8).
	void NoteGenerationCost(double ms);

	// --- leitura ---

	Snapshot GetSnapshot();

	/// Uma linha para o overlay. Real e aparente aparecem SEMPRE juntos e rotulados, mesmo quando
	/// iguais: ler "60" sozinho não diz se a emulação está em velocidade.
	std::string GetOverlayLine();

	/// Linhas detalhadas para log e relatório de compatibilidade (Fase 7 do plano).
	void AppendStatLines(std::vector<std::string>& out);

	namespace Detail
	{
		/// Substitui a fonte de tempo. Existe para o teste: toda a lógica que importa aqui é
		/// sobre JANELA e PERCENTIL, e verificá-la com o relógio de parede exigiria dormir de
		/// verdade — o que produz teste lento e intermitente, o pior par possível para um número
		/// que vai justificar decisões de desempenho. Passar nullptr volta ao relógio real.
		using ClockFn = Common::Timer::Value (*)();
		void SetClockForTesting(ClockFn fn);
	} // namespace Detail
} // namespace GSPresentationMetrics
