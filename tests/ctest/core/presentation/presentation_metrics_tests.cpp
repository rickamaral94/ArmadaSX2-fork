// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Common/GSPresentationMetrics.h"

#include "common/Timer.h"

#include <gtest/gtest.h>

using GSPresentationMetrics::FrameKind;
using GSPresentationMetrics::Snapshot;

namespace
{
	/// Relógio falso. Começa longe de zero porque a expiração calcula `now - janela` e um instante
	/// inicial pequeno esconderia um eventual underflow em vez de expô-lo.
	Common::Timer::Value s_now = Common::Timer::ConvertSecondsToValue(1000.0);

	Common::Timer::Value FakeClock()
	{
		return s_now;
	}

	void AdvanceMs(double ms)
	{
		s_now += Common::Timer::ConvertMillisecondsToValue(ms);
	}

	class PresentationMetricsTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			s_now = Common::Timer::ConvertSecondsToValue(1000.0);
			GSPresentationMetrics::Detail::SetClockForTesting(&FakeClock);
			GSPresentationMetrics::SetEnabled(true);
			GSPresentationMetrics::Reset();
		}

		void TearDown() override
		{
			GSPresentationMetrics::SetEnabled(false);
			GSPresentationMetrics::Detail::SetClockForTesting(nullptr);
		}

		/// `count` quadros reais espaçados de `interval_ms`, o primeiro no instante atual.
		void PresentRealFrames(int count, double interval_ms)
		{
			for (int i = 0; i < count; i++)
			{
				if (i > 0)
					AdvanceMs(interval_ms);
				GSPresentationMetrics::NotePresented(FrameKind::Real);
			}
		}
	};
} // namespace

// O critério de aceite da fase: desligado, o módulo não registra nada. Se esta falhar, medir passa
// a custar no caminho de apresentação de todo mundo, inclusive de quem nunca ligou a métrica.
TEST_F(PresentationMetricsTest, DisabledRecordsNothing)
{
	GSPresentationMetrics::SetEnabled(false);

	for (int i = 0; i < 100; i++)
	{
		GSPresentationMetrics::NotePresented(FrameKind::Real);
		GSPresentationMetrics::NotePresentCall(1.0, true);
		GSPresentationMetrics::NoteSkippedPresent();
		AdvanceMs(16.0);
	}

	const Snapshot snap = GSPresentationMetrics::GetSnapshot();
	EXPECT_EQ(snap.real_frames, 0u);
	EXPECT_EQ(snap.skipped_presents, 0u);
	EXPECT_FLOAT_EQ(snap.real_fps, 0.0f);
	EXPECT_FLOAT_EQ(snap.presented_fps, 0.0f);
	EXPECT_TRUE(GSPresentationMetrics::GetOverlayLine().empty());
}

// O ponto central do módulo: 30 quadros reais com um gerado entre cada par apresentam 60, e o
// número real continua 30. Se estes dois campos algum dia coincidirem por construção, frame
// generation vira uma forma de esconder emulação lenta — que é o que o projeto proíbe.
TEST_F(PresentationMetricsTest, RealAndPresentedNeverConflated)
{
	// Sem avançar depois do último quadro: 60 apresentações espaçadas de 16,667 ms ocupam 983 ms e
	// cabem inteiras na janela. Avançar ao final empurraria a primeira para fora e o teste mediria
	// a borda da janela em vez do que ele quer medir.
	for (int i = 0; i < 30; i++)
	{
		if (i > 0)
			AdvanceMs(16.667);
		GSPresentationMetrics::NotePresented(FrameKind::Real);
		AdvanceMs(16.667);
		GSPresentationMetrics::NotePresented(FrameKind::Generated);
	}

	const Snapshot snap = GSPresentationMetrics::GetSnapshot();
	EXPECT_EQ(snap.real_frames, 30u);
	EXPECT_EQ(snap.generated_frames, 30u);
	EXPECT_EQ(snap.duplicated_frames, 0u);
	EXPECT_NEAR(snap.real_fps, 30.0f, 0.001f);
	EXPECT_NEAR(snap.presented_fps, 60.0f, 0.001f);
}

// O invariante da Fase 8, medido dos dois lados: intercalar quadros GERADOS não pode mexer em
// nenhum número do lado real. Mesma linha do tempo real, uma vez sozinha e uma vez com um quadro
// gerado entre cada par — FPS real, média, mínimo e máximo de frametime têm de sair idênticos.
//
// É o que separa "apresentar mais quadros" de "emular mais rápido". Se essa igualdade quebrar, o
// número que o projeto usa para julgar desempenho passou a ser contaminado pelo recurso que ele
// deveria julgar.
TEST_F(PresentationMetricsTest, GeneratedFramesDoNotMoveASingleRealNumber)
{
	const auto RunRealTimeline = [](bool interleave_generated) {
		GSPresentationMetrics::Reset();
		s_now = Common::Timer::ConvertSecondsToValue(1000.0);
		for (int i = 0; i < 20; i++)
		{
			if (i > 0)
				AdvanceMs(20.0);
			GSPresentationMetrics::NotePresented(FrameKind::Real);
			// O quadro gerado entra no MESMO instante, de propósito: assim a linha do tempo dos
			// quadros reais é bit a bit a mesma nas duas execuções, e qualquer diferença nos
			// números reais só pode ter vindo do quadro gerado. (Andar com o relógio para trás
			// seria mais parecido com a realidade e menos com um teste: Timer::Value é sem sinal.)
			if (interleave_generated)
				GSPresentationMetrics::NotePresented(FrameKind::Generated);
		}
		return GSPresentationMetrics::GetSnapshot();
	};

	const Snapshot without = RunRealTimeline(false);
	const Snapshot with = RunRealTimeline(true);

	EXPECT_EQ(with.real_frames, without.real_frames);
	EXPECT_FLOAT_EQ(with.real_fps, without.real_fps);
	EXPECT_FLOAT_EQ(with.real_frametime_avg_ms, without.real_frametime_avg_ms);
	EXPECT_FLOAT_EQ(with.real_frametime_min_ms, without.real_frametime_min_ms);
	EXPECT_FLOAT_EQ(with.real_frametime_max_ms, without.real_frametime_max_ms);
	EXPECT_FLOAT_EQ(with.real_frametime_low1_ms, without.real_frametime_low1_ms);

	// E o lado apresentado, esse sim, tem de ter subido — senão o teste acima passaria com um
	// backend que não gera nada.
	EXPECT_EQ(with.generated_frames, 20u);
	EXPECT_GT(with.presented_fps, without.presented_fps);
}

// Quadro repetido é apresentação sem conteúdo novo: conta como apresentado, nunca como real.
TEST_F(PresentationMetricsTest, DuplicatesCountAsPresentedNotReal)
{
	for (int i = 0; i < 10; i++)
	{
		if (i > 0)
			AdvanceMs(33.333);
		GSPresentationMetrics::NotePresented(FrameKind::Real);
		AdvanceMs(33.333);
		GSPresentationMetrics::NotePresented(FrameKind::Duplicate);
	}

	const Snapshot snap = GSPresentationMetrics::GetSnapshot();
	EXPECT_EQ(snap.real_frames, 10u);
	EXPECT_EQ(snap.duplicated_frames, 10u);
	EXPECT_NEAR(snap.real_fps, 10.0f, 0.001f);
	EXPECT_NEAR(snap.presented_fps, 20.0f, 0.001f);
}

TEST_F(PresentationMetricsTest, FrametimeAverageMinMax)
{
	GSPresentationMetrics::NotePresented(FrameKind::Real);
	AdvanceMs(10.0);
	GSPresentationMetrics::NotePresented(FrameKind::Real);
	AdvanceMs(20.0);
	GSPresentationMetrics::NotePresented(FrameKind::Real);
	AdvanceMs(30.0);
	GSPresentationMetrics::NotePresented(FrameKind::Real);

	const Snapshot snap = GSPresentationMetrics::GetSnapshot();
	EXPECT_NEAR(snap.real_frametime_avg_ms, 20.0f, 0.01f);
	EXPECT_NEAR(snap.real_frametime_min_ms, 10.0f, 0.01f);
	EXPECT_NEAR(snap.real_frametime_max_ms, 30.0f, 0.01f);
}

// 1% low e engasgo existem para que uma média não esconda o pico. Um único intervalo de 100 ms no
// meio de 10 ms tem que aparecer nos dois.
TEST_F(PresentationMetricsTest, Low1AndStutterExposeTheSpike)
{
	PresentRealFrames(20, 10.0);
	AdvanceMs(100.0);
	GSPresentationMetrics::NotePresented(FrameKind::Real);
	PresentRealFrames(20, 10.0);

	const Snapshot snap = GSPresentationMetrics::GetSnapshot();
	EXPECT_NEAR(snap.real_frametime_low1_ms, 100.0f, 0.01f);
	EXPECT_NEAR(snap.real_low1_fps, 10.0f, 0.01f);
	EXPECT_EQ(snap.stutter_count, 1u);
	// A média continua próxima de 10 ms — que é exatamente o motivo de ela não bastar.
	EXPECT_LT(snap.real_frametime_avg_ms, 15.0f);
}

TEST_F(PresentationMetricsTest, WindowExpiresOldSamples)
{
	PresentRealFrames(10, 10.0);
	EXPECT_EQ(GSPresentationMetrics::GetSnapshot().real_frames, 10u);

	AdvanceMs(1500.0);
	GSPresentationMetrics::NotePresented(FrameKind::Real);

	const Snapshot snap = GSPresentationMetrics::GetSnapshot();
	EXPECT_EQ(snap.real_frames, 1u);
	EXPECT_NEAR(snap.real_fps, 1.0f, 0.001f);
}

TEST_F(PresentationMetricsTest, SkippedPresentsAndErrorsAreCounted)
{
	for (int i = 0; i < 5; i++)
	{
		GSPresentationMetrics::NoteSkippedPresent();
		AdvanceMs(16.0);
	}
	GSPresentationMetrics::NotePresentCall(0.5, true);
	GSPresentationMetrics::NotePresentCall(2.5, false);

	const Snapshot snap = GSPresentationMetrics::GetSnapshot();
	EXPECT_EQ(snap.skipped_presents, 5u);
	EXPECT_EQ(snap.present_errors, 1u);
	EXPECT_NEAR(snap.present_call_avg_ms, 1.5f, 0.01f);
	EXPECT_NEAR(snap.present_call_max_ms, 2.5f, 0.01f);
}

TEST_F(PresentationMetricsTest, GenerationCostIsAveraged)
{
	GSPresentationMetrics::NoteGenerationCost(3.0);
	AdvanceMs(16.0);
	GSPresentationMetrics::NoteGenerationCost(5.0);

	EXPECT_NEAR(GSPresentationMetrics::GetSnapshot().generation_avg_ms, 4.0f, 0.01f);
}

// O overlay tem que nomear os dois números. Um "60" sozinho na tela é a ambiguidade que este
// módulo inteiro existe para eliminar.
TEST_F(PresentationMetricsTest, OverlayLineNamesBothNumbers)
{
	PresentRealFrames(10, 33.333);

	const std::string line = GSPresentationMetrics::GetOverlayLine();
	EXPECT_NE(line.find("Real"), std::string::npos);
	EXPECT_NE(line.find("Apresentado"), std::string::npos);
}

TEST_F(PresentationMetricsTest, ResetClearsEverything)
{
	PresentRealFrames(10, 10.0);
	GSPresentationMetrics::Reset();

	const Snapshot snap = GSPresentationMetrics::GetSnapshot();
	EXPECT_EQ(snap.real_frames, 0u);
	EXPECT_FLOAT_EQ(snap.real_frametime_avg_ms, 0.0f);
}

// --- acumulação de sessão (Fase 6) ---

// A janela de 1 s responde "como está agora"; um benchmark precisa do trecho inteiro. Amostrar a
// janela periodicamente daria contagem dupla, porque janelas se sobrepõem — por isso a sessão tem
// contadores próprios.
TEST_F(PresentationMetricsTest, SessionCountsTheWholeRunNotTheWindow)
{
	GSPresentationMetrics::BeginSession();

	// 10 segundos a 30 FPS: muito além da janela de 1 s.
	for (int i = 0; i < 300; i++)
	{
		if (i > 0)
			AdvanceMs(33.333);
		GSPresentationMetrics::NotePresented(FrameKind::Real);
	}

	GSPresentationMetrics::EndSession();
	const auto session = GSPresentationMetrics::GetSessionStats();

	EXPECT_EQ(session.real_frames, 300u);
	EXPECT_NEAR(session.duration_seconds, 9.967, 0.05);
	EXPECT_NEAR(session.real_fps, 30.0f, 0.5f);
	EXPECT_NEAR(session.frametime_avg_ms, 33.333f, 0.1f);
}

TEST_F(PresentationMetricsTest, SessionKeepsRealAndPresentedApart)
{
	GSPresentationMetrics::BeginSession();
	for (int i = 0; i < 100; i++)
	{
		if (i > 0)
			AdvanceMs(16.667);
		GSPresentationMetrics::NotePresented(FrameKind::Real);
		AdvanceMs(16.667);
		GSPresentationMetrics::NotePresented(FrameKind::Generated);
	}
	GSPresentationMetrics::EndSession();

	const auto session = GSPresentationMetrics::GetSessionStats();
	EXPECT_EQ(session.real_frames, 100u);
	EXPECT_EQ(session.generated_frames, 100u);
	// O apresentado é o dobro do real, e os dois campos continuam distintos.
	EXPECT_NEAR(session.presented_fps / session.real_fps, 2.0f, 0.05f);
}

// O pico tem que sobreviver ao histograma: é o número que decide se um driver engasga.
TEST_F(PresentationMetricsTest, SessionPercentilesExposeTheSpike)
{
	GSPresentationMetrics::BeginSession();
	for (int i = 0; i < 199; i++)
	{
		if (i > 0)
			AdvanceMs(10.0);
		GSPresentationMetrics::NotePresented(FrameKind::Real);
	}
	AdvanceMs(120.0);
	GSPresentationMetrics::NotePresented(FrameKind::Real);
	GSPresentationMetrics::EndSession();

	const auto session = GSPresentationMetrics::GetSessionStats();
	EXPECT_NEAR(session.frametime_max_ms, 120.0f, 0.1f);
	// O 1% pior de 200 amostras são 2 quadros; um deles é o pico de 120 ms.
	EXPECT_GT(session.frametime_low1_ms, 30.0f);
	EXPECT_LT(session.frametime_avg_ms, 12.0f);
	EXPECT_GE(session.stutter_count, 1u);
}

TEST_F(PresentationMetricsTest, SessionIsInertBeforeBeginAndAfterEnd)
{
	// Sem BeginSession, nada é acumulado.
	PresentRealFrames(10, 10.0);
	EXPECT_EQ(GSPresentationMetrics::GetSessionStats().real_frames, 0u);

	GSPresentationMetrics::BeginSession();
	PresentRealFrames(5, 10.0);
	GSPresentationMetrics::EndSession();
	const u64 after_end = GSPresentationMetrics::GetSessionStats().real_frames;

	// Depois de EndSession os números congelam, mesmo com o jogo continuando.
	PresentRealFrames(50, 10.0);
	EXPECT_EQ(GSPresentationMetrics::GetSessionStats().real_frames, after_end);
}
