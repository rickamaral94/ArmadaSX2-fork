// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkDiagnostics.h"

#include "Fork/ForkConfig.h"
#include "Fork/ForkDriverIdentity.h"
#include "Fork/ForkGpuCapabilities.h"

#include "GS/GSPerfMon.h"
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
	// Ultimo valor lido de cada contador de perfmon. Ele ZERA sozinho a cada 30 quadros, entao
	// `agora - anterior` vira negativo na virada; quando isso acontece o valor atual JA e o delta.
	// Mesmo tratamento que o bridge do Android usa em Host::BeginPresentFrame.
	double s_last_fb_copy_draws = 0.0;
	double s_last_fb_copies = 0.0;
	double s_last_fb_copy_pixels = 0.0;
	double s_last_tex_copies = 0.0;
	double s_last_draw_calls = 0.0;
	double s_last_render_passes = 0.0;

	double PerfmonDelta(GSPerfMon::counter_t counter, double& last)
	{
		const double now = g_perfmon.GetCounter(counter);
		const double delta = (now < last) ? now : (now - last);
		last = now;
		return delta;
	}

	u32 s_last_shader_compiles = 0;
	u32 s_last_shader_source = 0;
	u64 s_last_shader_time_ns = 0;
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
	// A CADÊNCIA do que saiu para a tela, e não só a contagem.
	//
	// `presented fps=60` num jogo de 30 pode significar duas coisas opostas, e até a 8.10 o log
	// não distinguia: quadros a cada 16,7 ms — o recurso funcionando — ou pares 8,3/25,0, que dão
	// o mesmo 60 e se parecem com 30 fps com atraso a mais. `pace` é o que separa: com min e max
	// colados na média, a cadência é regular; afastados, os quadros estão saindo grudados.
	std::string line = fmt::format("{} presented fps={:.2f} real_frames={} generated={} duplicated={}",
		PREFIX, snapshot.presented_fps, snapshot.real_frames, snapshot.generated_frames,
		snapshot.duplicated_frames);

	if (snapshot.presented_frametime_avg_ms > 0.0f)
	{
		line += fmt::format(" pace_avg={:.2f}ms pace_min={:.2f}ms pace_max={:.2f}ms",
			snapshot.presented_frametime_avg_ms, snapshot.presented_frametime_min_ms,
			snapshot.presented_frametime_max_ms);
	}
	return line;
}

std::string ForkDiagnostics::FormatFrameGenLine(
	const Accumulator& accumulator, const ForkFrameGen::Policy& policy, float generation_avg_ms,
	float generation_warm_avg_ms, float frametime_avg_ms)
{
	const ForkFrameGen::Reason dominant = accumulator.DominantReason();
	const u32 engaged = accumulator.frames_by_reason[static_cast<size_t>(ForkFrameGen::Reason::Engaged)];
	const float engaged_share =
		accumulator.frames > 0 ? (100.0f * static_cast<float>(engaged) / static_cast<float>(accumulator.frames)) : 0.0f;

	// O orçamento efetivo, e não o teto absoluto: ele depende do intervalo real, então imprimir o
	// teto faria o log mostrar um número contra o qual nada foi comparado.
	const float budget = (frametime_avg_ms > 0.0f)
							 ? std::min(policy.budget_ms, policy.budget_fraction * frametime_avg_ms)
							 : policy.budget_ms;

	// `gen_avg` (tudo) e `gen_warm` (só o regime) aparecem os DOIS de propósito. Foi comparar um
	// com o outro que revelou por que o recurso vivia recusando: 13 ms isolados contra 6,5 ms em
	// sequência. Com um número só, esse mecanismo é invisível no log.
	return fmt::format(
		"{} framegen  mode={} engaged={:.1f}% transitions={} dominant={} gen_avg={:.2f}ms gen_warm={:.2f}ms gen_worst={:.2f}ms budget={:.2f}ms speed_floor={:.0f}% fps_floor={:.1f}",
		PREFIX, ForkFrameGen::ModeToString(policy.mode), engaged_share, accumulator.transitions,
		ForkFrameGen::ReasonToString(dominant), generation_avg_ms, generation_warm_avg_ms,
		accumulator.worst_generation_ms, budget, policy.min_speed_percent, policy.min_real_fps);
}

std::string ForkDiagnostics::FormatGsWorkLine(const GsWork& work)
{
	// A razao cópias/draws vem impressa em vez de deixada para quem le: e ela que responde se a
	// realimentacao e marginal ou dominante, e uma conta de cabeca em cima de dois numeros no log
	// e exatamente o tipo de coisa que se erra num relato.
	const float por_mil = (work.draw_calls > 0)
							  ? (1000.0f * static_cast<float>(work.feedback_copies) / static_cast<float>(work.draw_calls))
							  : 0.0f;

	std::string line = fmt::format(
		"{} gswork    fb_copy_draws={} fb_copies={} fb_copies_per_1k_draws={:.1f} fb_px={:.0f}",
		PREFIX, work.feedback_copy_draws, work.feedback_copies, por_mil, work.feedback_copy_pixels);

	// `of=` deixa explicita a relacao de INCLUSAO: fb_copies e um subconjunto de tex_copies, nao
	// uma parcela a somar. Sem isso alguem soma os dois e conta a mesma copia duas vezes.
	line += fmt::format(" tex_copies={} (fb of tex) draws={} passes={}",
		work.texture_copies, work.draw_calls, work.render_passes);
	return line;
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
	// Os três juntos, e não a contagem sozinha: `shader_compiles=305 shader_src=0 shader_ms=1.2`
	// e `shader_compiles=305 shader_src=140 shader_ms=890.0` são situações opostas que o campo
	// único mostrava idênticas.
	line += fmt::format(" shader_compiles={} shader_src={} shader_ms={:.1f}", load.shader_compiles,
		load.shader_source_compiles, load.shader_ms);
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

	// `vk` e a contagem de defeitos entram aqui porque são o que separa um driver do outro em
	// termos de CUSTO, e é a comparação que a linha existe para permitir. Medido no Odin 2, mesma
	// GPU: o blob da Qualcomm expõe Vulkan 1.3.128 com 9 defeitos e 4 desvios; o Turnip expõe
	// 1.4.359 com 2 e 1. Sem estes campos, chegar a essa frase exigiu decodificar bitmask à mão
	// de uma linha do upstream que nem sempre está no mesmo lugar.
	//
	// A versão é desempacotada na mão, e não com `VK_API_VERSION_*`: este módulo compila no
	// harness de teste sem GPU nem headers do Vulkan, e puxar um header inteiro para três
	// deslocamentos de bits tornaria os testes dependentes do SDK.
	//
	// Bitmask em hexadecimal, e não a lista de nomes, por uma razão prática: a lista muda de
	// tamanho conforme o driver e quebraria o alinhamento de uma linha feita para ser lida aos
	// pares. O número é estável, curto, e `bugs=0x300` contra `bugs=0x18073a0` já responde a
	// pergunta antes de qualquer decodificação.
	return fmt::format(
		"{} identity  gpu='{}' turnip={} requested='{}' active={} unexpected={} mesa={} vk={}.{}.{} "
		"bugs=0x{:x} workarounds=0x{:x} rules={}({}) sha256={}",
		PREFIX, caps.gpu_name, ForkGpuCapabilities::TurnipSupportToString(caps.turnip),
		identity.requested_driver.empty() ? "system" : identity.requested_driver,
		GpuProfileDetector::DriverToString(identity.active_driver),
		ForkDriverIdentity::IsUnexpected(identity.outcome) ? "YES" : "no",
		identity.mesa.known ? fmt::format("{}.{}.{}", identity.mesa.major, identity.mesa.minor, identity.mesa.patch)
							: std::string("-"),
		caps.vulkan_api_version >> 22, (caps.vulkan_api_version >> 12) & 0x3FFu,
		caps.vulkan_api_version & 0xFFFu, caps.driver_bugs, caps.driver_workarounds,
		caps.driver_matched_rules, caps.driver_matched_rule_ids,
		identity.package_sha256.empty() ? std::string("-") : identity.package_sha256);
}

void ForkDiagnostics::Reset()
{
	std::lock_guard lock(s_mutex);
	s_accumulator = Accumulator{};
	s_window_start = 0;
	s_identity_written = false;
	s_last_fb_copy_draws = g_perfmon.GetCounter(GSPerfMon::FeedbackLoopCopyDraws);
	s_last_fb_copies = g_perfmon.GetCounter(GSPerfMon::FeedbackLoopCopies);
	s_last_fb_copy_pixels = g_perfmon.GetCounter(GSPerfMon::FeedbackLoopCopyPixels);
	s_last_tex_copies = g_perfmon.GetCounter(GSPerfMon::TextureCopies);
	s_last_draw_calls = g_perfmon.GetCounter(GSPerfMon::DrawCalls);
	s_last_render_passes = g_perfmon.GetCounter(GSPerfMon::RenderPasses);
	s_last_shader_compiles = GSShaderCompileIndicator::s_total_count.load(std::memory_order_relaxed);
	s_last_shader_source = GSShaderCompileIndicator::s_total_source_count.load(std::memory_order_relaxed);
	s_last_shader_time_ns = GSShaderCompileIndicator::s_total_time_ns.load(std::memory_order_relaxed);
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
	std::string gswork_line;
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

		// Só marca como escrita quando a identidade já foi SONDADA. O quadro em branco que o
		// GSDeviceVK apresenta durante a criação da swapchain chega aqui ANTES de
		// ForkDriverIdentity::Publish, e o bloco saía dizendo `active=Qualcomm proprietary` num
		// aparelho rodando Turnip — a linha mais importante do log, errada. Se ainda não sondou,
		// tenta de novo no próximo bloco.
		if (!s_identity_written && ForkDriverIdentity::Get().probed)
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

		const u32 source_now = GSShaderCompileIndicator::s_total_source_count.load(std::memory_order_relaxed);
		load.shader_source_compiles = source_now - s_last_shader_source;
		s_last_shader_source = source_now;

		const u64 time_now = GSShaderCompileIndicator::s_total_time_ns.load(std::memory_order_relaxed);
		load.shader_ms = static_cast<float>(time_now - s_last_shader_time_ns) / 1e6f;
		s_last_shader_time_ns = time_now;
		GsWork work;
		work.feedback_copy_draws = static_cast<u32>(PerfmonDelta(GSPerfMon::FeedbackLoopCopyDraws, s_last_fb_copy_draws));
		work.feedback_copies = static_cast<u32>(PerfmonDelta(GSPerfMon::FeedbackLoopCopies, s_last_fb_copies));
		work.feedback_copy_pixels = PerfmonDelta(GSPerfMon::FeedbackLoopCopyPixels, s_last_fb_copy_pixels);
		work.texture_copies = static_cast<u32>(PerfmonDelta(GSPerfMon::TextureCopies, s_last_tex_copies));
		work.draw_calls = static_cast<u32>(PerfmonDelta(GSPerfMon::DrawCalls, s_last_draw_calls));
		work.render_passes = static_cast<u32>(PerfmonDelta(GSPerfMon::RenderPasses, s_last_render_passes));

		load_line = FormatLoadLine(load);
		gswork_line = FormatGsWorkLine(work);
		real_line = FormatRealLine(snapshot);
		presented_line = FormatPresentedLine(snapshot);
		framegen_line = FormatFrameGenLine(s_accumulator, ForkFrameGen::PolicyFromConfig(),
			snapshot.generation_avg_ms, snapshot.generation_warm_avg_ms, snapshot.real_frametime_avg_ms);

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
	Console.WriteLn("%s", gswork_line.c_str());
	Console.WriteLn("%s", real_line.c_str());
	Console.WriteLn("%s", presented_line.c_str());
	Console.WriteLn("%s", framegen_line.c_str());
}
