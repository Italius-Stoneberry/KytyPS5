#pragma once
// KYTY_ASYNC_UPLOAD: buffer upload copies (guest backing -> host-visible staging memory) run on
// one worker, in the order the render thread recorded them; a queue submission first waits for
// the copies recorded before it. The render thread only records the copy commands.
//
// The worker reads the backing view, never the guest range: a guest write or protection change
// cannot fault it, and unmapping drains the queue before the range can be reused.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <pthread.h>
#include <sched.h>
#include <thread>
#include <x86intrin.h>

extern "C" {
extern volatile std::atomic<uint32_t> kyty_local_async_upload_mode;
}

namespace AsyncUpload {

class Worker {
public:
	static constexpr uint64_t Capacity = 1u << 16;

	Worker(): m_jobs(std::make_unique<Job[]>(Capacity)), m_thread([this] { Run(); }) {}
	~Worker() {
		m_stop.store(true, std::memory_order_seq_cst);
		m_head.fetch_add(0, std::memory_order_seq_cst);
		m_head.notify_all();
		m_thread.join();
	}
	Worker(const Worker&)            = delete;
	Worker& operator=(const Worker&) = delete;

	// Render thread only. Returns the sequence number that covers this copy.
	uint64_t Push(uint8_t* destination, const uint8_t* source, uint64_t size) {
		const uint64_t head = m_head.load(std::memory_order_relaxed);
		while (head - m_done.load(std::memory_order_acquire) >= Capacity) {
			Kick();
			_mm_pause();
		}
		m_jobs[head % Capacity] = {destination, source, size};
		m_head.store(head + 1, std::memory_order_seq_cst);
		return head + 1;
	}
	// Render thread only: wakes the worker after a batch of pushes.
	void Kick() {
		if (m_sleeping.load(std::memory_order_seq_cst)) m_head.notify_one();
	}
	// Render thread only: the sequence number of the last pushed copy.
	[[nodiscard]] uint64_t Pushed() const { return m_head.load(std::memory_order_relaxed); }

	// Any thread: returns once every copy up to `sequence` is in staging memory.
	void Wait(uint64_t sequence) {
		if (m_done.load(std::memory_order_acquire) >= sequence) return;
		for (int spin = 0; spin < 4000; ++spin) {
			if (m_done.load(std::memory_order_acquire) >= sequence) return;
			_mm_pause();
		}
		m_waiters.fetch_add(1, std::memory_order_seq_cst);
		for (uint64_t done; (done = m_done.load(std::memory_order_seq_cst)) < sequence;)
			m_done.wait(done, std::memory_order_seq_cst);
		m_waiters.fetch_sub(1, std::memory_order_relaxed);
	}

private:
	struct Job {
		uint8_t*       destination;
		const uint8_t* source;
		uint64_t       size;
	};

	void Run() {
		(void)pthread_setname_np(pthread_self(), "Kyty.Upload");
		// Created by the render thread: do not inherit its dedicated CPU.
		if (const char* cpus = std::getenv("KYTY_RECORDING_CPUS"); cpus != nullptr) {
			cpu_set_t set;
			CPU_ZERO(&set);
			for (const char* p = cpus; *p != '\0';) {
				char*      end = nullptr;
				const long cpu = std::strtol(p, &end, 10);
				if (end == p) break;
				if (cpu >= 0 && cpu < CPU_SETSIZE) CPU_SET(static_cast<int>(cpu), &set);
				p = (*end == ',') ? end + 1 : end;
			}
			if (CPU_COUNT(&set) != 0) (void)sched_setaffinity(0, sizeof(set), &set);
		}
		uint64_t done = 0;
		for (;;) {
			uint64_t head = m_head.load(std::memory_order_acquire);
			for (int spin = 0; head == done && spin < 20000; ++spin) {
				_mm_pause();
				head = m_head.load(std::memory_order_acquire);
			}
			if (head == done) {
				m_sleeping.store(true, std::memory_order_seq_cst);
				head = m_head.load(std::memory_order_seq_cst);
				if (head == done) {
					if (m_stop.load(std::memory_order_seq_cst)) return;
					m_head.wait(done, std::memory_order_seq_cst);
				}
				m_sleeping.store(false, std::memory_order_relaxed);
				continue;
			}
			for (; done < head; ++done) {
				const Job& job = m_jobs[done % Capacity];
				std::memcpy(job.destination, job.source, job.size);
			}
			m_done.store(done, std::memory_order_seq_cst);
			if (m_waiters.load(std::memory_order_seq_cst) != 0) m_done.notify_all();
		}
	}

	std::unique_ptr<Job[]>             m_jobs;
	alignas(64) std::atomic<uint64_t>  m_head {0};
	alignas(64) std::atomic<uint64_t>  m_done {0};
	alignas(64) std::atomic<bool>      m_sleeping {false};
	std::atomic<uint32_t>              m_waiters {0};
	std::atomic<bool>                  m_stop {false};
	std::thread                        m_thread;
};

[[nodiscard]] inline bool Enabled() {
	return kyty_local_async_upload_mode.load(std::memory_order_relaxed) != 0;
}

// Set by the render thread before its first push; waits stay armed after the switch goes off.
inline std::atomic<bool> g_used {false};

inline Worker& Get() {
	// Never destroyed: threads that still record or submit may outlive static destruction.
	static Worker* const worker = new Worker();
	g_used.store(true, std::memory_order_relaxed);
	return *worker;
}

// Render thread: the sequence a submission recorded now must wait for (0: nothing pending).
[[nodiscard]] inline uint64_t SubmitSequence() {
	return g_used.load(std::memory_order_relaxed) ? Get().Pushed() : 0;
}

// Any thread, with a sequence taken on the render thread.
inline void Wait(uint64_t sequence) {
	if (sequence != 0) Get().Wait(sequence);
}

// Render thread: every recorded copy reaches staging memory before this returns.
inline void Drain() {
	Wait(SubmitSequence());
}

} // namespace AsyncUpload
