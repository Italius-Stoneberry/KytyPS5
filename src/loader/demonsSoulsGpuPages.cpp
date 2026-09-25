#include "loader/demonsSoulsGpuPages.h"
#include "common/logging/log.h"
#include "common/virtualMemory.h"
#include "graphics/host_gpu/hostMemory.h"
#include "graphics/host_gpu/renderer/demonsSouls.h"
#include "kernel/memory.h"
#include "loader/runtimeLinker.h"
#include <cstdlib>

namespace Loader::DemonsSoulsGpuPages {
namespace {
uint64_t base = 0, cave = 0;
size_t written = 0;
constexpr std::array<uint64_t, 3> AllSites {Sites[0], Sites[1], ContextSite};
std::array<std::array<uint8_t, 5>, AllSites.size()> original {}, installed {};
}
void Install(Program* program) {
#if defined(__x86_64__) || defined(_M_X64)
    // KYTY_GPU_BUFFER_PAGES=1: the two CBuffer allocation calls and the render-context call.
    const auto* enabled = std::getenv("KYTY_GPU_BUFFER_PAGES");
    if (!enabled || std::strcmp(enabled, "1") != 0 || cave || !program ||
        program->file_name.filename() != "eboot.bin" ||
        !Libs::Graphics::DemonsSouls::IsSupportedGame() || program->mapped_size >= CaveOffset ||
        program->base_vaddr > UINT64_MAX - CaveOffset - ImageSize ||
        program->tls.handler_vaddr < program->base_vaddr ||
        program->tls.handler_vaddr - program->base_vaddr > INT32_MAX) return;
    const auto* guest = reinterpret_cast<const uint8_t*>(program->base_vaddr);
    for (const auto& function : AuditedFunctions) {
        if (function.rva + function.size > program->mapped_size ||
            !Libs::Graphics::HostMemoryRangeIsReadable(program->base_vaddr + function.rva, function.size) ||
            !VerifyLoadedFunction(guest, function, program->tls.handler_vaddr - program->base_vaddr)) {
            LOGF("GPU buffer pages: audited function differs at %llx; retaining original allocations\n",
                 static_cast<unsigned long long>(function.rva));
            return;
        }
    }
    std::array<uint8_t, ImageSize> image;
    if (!BuildImage(image.data())) return;
    const size_t site_count = AllSites.size();
    for (size_t i = 0; i < site_count; ++i) {
        if (!BuildCall(installed[i], guest, AllSites[i])) return;
        std::memcpy(original[i].data(), guest + AllSites[i], 5);
    }
    const auto requested = program->base_vaddr + CaveOffset;
    const auto allocated = Libs::LibKernel::Memory::AllocateRuntimeMemory(requested, ImageSize,
        Common::VirtualMemory::Mode::ReadWrite, "gpu_buffer_pages", true);
    if (allocated != requested) {
        if (allocated) Libs::LibKernel::Memory::FreeGuestMemory(allocated, ImageSize);
        return;
    }
    base = program->base_vaddr;
    cave = allocated;
    std::memcpy(reinterpret_cast<void*>(cave), image.data(), image.size());
    if (!Common::VirtualMemory::FlushInstructionCache(cave, StatsOffset) ||
        !Libs::LibKernel::Memory::ProtectGuestMemory(cave, StatsOffset,
            Common::VirtualMemory::Mode::ExecuteRead, nullptr)) {
        Clear();
        return;
    }
    for (size_t i = 0; i < site_count; ++i) {
        const auto address = base + AllSites[i];
        std::memcpy(reinterpret_cast<void*>(address), installed[i].data(), 5);
        ++written;
        if (!Common::VirtualMemory::FlushInstructionCache(address, 5)) {
            Clear();
            return;
        }
    }
    LOGF("GPU buffer pages: %zu verified allocation calls installed, 4096-byte pages\n", site_count);
#endif
}
void Clear() {
    if (!cave) return;
    for (size_t i = 0; i < written; ++i) {
        auto* address = reinterpret_cast<void*>(base + AllSites[i]);
        if (std::memcmp(address, installed[i].data(), 5) == 0) {
            std::memcpy(address, original[i].data(), 5);
            Common::VirtualMemory::FlushInstructionCache(base + AllSites[i], 5);
        }
    }
    const auto* stats = reinterpret_cast<const uint64_t*>(cave + StatsOffset);
    LOGF("GPU buffer pages: cumulative requests=%llu logical=%llu physical=%llu\n",
         static_cast<unsigned long long>(stats[0]), static_cast<unsigned long long>(stats[1]),
         static_cast<unsigned long long>(stats[2]));
    Libs::LibKernel::Memory::FreeGuestMemory(cave, ImageSize);
    base = cave = written = 0;
}
} // namespace Loader::DemonsSoulsGpuPages
