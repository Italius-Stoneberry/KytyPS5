#pragma once
// Local diagnostic: a GPU timestamp after each recorded draw, dispatch or native XPR run while
// a live `tracem` runs (live-trace.h). The timestamp is recorded in order with the command, so
// the recording worker replays it right after; it costs one relaxed load otherwise.

#include "live-trace.h"
#include "graphics/host_gpu/vulkanCommon.h"

namespace LiveTrace {

enum MarkKind : uint64_t { MarkDraw = 1, MarkDispatch = 2, MarkNativeXpr = 3 };

inline void GpuMarkNow(vk::CommandBuffer command, MarkKind kind, uint64_t shader) {
	if (!g_marks_on.load(std::memory_order_relaxed) || g_mark_pool == nullptr || !command) return;
	const auto slot = g_mark_next.fetch_add(1, std::memory_order_relaxed);
	if (slot >= MarkSlots) return;
	VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdWriteTimestamp(static_cast<VkCommandBuffer>(command),
	                                                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
	                                                  static_cast<VkQueryPool>(g_mark_pool), slot);
	Event(GpuMark, slot, (shader & 0x0fffffffffffffffull) | kind << 60u);
}

// Marks the command recorded while this object lives; `handle` yields the command buffer then.
template <typename Handle>
struct MarkAfter {
	Handle   handle;
	MarkKind kind;
	uint64_t shader;
	~MarkAfter() {
		if (g_marks_on.load(std::memory_order_relaxed)) GpuMarkNow(handle(), kind, shader);
	}
};
template <typename Handle>
MarkAfter(Handle, MarkKind, uint64_t) -> MarkAfter<Handle>;

} // namespace LiveTrace
