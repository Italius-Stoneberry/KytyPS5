#ifndef EMULATOR_SRC_GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_CPINDIRECT_H_
#define EMULATOR_SRC_GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_CPINDIRECT_H_

// A DRAW_INDIRECT / DRAW_INDEX_INDIRECT record read on the CPU, drawn as the direct path's
// DrawIndexAuto / DrawIndex op (CommandProcessor::ExecDrawIndirect). One function builds that op
// for both sides of KYTY_DRAW_PREP_INDIRECT (graphicsRun.cpp): the sequencer, which publishes the
// draw to the draw-prep window from the record bytes it read, and the resolver, which builds the
// draw from its own read of the record and commits the slot only when the two reads are equal.
// Pure functions of the op and the record bytes (cp_sequencer_tests covers them).

#include "graphics/guest_gpu/command_processor/cpOps.h"

#include <cstdint>

namespace Libs::Graphics::CpSeq {

// DrawIndexedIndirectArgs (20 bytes) and DrawIndirectArgs (16 bytes), as dwords.
inline constexpr uint32_t IndirectRecordDwords = 5;

[[nodiscard]] constexpr uint32_t IndirectRecordSize(bool indexed) noexcept {
	return indexed ? 20u : 16u;
}

// DrawOffsetSource::IndirectArgs (render.h; graphicsRun.cpp checks the value).
inline constexpr uint32_t OffsetSourceIndirectArgs = 1;

// INDEX_TYPE's element size; 0 for a type the command processor rejects (the resolver's
// IndexElementSize stops the emulator on it, the sequencer just does not publish).
[[nodiscard]] constexpr uint64_t IndexElementBytes(uint32_t index_type_and_size) noexcept {
	switch (index_type_and_size) {
		case 0: return 2;
		case 1: return 4;
		case 2: return 1;
		default: return 0;
	}
}

// A zero instance count takes the back's instance state (NumInstances), which the draw's own
// record has just set (DrawFlagInheritInstances).
[[nodiscard]] inline DrawAutoOp CpuIndirectAutoDraw(uint32_t vertex_count, uint32_t instance_count,
                                                    uint32_t first_vertex,
                                                    uint32_t first_instance) {
	DrawAutoOp draw;
	draw.vertex_count   = vertex_count;
	draw.instance_count = instance_count;
	draw.first_vertex   = first_vertex;
	draw.first_instance = first_instance;
	draw.offset_source  = OffsetSourceIndirectArgs;
	draw.flags          = instance_count == 0 ? DrawFlagInheritInstances : 0u;
	return draw;
}

[[nodiscard]] inline DrawIndexOp CpuIndirectIndexDraw(uint64_t index_addr, uint32_t index_count,
                                                      uint32_t instance_count, int32_t base_vertex,
                                                      uint32_t first_instance,
                                                      uint32_t index_type_and_size) {
	DrawIndexOp draw;
	draw.index_addr          = index_addr;
	draw.index_count         = index_count;
	draw.instance_count      = instance_count;
	draw.index_type_and_size = index_type_and_size;
	draw.base_vertex         = base_vertex;
	draw.first_instance      = first_instance;
	draw.offset_source       = OffsetSourceIndirectArgs;
	draw.flags               = instance_count == 0 ? DrawFlagInheritInstances : 0u;
	return draw;
}

// The draw of one record of an indirect draw op: `index` when the op is indexed, else `automatic`.
struct CpuIndirectDraw {
	bool        indexed = false;
	DrawIndexOp index;
	DrawAutoOp  automatic;
	// The record's instance count: the back's instance state after the draw (m_num_instances).
	uint32_t instance_count = 0;
	// Indexed: the record's index count before the INDEX_BUFFER_SIZE clamp (debug log).
	uint32_t record_index_count = 0;
};

// `record`: the record's first IndirectRecordSize bytes (the rest is ignored). `index_size`:
// IndexElementBytes(op.index_type_and_size), non-zero for an indexed op.
[[nodiscard]] inline CpuIndirectDraw
MakeCpuIndirectDraw(const DrawIndirectOp& op, const uint32_t* record, uint64_t index_size) {
	CpuIndirectDraw draw;
	draw.indexed = (op.flags & IndirectFlagIndexed) != 0;
	if (!draw.indexed) {
		// DrawIndirectArgs: vertex_count_per_instance, instance_count, start_vertex_location,
		// start_instance_location.
		draw.instance_count = record[1];
		draw.automatic      = CpuIndirectAutoDraw(record[0], record[1], record[2], record[3]);
		return draw;
	}
	// DrawIndexedIndirectArgs: index_count_per_instance, instance_count, start_index_location,
	// base_vertex_location, start_instance_location. The index count is clamped to
	// INDEX_BUFFER_SIZE (ignoring the first index), as the CPU path always did.
	const auto index_addr = op.index_base_addr + static_cast<uint64_t>(record[2]) * index_size;
	const auto index_count =
	    op.index_buffer_size != 0
	        ? (record[0] < op.index_buffer_size ? record[0] : op.index_buffer_size)
	        : record[0];
	draw.instance_count     = record[1];
	draw.record_index_count = record[0];
	draw.index =
	    CpuIndirectIndexDraw(index_addr, index_count, record[1], static_cast<int32_t>(record[3]),
	                         record[4], op.index_type_and_size);
	return draw;
}

} // namespace Libs::Graphics::CpSeq

#endif // EMULATOR_SRC_GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_CPINDIRECT_H_
