#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_BDADIRTYREGIONS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_BDADIRTYREGIONS_H_

#include "graphics/host_gpu/regionDefinitions.h"

#include <atomic>
#include <cstdint>

// One bit per tracker region: set whenever the region's CPU modification epoch may have
// moved, or every bit when the BDA region list or the buffer registrations change. A clear
// bit proves that every BDA region request inside the region still holds its epochs, so a
// bounded BDA preparation only visits the set bits of its (usually 4 GiB) ranges.
namespace Libs::Graphics::BdaDirtyRegions {

inline constexpr uint64_t Regions = TRACKER_ADDRESS_SIZE / TRACKER_REGION_SIZE;
inline constexpr uint64_t Words   = Regions / 64;

inline std::atomic<uint64_t> g_bits[Words] = {};

inline void Mark(uint64_t region) {
	g_bits[region / 64].fetch_or(uint64_t {1} << (region % 64), std::memory_order_release);
}

inline void MarkAll() {
	for (auto& word: g_bits) word.store(~uint64_t {0}, std::memory_order_release);
}

} // namespace Libs::Graphics::BdaDirtyRegions

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_BDADIRTYREGIONS_H_
