// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkDiagnostics.h"

#include "Fork/ForkConfig.h"
#include "Fork/ForkDriverIdentity.h"
#include "Fork/ForkGpuCapabilities.h"

#include "common/Console.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <algorithm>
#include <mutex>

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
		ForkFrameGen::ReasonText(dominant), generation_avg_ms, accumulator.worst_generation_ms, policy.budget_ms, policy.min_speed_percent,
		policy.min_real_fps);
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
}

void ForkDiagnostics::NotePresent(const ForkFrameGen::Decision& decision)
{
	if (!ForkConfig::GetBool(ForkConfig::Option::DiagnosticsLog))
		return;

	// Lido fora do mutex e uma vez só: é o custo da geração medido pelo backend, e a média da
	// janela é a mesma que a régua usa para o orçamento — logar outro número seria explicar uma
	// suspensão com um valor que não a causou.
	const GSPresentationMetrics::Snapshot snapshot = GSPresentationMetrics::GetSnapshot();

	std::string identity_line;
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
			s_identity_written = true;
		}
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
		Console.WriteLn("%s", identity_line.c_str());
	Console.WriteLn("%s", real_line.c_str());
	Console.WriteLn("%s", presented_line.c_str());
	Console.WriteLn("%s", framegen_line.c_str());
}
