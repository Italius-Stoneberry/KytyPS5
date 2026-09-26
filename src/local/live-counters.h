#pragma once
// Local diagnostic event counters (printed per frame by the live `measure` command).
// Relaxed increments from any thread.

#include <atomic>
#include <cstdint>

namespace LiveCounters {

enum Id : uint32_t {
	WindowFault,        // guest write faults served by the CPU write window
	WindowPages,        // pages those windows made writable
	WriteFault,         // other guest write faults (buffer/texture invalidation)
	ReadFault,          // guest read faults (GPU-modified pages)
	Reprotect,          // region CPU-tracking re-arms that changed page state
	ReprotectPages,     // pages re-armed
	Unprotect,          // region CPU-tracking releases that changed page state
	UnprotectPages,     // pages released
	ProtectCalls,       // address-space protection changes (mprotect)
	ProtectCallsRender, // ... issued by the render thread
	UploadCopies,       // buffer upload copies
	UploadBytes,        // buffer upload bytes
	SyncDownloads,      // synchronous GPU downloads for guest access
	AsyncReadbacks,     // asynchronous guest readbacks started
	ReadbackDetaches,   // pending guest readbacks detached by a GPU write
	ReadbackEvictions,  // readbacks that waited for the oldest pending slot
	DispatchAfterDispatch, // dispatches whose previous draw/dispatch was a dispatch
	DispatchSameShader,    // ... of the same compute shader
	Pm4Suspends,           // PM4 executions suspended (WAIT_REG_MEM and similar)
	SubmissionRequeues,    // submissions put back blocked
	GuestCommands,         // host commands run for guest threads (SendCommand)
	RenderReadFaults,      // read faults taken by the render thread itself
	SrtWatchedReads,       // SRT word reads on watched (GPU-owned neighbour) pages
	Submissions,           // submissions processed (slices)
	Count
};

inline constexpr const char* Names[Count] = {
    "window_faults", "window_pages",     "write_faults",     "read_faults",   "reprotects",
    "reprotect_pages", "unprotects",     "unprotect_pages",  "protect_calls", "protect_calls_render",
    "upload_copies", "upload_bytes",     "sync_downloads",   "async_readbacks", "readback_detaches", "readback_evictions", "dispatch_after_dispatch", "dispatch_same_shader", "pm4_suspends", "submission_requeues", "guest_commands", "render_read_faults", "srt_watched_reads", "submission_slices"};

inline std::atomic<uint64_t> g_values[Count];
// Render thread: the last draw (0) or dispatch shader address.
inline uint64_t g_last_dispatch_shader = 0;

inline void Add(Id id, uint64_t n = 1) {
	g_values[id].fetch_add(n, std::memory_order_relaxed);
}

// Per 1 MiB granule (hashed, tagged): what cycles where.
enum GranuleField : uint32_t { GWindowPages, GReprotectPages, GUploadBytes, GUploadCalls, GGpuWrites, GGpuWriteBytes, GFields };
struct Granule {
	std::atomic<uint64_t> tag {0};
	std::atomic<uint64_t> values[GFields] {};
};
inline std::atomic_bool g_granules_on {false};
inline Granule          g_granules[4096];
inline std::atomic<uint64_t> g_granule_overflow {0};

inline void AddGranule(uint64_t address, GranuleField field, uint64_t n) {
	if (!g_granules_on.load(std::memory_order_relaxed)) return;
	const uint64_t key = (address >> 20u) + 1;
	for (uint64_t probe = 0; probe < 8; ++probe) {
		auto& granule = g_granules[(key * 0x9e3779b97f4a7c15ull >> 52u) + probe & 4095u];
		auto  tag     = granule.tag.load(std::memory_order_relaxed);
		if (tag == 0 && granule.tag.compare_exchange_strong(tag, key)) tag = key;
		if (tag != key) continue;
		granule.values[field].fetch_add(n, std::memory_order_relaxed);
		return;
	}
	g_granule_overflow.fetch_add(1, std::memory_order_relaxed);
}

// PM4 packets per opcode (render thread; relaxed atomics so the live thread can read).
inline std::atomic<uint64_t> g_pm4[256];

} // namespace LiveCounters
