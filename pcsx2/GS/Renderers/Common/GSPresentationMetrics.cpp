// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Common/GSPresentationMetrics.h"

#include "common/Timer.h"

#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace
{
	/// Largura da janela deslizante. 1 s é o mesmo horizonte que o usuário vê no overlay, então o
	/// número exibido e o número medido são o mesmo número.
	constexpr double WINDOW_SECONDS = 1.0;

	/// Teto de amostras retidas. 512 cobre a janela até ~500 Hz de apresentação, muito além de
	/// qualquer tela; passando disso as mais antigas caem, que é o comportamento correto para
	/// uma janela deslizante.
	constexpr size_t MAX_SAMPLES = 512;

	/// Um intervalo entre quadros REAIS acima deste múltiplo da mediana conta como engasgo.
	constexpr double STUTTER_FACTOR = 2.0;

	struct PresentedSample
	{
		Common::Timer::Value at;
		GSPresentationMetrics::FrameKind kind;
	};

	struct State
	{
		std::mutex mutex;

		std::vector<PresentedSample> presented;
		/// Intervalos entre quadros reais consecutivos, em ms, alinhados com o instante do quadro
		/// que os terminou (para poder expirar junto com a janela).
		std::vector<std::pair<Common::Timer::Value, double>> real_intervals;

		Common::Timer::Value last_real_at = 0;

		std::vector<std::pair<Common::Timer::Value, double>> present_calls;
		std::vector<std::pair<Common::Timer::Value, double>> generation_costs;
		std::vector<Common::Timer::Value> skipped;
		std::vector<Common::Timer::Value> errors;

		State()
		{
			// Reservado uma vez. O caminho quente não pode alocar: uma realocação no meio do
			// present é exatamente o engasgo que este módulo existe para medir.
			presented.reserve(MAX_SAMPLES);
			real_intervals.reserve(MAX_SAMPLES);
			present_calls.reserve(MAX_SAMPLES);
			generation_costs.reserve(MAX_SAMPLES);
			skipped.reserve(MAX_SAMPLES);
			errors.reserve(MAX_SAMPLES);
		}
	};

	std::atomic<bool> s_enabled{false};
	State s_state;

	/// nullptr = relógio real. Só o teste escreve aqui, e antes de qualquer registro.
	GSPresentationMetrics::Detail::ClockFn s_clock = nullptr;

	Common::Timer::Value Now()
	{
		return s_clock ? s_clock() : Common::Timer::GetCurrentValue();
	}

	/// Descarta o que saiu da janela. `now` é passado para que todos os cortes de uma mesma
	/// chamada usem o mesmo instante.
	template <typename T, typename GetTime>
	void ExpireOlderThan(std::vector<T>& samples, Common::Timer::Value cutoff, GetTime get_time)
	{
		const auto first_kept = std::find_if(samples.begin(), samples.end(),
			[&](const T& sample) { return get_time(sample) >= cutoff; });
		if (first_kept != samples.begin())
			samples.erase(samples.begin(), first_kept);
	}

	void ExpireAll(Common::Timer::Value now)
	{
		const Common::Timer::Value window = Common::Timer::ConvertSecondsToValue(WINDOW_SECONDS);
		const Common::Timer::Value cutoff = (now > window) ? (now - window) : 0;

		ExpireOlderThan(s_state.presented, cutoff, [](const PresentedSample& s) { return s.at; });
		ExpireOlderThan(s_state.real_intervals, cutoff, [](const auto& s) { return s.first; });
		ExpireOlderThan(s_state.present_calls, cutoff, [](const auto& s) { return s.first; });
		ExpireOlderThan(s_state.generation_costs, cutoff, [](const auto& s) { return s.first; });
		ExpireOlderThan(s_state.skipped, cutoff, [](const auto& s) { return s; });
		ExpireOlderThan(s_state.errors, cutoff, [](const auto& s) { return s; });
	}

	/// Empurra respeitando o teto, descartando a mais antiga. Nunca realoca depois do reserve.
	template <typename T>
	void PushCapped(std::vector<T>& samples, T value)
	{
		if (samples.size() >= MAX_SAMPLES)
			samples.erase(samples.begin());
		samples.push_back(std::move(value));
	}

	/// Média do 1% pior (pelo menos uma amostra) — a definição usual de "1% low".
	double Worst1PercentMean(std::vector<double> values)
	{
		if (values.empty())
			return 0.0;
		const size_t count = std::max<size_t>(1, values.size() / 100);
		std::partial_sort(values.begin(), values.begin() + count, values.end(), std::greater<double>());
		double total = 0.0;
		for (size_t i = 0; i < count; i++)
			total += values[i];
		return total / static_cast<double>(count);
	}

	double Median(std::vector<double> values)
	{
		if (values.empty())
			return 0.0;
		const size_t middle = values.size() / 2;
		std::nth_element(values.begin(), values.begin() + middle, values.end());
		return values[middle];
	}

	float ToFps(double frametime_ms)
	{
		return (frametime_ms > 0.0) ? static_cast<float>(1000.0 / frametime_ms) : 0.0f;
	}
} // namespace

void GSPresentationMetrics::SetEnabled(bool enabled)
{
	if (s_enabled.exchange(enabled, std::memory_order_release) == enabled)
		return;

	// Trocar de estado limpa a janela nos dois sentidos: dados de antes de desligar não descrevem
	// o que está acontecendo depois de ligar de novo.
	Reset();
}

bool GSPresentationMetrics::IsEnabled()
{
	return s_enabled.load(std::memory_order_acquire);
}

void GSPresentationMetrics::Reset()
{
	std::lock_guard lock(s_state.mutex);
	s_state.presented.clear();
	s_state.real_intervals.clear();
	s_state.present_calls.clear();
	s_state.generation_costs.clear();
	s_state.skipped.clear();
	s_state.errors.clear();
	s_state.last_real_at = 0;
}

void GSPresentationMetrics::NotePresented(FrameKind kind)
{
	if (!IsEnabled())
		return;

	const Common::Timer::Value now = Now();
	std::lock_guard lock(s_state.mutex);

	PushCapped(s_state.presented, PresentedSample{now, kind});

	if (kind == FrameKind::Real)
	{
		if (s_state.last_real_at != 0 && now > s_state.last_real_at)
		{
			const double interval_ms = Common::Timer::ConvertValueToMilliseconds(now - s_state.last_real_at);
			PushCapped(s_state.real_intervals, std::make_pair(now, interval_ms));
		}
		s_state.last_real_at = now;
	}

	ExpireAll(now);
}

void GSPresentationMetrics::NoteSkippedPresent()
{
	if (!IsEnabled())
		return;

	const Common::Timer::Value now = Now();
	std::lock_guard lock(s_state.mutex);
	PushCapped(s_state.skipped, now);
	ExpireAll(now);
}

void GSPresentationMetrics::NotePresentCall(double ms, bool ok)
{
	if (!IsEnabled())
		return;

	const Common::Timer::Value now = Now();
	std::lock_guard lock(s_state.mutex);
	PushCapped(s_state.present_calls, std::make_pair(now, ms));
	if (!ok)
		PushCapped(s_state.errors, now);
	ExpireAll(now);
}

void GSPresentationMetrics::NoteGenerationCost(double ms)
{
	if (!IsEnabled())
		return;

	const Common::Timer::Value now = Now();
	std::lock_guard lock(s_state.mutex);
	PushCapped(s_state.generation_costs, std::make_pair(now, ms));
	ExpireAll(now);
}

GSPresentationMetrics::Snapshot GSPresentationMetrics::GetSnapshot()
{
	Snapshot out = {};
	if (!IsEnabled())
		return out;

	const Common::Timer::Value now = Now();
	std::lock_guard lock(s_state.mutex);
	ExpireAll(now);

	for (const PresentedSample& sample : s_state.presented)
	{
		switch (sample.kind)
		{
			case FrameKind::Real:
				out.real_frames++;
				break;
			case FrameKind::Duplicate:
				out.duplicated_frames++;
				break;
			case FrameKind::Generated:
				out.generated_frames++;
				break;
		}
	}

	const u64 presented_total = out.real_frames + out.duplicated_frames + out.generated_frames;
	out.real_fps = static_cast<float>(static_cast<double>(out.real_frames) / WINDOW_SECONDS);
	out.presented_fps = static_cast<float>(static_cast<double>(presented_total) / WINDOW_SECONDS);
	out.skipped_presents = s_state.skipped.size();
	out.present_errors = s_state.errors.size();

	if (!s_state.real_intervals.empty())
	{
		std::vector<double> intervals;
		intervals.reserve(s_state.real_intervals.size());
		double total = 0.0;
		double lowest = s_state.real_intervals.front().second;
		double highest = lowest;
		for (const auto& [at, ms] : s_state.real_intervals)
		{
			intervals.push_back(ms);
			total += ms;
			lowest = std::min(lowest, ms);
			highest = std::max(highest, ms);
		}

		const double count = static_cast<double>(intervals.size());
		out.real_frametime_avg_ms = static_cast<float>(total / count);
		out.real_frametime_min_ms = static_cast<float>(lowest);
		out.real_frametime_max_ms = static_cast<float>(highest);

		const double low1 = Worst1PercentMean(intervals);
		out.real_frametime_low1_ms = static_cast<float>(low1);
		out.real_low1_fps = ToFps(low1);

		const double median = Median(intervals);
		if (median > 0.0)
		{
			const double threshold = median * STUTTER_FACTOR;
			for (const double ms : intervals)
			{
				if (ms > threshold)
					out.stutter_count++;
			}
		}
	}

	if (!s_state.present_calls.empty())
	{
		double total = 0.0;
		double highest = 0.0;
		for (const auto& [at, ms] : s_state.present_calls)
		{
			total += ms;
			highest = std::max(highest, ms);
		}
		out.present_call_avg_ms = static_cast<float>(total / static_cast<double>(s_state.present_calls.size()));
		out.present_call_max_ms = static_cast<float>(highest);
	}

	if (!s_state.generation_costs.empty())
	{
		double total = 0.0;
		for (const auto& [at, ms] : s_state.generation_costs)
			total += ms;
		out.generation_avg_ms =
			static_cast<float>(total / static_cast<double>(s_state.generation_costs.size()));
	}

	return out;
}

std::string GSPresentationMetrics::GetOverlayLine()
{
	if (!IsEnabled())
		return {};

	const Snapshot snap = GetSnapshot();
	// Real e apresentado sempre lado a lado e rotulados. Um número solto de "FPS" no overlay é
	// justamente o que permite confundir suavidade aparente com velocidade de emulação.
	std::string line = fmt::format("Real {:.1f} | Apresentado {:.1f} | frametime {:.2f} ms (1% low {:.1f})",
		snap.real_fps, snap.presented_fps, snap.real_frametime_avg_ms, snap.real_low1_fps);

	if (snap.generated_frames > 0)
		line += fmt::format(" | gerados {} ({:.2f} ms)", snap.generated_frames, snap.generation_avg_ms);
	if (snap.duplicated_frames > 0)
		line += fmt::format(" | repetidos {}", snap.duplicated_frames);
	if (snap.skipped_presents > 0)
		line += fmt::format(" | pulados {}", snap.skipped_presents);
	if (snap.stutter_count > 0)
		line += fmt::format(" | engasgos {}", snap.stutter_count);

	return line;
}

void GSPresentationMetrics::AppendStatLines(std::vector<std::string>& out)
{
	if (!IsEnabled())
		return;

	const Snapshot snap = GetSnapshot();
	out.push_back(fmt::format("Apresentação: FPS real {:.2f} | FPS apresentado {:.2f}",
		snap.real_fps, snap.presented_fps));
	out.push_back(fmt::format("Frametime real: méd {:.2f} ms | mín {:.2f} ms | máx {:.2f} ms | "
							  "1% low {:.2f} ms ({:.1f} FPS)",
		snap.real_frametime_avg_ms, snap.real_frametime_min_ms, snap.real_frametime_max_ms,
		snap.real_frametime_low1_ms, snap.real_low1_fps));
	out.push_back(fmt::format("Quadros: reais {} | repetidos {} | gerados {} | presents pulados {} | engasgos {}",
		snap.real_frames, snap.duplicated_frames, snap.generated_frames, snap.skipped_presents,
		snap.stutter_count));
	out.push_back(fmt::format("Custo: present méd {:.3f} ms | máx {:.3f} ms | geração méd {:.3f} ms | erros {}",
		snap.present_call_avg_ms, snap.present_call_max_ms, snap.generation_avg_ms, snap.present_errors));
}

void GSPresentationMetrics::Detail::SetClockForTesting(ClockFn fn)
{
	s_clock = fn;
	Reset();
}
