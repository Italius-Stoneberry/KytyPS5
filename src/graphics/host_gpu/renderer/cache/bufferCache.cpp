#include "graphics/host_gpu/renderer/cache/bufferCache.h"

#include "native-buffer-residency.h"
#include "live-census.h"
#include "live-counters.h"
#include "native-resource-state.h"
#include "gpu_tiler_shaders/lod_stats_pack_spv.h"

extern "C" {
volatile std::atomic<uint32_t> kyty_local_buffer_residency_mode {0};
volatile std::atomic<uint32_t> kyty_local_copy_feedback_mode {0};
// 1: pack the LOD report on the GPU instead of a CPU wait and copy.
volatile std::atomic<uint32_t> kyty_local_async_lod_stats_mode {0};
// 1: shader constants stream through a host-visible upload ring.
[[gnu::used]] volatile std::atomic_uint32_t kyty_local_stream_upload_mode {0};
extern volatile std::atomic_uint32_t kyty_local_frame_pipeline_mode;
}

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace Libs::Graphics {

namespace {

constexpr uint64_t MiB           = 1024 * 1024;
constexpr uint64_t GdsBufferSize = 64 * 1024;

// DownloadBufferMemory can process priority operations while waiting. Its
// exact intervals are retired after the entire copy batch, so a reentrant
// query must use the page tracker until that transaction has finished.
thread_local uint32_t native_buffer_download_depth = 0;
struct NativeBufferDownloadScope {
	NativeBufferDownloadScope() { ++native_buffer_download_depth; }
	~NativeBufferDownloadScope() { --native_buffer_download_depth; }
};

} // namespace

void BufferCache::WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source,
                                  uint64_t size) {
	auto* bytes = static_cast<const uint8_t*>(source);
	while (size != 0) {
		const auto chunk  = std::min(size, m_staging_buffer.Size());
		const auto offset = m_staging_buffer.Copy(bytes, chunk, 4);
		buffer.CopyFrom(m_scheduler.Current(), m_staging_buffer, offset, buffer.Offset(address),
		                chunk, vk::AccessFlagBits::eHostWrite);
		bytes += chunk;
		address += chunk;
		size -= chunk;
	}
}

struct BufferCache::DownloadCopy {
	Buffer*  buffer        = nullptr;
	uint64_t source_offset = 0;
	uint64_t address       = 0;
	uint64_t size          = 0;
};

struct BufferCache::GuestReadback {
	struct Part { uint64_t address, size, offset; };
	std::vector<Part> parts;
	std::vector<GuestRange> pages;
	uint64_t begin = 0, size = 0, tick = 0, packed_size = 0;
	size_t slot = 0;
	Buffer* download = nullptr;
	std::atomic<bool> copying {false};
	std::atomic<bool> copied {false};
};

// With the frame pipeline, guest reads of GPU-written memory are copied out
// asynchronously; read-only GPU bindings may overlap the copy.
static bool GuestReadbacksEnabled() {
	return kyty_local_frame_pipeline_mode.load(std::memory_order_relaxed) != 0;
}

std::shared_ptr<BufferCache::GuestReadback> BufferCache::BeginGuestReadback(
    uint64_t address, uint64_t size, bool* completed) {
	if (completed) *completed = false;
	constexpr uint64_t WindowSize = 512 * 1024;
	constexpr uint64_t Capacity = 2 * WindowSize;
	if (!m_resources || !GuestRange {address, size}.Valid() || size > WindowSize) return {};
	if (!GuestReadbacksEnabled()) return {};
	for (size_t slot = 0; slot < GuestReadbackSlots; ++slot) {
		const auto& pending = m_guest_readbacks[slot];
		if (!pending) continue;
		if (pending->copied.load(std::memory_order_acquire)) {
			FinishGuestReadback(slot);
			continue;
		}
		if (std::ranges::any_of(pending->pages, [&](const GuestRange& page) {
			return address >= page.address && address < page.End() && size <= page.End() - address;
		})) {
			return pending;
		}
	}
	// A request spanning several pending page spans cannot share just one ticket.
	DrainGuestReadback(address, size);
	auto& buffer = m_slot_buffers[FindBuffer(address, size)];
	const auto begin = std::max(address & ~(WindowSize - 1), buffer.CpuAddress());
	const auto end = std::min(std::max(begin + WindowSize, address + size),
	                          buffer.CpuAddress() + buffer.Size());
	if (!m_resources->IsMapped(begin, end - begin) ||
	    !LibKernel::Memory::IsUniqueGuestBackingRange(begin, end - begin) ||
	    m_texture_cache.HasTrackedDataOverlap(begin, end - begin)) return {};
	RangeSet available_pages;
	m_memory_tracker.ForEachDownloadRange<false>(begin, end - begin,
	    [&](uint64_t a, uint64_t bytes) noexcept {
		    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, a, bytes, "guest readback");
	    },
	    [&](uint64_t a, uint64_t bytes) noexcept {
		    available_pages.Add(a, bytes);
	    });
	// Widening is speculative: pages another ticket already covers must not cause
	// this unrelated request to wait or download the same bytes a second time.
	for (const auto& pending: m_guest_readbacks) {
		if (pending) for (const auto& page: pending->pages)
			available_pages.Subtract(page.address, page.size);
	}
	std::vector<DownloadCopy> copies;
	std::vector<GuestRange> pages;
	available_pages.ForEach([&](uint64_t a, uint64_t end) {
		pages.push_back({a, end - a});
		m_gpu_modified_ranges.ForEachIntersection(a, end - a, [&](RangeSet::Range range) {
			copies.push_back({&buffer, buffer.Offset(range.address), range.address, range.size});
		});
	});
	if (copies.empty()) {
		if (completed) *completed = true;
		return {};
	}
	uint64_t packed_size = 0;
	for (const auto& copy: copies) {
		packed_size += AlignDownload(DownloadEnvelope(copy).second);
		if (packed_size > Capacity) return {};
	}
	size_t slot = 0;
	while (slot < GuestReadbackSlots && m_guest_readbacks[slot]) ++slot;
	if (slot == GuestReadbackSlots) {
		slot = 0;
		for (size_t i = 1; i < GuestReadbackSlots; ++i)
			if (m_guest_readbacks[i]->tick < m_guest_readbacks[slot]->tick) slot = i;
		FinishGuestReadback(slot);
	}
	auto& download = m_guest_downloads[slot];
	if (!download)
		download = std::make_unique<Buffer>(m_graphics, m_scheduler,
		    MemoryUsage::Download, 0, vk::BufferUsageFlagBits::eTransferDst, Capacity);
	auto request = std::make_shared<GuestReadback>();
	request->begin = begin;
	request->size = end - begin;
	request->pages = std::move(pages);
	request->slot = slot;
	request->download = download.get();
	request->packed_size = packed_size;
	uint64_t cursor = 0;
	for (const auto& copy: copies) {
		const auto [source_begin, envelope_size] = DownloadEnvelope(copy);
		download->CopyFrom(m_scheduler.Current(), *copy.buffer, source_begin, cursor,
		    envelope_size, vk::AccessFlagBits::eMemoryWrite, vk::AccessFlagBits::eHostRead,
		    vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
		    vk::AccessFlagBits::eHostRead);
		request->parts.push_back({copy.address, copy.size, cursor + copy.source_offset - source_begin});
		cursor += AlignDownload(envelope_size);
	}
	request->tick = m_scheduler.CurrentTick();
	m_scheduler.Flush();
	// Keep GPU ownership and page protection until the caller has copied every byte.
	// Subsequent accesses to this window must retire this request before proceeding.
	m_guest_readbacks[slot] = request;
	m_active_guest_readbacks |= 1u << slot;
	LiveCounters::Add(LiveCounters::AsyncReadbacks);
	return request;
}

void BufferCache::CopyGuestReadback(const std::shared_ptr<GuestReadback>& request) {
	if (request->copying.exchange(true, std::memory_order_acq_rel)) {
		while (!request->copied.load(std::memory_order_acquire)) request->copied.wait(false);
		return;
	}
	m_scheduler.GetMasterSemaphore().Wait(request->tick);
	m_scheduler.WaitPriorityOperations(request->tick);
	request->download->Invalidate(0, request->packed_size);
	for (const auto& part: request->parts)
		LibKernel::Memory::WriteBacking(part.address,
		    request->download->Mapped().data() + part.offset, part.size);
	request->copied.store(true, std::memory_order_release);
	request->copied.notify_all();
}

void BufferCache::DrainGuestReadback(uint64_t address, uint64_t size, bool gpu_read_only) {
	// Read-only bindings use device bytes. Pending copies retain GPU ownership
	// and CPU-clean pages, so synchronization cannot upload over those bytes.
	if (!m_active_guest_readbacks || gpu_read_only) return;
	for (size_t slot = 0; slot < GuestReadbackSlots; ++slot) {
		const auto& request = m_guest_readbacks[slot];
		if (!request) continue;
		if (address != 0 && (address >= request->begin + request->size ||
		                    (address < request->begin && size <= request->begin - address))) continue;
		if (address != 0 && std::ranges::none_of(request->pages, [&](const GuestRange& page) {
			return address < page.End() && (address >= page.address || size > page.address - address);
		})) continue;
		FinishGuestReadback(slot);
	}
}

void BufferCache::FinishGuestReadback(size_t slot) {
	auto request = m_guest_readbacks[slot];
	if (!request) return;
	if (!request->copied.load(std::memory_order_acquire)) {
		LiveCensus::Scope census(LiveCensus::ReadbackWait, reinterpret_cast<uint64_t>(__builtin_return_address(0)),
		                         reinterpret_cast<uint64_t>(__builtin_return_address(1)));
		while (!request->copied.load(std::memory_order_acquire)) request->copied.wait(false);
	}
	for (const auto& part: request->parts) m_gpu_modified_ranges.Subtract(part.address, part.size);
	// The renderer may have dirtied other pages in the widened window since handoff.
	for (const auto& page: request->pages)
		m_memory_tracker.UnmarkRegionAsGpuModified(page.address, page.size);
	m_guest_readbacks[slot].reset();
	m_active_guest_readbacks &= ~(1u << slot);
}

// All metadata and mapped reads belong to the GPU thread. A slot is reused only
// after its copy tick completes; speculative copies never publish CPU ownership.
struct BufferCache::CopyFeedback {
	static constexpr uint64_t SlotSize = 64 * 1024;
	static constexpr size_t SlotCount = 512;
	struct Slot {
		uint64_t address = 0, size = 0, tick = 0, mapping_epoch = 0;
		vk::Buffer owner = nullptr;
	};
	Buffer download;
	std::array<Slot, SlotCount> slots {};
	std::map<uint64_t, size_t> index;
	size_t cursor = 0;
	CopyFeedback(GraphicContext& graphics, CommandScheduler& scheduler)
	    : download(graphics, scheduler, MemoryUsage::Download, 0,
	               vk::BufferUsageFlagBits::eTransferDst, SlotCount * SlotSize) {
		// Each slot has a separate non-coherent atom, even when adjacent slots are in flight.
		const auto atom = graphics.physical_device_properties.limits.nonCoherentAtomSize;
		EXIT_IF(atom == 0 || SlotSize % atom != 0);
	}
};

void BufferCache::InvalidateCopyFeedback(uint64_t vaddr, uint64_t size) {
	if (!m_copy_feedback || m_copy_feedback->index.empty()) return;
	auto& feedback = *m_copy_feedback;
	auto it = feedback.index.lower_bound(vaddr);
	if (it != feedback.index.begin()) {
		const auto prior = std::prev(it);
		const auto& slot = feedback.slots[prior->second];
		if (slot.address + slot.size > vaddr) it = prior;
	}
	while (it != feedback.index.end() && it->first < vaddr + size) {
		feedback.slots[it->second].address = 0;
		it = feedback.index.erase(it);
	}
}

void BufferCache::ScheduleCopyFeedback(uint64_t vaddr, uint64_t size) {
	if (kyty_local_copy_feedback_mode.load(std::memory_order_relaxed) == 0) return;
	const auto mapping_epoch = m_resources ? m_resources->MappingEpoch() : 0;
	if (!m_resources || !GuestRange {vaddr, size}.Valid() ||
	    ((vaddr | size) & 3u) != 0 || size > CopyFeedback::SlotSize ||
	    !m_gpu_modified_ranges.Contains(vaddr, size) ||
	    m_texture_cache.HasTrackedDataOverlap(vaddr, size) ||
	    !m_resources->IsMapped(vaddr, size) ||
	    !LibKernel::Memory::IsUniqueGuestBackingRange(vaddr, size)) {
		return;
	}
	const auto* owner_id = m_page_table.Find(vaddr >> PageTable::kPageBits);
	auto* owner = owner_id && *owner_id ? m_slot_buffers.try_get(*owner_id) : nullptr;
	if (!owner || owner->is_deleted || !owner->IsInBounds(vaddr, size)) {
		return;
	}
	if (!m_copy_feedback) m_copy_feedback = std::make_unique<CopyFeedback>(m_graphics, m_scheduler);
	auto& feedback = *m_copy_feedback;
	auto& master = m_scheduler.GetMasterSemaphore();
	size_t selected = CopyFeedback::SlotCount;
	for (unsigned refresh = 0; refresh < 2 && selected == CopyFeedback::SlotCount; ++refresh) {
		if (refresh) master.Refresh();
		const auto completed = master.KnownGpuTick();
		// Prefer an invalidated slot, then evict an old completed snapshot if necessary.
		for (unsigned evict = 0; evict < 2 && selected == CopyFeedback::SlotCount; ++evict) {
			for (size_t n = 0; n < CopyFeedback::SlotCount; ++n) {
				const auto i = (feedback.cursor + n) % CopyFeedback::SlotCount;
				const auto& slot = feedback.slots[i];
				if (slot.tick <= completed && (evict || slot.address == 0)) {
					selected = i;
					break;
				}
			}
		}
	}
	if (selected == CopyFeedback::SlotCount) {
		return; // Never submit or wait merely to obtain speculative storage.
	}
	if (m_resources->MappingEpoch() != mapping_epoch) return;
	auto& slot = feedback.slots[selected];
	if (slot.address) {
		feedback.index.erase(slot.address);
	}
	InvalidateCopyFeedback(vaddr, size);
	feedback.download.CopyFrom(m_scheduler.Current(), *owner, owner->Offset(vaddr),
	    selected * CopyFeedback::SlotSize, size, vk::AccessFlagBits::eMemoryWrite,
	    vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eHostRead,
	    vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
	    vk::AccessFlagBits::eHostRead);
	slot = {vaddr, size, m_scheduler.CurrentTick(), mapping_epoch, owner->Handle()};
	feedback.index.emplace(vaddr, selected);
	feedback.cursor = (selected + 1) % CopyFeedback::SlotCount;
}

bool BufferCache::TryReadCopyFeedback(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	if (kyty_local_copy_feedback_mode.load(std::memory_order_relaxed) == 0 || !m_copy_feedback || !m_resources)
		return false;
	const auto begin = vaddr & ~(TRACKER_PAGE_SIZE - 1);
	const auto end = (vaddr + size + TRACKER_PAGE_SIZE - 1) & ~(TRACKER_PAGE_SIZE - 1);
	if (!buffer.IsInBounds(begin, end - begin) || end - begin > CopyFeedback::SlotSize ||
	    m_texture_cache.HasTrackedDataOverlap(begin, end - begin)) {
		return false;
	}
	std::vector<DownloadCopy> copies;
	m_memory_tracker.ForEachDownloadRange<false>(begin, end - begin,
	    [&](uint64_t address, uint64_t bytes) noexcept {
		    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, address, bytes,
		                                           "copy feedback");
	    },
	    [&](uint64_t address, uint64_t bytes) noexcept {
		    m_gpu_modified_ranges.ForEachIntersection(address, bytes, [&](RangeSet::Range range) {
			    copies.push_back({&buffer, buffer.Offset(range.address), range.address, range.size});
		    });
	    });
	if (copies.empty()) return false;
	struct Part { uint64_t address, size, offset; };
	std::vector<Part> parts;
	auto& feedback = *m_copy_feedback;
	const auto mapping_epoch = m_resources->MappingEpoch();
	uint64_t latest_tick = 0;
	for (const auto& copy: copies) {
		auto cursor = copy.address;
		while (cursor < copy.address + copy.size) {
			auto it = feedback.index.upper_bound(cursor);
			if (it == feedback.index.begin()) return false;
			--it;
			const auto& slot = feedback.slots[it->second];
			if (cursor >= slot.address + slot.size) return false;
			if (slot.owner != buffer.Handle() || slot.mapping_epoch != mapping_epoch ||
			    !m_resources->IsMapped(slot.address, slot.size) ||
			    !LibKernel::Memory::IsUniqueGuestBackingRange(slot.address, slot.size)) {
				return false;
			}
			const auto bytes = std::min(copy.address + copy.size, slot.address + slot.size) - cursor;
			parts.push_back({cursor, bytes, it->second * CopyFeedback::SlotSize + cursor - slot.address});
			latest_tick = std::max(latest_tick, slot.tick);
			cursor += bytes;
		}
	}
	if (!m_scheduler.IsFree(latest_tick)) return false;
	m_scheduler.WaitPriorityOperations(latest_tick);
	if (m_resources->MappingEpoch() != mapping_epoch) return false;
	for (const auto& part: parts) feedback.download.Invalidate(part.offset, part.size);
	for (const auto& part: parts) {
		LibKernel::Memory::WriteBacking(part.address, feedback.download.Mapped().data() + part.offset, part.size);
	}
	for (const auto& copy: copies) m_gpu_modified_ranges.Subtract(copy.address, copy.size);
	// Only complete dirty-page coverage allows dropping protection. CPU-clean bytes
	// are never published from the snapshot. Any later upload/GPU write invalidates it.
	m_memory_tracker.UnmarkRegionAsGpuModified(begin, end - begin);
	return true;
}

void BufferCache::Register(BufferId id) {
	ChangeRegister<true>(id);
}

void BufferCache::Unregister(BufferId id) {
	ChangeRegister<false>(id);
}

template <bool insert>
void BufferCache::ChangeRegister(BufferId id) {
	DrainGuestReadback(m_slot_buffers[id].CpuAddress(), m_slot_buffers[id].Size());
	m_sync_buffers_valid = false;
	++m_registration_epoch;
	auto& buffer = m_slot_buffers[id];
	if constexpr (!insert) InvalidateCopyFeedback(buffer.CpuAddress(), buffer.Size());
	PageTable::PageRange pages {};
	EXIT_IF(!PageTable::TryGetPageRange(buffer.CpuAddress(), buffer.Size(), pages));
	for (size_t page = pages.first; page < pages.last_exclusive; ++page) {
		if constexpr (insert) {
			m_page_table[page] = id;
		} else {
			m_page_table[page] = {};
		}
	}
	const auto size_pages = pages.last_exclusive - pages.first;
	if constexpr (insert) {
		const auto [it, inserted] = m_buffers.emplace(buffer.CpuAddress(), id);
		(void)it;
		EXIT_IF(!inserted);
		m_total_used_memory += buffer.Size();
		buffer.lru_id = m_lru_cache.Insert(id, m_gc_tick);
		std::vector<vk::DeviceAddress> addresses;
		addresses.reserve(size_pages);
		for (uint64_t i = 0; i < size_pages; ++i) {
			addresses.push_back(buffer.BufferDeviceAddress() + (i << CACHING_PAGEBITS));
		}
		WriteDataBuffer(m_bda_pagetable_buffer, pages.first * sizeof(vk::DeviceAddress),
		                addresses.data(), addresses.size() * sizeof(vk::DeviceAddress));
	} else {
		const auto found = m_buffers.find(buffer.CpuAddress());
		EXIT_IF(found == m_buffers.end() || found->second != id);
		m_buffers.erase(found);
		EXIT_IF(buffer.Size() > m_total_used_memory);
		m_total_used_memory -= buffer.Size();
		m_lru_cache.Free(buffer.lru_id);
		m_bda_pagetable_buffer.Fill(pages.first * sizeof(vk::DeviceAddress),
		                            size_pages * sizeof(vk::DeviceAddress), 0);
		buffer.is_deleted = true;
	}
}

void BufferCache::TouchBuffer(const Buffer& buffer) {
	if (!buffer.is_deleted) {
		m_lru_cache.Touch(buffer.lru_id, m_gc_tick);
	}
}

void BufferCache::DeleteBuffer(BufferId id) {
	auto* buffer = m_slot_buffers.try_get(id);
	if (buffer == nullptr || buffer->is_deleted) {
		return;
	}
	Unregister(id);
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation([this, id] { m_slot_buffers.erase(id); });
	} else {
		m_slot_buffers.erase(id);
	}
}

std::pair<uint64_t, uint64_t> BufferCache::DownloadEnvelope(const DownloadCopy& copy) {
	if (copy.buffer == nullptr || copy.size == 0 || copy.source_offset > copy.buffer->Size() ||
	    copy.size > copy.buffer->Size() - copy.source_offset) {
		EXIT("BufferCache: invalid download copy\n");
	}
	const auto begin = copy.source_offset & ~uint64_t {3};
	if (copy.source_offset > UINT64_MAX - copy.size ||
	    copy.source_offset + copy.size > UINT64_MAX - 3) {
		EXIT("BufferCache: download copy alignment overflow\n");
	}
	const auto end = (copy.source_offset + copy.size + 3) & ~uint64_t {3};
	if (end > copy.buffer->Size()) {
		EXIT("BufferCache: aligned download copy exceeds its owner\n");
	}
	return {begin, end - begin};
}

void BufferCache::DownloadBufferMemory(std::span<const DownloadCopy> copies) {
	NativeBufferDownloadScope native_download_scope;
	std::vector<DownloadCopy> batch;
	batch.reserve(copies.size());
	uint64_t                  packed_size = 0;
	auto&                     download    = m_download_buffer;
	const auto flush = [&] {
		const auto [mapped, base_offset] = download.Map(packed_size, DOWNLOAD_ALIGNMENT);
		EXIT_IF(mapped == nullptr);
		uint64_t cursor = 0;
		for (const auto& copy: batch) {
			const auto [source_begin, envelope_size] = DownloadEnvelope(copy);
			download.CopyFrom(m_scheduler.Current(), *copy.buffer, source_begin, base_offset + cursor,
			                  envelope_size, vk::AccessFlagBits::eMemoryWrite, vk::AccessFlags {},
			                  vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
			                  vk::AccessFlagBits::eHostRead);
			cursor += AlignDownload(envelope_size);
		}
		download.Commit();
		const auto completion_tick = m_scheduler.CurrentTick();
		m_scheduler.Finish();
		m_scheduler.WaitPriorityOperations(completion_tick);
		cursor = 0;
		for (const auto& copy: batch) {
			const auto [source_begin, envelope_size] = DownloadEnvelope(copy);
			const auto offset = cursor + copy.source_offset - source_begin;
			download.Invalidate(base_offset + offset, copy.size);
			Libs::LibKernel::Memory::WriteBacking(copy.address, mapped + offset, copy.size);
			cursor += AlignDownload(envelope_size);
		}
		batch.clear();
		packed_size = 0;
	};
	for (auto copy: copies) {
		while (copy.size != 0) {
			const auto available = download.Size() - packed_size;
			const auto prefix    = copy.source_offset & 3u;
			const auto bytes     = std::min(copy.size, available - prefix);
			DownloadCopy part {copy.buffer, copy.source_offset, copy.address, bytes};
			const auto [source_begin, envelope_size] = DownloadEnvelope(part);
			(void)source_begin;
			packed_size += AlignDownload(envelope_size);
			batch.push_back(part);
			copy.source_offset += bytes;
			copy.address += bytes;
			copy.size -= bytes;
			if (packed_size == download.Size()) {
				flush();
			}
		}
	}
	if (!batch.empty()) {
		flush();
	}
	for (const auto& copy: copies) {
		m_gpu_modified_ranges.Subtract(copy.address, copy.size);
	}
}

bool BufferCache::TryReportLodStatsOnGpu(uint64_t address, bool reset) {
    constexpr uint64_t ReportSize = 0x840;
    if (address % 4 || !m_resources || !m_resources->IsMapped(address, ReportSize) ||
        !LibKernel::Memory::IsUniqueGuestBackingRange(address, ReportSize) ||
        m_texture_cache.HasTrackedDataOverlap(address, ReportSize)) return false;
    if (!m_lod_pack_pipeline) {
        std::array<vk::DescriptorSetLayoutBinding, 2> bindings {};
        for (uint32_t i = 0; i < bindings.size(); ++i)
            bindings[i] = {i, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr};
        vk::DescriptorSetLayoutCreateInfo descriptors {};
        descriptors.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
        descriptors.bindingCount = bindings.size(); descriptors.pBindings = bindings.data();
        RequireVulkanSuccess(m_graphics.device.createDescriptorSetLayout(&descriptors, nullptr, &m_lod_pack_descriptors),
                             "create LOD report descriptors");
        vk::PushConstantRange push {vk::ShaderStageFlagBits::eCompute, 0, 8};
        vk::PipelineLayoutCreateInfo layout {};
        layout.setLayoutCount = 1; layout.pSetLayouts = &m_lod_pack_descriptors;
        layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
        RequireVulkanSuccess(m_graphics.device.createPipelineLayout(&layout, nullptr, &m_lod_pack_layout),
                             "create LOD report pipeline layout");
        vk::ShaderModuleCreateInfo shader {};
        shader.codeSize = sizeof(LOD_STATS_PACK_SPV); shader.pCode = LOD_STATS_PACK_SPV;
        vk::ShaderModule module;
        RequireVulkanSuccess(m_graphics.device.createShaderModule(&shader, nullptr, &module), "create LOD report shader");
        vk::ComputePipelineCreateInfo pipeline {};
        pipeline.stage.stage = vk::ShaderStageFlagBits::eCompute;
        pipeline.stage.module = module; pipeline.stage.pName = "main";
        pipeline.layout = m_lod_pack_layout;
        RequireVulkanSuccess(m_graphics.device.createComputePipelines(nullptr, 1, &pipeline, nullptr, &m_lod_pack_pipeline),
                             "create LOD report pipeline");
        m_graphics.device.destroyShaderModule(module, nullptr);
    }
    // The output remains GPU-owned until the normal checked readback path
    // publishes all bytes. A CPU polling the ready word faults and waits for
    // this dispatch; never publish readiness on the CPU before GPU completion.
    const auto [output, offset] = ObtainBuffer(address, ReportSize, true);
    const auto alignment = m_graphics.GetPhysicalDeviceProperties().limits.minStorageBufferOffsetAlignment;
    const auto base = offset & ~(alignment - 1);
    const uint32_t constants[] {uint32_t((offset - base) / 4), reset ? 1u : 0u};
    const vk::DescriptorBufferInfo buffers[] {{m_lod_stats_buffer.Handle(), 0, 256 * 16},
                                             {output->Handle(), base, offset - base + ReportSize}};
    std::array<vk::WriteDescriptorSet, 2> writes {};
    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].dstBinding = i; writes[i].descriptorCount = 1;
        writes[i].descriptorType = vk::DescriptorType::eStorageBuffer; writes[i].pBufferInfo = &buffers[i];
    }
    m_scheduler.EndRendering();
    const auto command = m_scheduler.Current().Handle();
    vk::MemoryBarrier barrier {};
    barrier.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eComputeShader,
                            {}, 1, &barrier, 0, nullptr, 0, nullptr);
    command.bindPipeline(vk::PipelineBindPoint::eCompute, m_lod_pack_pipeline);
    command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_lod_pack_layout, 0, writes);
    command.pushConstants(m_lod_pack_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(constants), constants);
    command.dispatch(4, 1, 1);
    barrier.srcAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
    command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eAllCommands,
                            {}, 1, &barrier, 0, nullptr, 0, nullptr);
    return true;
}

void BufferCache::ReportLodStats(void* dst, uint32_t size, bool reset) {
	// Pack the 64-byte completion header and 256 eight-byte LOD counters.
	// The command processor keeps other packet layouts on the existing path.
	EXIT_IF(dst == nullptr || size != 0x840);
	if (kyty_local_async_lod_stats_mode.load(std::memory_order_relaxed) &&
	    TryReportLodStatsOnGpu(reinterpret_cast<uint64_t>(dst), reset))
		return;
	auto& command = m_scheduler.Current();
	command.EndRendering();
	vk::BufferMemoryBarrier barrier {};
	barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer = m_lod_stats_buffer.Handle();
	barrier.size = 256 * 16;
	command.Handle().pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	    vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1, &barrier, 0, nullptr);
	m_scheduler.Finish();
	m_lod_stats_buffer.Invalidate(0, 256 * 16);
	auto* words = reinterpret_cast<uint32_t*>(m_lod_stats_buffer.Mapped().data());
	std::memset(dst, 0, size);
	const uint32_t ready = 1;
	std::memcpy(dst, &ready, sizeof(ready));
	for (uint32_t i = 0; i < 256; ++i) {
		const uint64_t entry = (uint64_t(words[i * 4] & 15u) << 56u) |
		    (uint64_t(std::min(words[i * 4 + 1], 0xffffffu)) << 32u) | words[i * 4 + 1];
		std::memcpy(static_cast<uint8_t*>(dst) + 64 + i * 8, &entry, sizeof(entry));
		if (reset) {
			words[i * 4] = 15;
			words[i * 4 + 1] = words[i * 4 + 2] = words[i * 4 + 3] = 0;
		}
	}
	if (reset) m_lod_stats_buffer.Flush(0, 256 * 16);
}

BufferCache::BufferCache(GraphicContext& graphics, CommandScheduler& scheduler,
                         PageManager& page_manager, TextureCache& texture_cache,
                         GpuResourceManager* resources)
    : m_graphics(graphics), m_scheduler(scheduler), m_fault_manager(graphics, scheduler, *this),
      m_gds_buffer(graphics, scheduler, MemoryUsage::Stream, 0, AllFlags, GdsBufferSize),
      m_lod_stats_buffer(graphics, scheduler, MemoryUsage::Stream, 0, AllFlags, 256 * 16),
      m_bda_pagetable_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                             BDA_PAGETABLE_SIZE),
      m_memory_tracker(page_manager),
      m_staging_buffer(graphics, scheduler, MemoryUsage::Upload, 512 * MiB),
      m_stream_buffer(graphics, scheduler, MemoryUsage::Stream, 64 * MiB),
      m_host_shader_upload(graphics, scheduler, MemoryUsage::Upload, 64 * MiB),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 32 * MiB),
      m_device_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 128 * MiB),
      m_texture_cache(texture_cache), m_resources(resources) {
	std::memset(m_gds_buffer.Mapped().data(), 0, static_cast<size_t>(m_gds_buffer.Size()));
	m_gds_buffer.Flush(0, m_gds_buffer.Size());
	std::memset(m_lod_stats_buffer.Mapped().data(), 0, 256 * 16);
	for (uint32_t i = 0; i < 256; ++i) {
		reinterpret_cast<uint32_t*>(m_lod_stats_buffer.Mapped().data())[i * 4] = 15;
	}
	m_lod_stats_buffer.Flush(0, 256 * 16);
	SetVulkanObjectNameF(m_graphics.device, m_bda_pagetable_buffer.Handle(),
	                     "BDA Page Table Buffer");
	const auto null_id =
	    m_slot_buffers.insert(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, 16);
	EXIT_IF(null_id != NULL_BUFFER_ID);
	SetVulkanObjectNameF(m_graphics.device, GetBuffer(null_id).Handle(), "Kyty.NullBuffer");
	if (!m_graphics.CanReportMemoryUsage()) {
		return;
	}
	constexpr int64_t GiB              = 1024ll * 1024 * 1024;
	constexpr int64_t target_threshold = 8 * GiB;
	const auto        budget =
	    static_cast<int64_t>(std::min<uint64_t>(m_graphics.GetTotalMemoryBudget(), INT64_MAX));
	const auto threshold = std::min(budget, target_threshold);
	const auto expected  = std::min(budget - 6 * threshold / 10, budget - GiB);
	const auto critical  = std::min(budget - 2 * threshold / 10, budget - GiB / 2);
	m_trigger_gc_memory  = static_cast<uint64_t>(std::max<int64_t>(expected, GiB));
	m_critical_gc_memory = static_cast<uint64_t>(std::max<int64_t>(critical, 2 * GiB));
}

BufferCache::~BufferCache() {
	DrainGuestReadback();
    m_graphics.device.destroyPipeline(m_lod_pack_pipeline, nullptr);
    m_graphics.device.destroyPipelineLayout(m_lod_pack_layout, nullptr);
    m_graphics.device.destroyDescriptorSetLayout(m_lod_pack_descriptors, nullptr);
	if (!m_gpu_modified_ranges.Empty()) {
		EXIT("BufferCache: destroyed with pending GPU-modified ranges\n");
	}
	for (const auto& [vaddr, id]: m_buffers) {
		(void)vaddr;
		const auto& buffer = m_slot_buffers[id];
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: destroyed with GPU-modified buffer\n");
		}
	}
	m_buffers.clear();
}

void BufferCache::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid memory-invalidation range\n");
	}
	m_memory_tracker.InvalidateRegion(vaddr, size,
	                                  [this, vaddr, size] { ReadMemory(vaddr, size, true); });
}

void BufferCache::ReadMemory(uint64_t vaddr, uint64_t size, bool is_write) {
	if (!GuestGpu::IsGpuThread() && CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported buffer readback from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	if (!is_write && !GuestGpu::IsGpuThread() && GuestReadbacksEnabled()) {
		std::shared_ptr<GuestReadback> request;
		auto& gpu = m_scheduler.Context().GetGpu();
		gpu.SendCommandSync([&] {
			bool completed = false;
			request = BeginGuestReadback(vaddr, size, &completed);
			if (!request && !completed) ReadMemoryOnGpu(vaddr, size, false);
		});
		if (request) {
			CopyGuestReadback(request);
			gpu.SendCommandSync([this, request] {
				// An overlapping access may already have retired this request.
				if (m_guest_readbacks[request->slot] == request) FinishGuestReadback(request->slot);
			});
		}
		return;
	}
	m_scheduler.Context().GetGpu().SendCommandSync(
	    [this, vaddr, size, is_write] { ReadMemoryOnGpu(vaddr, size, is_write); });
}

void BufferCache::ReadMemoryOnGpu(uint64_t vaddr, uint64_t size, bool is_write) {
	DrainGuestReadback();
	if (is_write && !IsRegionRegistered(vaddr, size)) {
		return;
	}
	auto& buffer = m_slot_buffers[FindBuffer(vaddr, size)];
	if (TryReadCopyFeedback(buffer, vaddr, size)) {
		if (is_write) m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
		return;
	}

	// Widen nearby CPU reads so they share one GPU drain.
	constexpr uint64_t WindowSize   = 512 * 1024;
	const auto         buffer_begin = buffer.CpuAddress();
	const auto         buffer_end   = buffer_begin + buffer.Size();
	const auto         window_begin = std::max(vaddr & ~(WindowSize - 1), buffer_begin);
	const auto window_end = std::min(std::max(window_begin + WindowSize, vaddr + size), buffer_end);

	std::vector<DownloadCopy> copies;
	m_memory_tracker.ForEachDownloadRange<false>(
	    window_begin, window_end - window_begin,
	    [&](uint64_t address, uint64_t bytes) noexcept {
		    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, address, bytes,
		                                           "memory invalidation");
	    },
	    [&](uint64_t address, uint64_t bytes) noexcept {
		    m_gpu_modified_ranges.ForEachIntersection(address, bytes, [&](RangeSet::Range range) {
			    copies.push_back(
			        {&buffer, buffer.Offset(range.address), range.address, range.size});
		    });
	    });
	if (!copies.empty()) {
		LiveCounters::Add(LiveCounters::SyncDownloads);
		DownloadBufferMemory(copies);
		// The enumeration covered whole dirty pages and every exact interval on them.
		m_memory_tracker.UnmarkRegionAsGpuModified(window_begin, window_end - window_begin);
	}
	if (is_write) {
		m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
	}
}

BufferId BufferCache::FindBuffer(uint64_t vaddr, uint64_t size) {
	// Registration changes still drain separately before retiring an owner.
	DrainGuestReadback(vaddr, size, true);
	if (vaddr == 0) {
		return NULL_BUFFER_ID;
	}
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid buffer discovery request\n");
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			return *owner;
		}
	}
	return CreateBuffer(vaddr, size);
}

BufferCache::OverlapResult BufferCache::ResolveOverlaps(uint64_t vaddr, uint64_t size) {
	static constexpr int      StreamLeapThreshold = 16;
	static constexpr uint64_t StreamLeapSize      = CACHING_PAGESIZE * 128;

	auto       begin      = vaddr;
	auto       end        = vaddr + size;
	const auto find_first = [&](uint64_t address) {
		auto first = m_buffers.lower_bound(address);
		if (first != m_buffers.begin()) {
			const auto  previous = std::prev(first);
			const auto& buffer   = m_slot_buffers[previous->second];
			if (buffer.CpuAddress() + buffer.Size() > address) {
				first = previous;
			}
		}
		return first;
	};
	auto first           = find_first(begin);
	auto last            = first;
	int  stream_score    = 0;
	bool has_stream_leap = false;
	for (; last != m_buffers.end() && last->first < end; ++last) {
		const auto& buffer        = m_slot_buffers[last->second];
		const auto  buffer_begin  = buffer.CpuAddress();
		const auto  buffer_end    = buffer_begin + buffer.Size();
		const bool  expands_left  = buffer_begin < begin;
		const bool  expands_right = buffer_end > end;
		begin                     = std::min(begin, buffer_begin);
		end                       = std::max(end, buffer_end);
		if (!has_stream_leap && (stream_score += buffer.StreamScore()) > StreamLeapThreshold) {
			has_stream_leap = true;
			// Fix the shadPS4 bug that reserves space opposite to the incoming stream's growth.
			// The old buffer extending left of the request predicts growth to the right, and vice versa.
			if (expands_left) {
				end += std::min(StreamLeapSize, PageTable::kAddressSpaceSize - end);
			}
			if (expands_right) {
				const auto minimum = CACHING_PAGESIZE * 2;
				if (begin > minimum) {
					begin -= std::min(StreamLeapSize, begin - minimum);
				}
				first = find_first(begin);
				begin = std::min(begin, first->first);
			}
		}
	}
	return {first, last, begin, end, has_stream_leap};
}

void BufferCache::JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score) {
	auto& new_buffer = m_slot_buffers[new_id];
	auto& overlap    = m_slot_buffers[overlap_id];
	if (accumulate_stream_score) {
		new_buffer.IncreaseStreamScore(overlap.StreamScore() + 1);
	}
	new_buffer.CopyFrom(m_scheduler.Current(), overlap, 0,
	                    overlap.CpuAddress() - new_buffer.CpuAddress(), overlap.Size());
	DeleteBuffer(overlap_id);
}

BufferId BufferCache::CreateBuffer(uint64_t vaddr, uint64_t size) {
	EXIT_IF(m_scheduler.Current().IsInvalid());
	const auto end = (vaddr + size + CACHING_PAGESIZE - 1) & ~(CACHING_PAGESIZE - 1);
	vaddr &= ~(CACHING_PAGESIZE - 1);
	size               = end - vaddr;
	const auto overlap = ResolveOverlaps(vaddr, size);

	const auto id = m_slot_buffers.insert(
	    m_graphics, m_scheduler, MemoryUsage::DeviceLocal, overlap.begin,
	    AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress, overlap.end - overlap.begin);
	const auto& buffer = m_slot_buffers[id];
	SetVulkanObjectNameF(m_graphics.device, buffer.Handle(),
	                     "Kyty.GameBuffer[guest=0x{:016x} size=0x{:x}]", overlap.begin,
	                     overlap.end - overlap.begin);
	for (auto it = overlap.first; it != overlap.last;) {
		const auto old_id = (it++)->second;
		JoinOverlap(id, old_id, !overlap.has_stream_leap);
	}
	Register(id);
	return id;
}

bool BufferCache::SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size, bool is_written,
                                    bool is_texel_buffer) {
	if (!is_written && !m_memory_tracker.IsRegionCpuModified(vaddr, size)) {
		// CPU cleanliness does not prove that an aliased image is current.
		return is_texel_buffer && SynchronizeBufferFromImage(buffer, vaddr, size);
	}
	bool fully_gpu_modified = false;
	if (is_written && kyty_local_buffer_residency_mode.load(std::memory_order_relaxed) != 0 &&
	    native_buffer_download_depth == 0 && m_gpu_modified_ranges.Contains(vaddr, size)) {
		// Exact ranges are added only after SynchronizeBuffer marks their
		// pages GPU-owned. Readback subtracts them before clearing page
		// ownership; CPU writes and unmapping take that readback path.
		// A complete byte cover is sufficient, while a hole or partial
		// cover still takes the original page query.
		fully_gpu_modified = true;
	}
	if (is_written && (fully_gpu_modified || m_memory_tracker.IsRegionFullyGpuModified(vaddr, size))) {
		// ObtainBuffer still records the exact write range and invalidates its epoch.
		return false;
	}
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size = 0;
	vk::Buffer                  source;
	m_memory_tracker.ForEachUploadRange(
	    vaddr, size, is_written,
	    [&](uint64_t address, uint64_t bytes) noexcept {
		    copies.emplace_back(total_size, buffer.Offset(address), bytes);
		    total_size += bytes;
	    },
	    [&]() noexcept { source = UploadCopies(buffer, copies, total_size); });
	if (source) {
		for (const auto& copy: copies)
			InvalidateCopyFeedback(buffer.CpuAddress() + copy.dstOffset, copy.size);
		auto& command = m_scheduler.Current();
		command.EndRendering();
		const auto native = command.Handle();
		vk::BufferMemoryBarrier before {};
		before.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
		                       vk::AccessFlagBits::eTransferRead |
		                       vk::AccessFlagBits::eTransferWrite;
		before.dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer              = buffer.Handle();
		before.offset              = 0;
		before.size                = buffer.Size();
		native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
		                       vk::PipelineStageFlagBits::eTransfer,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &before, 0, nullptr);
		native.copyBuffer(source, buffer.Handle(), static_cast<uint32_t>(copies.size()),
		                  copies.data());
		auto after          = before;
		after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		after.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
		native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                       vk::PipelineStageFlagBits::eAllCommands,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &after, 0, nullptr);
	}
	if (is_texel_buffer && !is_written) {
		return SynchronizeBufferFromImage(buffer, vaddr, size);
	}
	return false;
}

vk::Buffer BufferCache::UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
                                     uint64_t total_size) {
	if (copies.empty()) {
		return nullptr;
	}
	LiveCounters::Add(LiveCounters::UploadCopies, copies.size());
	LiveCounters::Add(LiveCounters::UploadBytes, total_size);
	if (LiveCounters::g_granules_on.load(std::memory_order_relaxed)) {
		for (const auto& copy: copies) {
			LiveCounters::AddGranule(buffer.CpuAddress() + copy.dstOffset, LiveCounters::GUploadBytes, copy.size);
			LiveCounters::AddGranule(buffer.CpuAddress() + copy.dstOffset, LiveCounters::GUploadCalls, 1);
		}
	}

	auto [mapped, base_offset] = m_staging_buffer.Map(total_size, 4);
	if (mapped != nullptr) {
		for (auto& copy: copies) {
			const auto address = buffer.CpuAddress() + copy.dstOffset;
			std::memcpy(mapped + copy.srcOffset, reinterpret_cast<const void*>(address), copy.size);
			copy.srcOffset += base_offset;
		}
		m_staging_buffer.Commit();
		return m_staging_buffer.Handle();
	}

	auto temporary = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Upload, 0,
	                                         vk::BufferUsageFlagBits::eTransferSrc, total_size);
	for (const auto& copy: copies) {
		const auto address = buffer.CpuAddress() + copy.dstOffset;
		std::memcpy(temporary->Mapped().data() + copy.srcOffset,
		            reinterpret_cast<const void*>(address), copy.size);
	}
	temporary->Flush(0, total_size);
	const auto handle = temporary->Handle();
	m_scheduler.DeferOperation([owner = std::move(temporary)]() mutable { owner.reset(); });
	return handle;
}

void BufferCache::EnsureBufferContents(uint64_t vaddr, uint64_t size) {
	const auto id     = FindBuffer(vaddr, size);
	auto&      buffer = m_slot_buffers[id];
	TouchBuffer(buffer);
	(void)SynchronizeBuffer(buffer, vaddr, size, false, false);
}

StreamBuffer& BufferCache::GetShaderUploadBuffer() noexcept {
	return kyty_local_stream_upload_mode.load(std::memory_order_relaxed) != 0 ? m_host_shader_upload
	                                                                        : m_stream_buffer;
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBuffer(uint64_t vaddr, uint64_t size,
                                                       bool is_written, bool is_texel_buffer,
                                                       BufferId id) {
	DrainGuestReadback(vaddr, size, !is_written && !is_texel_buffer);
	auto& command = m_scheduler.Current();
	if (command.IsInvalid() || !GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: buffer request requires a recording command buffer\n");
	}

	if (!is_written && size <= CACHING_PAGESIZE &&
	    !m_memory_tracker.IsRegionGpuModified(vaddr, size) &&
	    m_memory_tracker.IsRegionCpuModified(vaddr, size)) {
		auto& upload = GetShaderUploadBuffer();
		const auto alignment = std::max<uint64_t>(
		    m_graphics.physical_device_properties.limits.minUniformBufferOffsetAlignment, 4);
		auto [mapped, offset] = upload.Map(size, alignment, false);
		if (mapped != nullptr && Libs::LibKernel::Memory::TryReadBackingToHost(vaddr, mapped, size)) {
			upload.Commit();
			return {&upload, offset};
		}
	}

	auto* buffer = m_slot_buffers.try_get(id);
	if (buffer == nullptr || buffer->is_deleted || !buffer->IsInBounds(vaddr, size)) {
		id     = FindBuffer(vaddr, size);
		buffer = &m_slot_buffers[id];
	}
	TouchBuffer(*buffer);
	(void)SynchronizeBuffer(*buffer, vaddr, size, is_written, is_texel_buffer);
	if (is_written) {
		InvalidateCopyFeedback(vaddr, size);
		m_gpu_modified_ranges.Add(vaddr, size);
	}
	return {buffer, buffer->Offset(vaddr)};
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBufferForImage(uint64_t vaddr, uint64_t size) {
	DrainGuestReadback(vaddr, size);
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid image source\n");
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			TouchBuffer(buffer);
			(void)SynchronizeBuffer(buffer, vaddr, size, false, false);
			return {&buffer, buffer.Offset(vaddr)};
		}
	}
	if (IsRegionGpuModified(vaddr, size)) {
		return ObtainBuffer(vaddr, size, false, false);
	}

	auto [staging, stage_offset] = m_staging_buffer.Map(size, 16);
	const char* prt_failure = "not-attempted";
	if (staging == nullptr || (!Libs::LibKernel::Memory::TryReadBacking(vaddr, staging, size) &&
	                           !Libs::LibKernel::Memory::TryReadPrtBacking(vaddr, staging, size, &prt_failure))) {
		EXIT("BufferCache: failed to read mapped guest image backing: "
		     "address=0x%016" PRIx64 " size=0x%016" PRIx64
		     " staging=%p staging_capacity=0x%016" PRIx64 " mapped=%u prt_failure=%s\n",
		     vaddr, size, static_cast<void*>(staging), m_staging_buffer.Size(),
		     m_resources != nullptr && m_resources->IsMapped(vaddr, size), prt_failure);
	}
	m_staging_buffer.Commit();
	return {&m_staging_buffer, stage_offset};
}

void BufferCache::FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds) {
	if ((vaddr & 3u) != 0 || size == 0 || (size & 3u) != 0 || size > UINT64_MAX - vaddr) {
		EXIT("BufferCache: fill range must be dword aligned\n");
	}
	if (is_gds) {
		if (vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - vaddr) {
			EXIT("BufferCache: GDS fill range is out of bounds\n");
		}
		m_gds_buffer.Fill(vaddr, size, value);
		return;
	}
	if (vaddr == 0) {
		EXIT("BufferCache: invalid fill memory address\n");
	}
	(void)m_texture_cache.ClearMeta(vaddr, value);
	if (!IsRegionGpuModified(vaddr, size)) {
		// Access the guest mapping so write faults invalidate cached buffers and images.
		auto* destination = reinterpret_cast<uint32_t*>(vaddr);
		std::fill(destination, destination + size / sizeof(uint32_t), value);
		return;
	}

	m_texture_cache.InvalidateMemoryFromGPU(vaddr, size);
	const auto id          = FindBuffer(vaddr, size);
	auto [dst, dst_offset] = ObtainBuffer(vaddr, size, true, true, id);
	EXIT_IF(dst == nullptr);
	dst->Fill(dst_offset, size, value);
}

void BufferCache::CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
                             bool src_gds) {
	const bool dst_memory = !dst_gds;
	const bool src_memory = !src_gds;
	if ((dst_memory && dst_vaddr == 0) || (src_memory && src_vaddr == 0) || size == 0 ||
	    ((dst_gds || src_gds) && ((dst_vaddr | src_vaddr | size) & 3u) != 0) ||
	    size > UINT64_MAX - dst_vaddr || size > UINT64_MAX - src_vaddr || (dst_gds && src_gds) ||
	    (dst_gds && (dst_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - dst_vaddr)) ||
	    (src_gds && (src_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - src_vaddr))) {
		EXIT("BufferCache: invalid copy range, src=0x%016" PRIx64 " dst=0x%016" PRIx64
		     " size=0x%016" PRIx64 " src_gds=%d dst_gds=%d\n",
		     src_vaddr, dst_vaddr, size, static_cast<int>(src_gds), static_cast<int>(dst_gds));
	}
	if (src_memory && dst_memory && !IsRegionGpuModified(dst_vaddr, size) &&
	    !IsRegionGpuModified(src_vaddr, size) && !m_texture_cache.FindImageFromRange(src_vaddr, size)) {
		std::memcpy(reinterpret_cast<void*>(dst_vaddr), reinterpret_cast<const void*>(src_vaddr),
		            size);
		return;
	}

	auto& command = m_scheduler.Current();
	if (dst_memory) {
		m_texture_cache.InvalidateMemoryFromGPU(dst_vaddr, size);
	}
	const auto src_id      = src_memory ? FindBuffer(src_vaddr, size) : BufferId {};
	const auto dst_id      = dst_memory ? FindBuffer(dst_vaddr, size) : BufferId {};
	auto [src, src_offset] = src_memory ? ObtainBuffer(src_vaddr, size, false, true, src_id)
	                                    : std::pair {&m_gds_buffer, src_vaddr};
	auto [dst, dst_offset] = dst_memory ? ObtainBuffer(dst_vaddr, size, true, true, dst_id)
	                                    : std::pair {&m_gds_buffer, dst_vaddr};
	EXIT_IF(src == nullptr || dst == nullptr);
	if (src == dst && src_offset < dst_offset + size && dst_offset < src_offset + size) {
		EXIT("BufferCache: resolved Vulkan copy ranges overlap\n");
	}
	dst->CopyFrom(command, *src, src_offset, dst_offset, size);
}

bool BufferCache::IsRegionRegistered(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid registered-region query\n");
	}
	// Cached buffers are ordered and non-overlapping. The last buffer beginning before the query
	// end is therefore the only possible intersection.
	const auto candidate = m_buffers.lower_bound(vaddr + size);
	if (candidate == m_buffers.begin()) {
		return false;
	}
	const auto& [address, id] = *std::prev(candidate);
	return address + m_slot_buffers[id].Size() > vaddr;
}

bool BufferCache::IsRegionGpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionGpuModified(vaddr, size);
}

bool BufferCache::HasGpuDirtyBytes(uint64_t vaddr, uint64_t size) {
	return m_gpu_modified_ranges.Intersects(vaddr, size);
}

bool BufferCache::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionCpuModified(vaddr, size);
}

void BufferCache::RunGarbageCollector() {
	const auto tick = m_gc_tick++;
	if (m_graphics.CanReportMemoryUsage()) {
		m_total_used_memory = m_graphics.GetDeviceMemoryUsage();
	}
	if (m_total_used_memory < m_trigger_gc_memory) {
		return;
	}
	// Most collection checks do no work. Only a real collection can enumerate
	// pending dirty bytes or retire their owner, so only that path needs a drain.
	DrainGuestReadback();

	const bool     aggressive = m_total_used_memory >= m_critical_gc_memory;
	const uint64_t age        = std::min<uint64_t>(aggressive ? 80 : 160, tick);
	const size_t   limit      = aggressive ? 64 : 32;

	std::vector<BufferId> dirty_buffers;
	std::vector<DownloadCopy> copies;
	size_t                    retire_count = 0;
	m_lru_cache.ForEachItemBelow(tick - age, [&](BufferId id) {
		auto& buffer = m_slot_buffers[id];
		EXIT_IF(buffer.is_deleted);
		m_memory_tracker.ValidateGpuDirtyOwnership(m_gpu_modified_ranges, buffer.CpuAddress(),
		                                           buffer.Size(), "garbage collection");
		const bool dirty = m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size());
		if (dirty && !aggressive) {
			return false;
		}
		if (dirty) {
			m_memory_tracker.ForEachDownloadRange<false>(
			    buffer.CpuAddress(), buffer.Size(),
			    [&](uint64_t dirty_address, uint64_t dirty_size) noexcept {
				    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, dirty_address,
				                                           dirty_size, "garbage collection");
			    },
			    [&](uint64_t dirty_address, uint64_t dirty_size) noexcept {
				    m_gpu_modified_ranges.ForEachIntersection(
				        dirty_address, dirty_size, [&](RangeSet::Range range) {
					    copies.push_back({&buffer, range.address - buffer.CpuAddress(),
					                      range.address, range.size});
				        });
				});
			dirty_buffers.push_back(id);
		} else {
			m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
			DeleteBuffer(id);
		}
		return ++retire_count == limit;
	});
	if (dirty_buffers.empty()) {
		return;
	}

	EXIT_IF(copies.empty());
	DownloadBufferMemory(copies);
	for (const auto id: dirty_buffers) {
		auto& buffer = m_slot_buffers[id];
		m_memory_tracker.UnmarkRegionAsGpuModified(buffer.CpuAddress(), buffer.Size());
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size()) ||
		    m_gpu_modified_ranges.Intersects(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: garbage collection retained GPU ownership\n");
		}
		m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
		Unregister(id);
		m_slot_buffers.erase(id);
	}
}

void BufferCache::ProcessFaultBuffer() {
	// This records into FaultManager's own buffer. Its later registration callbacks
	// pass through ChangeRegister, which drains before changing a guest owner.
	m_fault_manager.ProcessFaultBuffer();
}

void BufferCache::CollectMappedRegisteredRanges(const RangeSet& mapped,
                                                std::vector<RangeSet::Range>& ranges) const {
	ranges.clear();
	for (const auto& [address, id] : m_buffers) {
		mapped.ForEachIntersection(address, m_slot_buffers[id].Size(), [&](RangeSet::Range range) {
			if (!ranges.empty() && ranges.back().address + ranges.back().size == range.address)
				ranges.back().size += range.size;
			else ranges.push_back(range);
		});
	}
}

void BufferCache::SynchronizeRegionRequest(SyncRegionRequest& request) {
	DrainGuestReadback(request.address, request.size, true);
	const auto epoch = m_memory_tracker.CpuModificationEpoch(request.address, request.size);
	const auto registered = m_registration_epoch;
	if (epoch != 0 && request.cpu_epoch == epoch && request.registration_epoch == registered)
		return;
	SynchronizeBuffersInRange(request.address, request.size);
	request.cpu_epoch = 0;
	if (epoch != 0 && m_registration_epoch == registered &&
	    m_memory_tracker.CpuModificationEpoch(request.address, request.size) == epoch) {
		request.cpu_epoch = epoch;
		request.registration_epoch = registered;
	}
}

void BufferCache::SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size) {
	DrainGuestReadback(vaddr, size, true);
	if (!m_sync_buffers_valid) {
		m_sync_buffers.clear();
		m_sync_buffers.reserve(m_buffers.size());
		for (const auto& [address, id]: m_buffers) {
			auto& buffer = m_slot_buffers[id];
			const auto end = address + buffer.Size();
			for (auto start = address; start < end;) {
				const auto finish = std::min(end, (start / TRACKER_REGION_SIZE + 1) * TRACKER_REGION_SIZE);
				m_sync_buffers.push_back({start, finish, &buffer});
				start = finish;
			}
		}
		m_sync_stamps.assign(m_sync_buffers.size(), {});
		m_sync_buffers_valid = true;
	}
	const auto end = vaddr + size;

	auto it = std::lower_bound(m_sync_buffers.begin(), m_sync_buffers.end(), vaddr,
	                           [](const SyncBuffer& buffer, uint64_t address) {
		                           return buffer.end <= address;
	                           });
	for (; it != m_sync_buffers.end() && it->start < end; ++it) {
		const auto start  = std::max(it->start, vaddr);
		const auto finish = std::min(it->end, end);
		if (start < finish) {
			SyncStamp* stamp = nullptr;
			uint64_t epoch = 0;
			{
				stamp = &m_sync_stamps[static_cast<size_t>(it - m_sync_buffers.begin())];
				epoch = m_memory_tracker.CpuModificationEpoch(start, finish - start);
				if (epoch != 0 && stamp->epoch == epoch && start >= stamp->begin && finish <= stamp->end) continue;
			}
			(void)SynchronizeBuffer(*it->buffer, start, finish - start, false, false);
			if (stamp != nullptr && epoch != 0 && m_sync_buffers_valid &&
			    m_memory_tracker.CpuModificationEpoch(start, finish - start) == epoch) {
				*stamp = {start, finish, epoch};
			}
		}
	}

}

} // namespace Libs::Graphics
