// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkBenchmark.h"

#include "Fork/ForkBridge.h"

#include <gtest/gtest.h>

#include <string>

namespace
{
	bool AnyLineContains(const std::vector<std::string>& lines, std::string_view needle)
	{
		for (const std::string& line : lines)
		{
			if (line.find(needle) != std::string::npos)
				return true;
		}
		return false;
	}

	ForkBenchmark::Run MakeRun(std::string label, float real_fps, float presented_fps)
	{
		ForkBenchmark::Run run;
		run.label = std::move(label);
		run.duration_seconds = 60.0;
		run.real_fps = real_fps;
		run.presented_fps = presented_fps;
		run.frametime_avg_ms = (real_fps > 0.0f) ? (1000.0f / real_fps) : 0.0f;
		run.frametime_p99_ms = run.frametime_avg_ms * 1.5f;
		run.low1_fps = real_fps * 0.7f;
		run.gpu = "Adreno (TM) 750";
		run.driver = "MesaTurnip";
		run.driver_outcome = "CustomDriverActive";
		run.driver_as_requested = true;
		return run;
	}
} // namespace

// A regra do projeto, aplicada ao relatório: real e apresentado NUNCA na mesma linha. Juntá-los é
// exatamente como frame generation viraria maquiagem de emulação lenta.
TEST(ForkBenchmark, RealAndPresentedAreAlwaysSeparateLines)
{
	const ForkBenchmark::Run off = MakeRun("FG Off", 30.0f, 30.0f);
	const ForkBenchmark::Run on = MakeRun("FG On", 30.0f, 60.0f);

	const std::vector<std::string> lines = ForkBenchmark::CompareLines(off, on);

	EXPECT_TRUE(AnyLineContains(lines, "FPS real"));
	EXPECT_TRUE(AnyLineContains(lines, "FPS apresentado"));

	// Nenhuma linha pode conter os dois números ao mesmo tempo.
	for (const std::string& line : lines)
	{
		const bool has_real = line.find("FPS real") != std::string::npos;
		const bool has_presented = line.find("FPS apresentado") != std::string::npos;
		EXPECT_FALSE(has_real && has_presented) << line;
	}
}

// O caso que o projeto proíbe chamar de sucesso: FG dobrando o apresentado enquanto o real cai.
TEST(ForkBenchmark, FrameGenThatSlowsEmulationIsVisible)
{
	const ForkBenchmark::Run off = MakeRun("FG Off", 30.0f, 30.0f);
	const ForkBenchmark::Run on = MakeRun("FG On", 22.0f, 44.0f);

	const std::vector<std::string> lines = ForkBenchmark::CompareLines(off, on);

	// O apresentado melhora e o real piora — e as duas coisas aparecem, cada uma na sua linha.
	EXPECT_TRUE(AnyLineContains(lines, "FPS real"));
	bool real_line_says_worse = false;
	for (const std::string& line : lines)
	{
		if (line.find("FPS real") != std::string::npos && line.find("pior") != std::string::npos)
			real_line_says_worse = true;
	}
	EXPECT_TRUE(real_line_says_worse) << "a queda do FPS real tem que estar rotulada como pior";
}

// Um A/B inválido é pior que nenhum: ele produz um número que alguém cita depois sem voltar para
// checar as condições.
TEST(ForkBenchmark, FallbackToSystemDriverInvalidatesTheComparison)
{
	ForkBenchmark::Run baseline = MakeRun("System", 30.0f, 30.0f);
	ForkBenchmark::Run candidate = MakeRun("Turnip A", 33.0f, 33.0f);
	candidate.driver_as_requested = false;
	candidate.driver_outcome = "FellBackToSystem";

	const std::vector<std::string> warnings = ForkBenchmark::ValidityWarnings(baseline, candidate);
	EXPECT_FALSE(warnings.empty());
	EXPECT_TRUE(AnyLineContains(warnings, "não rodou no driver pedido"));

	// E o aviso aparece na comparação, não só em uma consulta separada que ninguém faz.
	EXPECT_TRUE(AnyLineContains(ForkBenchmark::CompareLines(baseline, candidate), "AVISO"));
}

// Compilação de shader durante a medição significa primeiro boot, não regime — e é o viés que a
// chave de cache por driver (Fase 4, item 3) existe para evitar.
TEST(ForkBenchmark, ShaderCompilationDuringTheRunIsFlagged)
{
	ForkBenchmark::Run baseline = MakeRun("System", 30.0f, 30.0f);
	ForkBenchmark::Run candidate = MakeRun("Turnip A", 28.0f, 28.0f);
	candidate.shader_compiles = 412;
	candidate.shader_compile_ms = 8300.0;

	EXPECT_TRUE(AnyLineContains(ForkBenchmark::ValidityWarnings(baseline, candidate), "primeiro boot"));
}

TEST(ForkBenchmark, ShortRunsAndMismatchedDurationsAreFlagged)
{
	ForkBenchmark::Run baseline = MakeRun("A", 30.0f, 30.0f);
	ForkBenchmark::Run candidate = MakeRun("B", 30.0f, 30.0f);
	candidate.duration_seconds = 10.0;

	const std::vector<std::string> warnings = ForkBenchmark::ValidityWarnings(baseline, candidate);
	EXPECT_TRUE(AnyLineContains(warnings, "trechos curtos"));
	EXPECT_TRUE(AnyLineContains(warnings, "durações diferem"));
}

TEST(ForkBenchmark, DifferentGpusCannotBeCompared)
{
	ForkBenchmark::Run baseline = MakeRun("A", 30.0f, 30.0f);
	ForkBenchmark::Run candidate = MakeRun("B", 40.0f, 40.0f);
	candidate.gpu = "Adreno (TM) 650";

	EXPECT_TRUE(AnyLineContains(ForkBenchmark::ValidityWarnings(baseline, candidate), "mesma GPU"));
}

TEST(ForkBenchmark, CleanComparisonHasNoWarnings)
{
	const ForkBenchmark::Run baseline = MakeRun("System", 30.0f, 30.0f);
	const ForkBenchmark::Run candidate = MakeRun("Turnip A", 34.0f, 34.0f);
	EXPECT_TRUE(ForkBenchmark::ValidityWarnings(baseline, candidate).empty());
}

TEST(ForkBenchmark, BeginRequiresALabelAndEndRequiresARun)
{
	ForkBenchmark::ClearRuns();
	EXPECT_FALSE(ForkBenchmark::IsRunning());

	// Sem rótulo a execução não serve para comparar, então a ponte recusa.
	EXPECT_NE(ForkBridge::Query("benchmark.begin:").find("\"ok\":false"), std::string::npos);
	EXPECT_NE(ForkBridge::Query("benchmark.end").find("\"ok\":false"), std::string::npos);

	EXPECT_NE(ForkBridge::Query("benchmark.begin:System").find("\"ok\":true"), std::string::npos);
	EXPECT_TRUE(ForkBenchmark::IsRunning());
	EXPECT_EQ(ForkBenchmark::CurrentLabel(), "System");

	EXPECT_NE(ForkBridge::Query("benchmark.end").find("\"ok\":true"), std::string::npos);
	EXPECT_FALSE(ForkBenchmark::IsRunning());
	EXPECT_EQ(ForkBenchmark::GetRuns().size(), 1u);

	ForkBenchmark::ClearRuns();
	EXPECT_TRUE(ForkBenchmark::GetRuns().empty());
}

// Quem abrir o CSV em uma planilha não pode conseguir somar real com apresentado por engano: são
// colunas nomeadas e separadas.
TEST(ForkBenchmark, CsvHeaderNamesBothFpsColumnsSeparately)
{
	ForkBenchmark::ClearRuns();
	ForkBridge::Query("benchmark.begin:Turnip A");
	ForkBridge::Query("benchmark.end");

	const std::string csv = ForkBenchmark::ToCsv();
	EXPECT_NE(csv.find("real_fps"), std::string::npos);
	EXPECT_NE(csv.find("presented_fps"), std::string::npos);
	EXPECT_NE(csv.find("shader_compiles"), std::string::npos);
	EXPECT_NE(csv.find("package_sha256"), std::string::npos);
	// O rótulo com espaço tem que sair entre aspas.
	EXPECT_NE(csv.find("\"Turnip A\""), std::string::npos);

	const std::string json = ForkBenchmark::ToJson();
	EXPECT_NE(json.find("\"realFps\""), std::string::npos);
	EXPECT_NE(json.find("\"presentedFps\""), std::string::npos);
	ForkBenchmark::ClearRuns();
}
