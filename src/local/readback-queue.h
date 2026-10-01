#pragma once
// KYTY_READBACK_QUEUE: guest readback copies of GPU data that is already written run on a
// transfer-only queue (the copy engine). A copy waits on the graphics timeline only for the
// last GPU write of the bytes it reads, instead of queueing behind everything submitted to the
// single graphics queue (a guest reading data written frames ago otherwise waits for the whole
// in-flight frame). Buffers are then shared by both queue families (VK_SHARING_MODE_CONCURRENT).
//
// Recording belongs to the GPU thread and bypasses the recording worker: this queue is not
// ordered with the graphics command stream, only with the graphics timeline. With deferred
// submits (KYTY_DEFERRED_SUBMIT) the submission itself goes through the worker, after the
// graphics submissions queued before it: a copy never waits on a tick the driver has not been
// given yet. Windows drivers stall presents, and every submission behind their device lock, on
// such a wait-before-signal, and the worker's own submission of that tick is one of them.

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"
#ifdef KYTY_LOCAL_VULKAN_RECORDING
#include "vulkan-recording.h"
#endif

#include <array>
#include <atomic>
#include <cstdint>
#include <span>

namespace ReadbackQueue {

class Queue {
public:
	static constexpr size_t Buffers = 64;
	struct Region {
		VkBuffer     source;
		VkBufferCopy copy;
	};
	// One copy's queue submission (plain data: it is copied into the recording stream).
	struct SubmitPacket {
		VkQueue         queue;
		VkCommandBuffer command;
		VkSemaphore     wait;
		uint64_t        wait_value;
		VkSemaphore     signal;
		uint64_t        signal_value;
	};
	template <typename Dispatch>
	static void Submit(const SubmitPacket& packet, const Dispatch& dispatch) {
		VkTimelineSemaphoreSubmitInfo values {};
		values.sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
		values.waitSemaphoreValueCount   = 1;
		values.pWaitSemaphoreValues      = &packet.wait_value;
		values.signalSemaphoreValueCount = 1;
		values.pSignalSemaphoreValues    = &packet.signal_value;
		const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		VkSubmitInfo submit {};
		submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.pNext                = &values;
		submit.waitSemaphoreCount   = 1;
		submit.pWaitSemaphores      = &packet.wait;
		submit.pWaitDstStageMask    = &stage;
		submit.commandBufferCount   = 1;
		submit.pCommandBuffers      = &packet.command;
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores    = &packet.signal;
		Check(dispatch.vkQueueSubmit(packet.queue, 1, &submit, VK_NULL_HANDLE), "submit readback copy");
	}
#ifdef KYTY_LOCAL_VULKAN_RECORDING
	static void ReplaySubmit(std::span<const LocalVulkanRecording::Segment> segments,
	                         const vk::detail::DispatchLoaderDynamic&      dispatch) {
		Submit(*static_cast<const SubmitPacket*>(segments[0].data), dispatch);
	}
#endif

	explicit Queue(Libs::Graphics::GraphicContext& graphics)
	    :
#ifdef KYTY_LOCAL_VULKAN_RECORDING
	      d(LocalVulkanRecording::DirectDispatch()),
#else
	      d(VULKAN_HPP_DEFAULT_DISPATCHER),
#endif
	      m_device(graphics.device), m_queue(graphics.readback_queue) {
		VkCommandPoolCreateInfo pool {};
		pool.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pool.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
		pool.queueFamilyIndex = graphics.readback_family;
		Check(d.vkCreateCommandPool(m_device, &pool, nullptr, &m_pool), "create readback command pool");
		VkCommandBufferAllocateInfo allocate {};
		allocate.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocate.commandPool        = m_pool;
		allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocate.commandBufferCount = Buffers;
		Check(d.vkAllocateCommandBuffers(m_device, &allocate, m_commands.data()), "allocate readback commands");
		VkSemaphoreTypeCreateInfo type {};
		type.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
		type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		VkSemaphoreCreateInfo semaphore {};
		semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		semaphore.pNext = &type;
		Check(d.vkCreateSemaphore(m_device, &semaphore, nullptr, &m_semaphore), "create readback timeline");
	}
	~Queue() {
		(void)d.vkQueueWaitIdle(m_queue);
		d.vkDestroySemaphore(m_device, m_semaphore, nullptr);
		d.vkDestroyCommandPool(m_device, m_pool, nullptr);
	}
	Queue(const Queue&)            = delete;
	Queue& operator=(const Queue&) = delete;

	// GPU thread: copies `regions` to `destination` once `timeline` reaches `wait_value`.
	// Returns the value this queue's timeline reaches when the bytes are available to the host.
	uint64_t Copy(std::span<const Region> regions, VkBuffer destination, VkSemaphore timeline,
	              uint64_t wait_value) {
		const uint64_t value   = ++m_submitted;
		const size_t   index   = value % Buffers;
		const auto     command = m_commands[index];
		if (m_used[index] != 0) Wait(m_used[index]);
		m_used[index] = value;
		Check(d.vkResetCommandBuffer(command, 0), "reset readback commands");
		VkCommandBufferBeginInfo begin {};
		begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		Check(d.vkBeginCommandBuffer(command, &begin), "begin readback commands");
		// An earlier copy on this queue may target the same download slot (a detached request).
		VkMemoryBarrier barrier {};
		barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		d.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
		                       &barrier, 0, nullptr, 0, nullptr);
		for (const auto& region: regions) d.vkCmdCopyBuffer(command, region.source, destination, 1, &region.copy);
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		d.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier,
		                       0, nullptr, 0, nullptr);
		Check(d.vkEndCommandBuffer(command), "end readback commands");
		const SubmitPacket packet {m_queue, command, timeline, wait_value, m_semaphore, value};
#ifdef KYTY_LOCAL_VULKAN_RECORDING
		const LocalVulkanRecording::Segment segments[] {{&packet, sizeof(packet)}};
		if (LocalVulkanRecording::EnqueueDeferred(ReplaySubmit, segments, true)) return value;
#endif
		Submit(packet, d);
		return value;
	}

	// GPU thread: the value of the last copy handed to this queue.
	[[nodiscard]] uint64_t Submitted() const { return m_submitted; }
	// Any thread.
	[[nodiscard]] bool Done(uint64_t value) {
		if (m_known.load(std::memory_order_acquire) >= value) return true;
		uint64_t counter = 0;
		Check(d.vkGetSemaphoreCounterValue(m_device, m_semaphore, &counter), "read readback timeline");
		Advance(counter);
		return counter >= value;
	}
	void Wait(uint64_t value) {
		if (Done(value)) return;
		VkSemaphoreWaitInfo wait {};
		wait.sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
		wait.semaphoreCount = 1;
		wait.pSemaphores    = &m_semaphore;
		wait.pValues        = &value;
		Check(d.vkWaitSemaphores(m_device, &wait, UINT64_MAX), "wait for readback copy");
		Advance(value);
	}

private:
	static void Check(VkResult result, const char* what) {
		if (result != VK_SUCCESS) EXIT("%s failed: %d\n", what, static_cast<int>(result));
	}
	void Advance(uint64_t value) {
		uint64_t known = m_known.load(std::memory_order_relaxed);
		while (known < value && !m_known.compare_exchange_weak(known, value, std::memory_order_release)) {}
	}

	const vk::detail::DispatchLoaderDynamic& d;
	VkDevice                                 m_device;
	VkQueue                                  m_queue;
	VkCommandPool                            m_pool      = VK_NULL_HANDLE;
	VkSemaphore                              m_semaphore = VK_NULL_HANDLE;
	std::array<VkCommandBuffer, Buffers>     m_commands {};
	std::array<uint64_t, Buffers>            m_used {};
	uint64_t                                 m_submitted = 0;
	std::atomic<uint64_t>                    m_known {0};
};

} // namespace ReadbackQueue
