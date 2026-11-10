// cache_invalidation_test.cpp
//
// 单文件压力测试：验证 dllmain.cpp 中「墓碑环 + 精确失效」的 descriptor 哈希
// 缓存方案在多线程 + 高频资源销毁下不会留下过期条目。
//
// 与 dllmain.cpp 中同名逻辑保持逐行一致（dead_texture_ring / note_destroyed_texture /
// drain_destroyed_textures / texture_destroyed_since / reset_descriptor_hash_cache）。
//
// 构建与运行（x64 Native Tools Command Prompt）：
//   cl /nologo /std:c++17 /O2 /EHsc /Fe:cache_invalidation_test.exe cache_invalidation_test.cpp
//   cache_invalidation_test.exe
//
// 输出末行为 ALL PASSED 即通过；任何 stale entries 都会打印详细现场并返回 1。

// Standalone stress test for the tombstone-ring invalidation used in dllmain.cpp.
//
// Model:
//   * Each handle H has a monotonically increasing "incarnation" gen[H].
//   * "destroy + recreate" of H  =>  gen[H]++  and  note_destroyed_texture(H).
//   * Worker threads perform resolve(H): drain ring, then either hit the cache or
//     recompute and insert {gen[H] at insert time, value}.
//   * After all activity stops, each worker drains to completion and asserts that
//     every remaining cache entry was produced by the CURRENT incarnation of its
//     handle (i.e. no stale hash survived).
//
// The check reads gen[] only as ground truth; the algorithm itself never does.

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

static constexpr uint64_t kDeadTextureRingSize = 1024; // power of two

struct dead_texture_ring
{
	struct slot
	{
		std::atomic<uint64_t> stamp{ 0 };
		std::atomic<uint64_t> handle{ 0 };
	};

	slot slots[kDeadTextureRingSize] = {};
	// First reserved sequence is 1: stamp==0 means "slot never written". If the ring
	// started at 0, the very first producer's slot would look "already complete"
	// (stamp 0 == seq 0) and its tombstone would be skipped forever.
	std::atomic<uint64_t> seq{ 1 };
};

static dead_texture_ring g_dead_textures;
static thread_local uint64_t t_dead_textures_seen = 1; // same base as seq

// cache entry: {incarnation at insert, value}
static thread_local std::unordered_map<uint64_t, std::pair<uint64_t, uint32_t>> t_resource_hashes;

static std::atomic<uint64_t> g_current_cache_size{ 0 };

static bool texture_destroyed_since(uint64_t handle, uint64_t since_seq)
{
	const uint64_t written = g_dead_textures.seq.load(std::memory_order_acquire);
	if (written <= since_seq)
		return false;

	if (written - since_seq > kDeadTextureRingSize)
		return true;

	for (uint64_t seq = since_seq; seq != written; ++seq)
	{
		const dead_texture_ring::slot &slot = g_dead_textures.slots[seq & (kDeadTextureRingSize - 1)];
		const uint64_t stamp = slot.stamp.load(std::memory_order_acquire);
		if (stamp < seq)
			continue;
		if (stamp != seq)
			return true;
		if (slot.handle.load(std::memory_order_acquire) == handle)
			return true;
	}

	return false;
}

static void note_destroyed_texture(uint64_t handle)
{
	if (handle == 0)
		return;

	const uint64_t seq = g_dead_textures.seq.fetch_add(1, std::memory_order_relaxed);
	dead_texture_ring::slot &slot = g_dead_textures.slots[seq & (kDeadTextureRingSize - 1)];

	slot.handle.store(handle, std::memory_order_relaxed);
	slot.stamp.store(seq, std::memory_order_release);
}

static void reset_descriptor_hash_cache(uint64_t caught_up_seq)
{
	if (!t_resource_hashes.empty())
	{
		g_current_cache_size.fetch_sub(static_cast<int>(t_resource_hashes.size()), std::memory_order_relaxed);
		t_resource_hashes.clear();
	}

	t_dead_textures_seen = caught_up_seq;
}

static void drain_destroyed_textures()
{
	const uint64_t written = g_dead_textures.seq.load(std::memory_order_acquire);
	if (written == t_dead_textures_seen)
		return;

	if (written - t_dead_textures_seen > kDeadTextureRingSize)
	{
		reset_descriptor_hash_cache(written);
		return;
	}

	while (t_dead_textures_seen != written)
	{
		const uint64_t seq = t_dead_textures_seen;
		const dead_texture_ring::slot &slot = g_dead_textures.slots[seq & (kDeadTextureRingSize - 1)];
		const uint64_t stamp = slot.stamp.load(std::memory_order_acquire);

		if (stamp != seq)
		{
			if (stamp > seq)
				reset_descriptor_hash_cache(written);
			return;
		}

		const uint64_t handle = slot.handle.load(std::memory_order_acquire);
		if (handle != 0 && t_resource_hashes.erase(handle) != 0)
			g_current_cache_size.fetch_sub(1, std::memory_order_relaxed);

		++t_dead_textures_seen;
	}
}

// ---------------------------------------------------------------------------
// test harness
// ---------------------------------------------------------------------------
static constexpr uint64_t kHandles = 4096;
static std::atomic<uint64_t> g_gen[kHandles];
static std::atomic<uint64_t> g_destroys{ 0 };
static std::atomic<bool> g_stop{ false };
static std::atomic<int> g_ready{ 0 };
static std::atomic<int> g_done{ 0 };

static std::atomic<uint64_t> g_stale_found{ 0 };
static std::atomic<uint64_t> g_hits{ 0 };
static std::atomic<uint64_t> g_misses{ 0 };

struct stale_report
{
	uint64_t handle;
	uint64_t stored_gen;
	uint64_t current_gen;
	uint64_t last_tomb_seq;
	uint64_t worker_seen;
	uint64_t ring_written;
	int worker_id;
};

static std::atomic<uint64_t> g_ring_written_at_verify{ 0 };
static stale_report g_reports[16];
static std::atomic<int> g_report_count{ 0 };
static std::atomic<uint64_t> g_tomb_seq[kHandles];

static uint32_t compute_hash(uint64_t handle, uint64_t gen)
{
	return static_cast<uint32_t>((handle * 2654435761u) ^ (gen * 40503u));
}

static void worker(int id)
{
	std::mt19937_64 rng(0x9E3779B97F4A7C15ull * (id + 1));

	g_ready.fetch_add(1);

	uint64_t iter = 0;
	while (!g_stop.load(std::memory_order_acquire))
	{
		const uint64_t h = 1 + rng() % (kHandles - 1);

		// Worker 0 deliberately lags so the "behind by more than the ring" reset
		// path is exercised as well.
		if (id != 0 || (iter++ % 200) == 0)
			drain_destroyed_textures();

		const auto it = t_resource_hashes.find(h);
		if (it != t_resource_hashes.end())
		{
			g_hits.fetch_add(1, std::memory_order_relaxed);
			// hit is fine as long as drain already removed dead incarnations
		}
		else
		{
			g_misses.fetch_add(1, std::memory_order_relaxed);

			// snapshot the ring position BEFORE reading the value (mirrors
			// resolve_descriptor_hash)
			const uint64_t ring_position = g_dead_textures.seq.load(std::memory_order_acquire);

			const uint64_t gen = g_gen[h].load(std::memory_order_acquire);
			t_resource_hashes.emplace(h, std::make_pair(gen, compute_hash(h, gen)));
			g_current_cache_size.fetch_add(1, std::memory_order_relaxed);

			// second-pass validation
			if (texture_destroyed_since(h, ring_position))
				reset_descriptor_hash_cache(g_dead_textures.seq.load(std::memory_order_acquire));
		}
	}

	// quiescent verification: drain to completion, then every survivor must be
	// produced by the current incarnation
	drain_destroyed_textures();

	const uint64_t written = g_dead_textures.seq.load(std::memory_order_acquire);

	for (const auto &entry : t_resource_hashes)
	{
		const uint64_t gen = g_gen[entry.first].load(std::memory_order_acquire);
		if (entry.second.first != gen)
		{
			g_stale_found.fetch_add(1, std::memory_order_relaxed);

			const int slot = g_report_count.fetch_add(1, std::memory_order_relaxed);
			if (slot < 16)
			{
				g_reports[slot].handle = entry.first;
				g_reports[slot].stored_gen = entry.second.first;
				g_reports[slot].current_gen = gen;
				g_reports[slot].last_tomb_seq = g_tomb_seq[entry.first].load(std::memory_order_acquire);
				g_reports[slot].worker_seen = t_dead_textures_seen;
				g_reports[slot].ring_written = written;
				g_reports[slot].worker_id = id;
			}
		}
	}

	g_done.fetch_add(1);
}

int main()
{
	for (uint64_t i = 0; i < kHandles; ++i)
		g_gen[i].store(0, std::memory_order_relaxed);

	// ---- Phase 0: regression test for the "first tombstone is dropped" bug ----
	// Cache an entry for a handle, destroy it (this is the very first tombstone the
	// process ever produces) and assert the invalidation is actually applied.
	{
		const uint64_t h = 7;
		t_resource_hashes.emplace(h, std::make_pair(g_gen[h].load(), compute_hash(h, 0)));
		g_current_cache_size.fetch_add(1, std::memory_order_relaxed);

		g_gen[h].fetch_add(1, std::memory_order_acq_rel); // handle reused by a new texture
		note_destroyed_texture(h);                        // first tombstone ever (seq == 1)
		drain_destroyed_textures();

		if (t_resource_hashes.count(h) != 0)
		{
			std::printf("FAILED: the first tombstone was dropped (seq/stamp base collision)\n");
			return 1;
		}
		std::printf("phase 0        : first tombstone honoured\n");
	}

	constexpr int kThreads = 6;
	std::vector<std::thread> workers;
	for (int i = 0; i < kThreads; ++i)
		workers.emplace_back(worker, i);

	while (g_ready.load() != kThreads)
		std::this_thread::yield();

	// destroyer: destroy + immediately recreate, exactly like engine resource churn
	std::mt19937_64 rng(0xDEADBEEFCAFEBABEull);
	for (int round = 0; round < 200000; ++round)
	{
		const uint64_t h = 1 + rng() % (kHandles - 1);
		g_gen[h].fetch_add(1, std::memory_order_acq_rel); // new incarnation
		g_tomb_seq[h].store(g_dead_textures.seq.load(std::memory_order_relaxed), std::memory_order_relaxed);
		note_destroyed_texture(h);
		g_destroys.fetch_add(1, std::memory_order_relaxed);

		if ((round & 0x3FF) == 0)
			std::this_thread::yield();
	}

	g_stop.store(true, std::memory_order_release);

	for (auto &t : workers)
		t.join();

	std::printf("destroy events : %llu\n", (unsigned long long)g_destroys.load());
	std::printf("cache hits     : %llu\n", (unsigned long long)g_hits.load());
	std::printf("cache misses   : %llu\n", (unsigned long long)g_misses.load());
	std::printf("live cache size: %lld (approx, per-thread sum)\n", (long long)g_current_cache_size.load());
	std::printf("stale entries  : %llu\n", (unsigned long long)g_stale_found.load());

	if (g_stale_found.load() != 0)
	{
		const int count = g_report_count.load() < 16 ? g_report_count.load() : 16;
		for (int i = 0; i < count; ++i)
		{
			const stale_report &r = g_reports[i];
			std::printf("  stale: handle=%llu stored_gen=%llu current_gen=%llu last_tomb_seq=%llu worker_seen=%llu written=%llu worker=%d\n",
				(unsigned long long)r.handle,
				(unsigned long long)r.stored_gen,
				(unsigned long long)r.current_gen,
				(unsigned long long)r.last_tomb_seq,
				(unsigned long long)r.worker_seen,
				(unsigned long long)r.ring_written,
				r.worker_id);
		}

		std::printf("FAILED: stale cache entries survived invalidation\n");
		return 1;
	}

	std::printf("ALL PASSED\n");
	return 0;
}
