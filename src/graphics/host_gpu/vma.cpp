#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <vector>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-private-field"
#pragma clang diagnostic ignored "-Wunused-variable"
#endif

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"
#include "live-counters.h"
#include "vram-pressure.h"

#include <cstdio>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <utility>

extern "C" {
// 8 GB GPUs (vram-pressure.h): 0 off, 1 on, 2 auto (the default: on when the device-local budget at
// start is under 10 GiB on a discrete GPU; a 16 GB or larger GPU keeps the previous behaviour).
[[gnu::used]] volatile std::atomic<uint32_t> kyty_local_vram_adaptive_mode {VramPressure::Auto};
[[gnu::used]] volatile std::atomic<uint32_t> kyty_local_image_pool_trim_mode {VramPressure::Auto};
[[gnu::used]] volatile std::atomic<uint32_t> kyty_local_texture_gc_time_mode {VramPressure::Auto};
[[gnu::used]] volatile std::atomic<uint32_t> kyty_local_vram_log_mode {VramPressure::Auto};
}

namespace Libs::Graphics {

void FlushBufferReclaimer(); // streamBuffer.cpp (KYTY_BUFFER_RECLAIM)

// Local diagnostic (live `vma <path>`): VMA is thread-safe, so the live thread writes it.
static VmaAllocator g_report_allocator = nullptr;
// Freed images kept for reuse (KYTY_IMAGE_POOL): at most 1 GiB, and a 16th of the GPU's memory budget.
static uint64_t g_image_pool_limit = 1024ull << 20;
// KYTY_VRAM_ADAPTIVE / KYTY_IMAGE_POOL_TRIM: the pool's limit this frame (RefreshMemoryPressure),
// UINT64_MAX while both are off (g_image_pool_limit then). DeleteImage reads it on whatever thread
// retires the image.
static std::atomic<uint64_t> g_image_pool_frame_limit {UINT64_MAX};
// The device-local heaps' budget and GetTotalMemoryBudget() when the allocator was created.
static uint64_t g_start_heap_budget  = 0;
static uint64_t g_start_total_budget = 0;
static void WriteVmaReport(const char* path) {
	if (g_report_allocator == nullptr) return;
	char* json = nullptr;
	vmaBuildStatsString(g_report_allocator, &json, VK_TRUE);
	if (FILE* file = std::fopen(path, "wb"); file != nullptr) {
		std::fputs(json, file);
		std::fclose(file);
	}
	vmaFreeStatsString(g_report_allocator, json);
}

bool GraphicContext::CreateAllocator() {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(instance == nullptr || physical_device == nullptr || device == nullptr ||
	        allocator != nullptr);

	VmaVulkanFunctions functions {};
	functions.vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
	functions.vkGetDeviceProcAddr   = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

	VmaAllocatorCreateInfo info {};
	info.instance         = instance;
	info.physicalDevice   = physical_device;
	info.device           = device;
	info.pVulkanFunctions = &functions;
	info.vulkanApiVersion = VULKAN_TARGET_API_VERSION;
	info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	if (memory_budget_ext_enabled) {
		info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
	}
	// KYTY_VRAM_LIMIT_MB=<n>: video memory as on a GPU with n MiB (VMA's heap size limit): its budget,
	// and allocations past it failing, for tests of memory pressure on smaller GPUs.
	std::array<VkDeviceSize, VK_MAX_MEMORY_HEAPS> heap_limits {};
	if (const char* text = std::getenv("KYTY_VRAM_LIMIT_MB"); text != nullptr && std::strtoull(text, nullptr, 10) > 0) {
		heap_limits.fill(VK_WHOLE_SIZE);
		for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
			if (physical_device_memory_properties.memoryHeaps[heap].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
				heap_limits[heap] = std::strtoull(text, nullptr, 10) << 20u;
			}
		}
		info.pHeapSizeLimit = heap_limits.data();
	}

	const auto result = static_cast<vk::Result>(vmaCreateAllocator(&info, &allocator));
	if (result != vk::Result::eSuccess) {
		LOGF("vmaCreateAllocator failed: %s\n", vk::to_string(result).c_str());
		return false;
	}
	g_report_allocator          = allocator;
	LiveCounters::g_vma_report = WriteVmaReport;
	g_image_pool_limit         = std::min<uint64_t>(1024ull << 20, GetTotalMemoryBudget() / 16);
	// KYTY_VRAM_* "auto" (vram-pressure.h): an RTX 3070 Ti reports 7011 MiB for heap 0 (plus a small
	// BAR heap), 16 GB GPUs about 15 GiB. KYTY_VRAM_LIMIT_MB counts (VMA caps the budget with it).
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	uint64_t heap_budget = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		if (physical_device_memory_properties.memoryHeaps[heap].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
			heap_budget += memory_budget_ext_enabled ? budgets[heap].budget
			                                         : physical_device_memory_properties.memoryHeaps[heap].size;
		}
	}
	g_start_heap_budget  = heap_budget;
	g_start_total_budget = GetTotalMemoryBudget();
	VramPressure::g_small.store(memory_budget_ext_enabled &&
	                            physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu &&
	                            g_start_heap_budget < VramPressure::SmallBudget);
	return true;
}

static void DestroyImagePool(VmaAllocator allocator);

void GraphicContext::DestroyAllocator() {
	if (allocator == nullptr) {
		return;
	}
	LiveCounters::g_vma_report = nullptr;
	g_report_allocator         = nullptr;
	FlushBufferReclaimer();
	DestroyImagePool(allocator);
	vmaDestroyAllocator(allocator);
	allocator = nullptr;
}

void GraphicContext::LogMemoryBudget() const {
	if (allocator == nullptr || physical_device == nullptr) {
		return;
	}

	const auto& properties = GetPhysicalDeviceMemoryProperties();
	VmaBudget   budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	for (uint32_t i = 0; i < properties.memoryHeapCount; i++) {
		LOGF("VMA heap %u: usage=%" PRIu64 ", budget=%" PRIu64 ", allocation=%" PRIu64
		     ", blocks=%" PRIu64 "\n",
		     i, static_cast<uint64_t>(budgets[i].usage), static_cast<uint64_t>(budgets[i].budget),
		     static_cast<uint64_t>(budgets[i].statistics.allocationBytes),
		     static_cast<uint64_t>(budgets[i].statistics.blockBytes));
	}
}

uint64_t GraphicContext::GetDeviceMemoryUsage() const {
	if (!CanReportMemoryUsage() || allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	uint64_t usage = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const bool device_local =
		    static_cast<bool>(physical_device_memory_properties.memoryHeaps[heap].flags &
		                      vk::MemoryHeapFlagBits::eDeviceLocal);
		if (!discrete || device_local) {
			usage += budgets[heap].usage;
		}
	}
	return usage;
}

uint64_t GraphicContext::GetTotalMemoryBudget() const {
	if (allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	uint64_t budget = 0;
	uint64_t local  = 0;
	uint64_t usage  = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const auto& properties = physical_device_memory_properties.memoryHeaps[heap];
		const bool  device_local =
		    static_cast<bool>(properties.flags & vk::MemoryHeapFlagBits::eDeviceLocal);
		if (device_local) {
			local += properties.size;
		}
		if (!discrete || device_local) {
			budget += CanReportMemoryUsage() ? budgets[heap].budget : properties.size;
			usage += CanReportMemoryUsage() ? budgets[heap].usage : 0;
		}
	}
	if (discrete) {
		return budget - std::min<uint64_t>(budget / 8, 1024ull * 1024 * 1024);
	}
	constexpr uint64_t system_reserve = 8ull * 1024 * 1024 * 1024;
	const auto         available      = budget > usage ? budget - usage : uint64_t {0};
	return std::max(local, available > system_reserve ? available - system_reserve : uint64_t {0});
}

// Recycling of freed images (KYTY_IMAGE_POOL). The game aliases transient render
// targets in one heap, so the texture cache deletes and re-creates the same few
// images every frame. An image reaches DeleteImage only after the GPU is done
// with it; a new image always starts in eUndefined layout, so reusing the VkImage
// and its memory for identical creation parameters is equivalent to a fresh
// allocation.
extern "C" {
volatile std::atomic<uint32_t> kyty_local_image_pool_mode {0};
}
namespace {
struct PooledImage {
	std::array<uint32_t, 10> key {};
	VkImage                  image      = VK_NULL_HANDLE;
	VmaAllocation            allocation = nullptr;
	uint64_t                 bytes      = 0;
};
std::mutex               g_image_pool_mutex;
std::vector<PooledImage> g_image_pool;
uint64_t                 g_image_pool_bytes = 0;

std::array<uint32_t, 10> ImagePoolKey(const vk::ImageCreateInfo& info) {
	return {static_cast<uint32_t>(static_cast<VkImageCreateFlags>(info.flags)), static_cast<uint32_t>(info.imageType),
	        static_cast<uint32_t>(info.format), info.extent.width, info.extent.height, info.extent.depth,
	        info.mipLevels, info.arrayLayers,
	        static_cast<uint32_t>(info.samples) | (static_cast<uint32_t>(info.tiling) << 16u),
	        static_cast<uint32_t>(static_cast<VkImageUsageFlags>(info.usage))};
}
std::array<uint32_t, 10> ImagePoolKey(const VulkanImage& image) {
	vk::ImageCreateInfo info {};
	info.flags       = image.flags;
	info.imageType   = image.image_type;
	info.format      = image.format;
	info.extent      = image.extent;
	info.mipLevels   = image.mip_levels;
	info.arrayLayers = image.layers;
	info.samples     = static_cast<vk::SampleCountFlagBits>(image.samples);
	info.tiling      = vk::ImageTiling::eOptimal;
	info.usage       = image.usage;
	return ImagePoolKey(info);
}
} // namespace

static void DestroyImagePool(VmaAllocator allocator) {
	std::scoped_lock lock(g_image_pool_mutex);
	for (const auto& pooled: g_image_pool) {
		vmaDestroyImage(allocator, pooled.image, pooled.allocation);
	}
	g_image_pool.clear();
	g_image_pool_bytes = 0;
}

// The oldest pooled images until the pool holds at most `limit` bytes; the bytes freed. A pooled
// image is one the GPU is done with (DeleteImage), so it can go at any time.
static uint64_t ShrinkImagePool(VmaAllocator allocator, uint64_t limit) {
	std::scoped_lock lock(g_image_pool_mutex);
	uint64_t         freed = 0;
	size_t           count = 0;
	while (g_image_pool_bytes > limit && count < g_image_pool.size()) {
		const auto& oldest = g_image_pool[count++];
		vmaDestroyImage(allocator, oldest.image, oldest.allocation);
		g_image_pool_bytes -= oldest.bytes;
		freed += oldest.bytes;
	}
	g_image_pool.erase(g_image_pool.begin(), g_image_pool.begin() + static_cast<std::ptrdiff_t>(count));
	if (freed != 0) {
		LiveCounters::Add(LiveCounters::ImagePoolTrims);
		LiveCounters::Add(LiveCounters::ImagePoolTrimKiB, freed >> 10u);
	}
	return freed;
}

uint64_t GraphicContext::TrimImagePool() {
	return allocator != nullptr ? ShrinkImagePool(allocator, 0) : 0;
}

// The device-local heaps' usage and budget as VMA reports them (the log's numbers).
static void DeviceLocalHeaps(VmaAllocator allocator, const vk::PhysicalDeviceMemoryProperties& properties,
                             uint64_t& usage, uint64_t& budget) {
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	usage = budget = 0;
	for (uint32_t heap = 0; heap < properties.memoryHeapCount; heap++) {
		if (properties.memoryHeaps[heap].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
			usage += budgets[heap].usage;
			budget += budgets[heap].budget;
		}
	}
}

// KYTY_VRAM_ADAPTIVE, KYTY_IMAGE_POOL_TRIM, KYTY_TEXTURE_GC_TIME, KYTY_VRAM_LOG (vram-pressure.h):
// once a frame on the render thread, before the texture cache's collection (which reads the
// snapshot, as does the buffer cache's after it).
void GraphicContext::RefreshMemoryPressure() {
	using Clock = std::chrono::steady_clock;
	using VramPressure::Active;
	if (allocator == nullptr) return;
	const auto mode_text = [](const volatile std::atomic_uint32_t& mode) {
		const auto value = mode.load(std::memory_order_relaxed);
		return value == 0 ? "0" : value == 1 ? "1" : Active(mode) ? "auto(on)" : "auto(off)";
	};
	static bool announced = false;
	if (!std::exchange(announced, true)) {
		std::printf("VRAM: device-local budget %" PRIu64 " MiB at start%s: KYTY_VRAM_ADAPTIVE=%s KYTY_IMAGE_POOL_TRIM=%s "
		            "(KYTY_IMAGE_POOL=%u) KYTY_TEXTURE_GC_TIME=%s KYTY_VRAM_LOG=%s\n",
		            g_start_heap_budget >> 20u,
		            !memory_budget_ext_enabled ? " (no VK_EXT_memory_budget: the switches have no effect)"
		            : VramPressure::g_small.load() ? " (small GPU: auto is on)" : " (auto is off)",
		            mode_text(kyty_local_vram_adaptive_mode), mode_text(kyty_local_image_pool_trim_mode),
		            kyty_local_image_pool_mode.load(std::memory_order_relaxed), mode_text(kyty_local_texture_gc_time_mode),
		            mode_text(kyty_local_vram_log_mode));
		std::fflush(stdout);
	}
	auto&      snapshot = VramPressure::g_snapshot;
	const bool adaptive = Active(kyty_local_vram_adaptive_mode);
	const bool trim     = Active(kyty_local_image_pool_trim_mode);
	const bool log      = Active(kyty_local_vram_log_mode);
	if (!memory_budget_ext_enabled ||
	    (!adaptive && !trim && !log && !Active(kyty_local_texture_gc_time_mode))) {
		// All off (also after a live switch back): the pool's own limit, the caches' budget at start.
		snapshot = {};
		g_image_pool_frame_limit.store(UINT64_MAX, std::memory_order_relaxed);
		return;
	}
	const auto now = Clock::now();
	// VMA asks the driver for the budget only every 30 allocations and frees; other programs taking
	// video memory (a browser, the compositor) would show up late or never. At most every 100 ms,
	// here: one vkGetPhysicalDeviceMemoryProperties2.
	static Clock::time_point fetched {};
	static uint32_t          frame_index = 0;
	if (adaptive && now - fetched >= std::chrono::milliseconds(100)) {
		frame_index = (frame_index + 1) & 0x7fffffffu;
		vmaSetCurrentFrameIndex(allocator, frame_index);
		fetched = now;
	}
	snapshot.budget          = adaptive ? GetTotalMemoryBudget() : g_start_total_budget;
	snapshot.usage           = GetDeviceMemoryUsage();
	const auto thresholds    = VramPressure::TextureThresholds(snapshot.budget);
	snapshot.level           = snapshot.usage >= snapshot.budget       ? VramPressure::Critical
	                           : snapshot.usage >= thresholds.critical ? VramPressure::High
	                           : snapshot.usage >= thresholds.pressure ? VramPressure::Pressured
	                                                                   : VramPressure::Normal;
	// The freed-image pool: KYTY_VRAM_ADAPTIVE keeps its 16th of the budget (at most 1 GiB) to the
	// budget of now. KYTY_IMAGE_POOL_TRIM: on an 8 GB GPU 191 MiB (a 32nd of the 6135 MiB budget; the
	// 16th was 383 MiB), 47 MiB under high pressure and nothing past the collections' budget (the
	// driver's less an eighth: from there 876 MiB are left before buffers go to system memory).
	uint64_t pool_limit = UINT64_MAX;
	if (adaptive || trim) {
		pool_limit = adaptive ? std::min<uint64_t>(1024ull << 20, snapshot.budget / 16) : g_image_pool_limit;
		if (trim) {
			pool_limit = std::min(pool_limit, snapshot.level == VramPressure::Critical ? 0
			                                  : snapshot.level == VramPressure::High   ? snapshot.budget / 128
			                                                                           : snapshot.budget / 32);
		}
		ShrinkImagePool(allocator, pool_limit);
	}
	g_image_pool_frame_limit.store(pool_limit, std::memory_order_relaxed);

	// The level's changes (at most one line every 2 s, with how many there were) and, with
	// KYTY_VRAM_LOG, a line every KYTY_VRAM_LOG_SECONDS: what the A/B of these switches reads.
	static VramPressure::Level printed = VramPressure::Levels;
	static Clock::time_point   printed_at {};
	static uint32_t            changes = 0;
	static VramPressure::Level last    = VramPressure::Levels;
	if (snapshot.level != last) {
		last = snapshot.level;
		++changes;
	}
	const auto heaps = [&] {
		uint64_t usage = 0, budget = 0;
		DeviceLocalHeaps(allocator, physical_device_memory_properties, usage, budget);
		size_t   images = 0;
		uint64_t pooled = 0;
		{
			std::scoped_lock lock(g_image_pool_mutex);
			images = g_image_pool.size();
			pooled = g_image_pool_bytes;
		}
		std::printf("device-local usage %" PRIu64 " of %" PRIu64 " MiB (collection budget %" PRIu64 ", pressure %" PRIu64
		            ", critical %" PRIu64 " MiB), image pool %" PRIu64 " MiB, %zu images",
		            usage >> 20u, budget >> 20u, snapshot.budget >> 20u, thresholds.pressure >> 20u,
		            thresholds.critical >> 20u, pooled >> 20u, images);
	};
	if ((adaptive || log) && snapshot.level != printed && now - printed_at >= std::chrono::seconds(2)) {
		std::printf("VRAM: pressure %s -> %s (%u change%s): ", printed == VramPressure::Levels ? "start" : VramPressure::LevelNames[printed],
		            VramPressure::LevelNames[snapshot.level], changes, changes == 1 ? "" : "s");
		heaps();
		std::printf("\n");
		std::fflush(stdout);
		printed    = snapshot.level;
		printed_at = now;
		changes    = 0;
	}
	static const double log_seconds = [] {
		const char* text  = std::getenv("KYTY_VRAM_LOG_SECONDS");
		const double value = text != nullptr ? std::atof(text) : 10.0;
		return value >= 1.0 ? value : 10.0;
	}();
	static Clock::time_point                  logged_at = now;
	static std::array<uint64_t, 6>            logged {};
	static constexpr std::array<LiveCounters::Id, 6> counted {
	    LiveCounters::TextureGcDeletes, LiveCounters::TextureGcDeleteKiB, LiveCounters::ImagePoolTrims,
	    LiveCounters::ImagePoolTrimKiB, LiveCounters::SysmemFallbacks,    LiveCounters::SysmemFallbackKiB};
	if (log && now - logged_at >= std::chrono::duration<double>(log_seconds)) {
		std::array<uint64_t, 6> delta {};
		for (size_t i = 0; i < counted.size(); ++i) {
			const auto value = LiveCounters::Value(counted[i]);
			delta[i]         = value - logged[i];
			logged[i]        = value;
		}
		std::printf("VRAM %.0f s: %s, ", std::chrono::duration<double>(now - logged_at).count(),
		            VramPressure::LevelNames[snapshot.level]);
		heaps();
		std::printf(" (limit %" PRIu64 " MiB); texture collection deleted %" PRIu64 " (%" PRIu64
		            " MiB), pool trims %" PRIu64 " (%" PRIu64 " MiB), system-memory fallbacks %" PRIu64 " (%" PRIu64
		            " MiB, %" PRIu64 " in all)\n",
		            (pool_limit != UINT64_MAX ? pool_limit : g_image_pool_limit) >> 20u, delta[0], delta[1] >> 10u,
		            delta[2], delta[3] >> 10u, delta[4], delta[5] >> 10u, logged[4]);
		std::fflush(stdout);
		logged_at = now;
	}
}

// Out of video memory: the first few times say so in the log, with the heaps' budgets.
void GraphicContext::ReportMemoryFallback(const char* what, uint64_t bytes) const {
	static std::atomic<uint32_t> reported {0};
	if (reported.fetch_add(1, std::memory_order_relaxed) >= 8) return;
	std::printf("Vulkan: video memory full, %s (%" PRIu64 " bytes)\n", what, bytes);
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	for (uint32_t i = 0; i < GetPhysicalDeviceMemoryProperties().memoryHeapCount; i++) {
		std::printf("  heap %u: usage %" PRIu64 " MiB of budget %" PRIu64 " MiB\n", i, budgets[i].usage >> 20u,
		            budgets[i].budget >> 20u);
	}
	std::fflush(stdout);
}

bool GraphicContext::CreateImage(const vk::ImageCreateInfo& image_info, VulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image != nullptr || image.allocation != nullptr);

	if (kyty_local_image_pool_mode.load(std::memory_order_relaxed) != 0 && image_info.pNext == nullptr &&
	    image_info.tiling == vk::ImageTiling::eOptimal &&
	    image_info.initialLayout == vk::ImageLayout::eUndefined) {
		const auto key = ImagePoolKey(image_info);
		std::scoped_lock lock(g_image_pool_mutex);
		for (auto it = g_image_pool.begin(); it != g_image_pool.end(); ++it) {
			if (it->key != key) {
				continue;
			}
			image.image      = it->image;
			image.allocation = it->allocation;
			g_image_pool_bytes -= it->bytes;
			g_image_pool.erase(it);
			image.format     = image_info.format;
			image.image_type = image_info.imageType;
			image.extent     = image_info.extent;
			image.layers     = image_info.arrayLayers;
			image.mip_levels = image_info.mipLevels;
			image.samples    = static_cast<uint32_t>(image_info.samples);
			image.usage      = image_info.usage;
			image.flags      = image_info.flags;
			image.state      = {.layout = image_info.initialLayout};
			image.subresource_states.clear();
			return true;
		}
	}

	VmaAllocationCreateInfo alloc_info {};
	alloc_info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	const auto create = [&] {
		vk::Image::CType native_image = VK_NULL_HANDLE;
		const auto       result       = static_cast<vk::Result>(
		    vmaCreateImage(allocator, static_cast<const vk::ImageCreateInfo::NativeType*>(image_info),
		                   &alloc_info, &native_image, &image.allocation, nullptr));
		image.image = native_image;
		return result == vk::Result::eSuccess;
	};
	// Out of video memory: without the freed images kept for reuse, then in system memory (slower
	// to sample, but the game goes on).
	if (!create()) {
		TrimImagePool();
		if (!create()) {
			alloc_info.requiredFlags  = 0;
			alloc_info.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
			const bool created        = create();
			VmaAllocationInfo allocated {};
			if (created) vmaGetAllocationInfo(allocator, image.allocation, &allocated);
			ReportMemoryFallback(created ? "an image is in system memory" : "an image could not be created",
			                     allocated.size);
			if (!created) return false;
			VkMemoryPropertyFlags placed = 0;
			vmaGetAllocationMemoryProperties(allocator, image.allocation, &placed);
			if ((placed & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0) {
				LiveCounters::Add(LiveCounters::SysmemFallbacks);
				LiveCounters::Add(LiveCounters::SysmemFallbackKiB, allocated.size >> 10u);
			}
		}
	}

	image.format     = image_info.format;
	image.image_type = image_info.imageType;
	image.extent     = image_info.extent;
	image.layers     = image_info.arrayLayers;
	image.mip_levels = image_info.mipLevels;
	image.samples    = static_cast<uint32_t>(image_info.samples);
	image.usage      = image_info.usage;
	image.flags      = image_info.flags;
	image.state      = {.layout = image_info.initialLayout};
	image.subresource_states.clear();

	return true;
}

void GraphicContext::DeleteImage(VulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image == nullptr || image.allocation == nullptr);

	if (kyty_local_image_pool_mode.load(std::memory_order_relaxed) != 0) {
		VmaAllocationInfo allocation_info {};
		vmaGetAllocationInfo(allocator, image.allocation, &allocation_info);
		std::scoped_lock lock(g_image_pool_mutex);
		g_image_pool.push_back({ImagePoolKey(image), image.image, image.allocation, allocation_info.size});
		g_image_pool_bytes += allocation_info.size;
		const auto frame_limit = g_image_pool_frame_limit.load(std::memory_order_relaxed);
		const auto limit       = frame_limit != UINT64_MAX ? frame_limit : g_image_pool_limit;
		while (g_image_pool_bytes > limit && !g_image_pool.empty()) {
			auto& oldest = g_image_pool.front();
			vmaDestroyImage(allocator, oldest.image, oldest.allocation);
			g_image_pool_bytes -= oldest.bytes;
			g_image_pool.erase(g_image_pool.begin());
		}
		image.image      = nullptr;
		image.allocation = nullptr;
		return;
	}
	vmaDestroyImage(allocator, image.image, image.allocation);
	image.image      = nullptr;
	image.allocation = nullptr;
}

} // namespace Libs::Graphics
