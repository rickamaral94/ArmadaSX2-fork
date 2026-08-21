// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ForkPipelineCompiler
{
	using TaskId = u64;
	using Result = std::uintptr_t;

	enum class State : u8
	{
		Missing,
		Pending,
		Ready,
		Failed,
	};

	enum class SubmitResult : u8
	{
		Queued,
		Existing,
		NotRunning,
	};

	enum class GateReason : u8
	{
		Allowed,
		Disabled,
		UnsupportedPlatform,
		UnknownDriver,
		DriverSerialized,
	};

	struct Gate
	{
		bool allowed = false;
		u32 worker_count = 0;
		GateReason reason = GateReason::Disabled;
	};

	/// Gate puro: perfil conservador nega; defeito de compilação multithread reduz para um worker;
	/// drivers conhecidos ficam limitados a um pool pequeno. `requested_workers` nunca é obedecido
	/// sem passar por esta função.
	Gate ResolveGate(bool enabled, bool supported_platform, bool conservative_driver,
		bool broken_multithreaded_compilation, u32 requested_workers);
	const char* GateReasonToString(GateReason reason);

	/// Como o pool trata VkPipelineCache para um gate já resolvido.
	struct CachePlan
	{
		/// Quantos caches PRIVADOS criar. Sempre igual ao número de workers, inclusive quando o
		/// gate serializa para um só.
		u32 private_caches = 0;
		/// Semear cada cache privado com o blob do cache principal.
		bool seed_from_main = false;
		/// Mesclar os privados no principal no Quiesce, depois do join.
		bool merge_into_main = false;
	};

	/// Regra de propriedade dos caches, separada da integração Vulkan para poder ser testada.
	///
	/// O QUE ESTA FUNÇÃO EXISTE PARA IMPEDIR: a primeira versão entregava o cache PRINCIPAL ao
	/// worker quando o gate serializava para um, e criava privados só a partir de dois. Aquilo
	/// era seguro por coincidência — porque todas as outras criações de pipeline acontecem no
	/// init do dispositivo e o fallback síncrono é inalcançável com o pool ativo — e não por
	/// construção. `vkCreateGraphicsPipelines` exige sincronização externa do `pipelineCache`,
	/// então qualquer criação preguiçosa futura na thread GS viraria comportamento indefinido.
	///
	/// Semear existe pelo motivo oposto: um cache privado nasce FRIO e recompilaria o que o
	/// principal já sabe, fazendo a correção de segurança custar desempenho na exata medição que
	/// ela viabiliza. `pInitialData` resolve, e um blob incompatível é descartado pelo driver.
	CachePlan PlanCaches(const Gate& gate);

	struct Stats
	{
		u64 requests = 0;
		u64 cache_hits = 0;
		u64 queued = 0;
		u64 completed = 0;
		u64 parallel_compiles = 0;
		u64 waits_at_use = 0;
		u64 failures = 0;
		u64 cancelled = 0;
		u64 compile_time_ns = 0;
		u64 wait_time_ns = 0;
		u32 peak_active = 0;
	};

	struct Task
	{
		TaskId id = 0;
		u64 group_id = 0;
		/// Recebe o índice estável do worker. A integração Vulkan usa esse índice para escolher um
		/// VkPipelineCache privado, nunca compartilhado simultaneamente por duas threads.
		std::function<Result(u32)> compile;
	};

	/// Fila pequena e independente de Vulkan. O renderer conserva a propriedade dos handles; esta
	/// classe só coordena estados e devolve resultados opacos para a thread que os adota.
	class Queue final
	{
	public:
		Queue() = default;
		~Queue();

		Queue(const Queue&) = delete;
		Queue& operator=(const Queue&) = delete;

		bool Start(u32 worker_count);
		SubmitResult Submit(Task task);
		State GetState(TaskId id) const;

		/// Espera somente se a tarefa ainda está Pending e remove o resultado da fila. Zero significa
		/// falha/cancelamento; um resultado válido passa a pertencer ao chamador.
		Result WaitAndTake(TaskId id);

		/// Cancela o que ainda não iniciou, espera chamadas em curso e devolve resultados Ready ainda
		/// não adotados. Pode ser chamado repetidamente e permite um Start posterior.
		std::vector<Result> CancelAndJoin();

		Stats GetStats() const;
		bool IsRunning() const;

	private:
		struct Entry
		{
			State state = State::Missing;
			Result result = 0;
			std::function<Result(u32)> compile;
		};

		void WorkerMain(u32 worker_index);

		mutable std::mutex m_mutex;
		std::condition_variable m_work_cv;
		std::condition_variable m_state_cv;
		std::unordered_map<TaskId, Entry> m_entries;
		std::vector<std::deque<TaskId>> m_worker_queues;
		std::vector<std::thread> m_workers;
		Stats m_stats;
		u32 m_active = 0;
		bool m_accepting = false;
	};
} // namespace ForkPipelineCompiler
