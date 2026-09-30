#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_

#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <string>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Canonical module-affecting resource state. Runtime addresses and descriptor payloads remain in
// ResourceSnapshot and therefore do not create shader permutations.
struct ResourceSpecialization {
	struct Buffer {
		uint32_t               packed_stride                   = 0;
		Prospero::BufferFormat descriptor_format               = Prospero::BufferFormat::kInvalid;
		uint32_t               descriptor_swizzle              = DstSel(4, 5, 6, 7);
		bool                   byte_base_offset                = false;
		bool                   operator==(const Buffer&) const = default;
	};

	struct Image {
		Prospero::TextureNumericClass numeric_class = Prospero::TextureNumericClass::Unsupported;
		Decoder::ImageDimension       dimension     = Decoder::ImageDimension::Unknown;
		uint32_t                      mip_count     = 1;
		Prospero::BufferFormat        conversion_format          = Prospero::BufferFormat::kInvalid;
		uint32_t                      shader_swizzle             = ShaderImageIdentitySwizzle;
		uint32_t                      indirect_root              = ImageResource::NoIndirectImage;
		uint32_t                      indirect_mapping_offset    = 0;
		uint32_t                      indirect_search_iterations = 0;
		bool                          cube                       = false;
		bool                          fmask                      = false;
		bool                          operator==(const Image&) const = default;
	};

	std::vector<Buffer> buffers;
	std::vector<Image>  images;

	bool operator==(const ResourceSpecialization&) const = default;
};

// Extracts the descriptor/SRT value graph before resource specialization. The returned plan owns
// its values and is independent of the translated shader CFG.
ResourcePlan ExtractResourcePlan(const Program& program);

// Resolves and specializes the immutable resource plan in one transaction. On failure both
// destinations are unchanged.
struct MaterializeReport {
	std::string reason;
	uint32_t    dropped_candidates = 0;
	uint32_t    dropped_shapes     = 0;
	std::string dropped_summary;
};

// Rendering-thread owned, tied to one immutable ResourcePlan. Only shader
// interpretation is reusable: addresses, sizes, SRT values, sampler payloads
// and uniform-fill values are always taken from the current transaction.
struct ResourceSpecializationGuard {
	struct BufferProbe {
		uint32_t low_address, stride, flags, flag_mask;
	};
	struct ImageProbe {
		uint32_t format, interpretation;
		bool null;
	};
	const ResourcePlan* owner = nullptr;
	std::vector<BufferProbe> buffers;
	std::vector<ImageProbe> images;
	std::vector<uint32_t> duplicate_samplers;
	ResourceSpecialization specialization;
	bool compact_images = false;
	uint64_t publication = 0;
};

bool MaterializeResources(const ResourcePlan& program, const SrtRuntime& runtime,
                          ResourceSnapshot& snapshot, ResourceSpecialization& specialization,
                          MaterializeReport* report = nullptr,
                          ResourceSpecializationGuard* shape_guard = nullptr,
                          const ResourceSpecialization** borrowed_specialization = nullptr);
// With borrowed_specialization supplied, a successful guard hit may leave the
// owned specialization unchanged and return the guard's specialization there.
// Other successful paths set it to null. A borrowed pointer is valid only until
// the next call using that guard; callers must consume it before reentering.

// Applies an already-derived specialization to native IR before layout and emission.
void ApplyResourceSpecialization(Program& program, const ResourceSpecialization& specialization);

// The canonical form of a specialization of portable modules (PortableShaders): the fields the
// code reads from its buffer words at run time (a stride below 4096 without swizzling) are cleared,
// so a warmup record from before canonicalizes to the permutation the game now selects (but for a
// null texture, which such a record holds as 2D; MaterializeResources now gives it the dimension
// the shader declares).
void CanonicalizeSpecialization(const ShaderInfo& info, ResourceSpecialization& specialization);

// The portable form of a (canonical) specialization: the formatted buffers the code only loads from
// decode the format of their buffer words (RuntimeBufferFormat) instead of the V#'s. Its module
// serves every format: the static precompile compiles it, and a program meets a format for the
// first time with it (PipelineCache). False when that leaves the specialization unchanged.
bool PortableFormats(const ShaderInfo& info, ResourceSpecialization& specialization);

// The buffer word (BufferWord) of a materialized V#.
uint32_t BufferDescriptorWord(const DescriptorValue& descriptor);

// A buffer format's layout and component type as a buffer word holds them (bits 0-6 of the word
// shifted right by BufferWord::LayoutShift); 0 for a format without them.
uint32_t BufferFormatClass(Prospero::BufferFormat format);

// The numeric class each image of a program most likely has, from how the program uses it (the
// texture's format, which decides it, is the game's data): Uint when what it reads is used as
// integers (bit operations, integer compares and conversions) and never as floats, or what it
// stores is made by integer operations; Float otherwise. For the static precompile.
std::vector<Prospero::TextureNumericClass> PredictImageNumericClasses(const Program& program);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_ */
