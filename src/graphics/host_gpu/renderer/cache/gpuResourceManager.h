#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPURESOURCEMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPURESOURCEMANAGER_H_

#include "common/abi.h"
#include "common/common.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"

#include <array>
#include <cstdint>
#include <shared_mutex>

namespace Libs::Graphics {

class CommandScheduler;
class GuestGpu;

class GpuResourceManager {
public:
	GpuResourceManager(GraphicContext& graphics, CommandScheduler& scheduler);
	~GpuResourceManager();
	KYTY_CLASS_NO_COPY(GpuResourceManager);

	[[nodiscard]] BufferCache&  GetBufferCache() { return m_buffer_cache; }
	[[nodiscard]] TextureCache& GetTextureCache() { return m_texture_cache; }
	[[nodiscard]] bool          HasReadWatchers(uint64_t vaddr, uint64_t size) const noexcept {
        return m_page_manager.HasReadWatchers(vaddr, size);
	}
	void                        SetGpu(GuestGpu* gpu) noexcept { m_gpu = gpu; }
	// After a guest protection change: tracked pages get the tracker's protection back.
	void ReapplyProtection(uint64_t vaddr, uint64_t size) { m_page_manager.ReapplyProtection(vaddr, size); }
	void SyncProtection(uint64_t vaddr, uint64_t size) { m_page_manager.SyncProtection(vaddr, size); }

	[[nodiscard]] bool HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept;
	[[nodiscard]] bool InvalidateMemory(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsMapped(uint64_t vaddr, uint64_t size) const noexcept;
	void               MapMemory(uint64_t vaddr, uint64_t size);
	// `releasing`: the caller unmaps or decommits the range right after (munmap, free,
	// memory pool decommit), not a range about to receive a mapping.
	void               UnmapMemory(uint64_t vaddr, uint64_t size, bool releasing = false);
	void               PrepareBda();
	void BeginAliasWrite() noexcept;
	void EndAliasWrite() noexcept;
	[[nodiscard]] uint64_t PreparationAliasEpoch() const noexcept;
	// Written only under m_mapped_ranges_mutex; readers compare it for change only,
	// so an acquire load needs no lock.
	[[nodiscard]] uint64_t MappingEpoch() const noexcept {
		return m_mapping_epoch.load(std::memory_order_acquire);
	}
	// Whether memory in [vaddr, vaddr + size) was unmapped after mapping epoch `epoch` (also
	// true when that is further back than the unmaps kept). GPU thread, as UnmapMemory.
	[[nodiscard]] bool UnmappedSince(uint64_t epoch, uint64_t vaddr, uint64_t size) const noexcept;

	bool PrepareBdaReadRanges(std::span<const GuestRange> ranges);
	// After each completed submission: its shaders' fault records and the image downloads.
	void               EndSubmission();
	// Once per flip: the frame count and the garbage collection (its ages count frames).
	void               AdvanceFrame();

private:
	[[nodiscard]] bool        TryInvalidateCpuWriteWindow(uint64_t fault);
	void                      SynchronizeDirtyBdaRegions(GuestRange range);
	void RefreshBdaRanges();
	PageManager               m_page_manager;
	CommandScheduler&         m_scheduler;
	BufferCache               m_buffer_cache;
	TextureCache              m_texture_cache;
	mutable std::shared_mutex m_mapped_ranges_mutex;
	RangeSet                  m_mapped_ranges;
	std::atomic<uint64_t> m_mapping_epoch {1};
	// The latest unmaps, with the mapping epoch each made (written by UnmapMemory's GPU-thread part).
	struct Unmap {
		uint64_t epoch = 0, begin = 0, end = 0;
	};
	std::array<Unmap, 64> m_unmaps {};
	uint64_t              m_unmap_count = 0;
	uint64_t m_bda_mapping_epoch = 0, m_bda_registration_epoch = 0;
	std::vector<BufferCache::SyncRegionRequest> m_bda_region_requests;
	GuestGpu*                 m_gpu = nullptr;
	std::atomic<uint64_t> m_preparation_alias_epoch {uint64_t{1} << 32};
	bool                      m_fault_process_pending = false;
	bool                      m_bda_used              = false;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPURESOURCEMANAGER_H_
