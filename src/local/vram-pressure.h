#pragma once
// Video memory on 8 GB GPUs. The texture and buffer caches computed their collection thresholds
// once, from the budget at start; the freed-image pool kept up to a 16th of the budget (383 MiB);
// and the texture collection freed only images unused for 80-160 frames (8-16 s at the 10 fps of
// an RTX 3070 Ti, whose 7011 MiB heap 0 budget was full: usage 7013 MiB, GPU buffers in system
// memory). Four switches, each 0 off, 1 on, 2 auto (the default: on when the GPU is small, see
// g_small, so a 12 GB or larger GPU keeps the previous behaviour):
//   KYTY_VRAM_ADAPTIVE    the collections' thresholds and the pool's 16th follow the budget the driver
//                         reports now (refreshed at most every 100 ms, other programs' video memory
//                         included), not the one at start
//   KYTY_IMAGE_POOL_TRIM  the freed-image pool (KYTY_IMAGE_POOL) holds a 32nd of the budget, a 128th
//                         under high pressure and nothing under critical pressure; a device-local
//                         buffer placed in system memory frees the pool and is placed once more
//   KYTY_TEXTURE_GC_TIME  under high and critical pressure the texture collection's ages are seconds
//                         (5 s, 2 s; never under 16 frames) and it deletes up to 64/128 images a pass
//   KYTY_VRAM_LOG         a "VRAM" line every KYTY_VRAM_LOG_SECONDS (default 10) and when the
//                         pressure level changes (at most every 2 s)
// GraphicContext::RefreshMemoryPressure (vma.cpp) runs once a frame, before the texture cache's
// collection; the startup line "VRAM: ..." says which modes are active. The live counters
// texture_gc_deletes, image_pool_trim(s|_kib) and sysmem_fallback(s|_kib) count with or without them.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

extern "C" {
extern volatile std::atomic_uint32_t kyty_local_vram_adaptive_mode;
extern volatile std::atomic_uint32_t kyty_local_image_pool_trim_mode;
extern volatile std::atomic_uint32_t kyty_local_texture_gc_time_mode;
extern volatile std::atomic_uint32_t kyty_local_vram_log_mode;
}

namespace VramPressure {

inline constexpr uint32_t Auto = 2;

// Set once by GraphicContext::CreateAllocator: a discrete GPU with VK_EXT_memory_budget whose
// device-local heaps' budget at start is under SmallBudget (8 and 10 GB GPUs; not 12 GB and up).
inline constexpr uint64_t SmallBudget = uint64_t {10} << 30u;
inline std::atomic_bool   g_small {false};

[[nodiscard]] inline bool Active(const volatile std::atomic_uint32_t& mode) {
	const auto value = mode.load(std::memory_order_relaxed);
	return value == 1 || (value == Auto && g_small.load(std::memory_order_relaxed));
}

// Normal: under the texture cache's pressure threshold; Pressured: under its critical threshold;
// High: under the collections' budget (GetTotalMemoryBudget: the driver's less an eighth, at most
// 1 GiB); Critical: past it, close to where VMA puts buffers in system memory.
enum Level : uint32_t { Normal, Pressured, High, Critical, Levels };
inline constexpr const char* LevelNames[Levels] = {"normal", "pressured", "high", "critical"};

// The texture cache's collection thresholds for a budget (GraphicContext::GetTotalMemoryBudget), the
// formula TextureCache's constructor had. On an 8 GB GPU (budget 6135 MiB): trigger 0, pressure
// 2454 MiB, critical 4908 MiB.
struct Thresholds {
	uint64_t trigger  = 0;
	uint64_t pressure = 0;
	uint64_t critical = 0;
};
[[nodiscard]] inline Thresholds TextureThresholds(uint64_t total_budget) {
	constexpr int64_t GiB    = 1024ll * 1024 * 1024;
	const auto        budget = static_cast<int64_t>(std::min<uint64_t>(total_budget, INT64_MAX));
	const auto threshold = std::min<int64_t>(budget, 8 * GiB);
	Thresholds out;
	out.pressure = static_cast<uint64_t>(
	    std::max<int64_t>(std::min(budget - 6 * threshold / 10, budget - GiB), GiB + GiB / 2));
	out.critical = static_cast<uint64_t>(
	    std::max<int64_t>(std::min(budget - 2 * threshold / 10, budget - GiB / 2), 3 * GiB));
	out.trigger = static_cast<uint64_t>(std::max<int64_t>((budget - threshold) / 2, 0));
	return out;
}

// KYTY_TEXTURE_GC_TIME: the age, in collections (one a frame), that selects what was last used at
// least `seconds` ago. times[n % N] is when collection n ran (steady-clock ns; this one, `tick`, ran
// at `now`). An image last touched while the collection counter was u was used before collection u
// ran, so when collection tick - k is old enough, so is everything with u <= tick - k (what the LRU's
// ForEachItemBelow(tick - k) visits). At most `age`, the frame-based age, so it only shortens it (at
// 60 fps the 80 frames of the first pass are 1.3 s, under the 5 s of high pressure); at least 16
// collections, the shortest age of the frame-based mode (it never takes an image the frames in
// flight use).
template <std::size_t N>
[[nodiscard]] inline uint64_t TimedAge(const std::array<int64_t, N>& times, uint64_t tick, int64_t now, double seconds,
                                       uint64_t age) {
	const auto     window = static_cast<int64_t>(seconds * 1e9);
	const uint64_t floor  = std::min<uint64_t>(16, tick);
	const uint64_t limit  = std::max(floor, std::min<uint64_t>({age, tick, N - 1}));
	uint64_t       k      = floor;
	while (k < limit && now - times[(tick - k) % N] < window) {
		++k;
	}
	return k;
}

// Render thread: written by GraphicContext::RefreshMemoryPressure, read by the caches' collections
// after it in the same frame. budget is 0 while no mode is active (or without VK_EXT_memory_budget).
struct Snapshot {
	uint64_t budget = 0; // the collections' budget: the live one (KYTY_VRAM_ADAPTIVE) or the one at start
	uint64_t usage  = 0; // device-local usage of this process
	Level    level  = Normal;
};
inline Snapshot g_snapshot;

} // namespace VramPressure
