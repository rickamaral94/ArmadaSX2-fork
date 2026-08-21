// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Fork/ForkPipelineCompiler.h"

#include "gtest/gtest.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace std::chrono_literals;

TEST(ForkPipelineCompiler, CapabilityGateIsConservativeAndBounded)
{
	using ForkPipelineCompiler::GateReason;

	auto gate = ForkPipelineCompiler::ResolveGate(false, true, false, false, 2);
	EXPECT_FALSE(gate.allowed);
	EXPECT_EQ(gate.reason, GateReason::Disabled);

	gate = ForkPipelineCompiler::ResolveGate(true, false, false, false, 2);
	EXPECT_FALSE(gate.allowed);
	EXPECT_EQ(gate.reason, GateReason::UnsupportedPlatform);

	gate = ForkPipelineCompiler::ResolveGate(true, true, true, false, 2);
	EXPECT_FALSE(gate.allowed);
	EXPECT_EQ(gate.reason, GateReason::UnknownDriver);

	gate = ForkPipelineCompiler::ResolveGate(true, true, false, true, 2);
	EXPECT_TRUE(gate.allowed);
	EXPECT_EQ(gate.worker_count, 1u);
	EXPECT_EQ(gate.reason, GateReason::DriverSerialized);

	gate = ForkPipelineCompiler::ResolveGate(true, true, false, false, 99);
	EXPECT_TRUE(gate.allowed);
	EXPECT_EQ(gate.worker_count, 2u);
	EXPECT_EQ(gate.reason, GateReason::Allowed);
}

TEST(ForkPipelineCompiler, StateTransitionsAndTake)
{
	ForkPipelineCompiler::Queue queue;
	ASSERT_TRUE(queue.Start(1));
	EXPECT_EQ(queue.GetState(7), ForkPipelineCompiler::State::Missing);

	std::promise<void> entered;
	std::promise<void> release;
	std::shared_future<void> release_future = release.get_future().share();
	ASSERT_EQ(queue.Submit({7, 3, [&](u32 worker) {
		EXPECT_EQ(worker, 0u);
		entered.set_value();
		release_future.wait();
		return ForkPipelineCompiler::Result{0x1234};
	}}), ForkPipelineCompiler::SubmitResult::Queued);

	entered.get_future().wait();
	EXPECT_EQ(queue.GetState(7), ForkPipelineCompiler::State::Pending);
	release.set_value();
	for (u32 i = 0; i < 1000 && queue.GetState(7) == ForkPipelineCompiler::State::Pending; i++)
		std::this_thread::sleep_for(1ms);
	EXPECT_EQ(queue.GetState(7), ForkPipelineCompiler::State::Ready);
	EXPECT_EQ(queue.WaitAndTake(7), ForkPipelineCompiler::Result{0x1234});
	EXPECT_EQ(queue.GetState(7), ForkPipelineCompiler::State::Missing);

	const ForkPipelineCompiler::Stats stats = queue.GetStats();
	EXPECT_EQ(stats.requests, 1u);
	EXPECT_EQ(stats.queued, 1u);
	EXPECT_EQ(stats.completed, 1u);
	EXPECT_EQ(stats.failures, 0u);
}

TEST(ForkPipelineCompiler, DuplicatePendingTaskIsNotCompiledTwice)
{
	ForkPipelineCompiler::Queue queue;
	ASSERT_TRUE(queue.Start(1));
	std::atomic<u32> calls{0};

	ASSERT_EQ(queue.Submit({11, 0, [&](u32) {
		calls.fetch_add(1, std::memory_order_relaxed);
		return ForkPipelineCompiler::Result{99};
	}}), ForkPipelineCompiler::SubmitResult::Queued);
	EXPECT_EQ(queue.Submit({11, 0, [&](u32) {
		calls.fetch_add(1, std::memory_order_relaxed);
		return ForkPipelineCompiler::Result{100};
	}}), ForkPipelineCompiler::SubmitResult::Existing);

	EXPECT_EQ(queue.WaitAndTake(11), ForkPipelineCompiler::Result{99});
	EXPECT_EQ(calls.load(std::memory_order_relaxed), 1u);
	EXPECT_EQ(queue.GetStats().cache_hits, 1u);
}

TEST(ForkPipelineCompiler, FailureIsObservableAndRemovable)
{
	ForkPipelineCompiler::Queue queue;
	ASSERT_TRUE(queue.Start(1));
	ASSERT_EQ(queue.Submit({17, 0, [](u32) { return ForkPipelineCompiler::Result{0}; }}),
		ForkPipelineCompiler::SubmitResult::Queued);

	for (u32 i = 0; i < 1000 && queue.GetState(17) == ForkPipelineCompiler::State::Pending; i++)
		std::this_thread::sleep_for(1ms);
	EXPECT_EQ(queue.GetState(17), ForkPipelineCompiler::State::Failed);
	EXPECT_EQ(queue.WaitAndTake(17), ForkPipelineCompiler::Result{0});
	EXPECT_EQ(queue.GetState(17), ForkPipelineCompiler::State::Missing);
	EXPECT_EQ(queue.GetStats().failures, 1u);
}

TEST(ForkPipelineCompiler, DifferentGroupsCanRunConcurrently)
{
	ForkPipelineCompiler::Queue queue;
	ASSERT_TRUE(queue.Start(2));
	std::atomic<u32> entered{0};
	std::promise<void> release;
	std::shared_future<void> release_future = release.get_future().share();
	auto compile = [&](u32) {
		entered.fetch_add(1, std::memory_order_release);
		release_future.wait();
		return ForkPipelineCompiler::Result{1};
	};

	ASSERT_EQ(queue.Submit({21, 0, compile}), ForkPipelineCompiler::SubmitResult::Queued);
	ASSERT_EQ(queue.Submit({22, 1, compile}), ForkPipelineCompiler::SubmitResult::Queued);
	for (u32 i = 0; i < 1000 && entered.load(std::memory_order_acquire) != 2; i++)
		std::this_thread::sleep_for(1ms);
	EXPECT_EQ(entered.load(std::memory_order_acquire), 2u);
	release.set_value();
	EXPECT_EQ(queue.WaitAndTake(21), ForkPipelineCompiler::Result{1});
	EXPECT_EQ(queue.WaitAndTake(22), ForkPipelineCompiler::Result{1});
	EXPECT_EQ(queue.GetStats().peak_active, 2u);
	EXPECT_GE(queue.GetStats().parallel_compiles, 1u);
}

TEST(ForkPipelineCompiler, CancelDropsQueuedWorkAndJoinsRunningWork)
{
	ForkPipelineCompiler::Queue queue;
	ASSERT_TRUE(queue.Start(1));
	std::promise<void> entered;
	std::promise<void> release;
	std::shared_future<void> release_future = release.get_future().share();
	std::atomic<bool> queued_ran{false};

	ASSERT_EQ(queue.Submit({31, 0, [&](u32) {
		entered.set_value();
		release_future.wait();
		return ForkPipelineCompiler::Result{0xCAFE};
	}}), ForkPipelineCompiler::SubmitResult::Queued);
	ASSERT_EQ(queue.Submit({32, 0, [&](u32) {
		queued_ran.store(true, std::memory_order_relaxed);
		return ForkPipelineCompiler::Result{0xBAD};
	}}), ForkPipelineCompiler::SubmitResult::Queued);
	entered.get_future().wait();

	auto shutdown = std::async(std::launch::async, [&]() { return queue.CancelAndJoin(); });
	EXPECT_EQ(shutdown.wait_for(10ms), std::future_status::timeout);
	release.set_value();
	const std::vector<ForkPipelineCompiler::Result> orphaned = shutdown.get();

	ASSERT_EQ(orphaned.size(), 1u);
	EXPECT_EQ(orphaned.front(), ForkPipelineCompiler::Result{0xCAFE});
	EXPECT_FALSE(queued_ran.load(std::memory_order_relaxed));
	EXPECT_FALSE(queue.IsRunning());
	EXPECT_EQ(queue.GetState(31), ForkPipelineCompiler::State::Missing);
	EXPECT_EQ(queue.GetStats().cancelled, 1u);
}

TEST(ForkPipelineCompiler, ShutdownCanBeRestarted)
{
	ForkPipelineCompiler::Queue queue;
	ASSERT_TRUE(queue.Start(1));
	EXPECT_TRUE(queue.CancelAndJoin().empty());
	ASSERT_TRUE(queue.Start(1));
	ASSERT_EQ(queue.Submit({41, 0, [](u32) { return ForkPipelineCompiler::Result{5}; }}),
		ForkPipelineCompiler::SubmitResult::Queued);
	EXPECT_EQ(queue.WaitAndTake(41), ForkPipelineCompiler::Result{5});
}

// --- propriedade dos caches ------------------------------------------------------------------
//
// Este par de testes REPROVA o desenho anterior, e é por isso que existe. A primeira versão
// entregava o cache PRINCIPAL ao worker quando o gate serializava para um, criando privados só a
// partir de dois — e então não mesclava nada, porque não havia o que mesclar. Aquilo era seguro
// por coincidência (todas as outras criações de pipeline são de init, e o fallback síncrono é
// inalcançável com o pool ativo), não por construção, e `vkCreateGraphicsPipelines` exige
// sincronização externa do pipelineCache.
TEST(ForkPipelineCompiler, SerialGateStillGetsItsOwnPrivateCache)
{
	using ForkPipelineCompiler::CachePlan;
	using ForkPipelineCompiler::Gate;
	using ForkPipelineCompiler::GateReason;
	using ForkPipelineCompiler::PlanCaches;
	using ForkPipelineCompiler::ResolveGate;

	const Gate serial = ResolveGate(true, true, false, true, 4);
	ASSERT_TRUE(serial.allowed);
	ASSERT_EQ(serial.worker_count, 1u);
	ASSERT_EQ(serial.reason, GateReason::DriverSerialized);

	const CachePlan plan = PlanCaches(serial);
	// Um, não zero: o cache principal nunca é entregue a um worker.
	EXPECT_EQ(plan.private_caches, 1u);
	// Frio custaria recompilar o que o principal já sabe, na medição que a fase viabiliza.
	EXPECT_TRUE(plan.seed_from_main);
	// Sem mescla, tudo que o worker compilou morreria com o pool.
	EXPECT_TRUE(plan.merge_into_main);
}

TEST(ForkPipelineCompiler, CachePlanMatchesWorkerCountAndIsEmptyWhenDenied)
{
	using ForkPipelineCompiler::CachePlan;
	using ForkPipelineCompiler::Gate;
	using ForkPipelineCompiler::PlanCaches;
	using ForkPipelineCompiler::ResolveGate;

	const Gate parallel = ResolveGate(true, true, false, false, 2);
	ASSERT_TRUE(parallel.allowed);
	EXPECT_EQ(PlanCaches(parallel).private_caches, parallel.worker_count);

	// Gate negado não cria, não semeia e não mescla nada.
	for (const Gate denied : {ResolveGate(false, true, false, false, 2),
			 ResolveGate(true, false, false, false, 2), ResolveGate(true, true, true, false, 2)})
	{
		ASSERT_FALSE(denied.allowed);
		const CachePlan plan = PlanCaches(denied);
		EXPECT_EQ(plan.private_caches, 0u);
		EXPECT_FALSE(plan.seed_from_main);
		EXPECT_FALSE(plan.merge_into_main);
	}
}
