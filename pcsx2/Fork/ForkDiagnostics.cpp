// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkDiagnostics.h"

#include "Fork/ForkConfig.h"
#include "Fork/ForkDriverIdentity.h"
#include "Fork/ForkGpuCapabilities.h"

#include "GS/GSShaderCompileIndicator.h"
#include "PerformanceMetrics.h"

#include "common/Console.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace
{
	/// Prefixo fixo. Um `grep @@FORK@@` tem de bastar: o Console sai por stdout e o Android o
	/// redireciona para o logcat sob a tag `STDOUT`, então filtro por tag não encontra nada.
	constexpr const char* PREFIX = "@@FORK@@";

	u64 (*s_clock)() = nullptr;

	u64 Now()
	{
		return s_clock ? s_clock() : static_cast<u64>(Common::Timer::GetCurrentValue());
	}

	std::mutex s_mutex;
	ForkDiagnostics::Accumulator s_accumulator;
	u64 s_window_start = 0;
	bool s_identity_written = false;
	u32 s_last_shader_compiles = 0;
} // namespace

void ForkDiagnostics::Detail::SetClockForTesting(u64 (*clock)())
{
	s_clock = clock;
}

void ForkDiagnostics::Accumulator::Note(const ForkFrameGen::Decision& decision, float generation_ms)
{
	frames++;
	const size_t index = static_cast<size_t>(decision.reason);
	if (index < frames_by_reason.size())
		frames_by_reason[index]++;

	worst_generation_ms = std::max(worst_generation_ms, generation_ms);

	if (has_previous && previous_state != decision.state)
		transitions++;
	has_previous = true;
	previous_state = decision.state;
}

void ForkDiagnostics::Accumulator::Reset()
{
	frames_by_reason.fill(0);
	transitions = 0;
	frames = 0;
	worst_generation_ms = 0.0f;
	// `has_previous`/`previous_state` sobrevivem ao intervalo de propósito: a transição que ocorre
	// EXATAMENTE na virada de um bloco para o outro é uma transição real, e zerar aqui a perderia.
}

ForkFrameGen::Reason ForkDiagnostics::Accumulator::DominantReason() const
{
	size_t best = 0;
	for (size_t i = 1; i < frames_by_reason.size(); i++)
	{
		if (frames_by_reason[i] > frames_by_reason[best])
			best = i;
	}
	return static_cast<ForkFrameGen::Reason>(best);
}

std::string ForkDiagnostics::FormatRealLine(const GSPresentationMetrics::Snapshot& snapshot)
{
	return fmt::format("{} real      fps={:.2f} frametime_avg={:.2f}ms 1%low={:.2f}ms min={:.2f}ms max={:.2f}ms",
		PREFIX, snapshot.real_fps, snapshot.real_frametime_avg_ms, snapshot.real_frametime_low1_ms,
		snapshot.real_frametime_min_ms, snapshot.real_frametime_max_ms);
}

std::string ForkDiagnostics::FormatPresentedLine(const GSPresentationMetrics::Snapshot& snapshot)
{
	return fmt::format("{} presented fps={:.2f} real_frames={} generated={} duplicated={}", PREFIX,
		snapshot.presented_fps, snapshot.real_frames, snapshot.generated_frames, snapshot.duplicated_frames);
}

std::string ForkDiagnostics::FormatFrameGenLine(
	const Accumulator& accumulator, const ForkFrameGen::Policy& policy, float generation_avg_ms)
{
	const ForkFrameGen::Reason dominant = accumulator.DominantReason();
	const u32 engaged = accumulator.frames_by_reason[static_cast<size_t>(ForkFrameGen::Reason::Engaged)];
	const float engaged_share =
		accumulator.frames > 0 ? (100.0f * static_cast<float>(engaged) / static_cast<float>(accumulator.frames)) : 0.0f;

	return fmt::format(
		"{} framegen  mode={} engaged={:.1f}% transitions={} dominant={} gen_avg={:.2f}ms gen_worst={:.2f}ms budget={:.2f}ms speed_floor={:.0f}% fps_floor={:.1f}",
		PREFIX, ForkFrameGen::ModeToString(policy.mode), engaged_share, accumulator.transitions,
		ForkFrameGen::ReasonToString(dominant), generation_avg_ms, accumulator.worst_generation_ms, policy.budget_ms, policy.min_speed_percent,
		policy.min_real_fps);
}

std::string ForkDiagnostics::FormatLoadLine(const Load& load)
{
	// Uma linha só, e sem FPS de apresentação nenhum: este bloco fala de CUSTO, e misturar aqui o
	// número que o usuário vê na tela é o começo de confundir os dois de novo.
	std::string line = fmt::format(
		"{} load      speed={:.1f}% vps={:.2f} cpu={:.0f}% gs={:.0f}%", PREFIX, load.speed_percent, load.vps,
		load.cpu_thread_usage, load.gs_thread_usage);

	if (load.has_gs_back_thread)
		line += fmt::format(" gs_back={:.0f}%", load.gs_back_thread_usage);
	if (load.vu_thread_usage > 0.0f)
		line += fmt::format(" vu={:.0f}%", load.vu_thread_usage);
	if (load.gpu_usage > 0.0f)
		line += fmt::format(" gpu={:.0f}%", load.gpu_usage);
	// Só quando o método de medição é válido: um "0.00" indistinguível de "não sei" é pior que a
	// ausência do campo.
	if (load.internal_fps_valid)
		line += fmt::format(" internal_fps={:.2f}", load.internal_fps);
	line += fmt::format(" shader_compiles={}", load.shader_compiles);
	return line;
}

std::string ForkDiagnostics::FormatHygieneLine(const Hygiene& hygiene)
{
	std::vector<const char*> problems;

	// Cada item aqui é algo que faz o NÚMERO MENTIR, não algo que faz o jogo renderizar errado.
	// A lista é curta de propósito: uma lista longa vira ruído e ninguém lê.
	if (hygiene.dumping_textures)
		problems.push_back("despejo-de-texturas(grava em disco por draw)");
	if (hygiene.loading_texture_pack)
		problems.push_back("pacote-de-texturas(I/O e VRAM extras)");
	if (hygiene.ee_cycle_rate_changed)
		problems.push_back("EECycleRate!=0(muda o tempo emulado)");
	if (hygiene.ee_cycle_skip_changed)
		problems.push_back("EECycleSkip!=0(muda o tempo emulado)");

	const std::string context =
		fmt::format(" | upscale={:.2f}x blend={}", hygiene.upscale_multiplier, hygiene.blending_level);

	if (problems.empty())
		return fmt::format("{} hygiene   limpo{}", PREFIX, context);

	std::string list;
	for (const char* problem : problems)
	{
		if (!list.empty())
			list += ", ";
		list += problem;
	}
	return fmt::format("{} hygiene   MEDICAO CONTAMINADA: {}{}", PREFIX, list, context);
}

std::string ForkDiagnostics::FormatIdentityLine()
{
	const ForkDriverIdentity::Identity identity = ForkDriverIdentity::Get();
	const ForkGpuCapabilities::Capabilities caps = ForkGpuCapabilities::Get();

	return fmt::format("{} identity  gpu='{}' turnip={} requested='{}' active={} unexpected={} mesa={} sha256={}",
		PREFIX, caps.gpu_name, ForkGpuCapabilities::TurnipSupportToString(caps.turnip),
		identity.requested_driver.empty() ? "system" : identity.requested_driver,
		GpuProfileDetector::DriverToString(identity.active_driver),
		ForkDriverIdentity::IsUnexpected(identity.outcome) ? "YES" : "no",
		identity.mesa.known ? fmt::format("{}.{}.{}", identity.mesa.major, identity.mesa.minor, identity.mesa.patch)
							: std::string("-"),
		identity.package_sha256.empty() ? std::string("-") : identity.package_sha256);
}

void ForkDiagnostics::Reset()
{
	std::lock_guard lock(s_mutex);
	s_accumulator = Accumulator{};
	s_window_start = 0;
	s_identity_written = false;
	s_last_shader_compiles = GSShaderCompileIndicator::s_total_count.load(std::memory_order_relaxed);
}

void ForkDiagnostics::NotePresent(const ForkFrameGen::Decision& decision, const Hygiene& hygiene)
{
	if (!ForkConfig::GetBool(ForkConfig::Option::DiagnosticsLog))
		return;

	// Lido fora do mutex e uma vez só: é o custo da geração medido pelo backend, e a média da
	// janela é a mesma que a régua usa para o orçamento — logar outro número seria explicar uma
	// suspensão com um valor que não a causou.
	const GSPresentationMetrics::Snapshot snapshot = GSPresentationMetrics::GetSnapshot();

	std::string identity_line;
	std::string hygiene_line;
	std::string load_line;
	std::string real_line;
	std::string presented_line;
	std::string framegen_line;

	{
		std::lock_guard lock(s_mutex);
		s_accumulator.Note(decision, snapshot.generation_avg_ms);

		const u64 now = Now();
		if (s_window_start == 0)
		{
			s_window_start = now;
			return;
		}

		const double interval = std::max(1.0, static_cast<double>(ForkConfig::GetFloat(
											   ForkConfig::Option::DiagnosticsIntervalSeconds)));
		if (Common::Timer::ConvertValueToSeconds(now - s_window_start) < interval)
			return;

		if (!s_identity_written)
		{
			identity_line = FormatIdentityLine();
			hygiene_line = FormatHygieneLine(hygiene);
			s_identity_written = true;
		}

		Load load;
		load.speed_percent = PerformanceMetrics::GetSpeed();
		load.vps = PerformanceMetrics::GetFPS();
		load.internal_fps = PerformanceMetrics::GetInternalFPS();
		load.internal_fps_valid = PerformanceMetrics::IsInternalFPSValid();
		load.cpu_thread_usage = PerformanceMetrics::GetCPUThreadUsage();
		load.gs_thread_usage = PerformanceMetrics::GetGSThreadUsage();
		load.has_gs_back_thread = PerformanceMetrics::HasGSBackThread();
		load.gs_back_thread_usage = PerformanceMetrics::GetGSBackThreadUsage();
		load.vu_thread_usage = PerformanceMetrics::GetVUThreadUsage();
		load.gpu_usage = PerformanceMetrics::GetGPUUsage();

		// Delta, não acumulado: o total de uma sessão inteira não diz em QUAL intervalo o
		// engasgo aconteceu, que é justamente a pergunta.
		const u32 compiles_now = GSShaderCompileIndicator::s_total_count.load(std::memory_order_relaxed);
		load.shader_compiles = compiles_now - s_last_shader_compiles;
		s_last_shader_compiles = compiles_now;
		load_line = FormatLoadLine(load);
		real_line = FormatRealLine(snapshot);
		presented_line = FormatPresentedLine(snapshot);
		framegen_line = FormatFrameGenLine(s_accumulator, ForkFrameGen::PolicyFromConfig(), snapshot.generation_avg_ms);

		s_accumulator.Reset();
		s_window_start = now;
	}

	// Escrito FORA do mutex: `Console` pode bloquear (no Android é um pipe para o logcat), e
	// segurar o mutex durante a escrita colocaria o thread de GS atrás do escritor de log a cada
	// bloco.
	if (!identity_line.empty())
	{
		Console.WriteLn("%s", identity_line.c_str());
		Console.WriteLn("%s", hygiene_line.c_str());
	}
	Console.WriteLn("%s", load_line.c_str());
	Console.WriteLn("%s", real_line.c_str());
	Console.WriteLn("%s", presented_line.c_str());
	Console.WriteLn("%s", framegen_line.c_str());
}
