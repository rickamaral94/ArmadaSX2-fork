// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkBenchmark.h"

#include "Fork/ForkBridge.h"
#include "Fork/ForkDriverIdentity.h"
#include "Fork/ForkGpuCapabilities.h"
#include "GS/GSShaderCompileIndicator.h"
#include "GS/Renderers/Common/GSPresentationMetrics.h"

#include "fmt/format.h"

#include <mutex>

namespace
{
	std::mutex s_mutex;
	std::vector<ForkBenchmark::Run> s_runs;
	bool s_running = false;
	std::string s_label;
	u32 s_shader_count_at_start = 0;
	u64 s_shader_time_ns_at_start = 0;

	/// Diferença relativa em porcentagem, com o sinal indicando melhora do candidato.
	double PercentDelta(double baseline, double candidate)
	{
		if (baseline <= 0.0)
			return 0.0;
		return ((candidate - baseline) / baseline) * 100.0;
	}

	std::string FormatDelta(double baseline, double candidate, const char* unit, bool higher_is_better)
	{
		const double delta = PercentDelta(baseline, candidate);
		const char* verdict = "igual";
		if (delta > 1.0)
			verdict = higher_is_better ? "melhor" : "pior";
		else if (delta < -1.0)
			verdict = higher_is_better ? "pior" : "melhor";

		return fmt::format("{:.2f}{} -> {:.2f}{} ({:+.1f}%, {})", baseline, unit, candidate, unit, delta, verdict);
	}
} // namespace

void ForkBenchmark::Begin(std::string_view label)
{
	{
		std::lock_guard lock(s_mutex);
		s_running = true;
		s_label = label;
		// Diferença de contadores cumulativos: é o único jeito de saber quantos shaders foram
		// compilados DURANTE esta execução, e não desde que o app abriu.
		s_shader_count_at_start = GSShaderCompileIndicator::s_total_count.load(std::memory_order_relaxed);
		s_shader_time_ns_at_start = GSShaderCompileIndicator::s_total_time_ns.load(std::memory_order_relaxed);
	}

	// Fora do lock: a métrica tem o mutex dela, e aninhar dois locks por ordens diferentes em
	// caminhos diferentes é como se constrói um deadlock.
	GSPresentationMetrics::SetEnabled(true);
	GSPresentationMetrics::BeginSession();
}

ForkBenchmark::Run ForkBenchmark::End()
{
	GSPresentationMetrics::EndSession();
	const GSPresentationMetrics::SessionStats stats = GSPresentationMetrics::GetSessionStats();
	const ForkDriverIdentity::Identity identity = ForkDriverIdentity::Get();
	const ForkGpuCapabilities::Capabilities caps = ForkGpuCapabilities::Get();

	Run run;
	run.duration_seconds = stats.duration_seconds;
	run.real_fps = stats.real_fps;
	run.presented_fps = stats.presented_fps;
	run.frametime_avg_ms = stats.frametime_avg_ms;
	run.frametime_min_ms = stats.frametime_min_ms;
	run.frametime_max_ms = stats.frametime_max_ms;
	run.frametime_p95_ms = stats.frametime_p95_ms;
	run.frametime_p99_ms = stats.frametime_p99_ms;
	run.frametime_low1_ms = stats.frametime_low1_ms;
	run.low1_fps = stats.low1_fps;
	run.stutter_count = stats.stutter_count;
	run.real_frames = stats.real_frames;
	run.duplicated_frames = stats.duplicated_frames;
	run.generated_frames = stats.generated_frames;
	run.skipped_presents = stats.skipped_presents;
	run.present_errors = stats.present_errors;
	run.generation_avg_ms = stats.generation_avg_ms;

	run.gpu = caps.gpu_name;
	run.driver = GpuProfileDetector::DriverToString(identity.active_driver);
	run.driver_outcome = ForkDriverIdentity::OutcomeToString(identity.outcome);
	run.mesa_version = identity.mesa.known
						   ? fmt::format("{}.{}.{}", identity.mesa.major, identity.mesa.minor, identity.mesa.patch)
						   : std::string();
	run.vulkan_version = ForkGpuCapabilities::FormatVulkanVersion(identity.vulkan_api_version);
	run.package_sha256 = identity.package_sha256;
	run.driver_as_requested = !ForkDriverIdentity::IsUnexpected(identity.outcome);

	{
		std::lock_guard lock(s_mutex);
		run.label = s_label;
		const u32 count_now = GSShaderCompileIndicator::s_total_count.load(std::memory_order_relaxed);
		const u64 time_now = GSShaderCompileIndicator::s_total_time_ns.load(std::memory_order_relaxed);
		run.shader_compiles = count_now - s_shader_count_at_start;
		run.shader_compile_ms = static_cast<double>(time_now - s_shader_time_ns_at_start) / 1'000'000.0;

		s_running = false;
		s_label.clear();
		s_runs.push_back(run);
	}

	return run;
}

bool ForkBenchmark::IsRunning()
{
	std::lock_guard lock(s_mutex);
	return s_running;
}

std::string ForkBenchmark::CurrentLabel()
{
	std::lock_guard lock(s_mutex);
	return s_label;
}

const std::vector<ForkBenchmark::Run>& ForkBenchmark::GetRuns()
{
	return s_runs;
}

void ForkBenchmark::ClearRuns()
{
	std::lock_guard lock(s_mutex);
	s_runs.clear();
}

std::vector<std::string> ForkBenchmark::ValidityWarnings(const Run& baseline, const Run& candidate)
{
	std::vector<std::string> warnings;

	// Um A/B inválido é pior que nenhum: ele produz um número que alguém vai citar em um relatório
	// de compatibilidade, e ninguém volta para checar as condições.
	for (const Run* run : {&baseline, &candidate})
	{
		if (!run->driver_as_requested)
		{
			warnings.push_back(fmt::format(
				"'{}' não rodou no driver pedido ({}) — a comparação mede outra coisa.", run->label,
				run->driver_outcome));
		}
		if (run->shader_compiles > 0)
		{
			warnings.push_back(fmt::format(
				"'{}' compilou {} shaders ({:.0f} ms) durante a medição — isso é primeiro boot, não regime.",
				run->label, run->shader_compiles, run->shader_compile_ms));
		}
		if (run->present_errors > 0)
			warnings.push_back(fmt::format("'{}' teve {} erros de present.", run->label, run->present_errors));
		if (run->duration_seconds < 30.0)
		{
			warnings.push_back(fmt::format("'{}' durou {:.0f} s — trechos curtos não capturam engasgos.",
				run->label, run->duration_seconds));
		}
	}

	if (baseline.gpu != candidate.gpu)
		warnings.push_back("As duas execuções não são da mesma GPU.");

	// Duração muito diferente significa trechos diferentes do jogo, e aí a diferença medida pode
	// ser da cena, não do driver.
	const double longer = std::max(baseline.duration_seconds, candidate.duration_seconds);
	const double shorter = std::min(baseline.duration_seconds, candidate.duration_seconds);
	if (shorter > 0.0 && (longer / shorter) > 1.25)
		warnings.push_back("As durações diferem em mais de 25% — provavelmente trechos diferentes.");

	return warnings;
}

std::vector<std::string> ForkBenchmark::CompareLines(const Run& baseline, const Run& candidate)
{
	std::vector<std::string> lines;
	lines.push_back(fmt::format("{} x {}", baseline.label, candidate.label));

	// Real e apresentado em LINHAS SEPARADAS, sempre, mesmo quando iguais. Somá-los ou mostrar só
	// um é exatamente como frame generation vira maquiagem de emulação lenta.
	lines.push_back("FPS real       : " + FormatDelta(baseline.real_fps, candidate.real_fps, "", true));
	lines.push_back("FPS apresentado: " + FormatDelta(baseline.presented_fps, candidate.presented_fps, "", true));
	lines.push_back("Frametime médio: " +
					FormatDelta(baseline.frametime_avg_ms, candidate.frametime_avg_ms, " ms", false));
	lines.push_back("1% low         : " + FormatDelta(baseline.low1_fps, candidate.low1_fps, " FPS", true));
	lines.push_back("p99 frametime  : " +
					FormatDelta(baseline.frametime_p99_ms, candidate.frametime_p99_ms, " ms", false));
	lines.push_back(fmt::format("Engasgos       : {} -> {}", baseline.stutter_count, candidate.stutter_count));
	lines.push_back(fmt::format("Driver         : {} ({}) -> {} ({})", baseline.driver, baseline.driver_outcome,
		candidate.driver, candidate.driver_outcome));

	for (const std::string& warning : ValidityWarnings(baseline, candidate))
		lines.push_back("AVISO: " + warning);

	return lines;
}

std::string ForkBenchmark::ToJson()
{
	std::lock_guard lock(s_mutex);
	std::string out = R"({"ok":true,"runs":[)";

	bool first = true;
	for (const Run& run : s_runs)
	{
		if (!first)
			out += ',';
		first = false;
		out += fmt::format(
			R"({{"label":"{}","durationSeconds":{:.3f},"realFps":{:.3f},"presentedFps":{:.3f},)"
			R"("frametimeAvgMs":{:.3f},"frametimeMinMs":{:.3f},"frametimeMaxMs":{:.3f},)"
			R"("frametimeP95Ms":{:.3f},"frametimeP99Ms":{:.3f},"low1Ms":{:.3f},"low1Fps":{:.3f},)"
			R"("stutters":{},"realFrames":{},"duplicatedFrames":{},"generatedFrames":{},)"
			R"("skippedPresents":{},"presentErrors":{},"generationAvgMs":{:.3f},)"
			R"("shaderCompiles":{},"shaderCompileMs":{:.1f},"gpu":"{}","driver":"{}",)"
			R"("driverOutcome":"{}","mesa":"{}","vulkan":"{}","packageSha256":"{}",)"
			R"("driverAsRequested":{}}})",
			ForkBridge::EscapeJson(run.label), run.duration_seconds, run.real_fps, run.presented_fps,
			run.frametime_avg_ms, run.frametime_min_ms, run.frametime_max_ms, run.frametime_p95_ms,
			run.frametime_p99_ms, run.frametime_low1_ms, run.low1_fps, run.stutter_count, run.real_frames,
			run.duplicated_frames, run.generated_frames, run.skipped_presents, run.present_errors,
			run.generation_avg_ms, run.shader_compiles, run.shader_compile_ms,
			ForkBridge::EscapeJson(run.gpu), ForkBridge::EscapeJson(run.driver),
			run.driver_outcome, run.mesa_version, run.vulkan_version, run.package_sha256,
			run.driver_as_requested ? "true" : "false");
	}

	out += "]}";
	return out;
}

std::string ForkBenchmark::ToCsv()
{
	std::lock_guard lock(s_mutex);

	// Cabeçalho nomeia real e apresentado como colunas distintas: quem abrir isto em uma planilha
	// não deve conseguir somá-los por engano.
	std::string out =
		"label,duration_s,real_fps,presented_fps,frametime_avg_ms,frametime_p99_ms,low1_fps,"
		"stutters,real_frames,generated_frames,present_errors,shader_compiles,shader_compile_ms,"
		"gpu,driver,driver_outcome,mesa,vulkan,package_sha256,driver_as_requested\n";

	for (const Run& run : s_runs)
	{
		// Aspas duplicadas no padrão CSV: nome de GPU contém vírgula ("Adreno (TM) 750, rev 2").
		const auto quote = [](const std::string& value) {
			std::string escaped;
			escaped.reserve(value.size() + 2);
			escaped += '"';
			for (const char ch : value)
			{
				if (ch == '"')
					escaped += '"';
				escaped += ch;
			}
			escaped += '"';
			return escaped;
		};

		out += fmt::format("{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{},{},{},{},{:.1f},{},{},{},{},{},{},{}\n",
			quote(run.label), run.duration_seconds, run.real_fps, run.presented_fps, run.frametime_avg_ms,
			run.frametime_p99_ms, run.low1_fps, run.stutter_count, run.real_frames, run.generated_frames,
			run.present_errors, run.shader_compiles, run.shader_compile_ms, quote(run.gpu), quote(run.driver),
			quote(run.driver_outcome), quote(run.mesa_version), quote(run.vulkan_version),
			quote(run.package_sha256), run.driver_as_requested ? "true" : "false");
	}

	return out;
}
