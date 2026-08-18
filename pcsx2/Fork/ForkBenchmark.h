// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>
#include <string_view>
#include <vector>

/// Framework A/B: mede execuções nomeadas e as compara.
///
/// O projeto decide driver e frame generation **por evidência**, e evidência exige que as duas
/// medições sejam comparáveis. Três coisas tornam um A/B inválido, e todas acontecem sem aviso:
///
///  1. **medir o driver errado.** O fallback do carregador é silencioso, então "Turnip A" pode ter
///     rodado no driver do sistema. Cada execução grava a identidade REAL e o SHA-256 do pacote.
///  2. **medir compilação em vez de regime.** A primeira execução de um driver compila shaders; a
///     segunda não. Por isso o tempo de compilação é medido e reportado separado — se ele for
///     grande, a execução não vale como medida de desempenho.
///  3. **misturar FPS real com FPS apresentado.** Frame generation muda um e não o outro. Os dois
///     são campos distintos, do começo ao fim, e a comparação os apresenta em linhas separadas.
namespace ForkBenchmark
{
	/// Uma execução medida.
	struct Run
	{
		std::string label; ///< "System", "Turnip A", "FG Off"...
		double duration_seconds = 0.0;

		// --- o que foi medido ---
		float real_fps = 0.0f;
		float presented_fps = 0.0f;
		float frametime_avg_ms = 0.0f;
		float frametime_min_ms = 0.0f;
		float frametime_max_ms = 0.0f;
		float frametime_p95_ms = 0.0f;
		float frametime_p99_ms = 0.0f;
		float frametime_low1_ms = 0.0f;
		float low1_fps = 0.0f;
		u64 stutter_count = 0;
		u64 real_frames = 0;
		u64 duplicated_frames = 0;
		u64 generated_frames = 0;
		u64 skipped_presents = 0;
		u64 present_errors = 0;
		float generation_avg_ms = 0.0f;

		/// Shaders compilados DURANTE a execução, por diferença de contadores cumulativos.
		u32 shader_compiles = 0;
		double shader_compile_ms = 0.0;

		// --- em que condições ---
		std::string gpu;
		std::string driver; ///< o que rodou de fato
		std::string driver_outcome; ///< veredito do carregamento
		std::string mesa_version;
		std::string vulkan_version;
		std::string package_sha256;
		bool driver_as_requested = true; ///< false quando houve fallback ou ICD inesperado
	};

	/// Começa a medir sob um rótulo. Reinicia se já houver uma em andamento.
	void Begin(std::string_view label);
	/// Encerra, guarda o resultado e o devolve.
	Run End();
	bool IsRunning();
	/// Rótulo em andamento, vazio quando não há execução.
	std::string CurrentLabel();

	const std::vector<Run>& GetRuns();
	void ClearRuns();

	// --- comparação e exportação ---

	/// Linhas de comparação entre duas execuções. FPS real e FPS apresentado **nunca** aparecem na
	/// mesma linha: são perguntas diferentes, e juntá-las é como se esconde emulação lenta.
	std::vector<std::string> CompareLines(const Run& baseline, const Run& candidate);

	/// Motivos pelos quais a comparação NÃO é confiável, vazio quando está tudo bem. Existe porque
	/// um A/B inválido é pior que nenhum: ele produz um número que alguém vai citar.
	std::vector<std::string> ValidityWarnings(const Run& baseline, const Run& candidate);

	std::string ToJson();
	std::string ToCsv();
} // namespace ForkBenchmark
