#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RANGESET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RANGESET_H_

#include "common/assert.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <vector>

extern "C" {
// 1: Add returns early for a range already covered, and lookups first try the
// interval the previous lookup found (valid until the set changes).
extern volatile std::atomic<uint32_t> kyty_local_range_set_fast_mode;
}

namespace Libs::Graphics {

class RangeSet final {
public:
	// The owner reads this set from one thread only (lookups then update the hint).
	void AllowFastPath() { m_fast_eligible = true; }

	struct Range {
		uint64_t address = 0;
		uint64_t size    = 0;
	};

	void Add(uint64_t address, uint64_t size) {
		const auto end = End(address, size);
		if (Fast() && Covered(address, end)) return;
		++m_version;
		auto       it  = m_ranges.lower_bound(address);
		if (it != m_ranges.begin() && std::prev(it)->second >= address) {
			it = std::prev(it);
		}
		uint64_t begin = address;
		uint64_t last  = end;
		while (it != m_ranges.end() && it->first <= last) {
			begin = std::min(begin, it->first);
			last  = std::max(last, it->second);
			it    = m_ranges.erase(it);
		}
		m_ranges.emplace(begin, last);
	}

	void Subtract(uint64_t address, uint64_t size) {
		const auto end = End(address, size);
		++m_version;
		auto       it  = m_ranges.lower_bound(address);
		if (it != m_ranges.begin() && std::prev(it)->second > address) {
			it = std::prev(it);
		}
		while (it != m_ranges.end() && it->first < end) {
			const auto begin = it->first;
			const auto last  = it->second;
			it               = m_ranges.erase(it);
			if (begin < address) {
				m_ranges.emplace(begin, address);
			}
			if (last > end) {
				m_ranges.emplace(end, last);
				break;
			}
		}
	}

	void Clear() {
		++m_version;
		m_ranges.clear();
	}

	template <typename Func>
	void ForEach(Func&& func) const {
		for (const auto& [begin, end]: m_ranges) {
			func(begin, end);
		}
	}

	[[nodiscard]] std::vector<Range> Intersections(uint64_t address, uint64_t size) const {
		std::vector<Range> result;
		ForEachIntersection(address, size, [&result](Range range) { result.push_back(range); });
		return result;
	}

	[[nodiscard]] bool Intersects(uint64_t address, uint64_t size) const {
		const auto end = End(address, size);
		auto       it  = m_ranges.lower_bound(address);
		if (it != m_ranges.begin() && std::prev(it)->second > address) {
			return true;
		}
		return it != m_ranges.end() && it->first < end;
	}

	[[nodiscard]] bool Contains(uint64_t address, uint64_t size) const {
		const auto end = End(address, size);
		if (Fast()) return Covered(address, end);
		auto       it  = m_ranges.upper_bound(address);
		if (it == m_ranges.begin()) {
			return false;
		}
		--it;
		return it->first <= address && it->second >= end;
	}

	template <typename Func>
	void ForEachIntersection(uint64_t address, uint64_t size, Func&& func) const {
		const auto end = End(address, size);
		auto       it  = m_ranges.upper_bound(address);
		if (it != m_ranges.begin()) {
			--it;
		}
		for (; it != m_ranges.end() && it->first < end; ++it) {
			const auto begin = std::max(address, it->first);
			const auto last  = std::min(end, it->second);
			if (begin < last) {
				func(Range {begin, last - begin});
			}
		}
	}

	[[nodiscard]] bool Empty() const { return m_ranges.empty(); }
	[[nodiscard]] size_t Count() const { return m_ranges.size(); }

private:
	[[nodiscard]] bool Fast() const {
		return m_fast_eligible && kyty_local_range_set_fast_mode.load(std::memory_order_relaxed) != 0;
	}

	// [address, end) lies inside one interval; remembers that interval. An interval found at
	// version V covers the same bytes while the set stays at V: a few are kept, since lookups
	// alternate between ranges (a dispatch's written buffers).
	bool Covered(uint64_t address, uint64_t end) const {
		for (const auto& hint: m_hints)
			if (hint.version == m_version && hint.begin <= address && end <= hint.end) return true;
		auto it = m_ranges.upper_bound(address);
		if (it == m_ranges.begin()) return false;
		--it;
		if (it->first > address || it->second < end) return false;
		m_hints[m_next_hint++ % m_hints.size()] = {m_version, it->first, it->second};
		return true;
	}

	static uint64_t End(uint64_t address, uint64_t size) {
		if (size == 0 || size > UINT64_MAX - address) {
			EXIT("invalid range-set address or size\n");
		}
		return address + size;
	}

	std::map<uint64_t, uint64_t> m_ranges;
	bool                         m_fast_eligible = false;
	uint64_t                     m_version = 1;
	struct Hint {
		uint64_t version = 0, begin = 0, end = 0;
	};
	mutable std::array<Hint, 4> m_hints {};
	mutable uint32_t            m_next_hint = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RANGESET_H_
