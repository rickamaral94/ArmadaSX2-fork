// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkPipelineCompiler.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace ForkPipelineCompiler
{
	Gate ResolveGate(bool enabled, bool supported_platform, bool conservative_driver,
		bool broken_multithreaded_compilation, u32 requested_workers)
	{
		if (!enabled)
			return {false, 0, GateReason::Disabled};
		if (!supported_platform)
			return {false, 0, GateReason::UnsupportedPlatform};
		if (conservative_driver)
			return {false, 0, GateReason::UnknownDriver};

		if (broken_multithreaded_compilation)
			return {true, 1, GateReason::DriverSerialized};

		return {true, std::clamp(requested_workers, 1u, 2u), GateReason::Allowed};
	}

	const char* GateReasonToString(GateReason reason)
	{
		switch (reason)
		{
			case GateReason::Allowed: return "allowed";
			case GateReason::Disabled: return "disabled";
			case GateReason::UnsupportedPlatform: return "unsupported-platform";
			case GateReason::UnknownDriver: return "unknown-driver";
			case GateReason::DriverSerialized: return "driver-serialized";
		}
		return "unknown";
	}

	CachePlan PlanCaches(const Gate& gate)
	{
		if (!gate.allowed || gate.worker_count == 0)
			return {};

		// Um cache por worker, sempre. Serializado ou paralelo, o principal fica com a thread GS.
		return {gate.worker_count, true, true};
	}

	Queue::~Queue()
	{
		CancelAndJoin();
	}

	bool Queue::Start(u32 worker_count)
	{
		if (worker_count == 0)
			return false;

		std::lock_guard lock(m_mutex);
		if (m_accepting || !m_workers.empty())
			return false;

		m_worker_queues.clear();
		m_worker_queues.resize(worker_count);
		m_accepting = true;

		// O projeto compila sem exceções. std::thread é usado da mesma forma pelos demais workers
		// do núcleo; reserve evita realocar handles depois que as threads começam.
		m_workers.reserve(worker_count);
		for (u32 i = 0; i < worker_count; i++)
			m_workers.emplace_back(&Queue::WorkerMain, this, i);

		return true;
	}

	SubmitResult Queue::Submit(Task task)
	{
		std::lock_guard lock(m_mutex);
		m_stats.requests++;

		if (!m_accepting || m_worker_queues.empty() || !task.compile)
			return SubmitResult::NotRunning;

		if (m_entries.find(task.id) != m_entries.end())
		{
			m_stats.cache_hits++;
			return SubmitResult::Existing;
		}

		Entry entry;
		entry.state = State::Pending;
		entry.compile = std::move(task.compile);
		m_entries.emplace(task.id, std::move(entry));

		const size_t worker = static_cast<size_t>(task.group_id % m_worker_queues.size());
		m_worker_queues[worker].push_back(task.id);
		m_stats.queued++;
		m_work_cv.notify_all();
		return SubmitResult::Queued;
	}

	State Queue::GetState(TaskId id) const
	{
		std::lock_guard lock(m_mutex);
		const auto it = m_entries.find(id);
		return (it == m_entries.end()) ? State::Missing : it->second.state;
	}

	Result Queue::WaitAndTake(TaskId id)
	{
		std::unique_lock lock(m_mutex);
		auto it = m_entries.find(id);
		if (it == m_entries.end())
			return 0;

		const bool must_wait = (it->second.state == State::Pending);
		const auto wait_start = std::chrono::steady_clock::now();
		if (must_wait)
		{
			m_stats.waits_at_use++;
			m_state_cv.wait(lock, [&]() {
				const auto current = m_entries.find(id);
				return current == m_entries.end() || current->second.state != State::Pending;
			});
			m_stats.wait_time_ns += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - wait_start).count());
		}

		it = m_entries.find(id);
		if (it == m_entries.end())
			return 0;

		const Result result = (it->second.state == State::Ready) ? it->second.result : 0;
		m_entries.erase(it);
		return result;
	}

	std::vector<Result> Queue::CancelAndJoin()
	{
		{
			std::lock_guard lock(m_mutex);
			m_accepting = false;
			for (std::deque<TaskId>& worker_queue : m_worker_queues)
			{
				for (const TaskId id : worker_queue)
				{
					auto it = m_entries.find(id);
					if (it != m_entries.end() && it->second.state == State::Pending)
					{
						it->second.state = State::Failed;
						it->second.compile = {};
						m_stats.cancelled++;
					}
				}
				worker_queue.clear();
			}
		}

		m_work_cv.notify_all();
		m_state_cv.notify_all();
		for (std::thread& worker : m_workers)
		{
			if (worker.joinable())
				worker.join();
		}

		std::vector<Result> ready;
		{
			std::lock_guard lock(m_mutex);
			for (const auto& [id, entry] : m_entries)
			{
				if (entry.state == State::Ready && entry.result != 0)
					ready.push_back(entry.result);
			}
			m_entries.clear();
			m_worker_queues.clear();
			m_workers.clear();
			m_active = 0;
		}
		m_state_cv.notify_all();
		return ready;
	}

	Stats Queue::GetStats() const
	{
		std::lock_guard lock(m_mutex);
		return m_stats;
	}

	bool Queue::IsRunning() const
	{
		std::lock_guard lock(m_mutex);
		return m_accepting;
	}

	void Queue::WorkerMain(u32 worker_index)
	{
		for (;;)
		{
			TaskId id = 0;
			std::function<Result(u32)> compile;
			{
				std::unique_lock lock(m_mutex);
				m_work_cv.wait(lock, [&]() {
					return !m_accepting || !m_worker_queues[worker_index].empty();
				});

				if (m_worker_queues[worker_index].empty())
				{
					if (!m_accepting)
						return;
					continue;
				}

				id = m_worker_queues[worker_index].front();
				m_worker_queues[worker_index].pop_front();
				auto it = m_entries.find(id);
				if (it == m_entries.end() || it->second.state != State::Pending || !it->second.compile)
					continue;

				compile = std::move(it->second.compile);
				m_active++;
				m_stats.peak_active = std::max(m_stats.peak_active, m_active);
				if (m_active > 1)
					m_stats.parallel_compiles++;
			}

			const auto compile_start = std::chrono::steady_clock::now();
			const Result result = compile(worker_index);
			const u64 compile_ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - compile_start).count());

			{
				std::lock_guard lock(m_mutex);
				m_active--;
				m_stats.compile_time_ns += compile_ns;
				m_stats.completed++;

				auto it = m_entries.find(id);
				if (it != m_entries.end() && it->second.state == State::Pending)
				{
					it->second.result = result;
					it->second.state = result ? State::Ready : State::Failed;
					if (!result)
						m_stats.failures++;
				}
			}
			m_state_cv.notify_all();
		}
	}
} // namespace ForkPipelineCompiler
