#pragma once
// Local measurement hook (KYTY_LIVE_FILE): one boot, many same-process A/B experiments.
// A side thread polls the command file every 100 ms and runs its commands once per new
// "id N" first line, printing LIVE_* lines to stdout (tools/local/live-bench.py).
//   poke32 <host address> <hex>   write a switch (live-bench resolves `sym NAME VALUE`)
//   peek <host address> <bytes>   hex dump (<= 256 bytes)
//   measure <seconds> <label>     presented frames, fps, per-frame CPU of the render and
//                                 recording threads
//   prof <seconds> <path>         4 kHz samples of render-thread CPU time
//   profw <seconds> <path>        4 kHz samples of render-thread wall time (waits included)
//   profp <seconds> <path>        4 kHz samples of process CPU time (with thread ids)
// A sample is 16 words: pc, the word at rsp, then up to 14 frame-pointer return addresses
// (process mode: pc, thread id | 1 << 63, zeros).
//   sleep <seconds>
// Without KYTY_LIVE_FILE nothing runs; the flip hook is one relaxed increment.

#include "live-census.h"
#include "live-counters.h"
#include "live-trace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <string>
#include <string_view>
#include <sys/syscall.h>
#include <thread>
#include <ucontext.h>
#include <unistd.h>

namespace LiveControl {

inline std::atomic_uint64_t g_flips {0};
inline std::atomic_bool     g_render_known {false};
inline pthread_t            g_render_thread {};
inline std::atomic<pid_t>   g_render_tid {0};
// Render-thread stack bounds for the frame-pointer walk.
inline uint64_t             g_stack_low = 0, g_stack_high = 0;
constexpr size_t             SampleWords = 16;
inline uint64_t              g_samples[1u << 23];
inline std::atomic<uint32_t> g_sample_count {0};
inline std::atomic_bool      g_process {false};

inline void Flip() {
	g_flips.fetch_add(1, std::memory_order_relaxed);
}

inline double ThreadCpuSeconds(pthread_t thread) {
	clockid_t clock {};
	timespec  now {};
	if (pthread_getcpuclockid(thread, &clock) != 0 || clock_gettime(clock, &now) != 0) return 0;
	return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1e9;
}

inline double NamedThreadsCpuSeconds(const char* name) {
	double     total = 0;
	const long ticks = sysconf(_SC_CLK_TCK);
	if (auto* dir = opendir("/proc/self/task")) {
		while (auto* entry = readdir(dir)) {
			if (entry->d_name[0] == '.') continue;
			char path[64], comm[32] {};
			std::snprintf(path, sizeof(path), "/proc/self/task/%s/comm", entry->d_name);
			if (auto* f = std::fopen(path, "re")) {
				if (std::fgets(comm, sizeof(comm), f)) comm[std::strcspn(comm, "\n")] = 0;
				std::fclose(f);
			}
			if (std::strcmp(comm, name) != 0) continue;
			std::snprintf(path, sizeof(path), "/proc/self/task/%s/stat", entry->d_name);
			if (auto* f = std::fopen(path, "re")) {
				char line[1024] {};
				if (std::fgets(line, sizeof(line), f)) {
					const char*        rest  = std::strrchr(line, ')');
					unsigned long long utime = 0, stime = 0;
					if (rest && std::sscanf(rest + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",
					                        &utime, &stime) == 2)
						total += static_cast<double>(utime + stime) / static_cast<double>(ticks);
				}
				std::fclose(f);
			}
		}
		closedir(dir);
	}
	return total;
}

inline void ProfSignal(int /*signal*/, siginfo_t* /*info*/, void* context) {
	const auto* uc  = static_cast<const ucontext_t*>(context);
	const auto  pc  = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
	const auto  rsp = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RSP]);
	const auto  idx = g_sample_count.fetch_add(1, std::memory_order_relaxed);
	if (SampleWords * (size_t(idx) + 1) <= std::size(g_samples)) {
		auto* record = g_samples + SampleWords * size_t(idx);
		record[0]    = pc;
		if (g_process.load(std::memory_order_relaxed)) {
			record[1] = static_cast<uint64_t>(syscall(SYS_gettid)) | (1ull << 63);
			for (size_t i = 2; i < SampleWords; ++i) record[i] = 0;
		} else {
			// Word at rsp (the caller of a leaf routine), then return addresses from the
			// frame-pointer chain, bounded to this stack.
			record[1]    = *reinterpret_cast<const uint64_t*>(rsp);
			auto rbp     = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RBP]);
			for (size_t i = 2; i < SampleWords; ++i) {
				record[i] = 0;
				if (rbp < rsp || rbp + 16 > g_stack_high || (rbp & 7u) != 0) continue;
				const auto* frame = reinterpret_cast<const uint64_t*>(rbp);
				record[i]         = frame[1];
				if (frame[0] <= rbp) {
					rbp = 0;
					continue;
				}
				rbp = frame[0];
			}
		}
	}
}

inline void Profile(uint64_t id, double seconds, const char* path, bool process, bool wall = false) {
	if (g_render_tid.load() == 0) return;
	g_process.store(process);
	struct sigaction action {};
	action.sa_sigaction = ProfSignal;
	action.sa_flags     = SA_SIGINFO | SA_RESTART;
	sigemptyset(&action.sa_mask);
	sigaction(SIGPROF, &action, nullptr);
	clockid_t clock = CLOCK_PROCESS_CPUTIME_ID;
	if (wall) {
		clock = CLOCK_MONOTONIC;
	} else if (!process && pthread_getcpuclockid(g_render_thread, &clock) != 0) {
		return;
	}
	sigevent event {};
	event.sigev_signo = SIGPROF;
	if (process) {
		event.sigev_notify = SIGEV_SIGNAL;
	} else {
		event.sigev_notify   = SIGEV_THREAD_ID;
		event._sigev_un._tid = g_render_tid.load();
	}
	timer_t timer {};
	if (timer_create(clock, &event, &timer) != 0) return;
	g_sample_count.store(0);
	itimerspec spec {};
	spec.it_interval.tv_nsec = 1000000000L / 4000;
	spec.it_value            = spec.it_interval;
	timer_settime(timer, 0, &spec, nullptr);
	std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
	timer_delete(timer);
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	const auto count = std::min<uint32_t>(g_sample_count.load(), std::size(g_samples) / SampleWords);
	if (auto* out = std::fopen(path, "wb")) {
		std::fwrite(g_samples, sizeof(uint64_t), SampleWords * size_t(count), out);
		std::fclose(out);
	}
	const std::string maps_path = std::string(path) + ".maps";
	if (auto* in = std::fopen("/proc/self/maps", "re")) {
		if (auto* out = std::fopen(maps_path.c_str(), "we")) {
			char buffer[65536];
			for (size_t got; (got = std::fread(buffer, 1, sizeof(buffer), in)) > 0;) std::fwrite(buffer, 1, got, out);
			std::fclose(out);
		}
		std::fclose(in);
	}
	std::printf("LIVE_PROF id=%" PRIu64 " samples=%u path=%s\n", id, count, path);
}

inline void Measure(uint64_t id, double seconds, const char* label) {
	std::array<uint64_t, LiveCounters::Count> counters0 {};
	for (size_t i = 0; i < counters0.size(); ++i) counters0[i] = LiveCounters::g_values[i].load();
	const auto   flips0  = g_flips.load();
	const double render0 = g_render_known ? ThreadCpuSeconds(g_render_thread) : 0;
	const double record0 = NamedThreadsCpuSeconds("Kyty.Record");
	const auto   begin   = std::chrono::steady_clock::now();
	std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
	const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
	const auto   frames  = g_flips.load() - flips0;
	const double render  = (g_render_known ? ThreadCpuSeconds(g_render_thread) : 0) - render0;
	const double record  = NamedThreadsCpuSeconds("Kyty.Record") - record0;
	const double per     = frames ? 1000.0 / static_cast<double>(frames) : 0;
	std::printf("LIVE_MEASURE id=%" PRIu64 " label=%s seconds=%.2f frames=%" PRIu64
	            " fps=%.2f render_ms=%.2f record_ms=%.2f render_busy=%.1f%%\n",
	            id, label, elapsed, frames, static_cast<double>(frames) / elapsed, render * per, record * per,
	            100.0 * render / elapsed);
	std::string counters;
	for (size_t i = 0; i < counters0.size(); ++i) {
		char text[96];
		std::snprintf(text, sizeof(text), " %s=%.1f", LiveCounters::Names[i],
		              static_cast<double>(LiveCounters::g_values[i].load() - counters0[i]) / (frames ? frames : 1));
		counters += text;
	}
	std::printf("LIVE_COUNTERS id=%" PRIu64 " label=%s%s\n", id, label, counters.c_str());
}

inline void Run(uint64_t id, const std::string& line) {
	char      command[16] {}, arg1[256] {}, arg2[256] {};
	const int n = std::sscanf(line.c_str(), "%15s %255s %255s", command, arg1, arg2);
	if (n < 1) return;
	const std::string_view cmd = command;
	if ((cmd == "poke32" || cmd == "peek") && n == 3 && std::strtoull(arg1, nullptr, 16) < 0x100000000ull) {
		// Host addresses of switches are far above 4 GiB; refuse guest-looking values.
		std::printf("LIVE_ERROR id=%" PRIu64 " line=%s (address below 4 GiB)\n", id, line.c_str());
	} else if (cmd == "poke32" && n == 3) {
		auto*      target = reinterpret_cast<void*>(std::strtoull(arg1, nullptr, 16));
		const auto value  = static_cast<uint32_t>(std::strtoul(arg2, nullptr, 16));
		std::memcpy(target, &value, 4);
	} else if (cmd == "peek" && n == 3) {
		const auto* data  = reinterpret_cast<const uint8_t*>(std::strtoull(arg1, nullptr, 16));
		const auto  bytes = std::min<size_t>(std::strtoul(arg2, nullptr, 0), 256);
		std::string hex;
		for (size_t i = 0; i < bytes; ++i) {
			char b[4];
			std::snprintf(b, sizeof(b), "%02x", data[i]);
			hex += b;
		}
		std::printf("LIVE_PEEK id=%" PRIu64 " va=%s bytes=%s\n", id, arg1, hex.c_str());
	} else if (cmd == "measure" && n >= 2) {
		Measure(id, std::strtod(arg1, nullptr), n == 3 ? arg2 : "-");
	} else if ((cmd == "prof" || cmd == "profp" || cmd == "profw") && n == 3) {
		Profile(id, std::strtod(arg1, nullptr), arg2, cmd == "profp", cmd == "profw");
	} else if ((cmd == "trace" || cmd == "tracew" || cmd == "tracem") && n == 3) {
		LiveTrace::g_count.store(0);
		LiveTrace::g_writes_on.store(cmd == "tracew");
		LiveTrace::g_mark_next.store(0);
		LiveTrace::g_marks_on.store(cmd == "tracem");
		const auto tsc0   = __rdtsc();
		const auto clock0 = std::chrono::steady_clock::now();
		LiveTrace::g_on.store(true);
		std::this_thread::sleep_for(std::chrono::duration<double>(std::strtod(arg1, nullptr)));
		LiveTrace::g_on.store(false);
		LiveTrace::g_writes_on.store(false);
		LiveTrace::g_marks_on.store(false);
		const auto tsc1   = __rdtsc();
		const auto clock1 = std::chrono::steady_clock::now();
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		if (cmd == "tracem" && LiveTrace::g_read_marks)
			LiveTrace::g_read_marks(std::min(LiveTrace::g_mark_next.load(), LiveTrace::MarkSlots));
		LiveTrace::Dump(arg2);
		const double seconds = std::chrono::duration<double>(clock1 - clock0).count();
		std::printf("LIVE_TRACE id=%" PRIu64 " records=%" PRIu64 " tsc_hz=%.0f path=%s\n", id,
		            std::min<uint64_t>(LiveTrace::g_count.load(), LiveTrace::Capacity),
		            static_cast<double>(tsc1 - tsc0) / seconds, arg2);
	} else if (cmd == "census" && n >= 2) {
		// census <seconds>: render-thread time per call kind and shader over the window.
		LiveCensus::g_on.store(false);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		for (auto& entry: LiveCensus::g_table) entry = {};
		const auto flips0 = g_flips.load();
		const auto tsc0   = __rdtsc();
		const auto t0     = std::chrono::steady_clock::now();
		LiveCensus::g_on.store(true);
		std::this_thread::sleep_for(std::chrono::duration<double>(std::strtod(arg1, nullptr)));
		LiveCensus::g_on.store(false);
		const auto   frames = std::max<uint64_t>(g_flips.load() - flips0, 1);
		const double ns_per_cycle =
		    std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() /
		    static_cast<double>(__rdtsc() - tsc0);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		Dl_info self {};
		dladdr(reinterpret_cast<void*>(&Flip), &self);
		std::printf("LIVE_CENSUS id=%" PRIu64 " frames=%" PRIu64 " base=%016" PRIx64 " exe=%s\n", id, frames,
		            reinterpret_cast<uint64_t>(self.dli_fbase), self.dli_fname ? self.dli_fname : "?");
		for (const auto& entry: LiveCensus::g_table) {
			if (!entry.used) continue;
			std::printf("LIVE_CENSUS_ENTRY id=%" PRIu64 " kind=%u a=%016" PRIx64 " b=%016" PRIx64
			            " calls_per_frame=%.2f ms_per_frame=%.4f\n",
			            id, entry.kind, entry.a, entry.b, static_cast<double>(entry.calls) / frames,
			            static_cast<double>(entry.cycles) * ns_per_cycle / 1e6 / frames);
		}
	} else if (cmd == "granules" && n >= 2) {
		// granules <seconds>: per 1 MiB granule, per frame: window pages, re-armed pages,
		// upload bytes and upload copies.
		for (auto& granule: LiveCounters::g_granules) {
			granule.tag.store(0);
			for (auto& value: granule.values) value.store(0);
		}
		const auto flips0 = g_flips.load();
		LiveCounters::g_granules_on.store(true);
		std::this_thread::sleep_for(std::chrono::duration<double>(std::strtod(arg1, nullptr)));
		LiveCounters::g_granules_on.store(false);
		const auto frames = std::max<uint64_t>(g_flips.load() - flips0, 1);
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		for (const auto& granule: LiveCounters::g_granules) {
			const auto tag = granule.tag.load();
			if (tag == 0) continue;
			std::printf("LIVE_GRANULE id=%" PRIu64 " address=%09" PRIx64 " window_pages=%.1f reprotect_pages=%.1f"
			            " upload_kb=%.1f upload_calls=%.1f gpu_writes=%.2f gpu_write_kb=%.1f\n",
			            id, (tag - 1) << 20u, double(granule.values[0].load()) / frames,
			            double(granule.values[1].load()) / frames, double(granule.values[2].load()) / 1024.0 / frames,
			            double(granule.values[3].load()) / frames, double(granule.values[4].load()) / frames,
			            double(granule.values[5].load()) / 1024.0 / frames);
		}
		std::printf("LIVE_GRANULE_OVERFLOW id=%" PRIu64 " count=%" PRIu64 "\n", id, LiveCounters::g_granule_overflow.load());
	} else if (cmd == "pm4" && n >= 2) {
		// pm4 <seconds>: PM4 packets per frame by opcode.
		std::array<uint64_t, 256> before {};
		for (size_t i = 0; i < before.size(); ++i) before[i] = LiveCounters::g_pm4[i].load();
		const auto flips0 = g_flips.load();
		std::this_thread::sleep_for(std::chrono::duration<double>(std::strtod(arg1, nullptr)));
		const auto frames = std::max<uint64_t>(g_flips.load() - flips0, 1);
		for (size_t i = 0; i < before.size(); ++i) {
			const auto count = LiveCounters::g_pm4[i].load() - before[i];
			if (count != 0)
				std::printf("LIVE_PM4 id=%" PRIu64 " opcode=0x%02zx per_frame=%.2f\n", id, i,
				            static_cast<double>(count) / static_cast<double>(frames));
		}
	} else if (cmd == "sleep" && n >= 2) {
		std::this_thread::sleep_for(std::chrono::duration<double>(std::strtod(arg1, nullptr)));
	} else {
		std::printf("LIVE_ERROR id=%" PRIu64 " line=%s\n", id, line.c_str());
	}
	std::fflush(stdout);
}

// Called on the render thread before it consumes commands.
inline void Start() {
	g_render_thread = pthread_self();
	g_render_tid.store(static_cast<pid_t>(syscall(SYS_gettid)));
	LiveCensus::g_render = true;
	pthread_attr_t attr;
	if (pthread_getattr_np(pthread_self(), &attr) == 0) {
		void*  stack = nullptr;
		size_t size  = 0;
		if (pthread_attr_getstack(&attr, &stack, &size) == 0) {
			g_stack_low  = reinterpret_cast<uint64_t>(stack);
			g_stack_high = g_stack_low + size;
		}
		pthread_attr_destroy(&attr);
	}
	g_render_known.store(true);
	static const char* const path = std::getenv("KYTY_LIVE_FILE");
	if (path == nullptr) return;
	std::thread([] {
		pthread_setname_np(pthread_self(), "Kyty.Live");
		uint64_t last = 0;
		for (;;) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			std::FILE* file = std::fopen(path, "re");
			if (file == nullptr) continue;
			std::string text;
			char        buffer[4096];
			for (size_t got; (got = std::fread(buffer, 1, sizeof(buffer), file)) > 0;) text.append(buffer, got);
			std::fclose(file);
			unsigned long long id = 0;
			if (std::sscanf(text.c_str(), "id %llu", &id) != 1 || id == last) continue;
			last         = id;
			size_t begin = text.find('\n');
			while (begin != std::string::npos && begin + 1 < text.size()) {
				const size_t end = text.find('\n', begin + 1);
				Run(id, text.substr(begin + 1, end == std::string::npos ? std::string::npos : end - begin - 1));
				begin = end;
			}
			std::printf("LIVE_DONE id=%llu\n", id);
			std::fflush(stdout);
		}
	}).detach();
}

} // namespace LiveControl
