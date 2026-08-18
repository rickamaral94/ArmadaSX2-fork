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
