#pragma once
// Local measurement hook (KYTY_LIVE_FILE): one boot, many same-process A/B experiments.
// A side thread polls the command file every 100 ms and runs its commands once per new
// "id N" first line, printing LIVE_* lines to stdout (tools/local/live-bench.py).
//   poke32 <host address> <hex>   write a switch (live-bench resolves `sym NAME VALUE`)
//   peek <host address> <bytes>   hex dump (<= 256 bytes)
//   measure <seconds> <label>     presented frames, fps, per-frame CPU of the render and
//                                 recording threads
//   prof <seconds> <path>         4 kHz samples of render-thread CPU time
//   profp <seconds> <path>        4 kHz samples of process CPU time (with thread ids)
//   sleep <seconds>
// Without KYTY_LIVE_FILE nothing runs; the flip hook is one relaxed increment.

#include <algorithm>
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
// Records of 5 words: pc, then the four words at rsp (callers of leaf routines), or in
// process mode the thread id with bit 63 set.
inline uint64_t              g_samples[1u << 22];
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
	if (5 * size_t(idx) + 4 < std::size(g_samples)) {
		auto* record = g_samples + 5 * size_t(idx);
		record[0]    = pc;
		if (g_process.load(std::memory_order_relaxed)) {
			record[1] = static_cast<uint64_t>(syscall(SYS_gettid)) | (1ull << 63);
			record[2] = record[3] = record[4] = 0;
		} else {
			// Word at rsp (the caller of a leaf routine), then return addresses from the
			// frame-pointer chain, bounded to this stack.
			record[1]    = *reinterpret_cast<const uint64_t*>(rsp);
			auto rbp     = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RBP]);
			for (int i = 2; i < 5; ++i) {
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

inline void Profile(uint64_t id, double seconds, const char* path, bool process) {
	if (g_render_tid.load() == 0) return;
	g_process.store(process);
	struct sigaction action {};
	action.sa_sigaction = ProfSignal;
	action.sa_flags     = SA_SIGINFO | SA_RESTART;
	sigemptyset(&action.sa_mask);
	sigaction(SIGPROF, &action, nullptr);
	clockid_t clock = CLOCK_PROCESS_CPUTIME_ID;
	if (!process && pthread_getcpuclockid(g_render_thread, &clock) != 0) return;
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
	const auto count = std::min<uint32_t>(g_sample_count.load(), std::size(g_samples) / 5);
	if (auto* out = std::fopen(path, "wb")) {
		std::fwrite(g_samples, sizeof(uint64_t), 5 * size_t(count), out);
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
}

inline void Run(uint64_t id, const std::string& line) {
	char      command[16] {}, arg1[256] {}, arg2[256] {};
	const int n = std::sscanf(line.c_str(), "%15s %255s %255s", command, arg1, arg2);
	if (n < 1) return;
	const std::string_view cmd = command;
	if (cmd == "poke32" && n == 3) {
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
	} else if ((cmd == "prof" || cmd == "profp") && n == 3) {
		Profile(id, std::strtod(arg1, nullptr), arg2, cmd == "profp");
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
