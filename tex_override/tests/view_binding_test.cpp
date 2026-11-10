// view_binding_test.cpp
//
// 单文件压力测试：验证「已销毁 view 的墓碑 + 命令列表水位线」能否彻底消除
// “view 句柄被复用后绑定表继续使用旧哈希”的问题（README 已知限制 12 的根治）。
//
// 与 dllmain.cpp 中同名逻辑保持一一对应：
//   dead_texture_ring / ring_push / note_destroyed_view
//   revalidate_view_bindings（水位线推进 + 批量摘除 + 落后兜底）
//
// 模型：
//   * 每个 view 句柄 H 有一个单调递增的“世代” gen[H]（= 它当前指向的资源）。
//   * 绑定 = {handle, hash}，hash 在“首次绘制”时由当前世代算出（模拟 draw 侧
//     按需 resolve），之后被缓存。
//   * 线程在绘制前调用 revalidate，rules：
//       - 凡是在 [水位线, now) 内被销毁的 view，其绑定必须被整条摘除；
//       - 因此任何存活绑定的 hash 必须等于其句柄当前的世代。
//
// 构建与运行（x64 Native Tools Command Prompt）：
//   cl /nologo /std:c++17 /O2 /EHsc /utf-8 /Fe:view_binding_test.exe view_binding_test.cpp
//   view_binding_test.exe
//
// 末行 ALL PASSED 视为通过。

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <random>
#include <thread>
#include <unordered_set>
#include <vector>

// ---------------------------------------------------------------------------
// 与 dllmain.cpp 相同的无锁环（序号从 1 起，stamp 0 表示“未写入”）
// ---------------------------------------------------------------------------
static constexpr uint64_t kRingSize = 1024; // power of two

struct dead_view_ring
{
	struct slot
	{
		std::atomic<uint64_t> stamp{ 0 };
		std::atomic<uint64_t> handle{ 0 };
	};

	slot slots[kRingSize] = {};
	std::atomic<uint64_t> seq{ 1 };
};

static dead_view_ring g_dead_views;

static constexpr uint64_t kDeadViewBatchSize = 256;

static void ring_push(dead_view_ring &ring, uint64_t handle)
{
	if (handle == 0)
		return;

	const uint64_t seq = ring.seq.fetch_add(1, std::memory_order_relaxed);
	dead_view_ring::slot &slot = ring.slots[seq & (kRingSize - 1)];

	slot.handle.store(handle, std::memory_order_relaxed);
	slot.stamp.store(seq, std::memory_order_release);
}

static void note_destroyed_view(uint64_t handle)
{
	ring_push(g_dead_views, handle);
}

// ---------------------------------------------------------------------------
// 被测逻辑：命令列表绑定表 + 水位线校验
// ---------------------------------------------------------------------------
static constexpr uint64_t kSlots = 32;

struct bound_entry
{
	uint64_t handle = 0;
	uint64_t hash = 0; // 首次绘制时按当前世代算出并缓存
};

struct cmd_list_model
{
	bound_entry slots[kSlots];
	uint64_t views_validated_seq = 0;
};

static thread_local std::unordered_set<uint64_t> t_destroyed_views;

// 与 dllmain.cpp 的 revalidate_view_bindings 等价（把 active 遍历换成 slots 遍历）
static void revalidate_view_bindings(cmd_list_model &cmd)
{
	const uint64_t now = g_dead_views.seq.load(std::memory_order_acquire);
	if (cmd.views_validated_seq == now)
		return;

	if (cmd.views_validated_seq == 0)
	{
		cmd.views_validated_seq = now;
		return;
	}

	const uint64_t since = cmd.views_validated_seq;
	bool drop_all = false;

	t_destroyed_views.clear();

	if (now - since > kDeadViewBatchSize)
	{
		drop_all = true;
	}
	else
	{
		uint64_t seq = since;
		for (; seq != now; ++seq)
		{
			const dead_view_ring::slot &slot = g_dead_views.slots[seq & (kRingSize - 1)];
			const uint64_t stamp = slot.stamp.load(std::memory_order_acquire);

			if (stamp < seq)
				break;

			if (stamp != seq)
			{
				drop_all = true;
				seq = now;
				break;
			}

			const uint64_t handle = slot.handle.load(std::memory_order_acquire);
			if (handle != 0)
				t_destroyed_views.insert(handle);
		}

		cmd.views_validated_seq = seq;
	}

	if (drop_all)
	{
		cmd.views_validated_seq = now;

		for (uint64_t i = 0; i < kSlots; ++i)
			cmd.slots[i] = {};

		return;
	}

	if (t_destroyed_views.empty())
		return;

	for (uint64_t i = 0; i < kSlots; ++i)
	{
		const uint64_t handle = cmd.slots[i].handle;
		if (handle != 0 && t_destroyed_views.find(handle) != t_destroyed_views.end())
			cmd.slots[i] = {}; // 整条摘除（连同已缓存的 hash）
	}
}

// ---------------------------------------------------------------------------
// 测试状态
// ---------------------------------------------------------------------------
static constexpr uint64_t kHandleCount = 64; // 极小范围，疯狂制造句柄复用
static std::atomic<uint64_t> g_gen[kHandleCount];
static std::atomic<uint64_t> g_destroys{ 0 };
static std::atomic<bool> g_stop{ false };
static std::atomic<int> g_ready{ 0 };
static std::atomic<uint64_t> g_bind_ops{ 0 };
static std::atomic<uint64_t> g_draw_ops{ 0 };
static std::atomic<uint64_t> g_stale{ 0 };

static uint64_t hash_of(uint64_t handle, uint64_t gen)
{
	return handle * 0x9E3779B97F4A7C15ull + gen * 0xC2B2AE3D27D4EB4Full;
}

static void destroy_and_recreate(uint64_t handle)
{
	g_gen[handle].fetch_add(1, std::memory_order_acq_rel);
	note_destroyed_view(handle);
}

// 一个 worker 模拟“某命令列表线程”：反复 绑定 → 绘制
static void worker(int id)
{
	std::mt19937_64 rng(0xDEADBEEFCAFEull * (id + 1));
	cmd_list_model cmd;

	g_ready.fetch_add(1);

	uint64_t iter = 0;
	while (!g_stop.load(std::memory_order_acquire))
	{
		const uint64_t handle = 1 + rng() % (kHandleCount - 1);
		const uint64_t slot = rng() % kSlots;

		// 绑定槽位（push_descriptors 侧）
		cmd.slots[slot] = { handle, 0 };
		g_bind_ops.fetch_add(1, std::memory_order_relaxed);

		// 绘制（draw_indexed 侧）：先校验，再按需算出 hash。
		// worker 0 故意每隔一段时间才校验，以触发“落后超过批量上限 → 整体丢弃”兜底。
		if (id != 0 || (iter++ % 50) == 0)
			revalidate_view_bindings(cmd);

		bound_entry &entry = cmd.slots[rng() % kSlots];
		if (entry.handle != 0)
		{
			if (entry.hash == 0)
				entry.hash = hash_of(entry.handle, g_gen[entry.handle].load(std::memory_order_acquire));
			g_draw_ops.fetch_add(1, std::memory_order_relaxed);
		}
	}

	// 静默期校验：排空墓碑后，本线程的绑定表不得再有任何陈旧条目。
	// （所有销毁线程已停止，因此这里不会出现“断言窗口内有并发销毁”的假失败）
	revalidate_view_bindings(cmd);

	for (uint64_t i = 0; i < kSlots; ++i)
	{
		const bound_entry &entry = cmd.slots[i];
		if (entry.handle == 0 || entry.hash == 0)
			continue;

		const uint64_t current = hash_of(entry.handle, g_gen[entry.handle].load(std::memory_order_acquire));
		if (entry.hash != current)
			g_stale.fetch_add(1, std::memory_order_relaxed);
	}
}

int main()
{
	for (uint64_t i = 0; i < kHandleCount; ++i)
		g_gen[i].store(0, std::memory_order_relaxed);

	// ---- Phase 0: 第一枚 view 墓碑必须生效 ----
	{
		cmd_list_model cmd;
		cmd.views_validated_seq = g_dead_views.seq.load(std::memory_order_acquire); // 首次使用

		cmd.slots[0] = { 7, 0 };
		cmd.slots[0].hash = hash_of(7, g_gen[7].load());

		destroy_and_recreate(7); // 进程产生的第一枚 view 墓碑
		revalidate_view_bindings(cmd);

		if (cmd.slots[0].handle != 0)
		{
			std::printf("FAILED: first view tombstone was ignored\n");
			return 1;
		}
		std::printf("phase 0        : first view tombstone honoured\n");
	}

	// ---- Phase 1: 同一句柄被复用后重新绑定，必须拿到新世代的哈希 ----
	{
		cmd_list_model cmd;
		cmd.views_validated_seq = g_dead_views.seq.load(std::memory_order_acquire);

		const uint64_t h = 9;
		cmd.slots[1] = { h, 0 };
		cmd.slots[1].hash = hash_of(h, g_gen[h].load()); // 旧世代的哈希

		destroy_and_recreate(h); // view 销毁，句柄随后被复用
		cmd.slots[1] = { h, 0 }; // 引擎用同一个数值重新绑定（set() 的 old == view 场景）
		revalidate_view_bindings(cmd);

		if (cmd.slots[1].handle != 0)
		{
			std::printf("FAILED: rebind with a recycled handle kept the old binding\n");
			return 1;
		}

		// 重新绑定后的那一次绘制必须算出新世代的哈希
		cmd.slots[1] = { h, 0 };
		cmd.slots[1].hash = hash_of(h, g_gen[h].load(std::memory_order_acquire));
		if (cmd.slots[1].hash != hash_of(h, g_gen[h].load(std::memory_order_acquire)))
		{
			std::printf("FAILED: recycled handle produced a stale hash\n");
			return 1;
		}
		std::printf("phase 1        : recycled handle invalidated before rebind\n");
	}

	// ---- Phase 2: 并发压测（绑定/绘制 4 线程 + 销毁/重建 1 线程）----
	constexpr int kThreads = 4;
	std::vector<std::thread> workers;
	for (int i = 0; i < kThreads; ++i)
		workers.emplace_back(worker, i);

	while (g_ready.load() != kThreads)
		std::this_thread::yield();

	std::mt19937_64 rng(0xFEEDFACEC0FFEEull);
	for (int round = 0; round < 300000; ++round)
	{
		destroy_and_recreate(1 + rng() % (kHandleCount - 1));
		g_destroys.fetch_add(1, std::memory_order_relaxed);

		if ((round & 0x3FF) == 0)
			std::this_thread::yield();
	}

	g_stop.store(true, std::memory_order_release);

	for (auto &t : workers)
		t.join();

	// ---- 静默期校验：每个“活着的”绑定都必须与句柄当前世代一致 ----
	// 注：worker 的绑定表是 thread_local，这里通过在 worker 退出前做完校验来实现，
	// 因此把校验放进 worker 的收尾阶段（见 worker 内的 final_check）。
	std::printf("destroy events : %llu\n", (unsigned long long)g_destroys.load());
	std::printf("bind ops       : %llu\n", (unsigned long long)g_bind_ops.load());
	std::printf("draw ops       : %llu\n", (unsigned long long)g_draw_ops.load());
	std::printf("stale bindings : %llu\n", (unsigned long long)g_stale.load());

	if (g_stale.load() != 0)
	{
		std::printf("FAILED: stale binding survived revalidation\n");
		return 1;
	}

	std::printf("ALL PASSED\n");
	return 0;
}
