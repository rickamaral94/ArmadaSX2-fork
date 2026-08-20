// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"
#include "common/Timer.h"

#include <atomic>

namespace GSShaderCompileIndicator
{
	inline constexpr u64 RECENT_COMPILE_HOLD_NS = 1'500'000'000ULL;

	inline std::atomic<u32> s_count{0};
	inline std::atomic<u64> s_time_ns{0};
	inline std::atomic<u64> s_last_time{0};

	/// Totais que NUNCA zeram. Os contadores acima existem para o indicador de tela e são zerados
	/// entre rajadas, o que os torna inúteis para contabilidade: o benchmark da Fase 6 mede
	/// "tempo de compilação de shader" ao longo de uma execução inteira, por diferença entre
	/// início e fim. Custo: dois incrementos relaxed por shader compilado, algo que já leva
	/// milissegundos.
	inline std::atomic<u32> s_total_count{0};
	inline std::atomic<u64> s_total_time_ns{0};

	/// @@ARMSX2_SHADER_KIND@@ O subconjunto que é compilação de FONTE (GLSL -> SPIR-V), separado
	/// da criação de pipeline.
	///
	/// O contador único somava as duas, e elas não se parecem em nada: compilar fonte custa
	/// dezenas de milissegundos e é o que trava um quadro; criar pipeline com o cache do driver
	/// quente custa microssegundos. Lidos juntos, `shader_compiles=305` num boot com cache quente
	/// parece um problema grave e não é — foi o que me fez atribuir engasgos à compilação em
	/// janelas onde ela não custava nada.
	///
	/// Medido nas alphas: os primeiros 2 min do mesmo jogo caíram de 556 para 305 conforme o
	/// cache de SPIR-V esquentou, e pararam ali. Os 305 que sobram são criação de pipeline, não
	/// compilação — e é por isso que precisam de nome próprio.
	inline std::atomic<u32> s_total_source_count{0};
	inline std::atomic<u64> s_total_source_time_ns{0};

	inline u64 GetRecentCompileHold()
	{
		static const u64 hold = static_cast<u64>(Common::Timer::ConvertNanosecondsToValue(static_cast<double>(RECENT_COMPILE_HOLD_NS)));
		return hold;
	}

	inline void OnCompileDone(u64 duration_ns, u64 start_time)
	{
		const u64 now = Common::Timer::GetCurrentValue();
		const u64 last = s_last_time.load(std::memory_order_relaxed);
		if (last != 0 && start_time > last && (start_time - last) >= GetRecentCompileHold())
		{
			s_count.store(0, std::memory_order_relaxed);
			s_time_ns.store(0, std::memory_order_relaxed);
		}

		s_count.fetch_add(1, std::memory_order_relaxed);
		s_time_ns.fetch_add(duration_ns, std::memory_order_relaxed);
		s_total_count.fetch_add(1, std::memory_order_relaxed);
		s_total_time_ns.fetch_add(duration_ns, std::memory_order_relaxed);
		s_last_time.store(now, std::memory_order_relaxed);
	}

	/// Variante para o caminho de compilação de FONTE. Alimenta os mesmos totais — para nada que
	/// já lia `s_total_count` mudar de significado — e mais o par específico.
	inline void OnSourceCompileDone(u64 duration_ns, u64 start_time)
	{
		OnCompileDone(duration_ns, start_time);
		s_total_source_count.fetch_add(1, std::memory_order_relaxed);
		s_total_source_time_ns.fetch_add(duration_ns, std::memory_order_relaxed);
	}

	inline u32 GetCount()
	{
		return s_count.load(std::memory_order_relaxed);
	}

	inline u32 GetTimeMs()
	{
		const u64 time_ns = s_time_ns.load(std::memory_order_relaxed);
		const u32 ms = static_cast<u32>(time_ns / 1000000);
		if (ms > 0)
			return ms;

		return GetCount() > 0 ? 1u : 0u;
	}

	inline bool IsVisible()
	{
		if (GetCount() == 0)
			return false;

		const u64 last = s_last_time.load(std::memory_order_relaxed);
		if (last == 0)
			return false;

		return (Common::Timer::GetCurrentValue() - last) < GetRecentCompileHold();
	}

	inline float GetFadeAlpha()
	{
		const u64 last = s_last_time.load(std::memory_order_relaxed);
		if (last == 0)
			return 0.0f;

		const u64 now = Common::Timer::GetCurrentValue();
		if (now <= last)
			return 1.0f;

		const u64 hold = GetRecentCompileHold();
		const u64 elapsed = now - last;
		if (elapsed >= hold)
			return 0.0f;

		return 1.0f - static_cast<float>(elapsed) / static_cast<float>(hold);
	}

	struct CompileTimer
	{
		Common::Timer timer;
		/// True no caminho de compilação de fonte. Falso (o padrão) na criação de pipeline, para
		/// que todos os pontos de medição existentes continuem valendo sem alteração.
		bool source = false;

		CompileTimer() = default;
		explicit CompileTimer(bool is_source)
			: source(is_source)
		{
		}

		~CompileTimer()
		{
			if (source)
				OnSourceCompileDone(static_cast<u64>(timer.GetTimeNanoseconds()), timer.GetStartValue());
			else
				OnCompileDone(static_cast<u64>(timer.GetTimeNanoseconds()), timer.GetStartValue());
		}

		CompileTimer(const CompileTimer&) = delete;
		CompileTimer& operator=(const CompileTimer&) = delete;
	};
} // namespace GSShaderCompileIndicator
