#pragma once
// Local diagnostic (causal probe): spin kyty_local_debug_delay_ns per call at the sites
// set in kyty_local_debug_delay_site, to measure how frame time responds to render-thread
// time in each part of the frame. One relaxed load per site while off (the default).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <x86intrin.h>

extern "C" {
extern volatile std::atomic<uint32_t> kyty_local_debug_delay_ns;
extern volatile std::atomic<uint32_t> kyty_local_debug_delay_site;
}

namespace DebugDelay {

enum Site : uint32_t { GraphicsDispatch = 1, ComputeDispatch = 2, NativeXpr = 4, Draw = 8 };

inline void At(uint32_t site) {
	const uint32_t ns = kyty_local_debug_delay_ns.load(std::memory_order_relaxed);
	if (ns == 0 || (kyty_local_debug_delay_site.load(std::memory_order_relaxed) & site) == 0) return;
	const auto end = std::chrono::steady_clock::now() + std::chrono::nanoseconds(ns);
	while (std::chrono::steady_clock::now() < end) _mm_pause();
}

} // namespace DebugDelay
