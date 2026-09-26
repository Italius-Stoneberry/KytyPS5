#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_

#include "common/common.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <memory>
#include <vector>

namespace Libs::Graphics {

enum class PageFaultAccess { Read, Write, Execute, Unknown };

class PageManager final {
public:
	PageManager();
	// The owner must stop all PageManager callers before destruction.
	~PageManager();

	KYTY_CLASS_NO_COPY(PageManager);

	[[nodiscard]] uint64_t GetPageSize() const;
	// A hint only: callers must still check exact GPU ownership before reading
	// a backing alias. A missing hint retains the normal faulting guest load.
	[[nodiscard]] bool HasReadWatchers(uint64_t vaddr, uint64_t size) const noexcept;

	// Restores the watchers' protection after the host protection of watched pages was
	// changed behind the tracker's back (a guest mprotect). Unwatched pages keep theirs.
	void ReapplyProtection(uint64_t vaddr, uint64_t size);

	// KYTY_ASYNC_REPROTECT: while a sink is set on this thread, adding write watchers
	// updates the page state but records the address ranges instead of changing the host
	// protection; ReapplyProtection of those ranges applies the current state later.
	struct DeferredRange {
		uint64_t address = 0, size = 0;
	};
	static void SetDeferredWriteProtectSink(std::vector<DeferredRange>* sink) noexcept;

	template <bool track>
	void UpdatePageWatchers(uint64_t vaddr, uint64_t size);
	template <bool track, bool is_read = false>
	void UpdatePageWatchersForRegion(uint64_t base_addr, RegionBits& mask);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
