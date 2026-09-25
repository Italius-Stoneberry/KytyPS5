#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace vk::detail { class DispatchLoaderDynamic; }

// Vulkan command recording on a worker thread (KYTY_VULKAN_RECORDING). The
// renderer remains the sole resource/cache owner; a producer scope transfers
// only fully copied Vulkan call arguments.
namespace LocalVulkanRecording {
void Install();
// Complete this producer's CPU recording before handing its Vulkan objects to
// another thread. A receiver's drain cannot flush the sender's thread-local queue.
// This does not wait for GPU execution.
void Drain();
struct Segment {
    const void* data = nullptr;
    size_t size = 0;
};
using ReplayPacket = void (*)(std::span<const Segment>, const vk::detail::DispatchLoaderDynamic&);
// Draws and descriptor encoding are recorded as whole packets.
bool PacketsEnabled();
// Segments are copied transactionally. An optional immutable owner is retained
// until replay finishes; callers never lend stack or mutable-cache pointers.
bool EnqueuePacket(ReplayPacket replay, std::span<const Segment> segments,
                   std::shared_ptr<const void> owner = {});
// Preserve order when a packet cannot fit. Does not retain any argument.
void ReplayInline(ReplayPacket replay, std::span<const Segment> segments);
uint64_t StateEpoch();
class ProducerScope {
public:
    ProducerScope();
    ~ProducerScope();
    ProducerScope(const ProducerScope&) = delete;
    ProducerScope& operator=(const ProducerScope&) = delete;
};
}
