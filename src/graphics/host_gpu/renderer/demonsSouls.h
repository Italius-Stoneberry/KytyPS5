#pragma once

#include <cstdint>
#include <string_view>

namespace Libs::Graphics {
class BufferCache;
struct ShaderComputeInputInfo;
namespace DemonsSouls {
// Demon's Souls in the regions seen (PPSA01341 01.007.000, PPSA01340 01.005.000), any version: each
// code patch checks the bytes it replaces (or finds the same code elsewhere), the periodic copy its
// shader hash, and the explicit compute boundaries are the engine's.
constexpr bool IsSupportedTitle(std::string_view title) {
	return title == "PPSA01340" || title == "PPSA01341";
}
bool IsSupportedGame();
bool TryLinearCopy(const ShaderComputeInputInfo& input, BufferCache& cache, uint32_t x, uint32_t y,
                   uint32_t z, uint32_t mode);
} // namespace DemonsSouls
} // namespace Libs::Graphics
