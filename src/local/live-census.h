#pragma once
// Local diagnostic: render-thread time per call kind and shader (live `census`).
// A scope costs one relaxed load while the census is off. The table is written only
// by the render thread; the live thread reads it after switching the census off.

#include <atomic>
#include <cstdint>
#include <x86intrin.h>

namespace LiveCensus {

enum Kind : uint32_t {
	Dispatch      = 0, // a: compute shader, b: indirect
	Draw          = 1, // a: vertex (GS) shader, b: pixel shader
	NativeXpr     = 2,
	DispatchPhase = 3, // a: compute shader, b: phase
	GpuWait       = 4, // a, b: return addresses (caller, its caller)
	ReadbackWait  = 5, // a, b: return addresses
	SyncDownload  = 6, // a: 1 MiB granule, b: write access
	GraphicsPrograms = 7, // program lookup and SRT evaluation of a draw
	NativeGather  = 8, // native XPR record word gather
	Kinds         = 9
};

struct Entry {
	uint64_t a = 0, b = 0, calls = 0, cycles = 0;
	uint32_t kind = 0;
	bool     used = false;
};

constexpr size_t      TableSize = 8192;
inline std::atomic_bool g_on {false};
// Set on the render thread: other threads never touch the table.
inline thread_local bool g_render = false;
inline Entry            g_table[TableSize];

inline void Add(uint32_t kind, uint64_t a, uint64_t b, uint64_t cycles) {
	uint64_t hash = (a * 0x9e3779b97f4a7c15ull) ^ (b * 0xc2b2ae3d27d4eb4full) ^ kind;
	for (size_t probe = 0; probe < TableSize; ++probe) {
		auto& entry = g_table[(hash + probe) & (TableSize - 1)];
		if (!entry.used) {
			entry = {a, b, 0, 0, kind, true};
		} else if (entry.a != a || entry.b != b || entry.kind != kind) {
			continue;
		}
		entry.calls += 1;
		entry.cycles += cycles;
		return;
	}
}

class Scope {
public:
	Scope(uint32_t kind, uint64_t a, uint64_t b = 0): m_on(g_on.load(std::memory_order_relaxed) && g_render) {
		if (m_on) {
			m_kind  = kind;
			m_a     = a;
			m_b     = b;
			m_start = __rdtsc();
		}
	}
	~Scope() {
		if (m_on) Add(m_kind, m_a, m_b, __rdtsc() - m_start);
	}
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;

private:
	bool     m_on;
	uint32_t m_kind  = 0;
	uint64_t m_a     = 0, m_b = 0, m_start = 0;
};

} // namespace LiveCensus
