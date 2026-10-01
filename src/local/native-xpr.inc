// --- Native XPR draws --------------------------------------------------------
//
// Included by renderDraw.cpp (local Vulkan recording builds only). The normal
// path, asked to store, leaves two entries behind:
//  * a draw state, keyed by all graphics registers except user data: the render
//    targets, their attachment layouts and the dynamic state;
//  * an object record, keyed by the shader addresses and user data: one
//    descriptor set written once with the object's buffers, images, samplers
//    and fixed renderer buffers; the constants (flattened SRT, shader data,
//    push data); per draw state, the pipeline variant with native_bindings -
//    the same SPIR-V, the flattened-SRT and shader-data bindings dynamic storage
//    buffers in the shader upload ring; and what proves the record current.
// A native draw copies the constants into the upload ring, binds the set with
// the ring offsets as dynamic offsets, pushes the push data and draws from
// GPU-side indirect arguments.
//
// Runtime (KYTY_NATIVE_XPR=1): the command processor offers every
// DRAW_INDEX_INDIRECT run (consecutive packets differing only in the argument
// offset share one object). A record is proven current once per frame:
//  * every guest word its SRT evaluation read still has the logged value, read
//    the way the evaluation reads it; words learned to land only in the
//    flattened SRT (from an earlier re-store of the same key) are patched;
//  * the guest mapping is unchanged and no guest readback is pending;
//  * every buffer is still the same live game buffer and CPU-clean (proved by
//    the tracker's region epochs, checked precisely when they moved);
//  * every sampled image is still registered with the same backing and is not
//    CPU-dirty.
// Per draw only the live-object epochs are compared. Anything else - a missing
// record, state or pipeline, a failed proof - falls back to the normal path,
// which stores again (reads logged through ShaderReadObserver).
//
// Verification (KYTY_NATIVE_XPR=2): a draw the native path would emit takes the
// normal path instead, which compares what it prepared with the record
// (NativeXprVerify). The offline replay (xpr_replay_tests --native) checks the
// emission pixel by pixel against the normal path.

extern "C" {
// 0 off, 1 native XPR draws, 2 verification.
[[gnu::used]] volatile std::atomic<uint32_t> kyty_local_native_xpr_mode {0};
// Negative controls for the offline replay only: bit 0 zeroes the vertex
// stage's flattened SRT in the upload, bit 1 skips the draw call.
[[gnu::used]] volatile std::atomic<uint32_t> kyty_local_native_xpr_debug {0};
// 1: objects are drawn in last frame's order, so the record that followed this
// one last frame is prefetched while this draw runs.
[[gnu::used]] volatile std::atomic<uint32_t> kyty_local_native_xpr_predict_mode {0};
// 1: native image proofs check the image's own registration (see NativeXprValidateResources).
[[gnu::used]] volatile std::atomic<uint32_t> kyty_local_native_image_proof_mode {0};
// 1: a store whose pipeline variant is not compiled yet hands it to the compile workers and
// is skipped; the draw takes the normal path until a later store finds the variant ready.
[[gnu::used]] volatile std::atomic<uint32_t> kyty_local_async_xpr_pipelines_mode {0};
}
namespace {
// word_patch value of a logged word whose change reaches nothing the evaluation produces.
constexpr int32_t NativeXprIgnore = -2;

// T# fields that only feed the LOD statistics (MIN_LOD_WARN, MIP_STATS_CNT_EN,
// MIP_STATS_CNT_ID): the game reassigns the counters every frame. They reach the
// shader data only, never the image resolution or view.
constexpr std::array<uint32_t, 8> NativeXprLodStatsMask {0, 0, 0, 0, 0, (0xfffu << 8u) | (1u << 25u), 0xffu, 0};

// Gather feed kind of a flattened-SRT copy of a structural word (beside the
// SrtReadTrace::Kind descriptor kinds).
constexpr uint32_t NativeXprFlatFeed = 0xf;

// Longest gather span, in words.
constexpr uint32_t NativeXprSpanWords = 256;

// word_patch values below this encode a T# LOD-statistics dword:
// NativeXprImagePatchBase - (stage << 20 | image << 4 | dword).
constexpr int32_t NativeXprImagePatchBase = -16;

int32_t NativeXprImagePatch(uint32_t stage, uint32_t image, uint32_t dword) {
	return NativeXprImagePatchBase - static_cast<int32_t>(stage << 20u | image << 4u | dword);
}

// The LOD-statistics shader-data words of `images`, as RebindBuffers computes them.
void NativeXprApplyLodStats(const ShaderRecompiler::IR::CompiledShaderInfo&            program,
                            const std::vector<ShaderRecompiler::IR::DescriptorValue>& images,
                            std::vector<uint32_t>&                                     shader_data) {
	const auto& layout = program.bindings;
	for (uint32_t i = 0; i < layout.lod_stats_count && i < images.size(); ++i) {
		const auto desc = DecodeNativeDescriptor<ShaderTextureResource>(images[i]);
		shader_data[layout.LodStatsDword() + i] =
		    desc.MipStatsCntEn() ? (0x80000000u | desc.MipStatsCntId() | (uint32_t(desc.BaseLevel()) << 8u) |
		                            (uint32_t(desc.MinLodWarn5()) << 12u))
		                         : 0u;
	}
}

bool NativeXprSameImagesButLodStats(const std::vector<ShaderRecompiler::IR::DescriptorValue>& a,
                                    const std::vector<ShaderRecompiler::IR::DescriptorValue>& b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i) {
		if (a[i].dword_count != b[i].dword_count) return false;
		for (uint32_t w = 0; w < a[i].dwords.size(); ++w)
			if ((a[i].dwords[w] & ~NativeXprLodStatsMask[w]) != (b[i].dwords[w] & ~NativeXprLodStatsMask[w]))
				return false;
	}
	return true;
}

// Identity of a set's images: the image descriptors without the LOD-statistics fields.
uint64_t NativeXprImageSignature(std::span<const ShaderRecompiler::IR::ResourceSnapshot* const> snapshots) {
	uint64_t hash = 0xcbf29ce484222325ull;
	for (const auto* snapshot: snapshots)
		for (const auto& image: snapshot->images) {
			for (uint32_t w = 0; w < image.dwords.size(); ++w)
				hash = (hash ^ (image.dwords[w] & ~NativeXprLodStatsMask[w])) * 0x100000001b3ull;
			hash = (hash ^ 0xffu) * 0x100000001b3ull;
		}
	return hash;
}

void NativeXprPrefetch(const void* data, size_t bytes) {
	const auto* at = static_cast<const char*>(data);
	for (size_t offset = 0; offset < bytes; offset += 64) __builtin_prefetch(at + offset);
}

template <typename T>
void NativeXprPrefetch(const std::vector<T>& values) {
	NativeXprPrefetch(values.data(), std::min<size_t>(values.size() * sizeof(T), 1024));
}

// The reader ReadShaderRawGuestMemory uses for every SRT word.
uint32_t NativeXprReadGuestWord(uint64_t address) {
	uint32_t value = 0;
	if (!Libs::LibKernel::Memory::TryReadGpuCleanBackingOnWatchedPage(address, &value, sizeof(value)))
		std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
	return value;
}
} // namespace

// Draw state, identified by every graphics register except user data: the
// render targets as a draw under that state left them (reopened renderings
// load), their attachment layouts and the dynamic state.
struct RenderExecutor::NativeXprDrawState {
	struct Target {
		ImageId               id;
		vk::ImageLayout       layout = vk::ImageLayout::eUndefined;
		vk::AccessFlags2      access;
		ImageSubresourceRange range;
		uint64_t              metadata = 0; // surface metadata (HTile/DCC) address, 0 none
		uint32_t              slice    = 0;
		uint64_t              serial   = 0; // the image object the attachment views belong to
	};
	RenderState                      rendering;       // reopened: loads
	RenderState                      first_rendering; // as the stored draw began it
	std::vector<Target>              targets;
	LocalDrawRecording::DynamicState dynamic;
	// Metadata epoch under which no target had a pending fast clear.
	mutable uint64_t                 meta_epoch = 0;
};

// One object: its resources under the compiled programs it was translated with.
struct RenderExecutor::NativeXprRecord {
	struct Stage {
		const ShaderRecompiler::IR::CompiledShaderInfo* program = nullptr;
		std::vector<uint32_t> flattened;   // flattened SRT
		std::vector<uint32_t> shader_data; // push data and/or the shader-data binding
		bool     srt_binding  = false;
		bool     data_binding = false;
		uint32_t push_start   = ShaderRecompiler::IR::PushData::NoStart;
		ShaderRecompiler::IR::ResourceSnapshot snapshot; // structure, for learning
		// SRT evaluation inputs, for re-evaluation when read words changed.
		std::vector<uint32_t> user_data;
		uint64_t              shader_base = 0;
		// What the set holds for this stage, for verification (mode 2).
		std::vector<vk::ImageView> views;
		std::vector<vk::Sampler>   samplers;
		// Resolved images and, per image, its (position in set_images, mip element);
		// per buffer, its positions in set_buffers.
		std::vector<TextureBinding>                            textures;
		std::vector<std::vector<std::pair<uint32_t, uint32_t>>> image_positions;
		std::vector<std::vector<uint32_t>>                      buffer_positions;
	};
	struct BufferDep {
		uint64_t   address = 0, size = 0;
		BufferId   id;
		vk::Buffer handle = nullptr;
		bool       texel  = false;
		uint32_t   stage = 0, slot = 0;
	};
	// The fields every use reads come first (one cache line); the descriptor, read only
	// when the proof is redone, last.
	struct ImageDep {
		ImageId               id;
		vk::ImageLayout       layout = vk::ImageLayout::eUndefined;
		ImageSubresourceRange range;
		// The image object the set's views belong to: a deleted image's slot can
		// hold another image with the same backing, which IsSampledImageCurrent accepts.
		uint64_t              serial = 0;
		uint32_t              registration = 0; // the image's registration the proof saw
		TextureCache::ImageDesc desc;
	};
	// The images' ids alone, contiguous: every use checks them against the draw's targets
	// (an ImageDep holds a whole ImageDesc, a cache line or more apart from the next id).
	std::vector<ImageId> image_ids;
	void                 IndexImages() {
		image_ids.clear();
		for (const auto& dep: images) image_ids.push_back(dep.id);
	}
	std::vector<uint32_t> key;
	// The first key words again, inline: a lookup compares most keys without
	// the vector's cache miss.
	static constexpr uint32_t KeyHead = 36;
	uint32_t                  key_size = 0;
	std::array<uint32_t, KeyHead> key_head {};
	void                      HoldKey() {
        key_size = static_cast<uint32_t>(key.size());
        std::copy_n(key.begin(), std::min(key_size, KeyHead), key_head.begin());
	}
	[[nodiscard]] bool SameKey(const std::vector<uint32_t>& other) const {
		if (other.size() != key_size) return false;
		const auto head = std::min(key_size, KeyHead);
		return std::memcmp(key_head.data(), other.data(), head * 4ull) == 0 &&
		       (key_size == head ||
		        std::memcmp(key.data() + head, other.data() + head, (key_size - head) * 4ull) == 0);
	}
	uint64_t              vertex_program = 0, pixel_program = 0;
	vk::DescriptorSet     set    = nullptr;
	vk::DescriptorPool    pool   = nullptr;
	vk::Buffer            upload = nullptr; // ring the dynamic bindings point into
	std::array<Stage, 2>  stages;
	uint32_t              stage_count = 1;
	vk::ShaderStageFlags  push_stages;
	bool                  has_push = false;
	// Native pipeline variant per draw state this object was drawn under.
	std::vector<std::pair<uint64_t, const PipelineCache::Pipeline*>> pipelines;
	// Direct draws (DRAW_INDEX_2 / DRAW_INDEX_OFFSET_2): the vertex and instance offsets the
	// normal path resolved under a draw state. Both come from the index-offset register or the
	// vertex shader's offset user-data SGPRs, which the state and object keys cover.
	struct DirectOffsets {
		uint64_t state    = 0;
		int32_t  vertex   = 0;
		uint32_t instance = 0;
	};
	std::vector<DirectOffsets> direct_offsets;
	// Validation (runtime).
	std::vector<std::pair<uint64_t, uint32_t>> reads; // (address, words)
	std::vector<uint32_t>  words;
	std::vector<int32_t>   word_patch; // stage << 24 | flattened index, -1 unknown
	std::vector<BufferDep> buffers;
	std::vector<ImageDep>  images;
	uint64_t mapping_epoch      = 0;
	uint64_t cpu_epoch          = 0; // sum of the buffer regions' CPU epochs, 0 unknown
	uint64_t registration_epoch = 0; // buffer cache registrations when proven live
	uint64_t resolution_epoch   = 0; // texture cache registrations when proven live
	uint64_t start_epoch        = 0; // texture cache image starts when texel ranges were proven
	uint64_t validated_frame    = UINT64_MAX;
	uint64_t retire_tick        = 0;
	uint64_t used_frame         = 0; // last frame a draw used the record
	uint64_t uses               = 0;
	uint64_t next_hash          = 0; // key hash of the record the next native draw used
	uint64_t next2_hash         = 0; // ... the one after it (its index slot is fetched a draw ahead)
	uint64_t image_dirty_epoch  = 0;     // Image::CpuDirtyEpoch() when the images were proven clean
	// The payload the set was written with; an identical rebind keeps the set.
	std::vector<vk::DescriptorBufferInfo> set_buffers;
	std::vector<vk::DescriptorImageInfo>  set_images;
	struct WriteSlot {
		uint32_t           binding = 0, count = 0, start = 0;
		vk::DescriptorType type {};
		bool               image = false;
	};
	std::vector<WriteSlot> set_writes;
	// Earlier sets that differ from the current one only in images (ring
	// textures rotate through a few bases): switching back to one needs no
	// resolution and no new set. Dropped when anything else changes.
	struct Variant {
		uint64_t                                   signature = 0;
		vk::DescriptorSet                          set       = nullptr;
		vk::DescriptorPool                         pool      = nullptr;
		std::vector<vk::DescriptorImageInfo>       set_images;
		std::array<std::vector<TextureBinding>, 2> textures;
		std::array<std::vector<vk::ImageView>, 2>  views;
		std::vector<ImageDep>                      images;
	};
	std::vector<Variant> variants;
	uint64_t             image_signature = 0; // of the current set's images
	// Traced records (every stage evaluated by a single linear SRT plan): the
	// structural reads are compared on each use, the data reads are copied
	// straight into the flattened SRT; no learning or patching.
	struct Gather {
		bool traced = false;
		bool broken = false; // descriptors patched but not resolved: retire
		struct Run {
			uint64_t address = 0;
			uint32_t count   = 0;
			uint32_t first   = 0; // into values (structural) / targets_at (data)
			bool     clean   = false;
		};
		std::vector<Run>      structural;
		std::vector<uint32_t> values;
		std::vector<uint8_t>  uses; // per structural value: SrtReadTrace::Use bits
		// Per structural value: the snapshot descriptor dwords it is copied to,
		// stage << 28 | kind << 24 | index << 8 | dword.
		std::vector<uint32_t> feeds_at; // one extra end entry
		std::vector<uint32_t> feeds;
		std::vector<Run>      data;
		std::vector<uint32_t> targets_at; // per data dword: start in targets; one extra end entry
		std::vector<uint32_t> targets;    // stage << 24 | flat index
		// The non-clean structural and the data words, read as page-local spans
		// (small gaps included). `words` is what the record currently holds for
		// them: a validation handles only the words that differ.
		struct Span {
			uint64_t address = 0;
			uint32_t count   = 0;
			uint32_t first   = 0; // into words
		};
		std::vector<Span>     spans;
		std::vector<uint32_t> words;
		std::vector<int32_t>  word_value; // structural value index, or -1
		std::vector<int32_t>  word_data;  // data dword index (targets_at), or -1
		std::vector<uint32_t> value_word; // per non-clean structural value: its index in words
		// The clean structural words, as page-local spans; per span word its
		// structural value index, or -1 for a gap.
		std::vector<Span>    clean_spans;
		std::vector<int32_t> clean_value;
	} gather;
};

namespace {
// (A template: the record type is private to RenderExecutor.)
// The lines of the record's fields a use reads (addresses only: no load waits).
template <typename Record>
void NativeXprPrefetchFields(const Record& record) {
	for (const void* field: {static_cast<const void*>(&record.key), static_cast<const void*>(&record.key_head[16]),
	                         static_cast<const void*>(&record.key_head[32]), static_cast<const void*>(&record.set),
	                         static_cast<const void*>(&record.stages[0].flattened),
	                         static_cast<const void*>(&record.stages[0].push_start),
	                         static_cast<const void*>(&record.stages[1].flattened),
	                         static_cast<const void*>(&record.stages[1].push_start),
	                         static_cast<const void*>(&record.pipelines), static_cast<const void*>(&record.buffers),
	                         static_cast<const void*>(&record.images), static_cast<const void*>(&record.cpu_epoch),
	                         static_cast<const void*>(&record.used_frame), static_cast<const void*>(&record.gather),
	                         static_cast<const void*>(&record.gather.clean_spans), static_cast<const void*>(&record.gather.spans),
	                         static_cast<const void*>(&record.gather.words)})
		__builtin_prefetch(field);
}

// The arrays a use reads in full (once the fields above are in).
template <typename Record>
void NativeXprPrefetchArrays(const Record& record) {
	const auto& gather = record.gather;
	NativeXprPrefetch(gather.spans);
	NativeXprPrefetch(gather.words);
	NativeXprPrefetch(gather.clean_spans);
	// Read only for changed words, but constants change every frame.
	NativeXprPrefetch(gather.word_data.data(), std::min<size_t>(gather.word_data.size() * 4, 512));
	NativeXprPrefetch(gather.word_value.data(), std::min<size_t>(gather.word_value.size() * 4, 512));
	NativeXprPrefetch(gather.targets_at.data(), std::min<size_t>(gather.targets_at.size() * 4, 512));
	NativeXprPrefetch(gather.targets.data(), std::min<size_t>(gather.targets.size() * 4, 512));
	NativeXprPrefetch(gather.uses.data(), std::min<size_t>(gather.uses.size(), 256));
	NativeXprPrefetch(gather.feeds_at.data(), std::min<size_t>(gather.feeds_at.size() * 4, 256));
	NativeXprPrefetch(gather.feeds.data(), std::min<size_t>(gather.feeds.size() * 4, 256));
	NativeXprPrefetch(gather.value_word.data(), std::min<size_t>(gather.value_word.size() * 4, 256));
	NativeXprPrefetch(record.pipelines);
	// An image dependency is large (its descriptor): the first line of each holds what a use reads.
	NativeXprPrefetch(record.image_ids);
	for (size_t i = 0; i < record.images.size() && i < 16; ++i) __builtin_prefetch(&record.images[i]);
	NativeXprPrefetch(record.buffers);
	if (!gather.traced) {
		NativeXprPrefetch(record.reads);
		NativeXprPrefetch(record.words);
		NativeXprPrefetch(record.word_patch);
	}
	for (uint32_t st = 0; st < record.stage_count; ++st) {
		NativeXprPrefetch(record.stages[st].flattened);
		NativeXprPrefetch(record.stages[st].shader_data);
	}
}
} // namespace

struct RenderExecutor::NativeXprCache {
	vk::Device                                                        device;
	std::unordered_map<uint64_t, std::unique_ptr<NativeXprRecord>>    records;
	// Open-addressing index over `records` (linear probing, at most half full):
	// one cache line per lookup instead of bucket, node and owner.
	struct IndexSlot {
		uint64_t         hash   = 0; // 0 empty (hash 0 is stored as 1)
		NativeXprRecord* record = nullptr;
	};
	std::vector<IndexSlot> index = std::vector<IndexSlot>(8192);
	size_t                 index_count = 0;
	static uint64_t        IndexHash(uint64_t hash) { return hash != 0 ? hash : 1; }
	// Where a lookup of `hash` starts probing (to fetch that line ahead of the lookup).
	[[nodiscard]] const void* IndexSlotOf(uint64_t hash) const {
		return &index[IndexHash(hash) & (index.size() - 1)];
	}
	[[nodiscard]] NativeXprRecord* IndexFind(uint64_t hash) const {
		hash            = IndexHash(hash);
		const auto mask = index.size() - 1;
		for (auto at = hash & mask;; at = (at + 1) & mask) {
			if (index[at].hash == hash) return index[at].record;
			if (index[at].hash == 0) return nullptr;
		}
	}
	void IndexInsert(uint64_t hash, NativeXprRecord* record) {
		if ((index_count + 1) * 2 > index.size()) {
			std::vector<IndexSlot> old(index.size() * 2);
			old.swap(index);
			index_count = 0;
			for (const auto& slot: old)
				if (slot.hash != 0) IndexInsert(slot.hash, slot.record);
		}
		hash            = IndexHash(hash);
		const auto mask = index.size() - 1;
		for (auto at = hash & mask;; at = (at + 1) & mask) {
			if (index[at].hash == hash) {
				index[at].record = record;
				return;
			}
			if (index[at].hash == 0) {
				index[at] = {hash, record};
				++index_count;
				return;
			}
		}
	}
	void IndexErase(uint64_t hash) {
		hash            = IndexHash(hash);
		const auto mask = index.size() - 1;
		auto       at   = hash & mask;
		for (; index[at].hash != hash; at = (at + 1) & mask)
			if (index[at].hash == 0) return;
		// Backward shift: later entries of the probe run move into the hole.
		for (auto next = (at + 1) & mask; index[next].hash != 0; next = (next + 1) & mask) {
			const auto home = index[next].hash & mask;
			if (((next - home) & mask) >= ((next - at) & mask)) {
				index[at] = index[next];
				at        = next;
			}
		}
		index[at] = {};
		--index_count;
	}
	std::unordered_map<uint64_t, std::unique_ptr<NativeXprDrawState>> states;
	// Invalidated records: their sets retire once the GPU passed retire_tick;
	// their logged reads stay for learning until the key is stored again.
	std::unordered_map<uint64_t, std::unique_ptr<NativeXprRecord>> stale;
	struct RetiredSet {
		vk::DescriptorPool pool;
		vk::DescriptorSet  set;
		uint64_t           tick = 0;
	};
	std::vector<RetiredSet> retired;
	std::vector<vk::DescriptorPool>                                pools;
	std::vector<uint32_t>                                          key;
	// Store request: the reads of the normal draw's SRT evaluation.
	std::vector<std::pair<uint64_t, uint64_t>> reads;
	bool                                       logging = false;
	// Scratch for validation.
	// Copy of the texture cache's image start log: every (epoch, address) with
	// starts_from < epoch <= starts_seen.
	std::vector<std::pair<uint64_t, uint64_t>> recent_starts;
	uint64_t                                   starts_from = 0, starts_seen = 0;
	std::vector<vk::DescriptorImageInfo>           previous_images;
	std::vector<NativeXprRecord::ImageDep>         previous_deps;
	std::array<std::vector<TextureBinding>, 2>     previous_textures;
	std::array<std::vector<vk::ImageView>, 2>      previous_views;
	std::vector<std::pair<uint32_t, uint32_t>> rebind_images;  // (stage, image)
	std::vector<std::pair<uint32_t, uint32_t>> rebind_buffers; // (stage, buffer)
	std::vector<TextureBinding>                rebind_textures;
	std::vector<uint32_t>                      span;
	std::vector<std::pair<uint32_t, uint32_t>> changed;
	// What the last native emission left bound in which command buffer.
	struct Emit {
		vk::CommandBuffer              command;
		uint64_t                       generation = 0;
		const NativeXprDrawState*      state      = nullptr;
		const PipelineCache::Pipeline* pipeline   = nullptr;
		uint64_t                       index_base = 0, index_bytes = 0;
		vk::IndexType                  index_type = vk::IndexType::eUint16;
	} emit;
	// Programs the SRT trace cannot cover (no single linear plan): never retried.
	std::unordered_set<const ShaderRecompiler::IR::CompiledShaderInfo*> untraceable;
	// Per shader-address pair (key words 0-3): records stored and records that
	// left unused. A pair whose records are never reused (user data that changes
	// every frame) stops storing.
	struct PairStats {
		uint64_t stored = 0, wasted = 0;
		bool     blocked = false;
		bool     unstorable = false; // a program writes memory or uses DMA/GDS (NativeXprStore)
	};
	std::unordered_map<uint64_t, PairStats> pairs;
	// Keys that missed, with the frame of their latest miss. A record pays off only
	// when its key comes again, so it is stored at the second sighting within the
	// frames a record outlives unused: keys new every frame (user data pointing at
	// per-frame allocations) are never stored, even in pairs whose other objects are.
	struct SeenKey {
		uint64_t hash = 0, frame = 0;
	};
	std::vector<SeenKey> seen = std::vector<SeenKey>(8192);
	bool SeenAgain(uint64_t hash, uint64_t frame) {
		hash             = IndexHash(hash);
		const auto mask  = seen.size() - 1;
		SeenKey*   empty = nullptr;
		for (uint64_t i = 0, at = hash & mask; i < 8; ++i, at = (at + 1) & mask) {
			auto& entry = seen[at];
			if (entry.hash == hash) {
				const bool again = entry.frame + 2 >= frame;
				entry.frame      = frame;
				return again;
			}
			if (empty == nullptr && (entry.hash == 0 || entry.frame + 2 < frame)) empty = &entry;
		}
		*(empty != nullptr ? empty : &seen[hash & mask]) = {hash, frame};
		return false;
	}
	uint64_t                                swept_frame = UINT64_MAX;
	// Draw-state key of the latest draw (native or normal), for clean draws.
	uint64_t state_key       = 0;
	bool     state_key_valid = false;
	uint64_t last_hash       = 0; // key hash of the previous native draw's record
	uint64_t last2_hash      = 0; // ... and of the one before it
	uint64_t                  last_state_key = 0; // the latest states lookup (entries never move)
	const NativeXprDrawState* last_state     = nullptr;
	uint64_t collected_frame = UINT64_MAX;
	~NativeXprCache() {
		for (auto pool: pools) device.destroyDescriptorPool(pool);
	}
};

namespace {
uint64_t NativeXprPairKey(const std::vector<uint32_t>& key) {
	return key.size() < 4 ? 0 : XXH3_64bits(key.data(), 4 * sizeof(uint32_t));
}
} // namespace

RenderExecutor::NativeXprCache& RenderExecutor::NativeXprState() {
	if (!m_native_xpr) {
		m_native_xpr         = std::make_shared<NativeXprCache>();
		m_native_xpr->device = m_context.GetGraphics().device;
	}
	return *m_native_xpr;
}

void RenderExecutor::NativeXprRequestStore() {
	m_native_xpr_store = true;
}

std::vector<std::pair<uint64_t, uint64_t>>* RenderExecutor::NativeXprReadLog() {
	if (!m_native_xpr_store) return nullptr;
	auto& cache   = NativeXprState();
	cache.logging = true;
	cache.reads.clear();
	return &cache.reads;
}

void RenderExecutor::NativeXprEndPacket() {
	m_native_xpr_store  = false;
	m_native_xpr_verify = {};
	if (m_native_xpr) m_native_xpr->logging = false;
}

// Object identity: both shader addresses and every user-data SGPR (the XPR
// geometry root and whatever else the SRT chains start from).
void RenderExecutor::NativeXprKey(CommandBuffer& buffer, std::vector<uint32_t>& key) const {
	const auto& sh = buffer.GetShaders();
	const auto  es = sh.GetVs().es_regs.data_addr;
	const auto  vs = es != 0 ? es : sh.GetVs().gs_regs.data_addr;
	const auto  ps = sh.GetPs().ps_regs.data_addr;
	key.clear();
	key.push_back(static_cast<uint32_t>(vs));
	key.push_back(static_cast<uint32_t>(vs >> 32u));
	key.push_back(static_cast<uint32_t>(ps));
	key.push_back(static_cast<uint32_t>(ps >> 32u));
	for (const auto* sgprs: {&sh.GetVs().hs_user_sgpr, &sh.GetVs().gs_user_sgpr, &sh.GetPs().ps_user_sgpr}) {
		const auto count = std::min<uint32_t>(sgprs->count, HW::UserSgprInfo::SGPRS_MAX);
		key.push_back(count);
		key.insert(key.end(), sgprs->value, sgprs->value + count);
	}
}

// Draw-state identity: the whole graphics context, the user configuration and
// the graphics shader stage registers with the user-data layout (types and
// counts) but not the user-data values, which identify the object instead.
uint64_t RenderExecutor::NativeXprStateKey(CommandBuffer& buffer) const {
	const auto& ctx = buffer.GetRegisters();
	const auto& cfg = buffer.GetUserConfig();
	const auto& vs  = buffer.GetShaders().GetVs();
	const auto& ps  = buffer.GetShaders().GetPs();
	auto hash = XXH3_64bits(&ctx, sizeof(ctx));
	hash      = XXH3_64bits_withSeed(&cfg, sizeof(cfg), hash);
	hash      = XXH3_64bits_withSeed(&vs.es_regs, sizeof(vs.es_regs), hash);
	hash      = XXH3_64bits_withSeed(&vs.ls_regs, sizeof(vs.ls_regs), hash);
	hash      = XXH3_64bits_withSeed(&vs.hs_regs, sizeof(vs.hs_regs), hash);
	hash      = XXH3_64bits_withSeed(&vs.gs_regs, sizeof(vs.gs_regs), hash);
	hash      = XXH3_64bits_withSeed(&ps.ps_regs, sizeof(ps.ps_regs), hash);
	for (const auto* sgprs: {&vs.hs_user_sgpr, &vs.gs_user_sgpr, &ps.ps_user_sgpr}) {
		hash = XXH3_64bits_withSeed(sgprs->type, sizeof(sgprs->type), hash);
		hash = XXH3_64bits_withSeed(&sgprs->count, sizeof(sgprs->count), hash);
	}
	return hash;
}

vk::DescriptorSet RenderExecutor::NativeXprAllocateSet(vk::DescriptorSetLayout layout,
                                                       vk::DescriptorPool& pool_out) {
	auto&      cache  = NativeXprState();
	const auto device = cache.device;
	vk::DescriptorSetAllocateInfo allocate {};
	allocate.descriptorSetCount = 1;
	allocate.pSetLayouts        = &layout;
	vk::DescriptorSet set       = nullptr;
	// Freed sets return to their pool; try the newest pools first.
	for (size_t i = 0; i < cache.pools.size() && i < 4; ++i) {
		const auto pool         = cache.pools[cache.pools.size() - 1 - i];
		allocate.descriptorPool = pool;
		if (device.allocateDescriptorSets(&allocate, &set) == vk::Result::eSuccess) {
			pool_out = pool;
			return set;
		}
	}
	constexpr uint32_t Sets = 1024;
	const std::array<vk::DescriptorPoolSize, 5> sizes {{
	    {vk::DescriptorType::eStorageBuffer, Sets * 16},
	    {vk::DescriptorType::eStorageBufferDynamic, Sets * 4},
	    {vk::DescriptorType::eSampledImage, Sets * 32},
	    {vk::DescriptorType::eSampler, Sets * 8},
	    {vk::DescriptorType::eStorageImage, Sets * 4},
	}};
	vk::DescriptorPoolCreateInfo create {};
	create.flags         = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
	create.maxSets       = Sets;
	create.poolSizeCount = static_cast<uint32_t>(sizes.size());
	create.pPoolSizes    = sizes.data();
	vk::DescriptorPool pool = nullptr;
	EXIT_IF(device.createDescriptorPool(&create, nullptr, &pool) != vk::Result::eSuccess);
	cache.pools.push_back(pool);
	allocate.descriptorPool = pool;
	EXIT_IF(device.allocateDescriptorSets(&allocate, &set) != vk::Result::eSuccess);
	pool_out = pool;
	return set;
}

// A descriptor set leaves service; it may be in flight until the current tick.
void RenderExecutor::NativeXprRetireSet(vk::DescriptorPool pool, vk::DescriptorSet set) {
	if (!set) return;
	NativeXprState().retired.push_back({pool, set, m_context.GetCommandScheduler().CurrentTick()});
}

void RenderExecutor::NativeXprDropVariants(NativeXprRecord& record) {
	for (auto& variant: record.variants) NativeXprRetireSet(variant.pool, variant.set);
	record.variants.clear();
}

// A record leaves service; its reads stay for learning when the key is stored again.
void RenderExecutor::NativeXprRetire(std::unique_ptr<NativeXprRecord> record, bool keep_for_learning) {
	auto& cache = NativeXprState();
	NativeXprRetireSet(record->pool, record->set);
	record->set = nullptr;
	NativeXprDropVariants(*record);
	if (!keep_for_learning) return;
	const auto hash = XXH3_64bits(record->key.data(), record->key.size() * sizeof(uint32_t));
	if (cache.stale.size() >= 65536) cache.stale.clear();
	cache.stale[hash] = std::move(record);
}

// Once per frame: free the retired sets the GPU is done with, one call per pool
// (each call drains the recording queue).
void RenderExecutor::NativeXprCollect() {
	if (!m_native_xpr) return;
	auto&      cache = *m_native_xpr;
	const auto frame = m_context.FrameNumber();
	if (frame - cache.swept_frame >= 8) {
		// Every eighth frame, records no draw used for two frames leave (their
		// objects are gone or their keys changed); a pair whose records keep
		// leaving unused stops storing.
		cache.swept_frame = frame;
		for (auto it = cache.records.begin(); it != cache.records.end();) {
			auto& record = *it->second;
			if (record.used_frame + 2 >= frame) {
				++it;
				continue;
			}
			auto& pair = cache.pairs[NativeXprPairKey(record.key)];
			if (record.uses == 0 && ++pair.wasted > 32 && pair.wasted * 10 > pair.stored * 9 && !pair.blocked) {
				pair.blocked = true;
			}
			cache.IndexErase(it->first);
			NativeXprRetire(std::move(it->second), false);
			it = cache.records.erase(it);
		}
	}
	if (cache.collected_frame == frame || cache.retired.empty()) return;
	cache.collected_frame = frame;
	auto& scheduler       = m_context.GetCommandScheduler();
	std::vector<NativeXprCache::RetiredSet> done;
	std::erase_if(cache.retired, [&](const NativeXprCache::RetiredSet& item) {
		if (!scheduler.IsFree(item.tick)) return false;
		done.push_back(item);
		return true;
	});
	std::ranges::sort(done, {}, [](const auto& item) { return static_cast<VkDescriptorPool>(item.pool); });
	std::vector<vk::DescriptorSet> batch;
	for (size_t first = 0; first < done.size();) {
		size_t last = first;
		batch.clear();
		while (last < done.size() && done[last].pool == done[first].pool) batch.push_back(done[last++].set);
		(void)cache.device.freeDescriptorSets(done[first].pool, static_cast<uint32_t>(batch.size()), batch.data());
		first = last;
	}
}

// A fresh record replaced a stale one whose logged reads were at the same
// addresses. Words whose change moved only the flattened SRT, each to a unique
// position carrying its new value, are learned as patchable constant data.
void RenderExecutor::NativeXprLearn(const NativeXprRecord& stale, NativeXprRecord& fresh) {
	fresh.word_patch.assign(fresh.words.size(), -1);
	if (stale.reads != fresh.reads || stale.stage_count != fresh.stage_count ||
	    stale.vertex_program != fresh.vertex_program || stale.pixel_program != fresh.pixel_program)
		return;
	// Everything but the flattened constants and the T# LOD-statistics fields (and
	// the shader-data words derived from them) must be unchanged.
	for (uint32_t st = 0; st < fresh.stage_count; ++st) {
		const auto& a = stale.stages[st];
		const auto& b = fresh.stages[st];
		if (!(a.snapshot.buffers == b.snapshot.buffers) || !(a.snapshot.samplers == b.snapshot.samplers) ||
		    a.snapshot.user_data != b.snapshot.user_data || !(a.snapshot.uniform_fill == b.snapshot.uniform_fill) ||
		    a.flattened.size() != b.flattened.size() ||
		    !NativeXprSameImagesButLodStats(a.snapshot.images, b.snapshot.images) ||
		    a.shader_data.size() != b.shader_data.size()) {
			return;
		}
		const auto& layout = b.program->bindings;
		for (uint32_t i = 0; i < a.shader_data.size(); ++i) {
			const bool lod = i >= layout.LodStatsDword() && i < layout.LodStatsDword() + layout.lod_stats_count;
			if (!lod && a.shader_data[i] != b.shader_data[i]) return;
		}
	}
	// What moved: flattened positions and T# LOD dwords, each to be explained by
	// exactly one changed word carrying its new value.
	std::vector<int32_t> positions;
	for (uint32_t st = 0; st < fresh.stage_count; ++st) {
		const auto& a = stale.stages[st];
		const auto& b = fresh.stages[st];
		for (size_t j = 0; j < a.flattened.size(); ++j)
			if (a.flattened[j] != b.flattened[j]) positions.push_back(static_cast<int32_t>(st << 24u | j));
		for (uint32_t i = 0; i < b.snapshot.images.size(); ++i)
			for (uint32_t w = 0; w < b.snapshot.images[i].dwords.size(); ++w)
				if (a.snapshot.images[i].dwords[w] != b.snapshot.images[i].dwords[w])
					positions.push_back(NativeXprImagePatch(st, i, w));
	}
	const auto value_at = [&](int32_t position) {
		if (position >= 0)
			return fresh.stages[static_cast<uint32_t>(position) >> 24u].flattened[static_cast<uint32_t>(position) & 0xffffffu];
		const auto code = static_cast<uint32_t>(NativeXprImagePatchBase - position);
		return fresh.stages[code >> 20u].snapshot.images[(code >> 4u) & 0xffffu].dwords[code & 0xfu];
	};
	// A changed word is patchable when exactly one moved position took its new
	// value, and ignorable (NativeXprIgnore) when nothing moved with it: flattened
	// entries and descriptor dwords are whole-word copies, so a word that reached
	// any of them would have changed it.
	std::vector<uint8_t> explained(positions.size(), 0);
	bool                 ambiguous = false;
	for (size_t k = 0; k < fresh.words.size(); ++k) {
		if (stale.word_patch.size() == fresh.words.size() && stale.word_patch[k] != -1 &&
		    stale.words[k] == fresh.words[k]) {
			fresh.word_patch[k] = stale.word_patch[k];
			continue;
		}
		if (stale.words[k] == fresh.words[k]) continue;
		int32_t match   = -1;
		size_t  matches = 0;
		for (size_t p = 0; p < positions.size(); ++p) {
			if (value_at(positions[p]) == fresh.words[k]) {
				match        = positions[p];
				explained[p] = 1;
				++matches;
			}
		}
		if (matches == 1) fresh.word_patch[k] = match;
		else if (matches == 0) fresh.word_patch[k] = NativeXprIgnore;
		else ambiguous = true;
	}
	// A change no word explains means the data is not a plain copy.
	if (ambiguous || std::ranges::find(explained, uint8_t {0}) != explained.end()) {
		fresh.word_patch.assign(fresh.words.size(), -1);
	}
}

// Resolved bindings -> record: the stage data, the runtime proof dependencies and
// the descriptor set (kept when an identical payload is already written).
// Returns a reject reason, or nullptr.
const char* RenderExecutor::NativeXprBindResources(NativeXprRecord& record,
                                                   std::span<PreparedBindings* const> stages,
                                                   vk::DescriptorSetLayout layout, bool runtime) {
	auto& buffers   = m_context.GetBufferCache();
	auto& textures  = m_context.GetTextureCache();
	auto& resources = m_context.GetGpuResources();
	for (const auto* prepared: stages) {
		const auto& program = *prepared->runtime->program;
		if (program.info.uses_dma || prepared->gds.buffer != nullptr) return "dma_gds";
		for (const auto& resource: program.info.buffers)
			if (resource.written || resource.atomic) return "written_buffer";
		for (const auto& image: program.info.images)
			if (image.written || image.atomic) return "written_image";
		for (const auto& image: prepared->images)
			if (image.desc.type == TextureCache::BindingType::Storage) return "storage_image";
	}
	std::vector<NativeXprRecord::BufferDep> buffer_deps;
	std::vector<NativeXprRecord::ImageDep>  image_deps;
	const auto                              start_epoch = textures.StartEpoch();
	if (runtime) {
		for (uint32_t st = 0; st < stages.size(); ++st) {
			auto*       prepared = stages[st];
			const auto& program  = *prepared->runtime->program;
			for (size_t i = 0; i < program.info.buffers.size(); ++i) {
				const auto& source = prepared->buffer_sources[i];
				if (source.address == 0 || source.size == 0) continue;
				// Served by the upload ring or pending CPU changes: not a fixed descriptor.
				const auto* owner = source.id ? buffers.GetBufferIfLive(source.id) : nullptr;
				bool        cpu   = buffers.IsRegionCpuModified(source.address, source.size);
				if (owner != nullptr && cpu && !buffers.IsRegionGpuModified(source.address, source.size)) {
					// The normal path streams small ranges of CPU-dirty pages through the
					// upload ring on every draw and never cleans the owner. Clean it once:
					// a later CPU write faults and the record's proof notices.
					buffers.EnsureBufferContents(source.address, source.size);
					const auto [resolved, offset] = buffers.ObtainBuffer(
					    source.address, source.size, false, program.info.buffers[i].formatted, source.id);
					cpu = buffers.IsRegionCpuModified(source.address, source.size);
					if (resolved == owner && !cpu) {
						const auto alignment  = m_context.GetGraphics().StorageMinAlignment();
						const auto aligned    = offset - offset % alignment;
						const auto adjustment = offset - aligned;
						if (adjustment < 256) {
							prepared->buffers[i] = {resolved->Handle(), aligned, source.size + adjustment};
							auto& word = prepared->shader_data[program.bindings.memory_offset_dword + i / 4u];
							const auto shift = (i % 4u) * 8u;
							word = (word & ~(0xffu << shift)) | (static_cast<uint32_t>(adjustment) << shift);
						}
					}
				}
				if (owner == nullptr || owner->Handle() != prepared->buffers[i].buffer || cpu) {
					return "dynamic_buffer";
				}
				// ObtainBuffer refreshes a texel buffer from a GPU-owned image that starts
				// exactly at its address (SynchronizeBufferFromImage); the record cannot.
				// Any image starting there rejects it.
				const bool texel = program.info.buffers[i].formatted;
				if (texel && textures.HasImageStartingAt(source.address)) {
					return "texel_image";
				}
				buffer_deps.push_back({source.address, source.size, source.id, prepared->buffers[i].buffer, texel, st,
				                       static_cast<uint32_t>(i)});
			}
			for (const auto& image: prepared->images) {
				if (image.layout == vk::ImageLayout::eGeneral) {
					return "general_image";
				}
				if (image.desc.info.data.Empty()) continue;
				if (!textures.IsSampledImageCurrent(image.image_id, image.desc)) return "image_not_current";
				const auto& view = image.desc.view_info;
				image_deps.push_back({image.image_id, image.layout,
				                      {view.base_level, view.level_count, view.base_layer, view.layer_count},
				                      textures.m_slot_images[image.image_id].serial,
				                      textures.m_slot_images[image.image_id].registrations, image.desc});
			}
		}
	}

	// Stage data and the descriptor payload; pointers once the vectors stop growing.
	struct Pending {
		vk::WriteDescriptorSet write;
		size_t                 start = 0;
		bool                   image = false;
	};
	std::vector<Pending>                  pending;
	std::vector<vk::DescriptorBufferInfo> buffer_infos;
	std::vector<vk::DescriptorImageInfo>  image_infos;
	using Kind = ShaderRecompiler::IR::DescriptorBindingKind;
	const auto fixed_view = [](const Buffer* value) -> vk::DescriptorBufferInfo {
		EXIT_IF(value == nullptr);
		return {value->Handle(), 0, value->Size()};
	};
	const auto upload = buffers.GetShaderUploadBuffer().Handle();
	std::array<NativeXprRecord::Stage, 2> data;
	std::array<std::vector<std::vector<std::pair<uint32_t, uint32_t>>>, 2> positions;
	std::array<std::vector<std::vector<uint32_t>>, 2>                     buffer_positions;
	vk::ShaderStageFlags push_stages = vk::ShaderStageFlagBits::eFragment;
	bool                 has_push    = false;
	for (uint32_t index = 0; index < stages.size(); ++index) {
		const auto& prepared = *stages[index];
		const auto& program  = *prepared.runtime->program;
		auto&       stage    = data[index];
		stage.program        = &program;
		stage.flattened      = prepared.runtime->resources.flattened_srt;
		stage.shader_data    = prepared.shader_data;
		stage.snapshot       = prepared.runtime->resources;
		EXIT_IF(stage.shader_data.size() != program.bindings.ShaderDataDwords());
		if (program.bindings.UsesPushData()) {
			stage.push_start = program.bindings.push_data_start_dword;
			has_push         = true;
		}
		push_stages |= program.stage == ShaderType::Pixel  ? vk::ShaderStageFlagBits::eFragment
		               : program.stage == ShaderType::Mesh ? vk::ShaderStageFlagBits::eMeshEXT
		                                                   : vk::ShaderStageFlagBits::eVertex;
		std::vector<uint32_t> occurrences(prepared.images.size(), 0);
		positions[index].assign(prepared.images.size(), {});
		buffer_positions[index].assign(prepared.buffers.size(), {});
		for (const auto& binding: program.bindings.descriptors) {
			Pending item;
			item.write.dstBinding      = ShaderRecompiler::IR::NativeBinding(program.stage, binding.kind);
			item.write.descriptorType  = NativeDescriptorType(binding.kind);
			item.write.descriptorCount = NativeDescriptorCount(binding);
			item.image = ShaderRecompiler::IR::ImageBindingResourceClass(binding.kind) !=
			                 ShaderRecompiler::IR::ImageResourceClass::None ||
			             binding.kind == Kind::Samplers;
			item.start = item.image ? image_infos.size() : buffer_infos.size();
			if (binding.kind == Kind::Samplers) {
				for (const auto resource: binding.resources) {
					const auto sampler = prepared.samplers.at(resource);
					EXIT_IF(sampler == nullptr);
					image_infos.emplace_back(sampler, nullptr, vk::ImageLayout::eUndefined);
				}
			} else if (item.image) {
				for (const auto resource: binding.resources) {
					const auto element = occurrences.at(resource)++;
					positions[index].at(resource).emplace_back(static_cast<uint32_t>(image_infos.size()), element);
					image_infos.push_back(MakeImageInfo(prepared.images.at(resource), element));
				}
			} else {
				switch (binding.kind) {
					case Kind::Buffers:
						for (const auto resource: binding.resources) {
							const auto& view = prepared.buffers.at(resource);
							EXIT_IF(view.buffer == nullptr);
							buffer_positions[index].at(resource).push_back(static_cast<uint32_t>(buffer_infos.size()));
							buffer_infos.push_back(view);
						}
						break;
					case Kind::LodStats: buffer_infos.push_back(fixed_view(buffers.GetLodStatsBuffer())); break;
					case Kind::BdaPagetable: buffer_infos.push_back(fixed_view(buffers.GetBdaPageTableBuffer())); break;
					case Kind::FaultBuffer: buffer_infos.push_back(fixed_view(buffers.GetFaultBuffer())); break;
					case Kind::FlattenedSrt:
					case Kind::ShaderData: {
						const auto& values = binding.kind == Kind::FlattenedSrt ? stage.flattened : stage.shader_data;
						if (values.empty()) return "empty_dynamic";
						(binding.kind == Kind::FlattenedSrt ? stage.srt_binding : stage.data_binding) = true;
						item.write.descriptorType = vk::DescriptorType::eStorageBufferDynamic;
						buffer_infos.emplace_back(upload, 0, values.size() * sizeof(uint32_t));
						break;
					}
					default: return "binding_kind";
				}
			}
			EXIT_IF((item.image ? image_infos.size() : buffer_infos.size()) - item.start != item.write.descriptorCount);
			pending.push_back(item);
		}
	}

	// Accepted: publish into the record.
	for (uint32_t index = 0; index < stages.size(); ++index) {
		auto& stage       = record.stages[index];
		stage.views.clear();
		for (const auto& image: stages[index]->images) stage.views.push_back(image.image_view);
		stage.samplers        = stages[index]->samplers;
		stage.textures         = stages[index]->images;
		stage.image_positions  = std::move(positions[index]);
		stage.buffer_positions = std::move(buffer_positions[index]);
		stage.program     = data[index].program;
		stage.flattened   = std::move(data[index].flattened);
		stage.shader_data = std::move(data[index].shader_data);
		stage.snapshot    = std::move(data[index].snapshot);
		stage.srt_binding  = data[index].srt_binding;
		stage.data_binding = data[index].data_binding;
		stage.push_start   = data[index].push_start;
	}
	record.stage_count = static_cast<uint32_t>(stages.size());
	record.push_stages = push_stages;
	record.has_push    = has_push;
	record.upload      = upload;
	// A new image object can get a destroyed view's handle: an identical payload
	// keeps the set only over the same image objects.
	const bool same_images = !runtime || std::ranges::equal(record.images, image_deps, [](const auto& a, const auto& b) {
		return a.id == b.id && a.serial == b.serial;
	});
	if (runtime) {
		record.buffers            = std::move(buffer_deps);
		record.images             = std::move(image_deps);
		record.IndexImages();
		record.mapping_epoch      = resources.MappingEpoch();
		record.registration_epoch = buffers.RegistrationEpoch();
		record.resolution_epoch   = textures.ResolutionEpoch();
		record.start_epoch        = start_epoch;
		uint64_t cpu_epoch        = 0;
		for (const auto& dep: record.buffers) {
			const auto epoch = buffers.CpuModificationEpoch(dep.address, dep.size);
			if (epoch == 0) {
				cpu_epoch = 0;
				break;
			}
			cpu_epoch += epoch;
		}
		record.cpu_epoch       = cpu_epoch;
		record.validated_frame = m_context.FrameNumber();
	}
	const auto same_payload = [](const auto& a, const auto& b) {
		return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0;
	};
	{
		std::array<const ShaderRecompiler::IR::ResourceSnapshot*, 2> snapshots {};
		for (uint32_t st = 0; st < record.stage_count; ++st) snapshots[st] = &record.stages[st].snapshot;
		record.image_signature = NativeXprImageSignature({snapshots.data(), record.stage_count});
	}
	if (record.set && same_images && same_payload(record.set_buffers, buffer_infos) &&
	    same_payload(record.set_images, image_infos))
		return nullptr;
	NativeXprDropVariants(record);
	NativeXprRetireSet(record.pool, record.set);
	record.set = NativeXprAllocateSet(layout, record.pool);
	std::vector<vk::WriteDescriptorSet> writes;
	writes.reserve(pending.size());
	for (auto& item: pending) {
		item.write.dstSet = record.set;
		if (item.image) item.write.pImageInfo = image_infos.data() + item.start;
		else item.write.pBufferInfo = buffer_infos.data() + item.start;
		writes.push_back(item.write);
	}
	if (!writes.empty())
		m_context.GetGraphics().device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0,
		                                                    nullptr);
	record.set_buffers = std::move(buffer_infos);
	record.set_images  = std::move(image_infos);
	record.set_writes.clear();
	for (const auto& item: pending)
		record.set_writes.push_back({item.write.dstBinding, item.write.descriptorCount, static_cast<uint32_t>(item.start),
		                             item.write.descriptorType, item.image});
	return nullptr;
}

// The record's images changed back to those of an earlier set: bind that set
// again if its images are all still current (as a record proof holds them) and
// CPU-clean; the current set becomes the variant.
bool RenderExecutor::NativeXprSwitchVariant(NativeXprRecord& record, uint64_t signature) {
	const auto found = std::ranges::find(record.variants, signature, &NativeXprRecord::Variant::signature);
	if (found == record.variants.end()) return false;
	auto& textures = m_context.GetTextureCache();
	for (const auto& dep: found->images) {
		const auto* image = textures.m_slot_images.try_get(dep.id);
		if (image == nullptr || image->serial != dep.serial || image->IsCpuDirty() ||
		    !textures.IsSampledImageCurrent(dep.id, dep.desc)) {
			NativeXprRetireSet(found->pool, found->set);
			record.variants.erase(found);
			return false;
		}
	}
	for (const auto& dep: found->images) (void)textures.GetImage(dep.id); // LRU, as FindTexture does
	auto& variant = *found;
	std::swap(variant.signature, record.image_signature);
	std::swap(variant.set, record.set);
	std::swap(variant.pool, record.pool);
	std::swap(variant.set_images, record.set_images);
	std::swap(variant.images, record.images);
	record.IndexImages();
	for (uint32_t st = 0; st < record.stage_count; ++st) {
		std::swap(variant.textures[st], record.stages[st].textures);
		std::swap(variant.views[st], record.stages[st].views);
	}
	record.resolution_epoch = textures.ResolutionEpoch();
	return true;
}

// Only image or buffer base addresses changed: resolve those slots again (the
// texture resolve cache makes a repeated image cheap), replace their
// descriptors in the stored payload and bind a new set. False when a slot does
// not resolve to what a record may hold; the caller then resolves everything.
bool RenderExecutor::NativeXprRebindSlots(NativeXprRecord& record,
                                          std::span<const std::pair<uint32_t, uint32_t>> images,
                                          std::span<const std::pair<uint32_t, uint32_t>> buffer_slots) {
	if (record.pipelines.empty() || record.set_writes.empty()) return false;
	auto& textures = m_context.GetTextureCache();
	auto& buffers  = m_context.GetBufferCache();
	std::array<const ShaderRecompiler::IR::ResourceSnapshot*, 2> snapshots {};
	for (uint32_t st = 0; st < record.stage_count; ++st) snapshots[st] = &record.stages[st].snapshot;
	const auto signature = NativeXprImageSignature({snapshots.data(), record.stage_count});
	if (buffer_slots.empty() && NativeXprSwitchVariant(record, signature)) return true;
	// All or nothing: the full rebind keeps the set when its payload equals the
	// stored one, so the stored payload must never run ahead of the set.
	auto& resolved = m_native_xpr->rebind_textures;
	resolved.clear();
	for (const auto& [st, index]: images) {
		auto& stage = record.stages[st];
		if (index >= stage.textures.size() || index >= stage.image_positions.size() ||
		    index >= stage.program->info.images.size())
			return false;
		const auto& resource = stage.program->info.images[index];
		if (resource.mip_mode == ShaderRecompiler::IR::ImageMipMode::DynamicStorage) return false;
		auto binding = ResolveTexture(resource, stage.snapshot.images[index]);
		if (binding.desc.type != TextureCache::BindingType::Texture || binding.desc.info.data.Empty()) return false;
		auto* image = textures.m_slot_images.try_get(binding.image_id);
		if (image == nullptr || image->binding.is_target || image->binding.force_general) return false;
		binding.image_view = textures.FindTexture(binding.image_id, binding.desc);
		binding.mip_views.clear();
		auto& owner = textures.GetImage(binding.image_id);
		owner.usage.texture = true;
		binding.layout = owner.info.IsDepth() ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
		                                      : vk::ImageLayout::eShaderReadOnlyOptimal;
		if (!textures.IsSampledImageCurrent(binding.image_id, binding.desc)) return false;
		resolved.push_back(std::move(binding));
	}
	// Buffers as FindBuffers/RebindBuffers resolve them, held as a store holds
	// them: a live owner, CPU-clean (cleaned here as the store cleans it), no
	// image starting at a texel range.
	struct BufferSlot {
		vk::DescriptorBufferInfo  info;
		uint32_t                  adjustment = 0;
		NativeXprRecord::BufferDep dep;
	};
	std::array<BufferSlot, 16> fresh_buffers;
	if (buffer_slots.size() > fresh_buffers.size()) return false;
	const auto  alignment = m_context.GetGraphics().StorageMinAlignment();
	const auto  max_range = m_context.GetGraphics().GetPhysicalDeviceProperties().limits.maxStorageBufferRange;
	for (size_t i = 0; i < buffer_slots.size(); ++i) {
		const auto [st, index] = buffer_slots[i];
		auto& stage            = record.stages[st];
		if (index >= stage.program->info.buffers.size() || index >= stage.buffer_positions.size() ||
		    index >= stage.snapshot.buffers.size())
			return false;
		const auto& resource = stage.program->info.buffers[index];
		if (resource.written || resource.atomic) return false;
		const auto descriptor = DecodeNativeDescriptor<ShaderBufferResource>(stage.snapshot.buffers[index]);
		const auto address    = descriptor.Base48();
		const auto stride     = descriptor.Stride();
		const auto records    = descriptor.NumRecords();
		const auto requested  = stride != 0 ? static_cast<uint64_t>(stride) * records : records;
		if (address == 0 || requested == 0) return false;
		const auto size  = Libs::LibKernel::Memory::ClampRangeSize(address, requested);
		if (size == 0) return false;
		const auto id    = buffers.FindBuffer(address, size);
		const auto* owner = buffers.GetBufferIfLive(id);
		if (owner == nullptr) return false;
		if (buffers.IsRegionCpuModified(address, size)) {
			if (buffers.IsRegionGpuModified(address, size)) return false;
			buffers.EnsureBufferContents(address, size);
		}
		const auto [served, offset] = buffers.ObtainBuffer(address, size, false, resource.formatted, id);
		if (served != owner || buffers.IsRegionCpuModified(address, size)) return false;
		if (resource.formatted && textures.HasImageStartingAt(address)) return false;
		const auto aligned    = offset - offset % alignment;
		const auto adjustment = offset - aligned;
		if (adjustment >= 256 || size > max_range - adjustment) return false;
		fresh_buffers[i] = {{owner->Handle(), aligned, size + adjustment},
		                    static_cast<uint32_t>(adjustment),
		                    {address, size, id, owner->Handle(), resource.formatted, st, index}};
	}
	// The set's other images are written again as they are: their objects (and
	// so their views) must still be the ones resolved. Otherwise resolve everything.
	for (const auto& dep: record.images) {
		const auto* image = textures.m_slot_images.try_get(dep.id);
		if (image == nullptr || !image->registered || image->serial != dep.serial) return false;
	}
	if (buffer_slots.empty()) {
		// What the current set holds, kept with it as a variant below.
		auto& cache            = *m_native_xpr;
		cache.previous_images  = record.set_images;
		cache.previous_deps    = record.images;
		for (uint32_t st = 0; st < record.stage_count; ++st) {
			cache.previous_textures[st] = record.stages[st].textures;
			cache.previous_views[st]    = record.stages[st].views;
		}
	}
	for (size_t i = 0; i < images.size(); ++i) {
		const auto [st, index] = images[i];
		auto& stage            = record.stages[st];
		for (const auto& [position, element]: stage.image_positions[index])
			record.set_images[position] = MakeImageInfo(resolved[i], element);
		stage.views[index]    = resolved[i].image_view;
		stage.textures[index] = std::move(resolved[i]);
	}
	for (size_t i = 0; i < buffer_slots.size(); ++i) {
		const auto [st, index] = buffer_slots[i];
		auto&       stage      = record.stages[st];
		const auto& update     = fresh_buffers[i];
		for (const auto position: stage.buffer_positions[index]) record.set_buffers[position] = update.info;
		auto&      word  = stage.shader_data[stage.program->bindings.memory_offset_dword + index / 4u];
		const auto shift = (index % 4u) * 8u;
		word             = (word & ~(0xffu << shift)) | (update.adjustment << shift);
		const auto dep   = std::ranges::find_if(record.buffers, [&](const NativeXprRecord::BufferDep& item) {
            return item.stage == st && item.slot == index;
        });
		if (dep != record.buffers.end()) *dep = update.dep;
		else record.buffers.push_back(update.dep);
	}
	if (!images.empty()) {
		// Proof dependencies follow the resolved images.
		record.images.clear();
		for (uint32_t st = 0; st < record.stage_count; ++st)
			for (const auto& image: record.stages[st].textures) {
				if (image.desc.info.data.Empty()) continue;
				const auto& view = image.desc.view_info;
				record.images.push_back({image.image_id, image.layout,
				                         {view.base_level, view.level_count, view.base_layer, view.layer_count},
				                         textures.m_slot_images[image.image_id].serial,
				                         textures.m_slot_images[image.image_id].registrations, image.desc});
			}
		record.IndexImages();
		record.resolution_epoch = textures.ResolutionEpoch();
	}
	// New buffer dependencies are proven again before the draw.
	if (!buffer_slots.empty()) record.validated_frame = 0;
	if (buffer_slots.empty() && record.set) {
		// The old set differs only in images: keep it for a switch back.
		if (record.variants.size() >= 3) {
			NativeXprRetireSet(record.variants.front().pool, record.variants.front().set);
			record.variants.erase(record.variants.begin());
		}
		auto& kept      = record.variants.emplace_back();
		kept.signature  = record.image_signature;
		kept.set        = record.set;
		kept.pool       = record.pool;
		kept.set_images = m_native_xpr->previous_images;
		kept.images     = std::move(m_native_xpr->previous_deps);
		for (uint32_t st = 0; st < record.stage_count; ++st) {
			kept.textures[st] = std::move(m_native_xpr->previous_textures[st]);
			kept.views[st]    = std::move(m_native_xpr->previous_views[st]);
		}
	} else {
		NativeXprDropVariants(record);
		NativeXprRetireSet(record.pool, record.set);
	}
	record.image_signature = signature;
	record.set = NativeXprAllocateSet(record.pipelines.front().second->descriptor_set_layout, record.pool);
	std::vector<vk::WriteDescriptorSet> writes;
	writes.reserve(record.set_writes.size());
	for (const auto& slot: record.set_writes) {
		vk::WriteDescriptorSet write {};
		write.dstSet          = record.set;
		write.dstBinding      = slot.binding;
		write.descriptorCount = slot.count;
		write.descriptorType  = slot.type;
		if (slot.image) write.pImageInfo = record.set_images.data() + slot.start;
		else write.pBufferInfo = record.set_buffers.data() + slot.start;
		writes.push_back(write);
	}
	m_context.GetGraphics().device.updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0,
	                                                    nullptr);
	return true;
}

// A record whose descriptors changed: resolve every slot again the way the
// normal path's preparation does, with plain sampled-image layouts, and bind.
bool RenderExecutor::NativeXprRebind(NativeXprRecord&                                        record,
                                     std::array<ShaderRecompiler::IR::ResourceSnapshot, 2>& fresh) {
	if (record.pipelines.empty()) return false;
	std::array<ShaderStageRuntime, 2> runtimes;
	std::array<PreparedBindings, 2>   prepared;
	std::array<PreparedBindings*, 2>  stages {};
	auto&                             textures = m_context.GetTextureCache();
	bool                              ok       = true;
	for (uint32_t st = 0; st < record.stage_count && ok; ++st) {
		runtimes[st].program   = record.stages[st].program;
		runtimes[st].resources = std::move(fresh[st]);
		PrepareBindingsInto(runtimes[st], prepared[st]);
		stages[st] = &prepared[st];
	}
	for (uint32_t st = 0; st < record.stage_count && ok; ++st) FindBuffers(prepared[st]);
	for (uint32_t st = 0; st < record.stage_count && ok; ++st) RebindBuffers(prepared[st]);
	for (uint32_t st = 0; st < record.stage_count && ok; ++st) RebindImages(prepared[st]);
	for (uint32_t st = 0; st < record.stage_count && ok; ++st)
		for (auto& binding: prepared[st].images) {
			auto& image = textures.GetImage(binding.image_id);
			if (image.info.data.Empty() || binding.desc.type == TextureCache::BindingType::Storage ||
			    image.binding.is_target || image.binding.force_general) {
				ok = false;
				break;
			}
			binding.layout = image.info.IsDepth() ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
			                                      : vk::ImageLayout::eShaderReadOnlyOptimal;
		}
	ResetBindings();
	return ok && NativeXprBindResources(record, {stages.data(), record.stage_count},
	                                    record.pipelines.front().second->descriptor_set_layout,
	                                    true) == nullptr;
}

// Called by ExecutePreparedDraw after CommitBindings (image layouts are final)
// for a draw whose caller requested a store.
void RenderExecutor::NativeXprStore(CommandBuffer& buffer, const DrawRenderState& state,
                                    vk::PrimitiveTopology topology, bool primitive_restart_enable,
                                    const RenderState& rendering,
                                    std::span<PreparedBindings* const> stages, const DrawEmitInfo& emit) {
	LiveCounters::Add(LiveCounters::XprStores);
	auto&      cache   = NativeXprState();
	const bool runtime = cache.logging;
	// What the programs are (stage layout, DMA/GDS, written resources) never changes for
	// the shader pair: it stops asking for stores (every request logs the draw's reads).
	const auto refuse_program = [&] {
		LiveCounters::Add(LiveCounters::XprRefuseProgram);
		std::vector<uint32_t> key;
		NativeXprKey(buffer, key);
		auto& pair = cache.pairs[NativeXprPairKey(key)];
		pair.unstorable = true;
	};
	if (stages.empty() || stages.size() > 2 || (stages.size() == 2) != state.ps_active) return refuse_program();
	auto& textures = m_context.GetTextureCache();
	// Only stages without DMA, GDS or any written resource are recorded.
	for (const auto* prepared: stages) {
		const auto& program = *prepared->runtime->program;
		if (program.info.uses_dma || prepared->gds.buffer != nullptr) return refuse_program();
		for (const auto& resource: program.info.buffers)
			if (resource.written || resource.atomic) return refuse_program();
		for (const auto& image: program.info.images)
			if (image.written || image.atomic) return refuse_program();
		for (const auto& image: prepared->images)
			if (image.desc.type == TextureCache::BindingType::Storage) return refuse_program();
	}

	auto       record    = std::make_unique<NativeXprRecord>();
	const auto state_key = NativeXprStateKey(buffer);
	auto&      pipelines = m_context.GetPipelineCache();
	const auto colors    = std::span {state.color_info, state.color_count};
	const auto* pixel    = state.ps_active ? &state.ps_input_info : nullptr;
	auto* const variant  = kyty_local_async_xpr_pipelines_mode.load(std::memory_order_relaxed) != 0
	                          ? pipelines.TryCreateGraphicsPipeline(colors, state.depth_info, state.vs_input_info,
	                                                                buffer, pixel, topology, primitive_restart_enable,
	                                                                state.programs.vertex, state.programs.pixel, true)
	                          : &pipelines.CreateGraphicsPipeline(colors, state.depth_info, state.vs_input_info,
	                                                              buffer, pixel, topology, primitive_restart_enable,
	                                                              state.programs.vertex, state.programs.pixel, true);
	if (variant == nullptr) {
		LiveCounters::Add(LiveCounters::XprStorePending);
		return;
	}
	auto& pipeline = *variant;
	EXIT_IF(!pipeline.native_bindings || pipeline.uses_push_descriptors);
	record->vertex_program = state.programs.vertex.id;
	record->pixel_program  = state.ps_active ? state.programs.pixel.id : 0;
	record->pipelines.emplace_back(state_key, &pipeline);
	if (emit.state_offsets)
		record->direct_offsets.push_back({state_key, emit.vertex_offset, emit.first_instance});
	record->stage_count = static_cast<uint32_t>(stages.size());
	if (runtime) {
		uint64_t total_words = 0;
		for (const auto& [address, size]: cache.reads) {
			if (address % 4 != 0 || size % 4 != 0 || size == 0 || size > 4096)
				return LiveCounters::Add(LiveCounters::XprRefuseReads);
			total_words += size / 4;
		}
		if (total_words > 2048) return LiveCounters::Add(LiveCounters::XprRefuseReads);
		record->reads.reserve(cache.reads.size());
		record->words.reserve(total_words);
		for (const auto& [address, size]: cache.reads) {
			record->reads.emplace_back(address, static_cast<uint32_t>(size / 4));
			for (uint64_t offset = 0; offset < size; offset += 4)
				record->words.push_back(NativeXprReadGuestWord(address + offset));
		}
		// What RefreshShaders evaluated each stage's SRT with.
		const auto& sh  = buffer.GetShaders();
		const auto& ctx = buffer.GetRegisters();
		ShaderVertexInputInfo vertex_info;
		ResetNativeVertexInput(vertex_info);
		const auto vertex_params = PrepareProgram(sh.GetVs(), ctx, buffer.GetUserConfig(), vertex_info);
		record->stages[0].user_data.assign(vertex_params.user_data.begin(), vertex_params.user_data.end());
		record->stages[0].shader_base = vertex_params.Base();
		if (stages.size() == 2) {
			std::array<Prospero::ColorComponentMapping, RENDER_COLOR_ATTACHMENTS_MAX> mapping {};
			for (uint32_t i = 0; i < state.color_count; i++)
				mapping[state.color_info[i].target_slot] = state.color_info[i].export_mapping;
			ShaderPixelInputInfo pixel_info;
			ResetNativeStageInput(pixel_info);
			const auto pixel_params = PrepareProgram(sh.GetPs(), ctx.GetShaderRegisters(), mapping, pixel_info);
			record->stages[1].user_data.assign(pixel_params.user_data.begin(), pixel_params.user_data.end());
			record->stages[1].shader_base = pixel_params.Base();
		}
	}
	if (NativeXprBindResources(*record, stages, pipeline.descriptor_set_layout, runtime) != nullptr)
		return LiveCounters::Add(LiveCounters::XprRefuseBind);
	if (runtime) NativeXprRetrace(*record);

	// Render targets as this draw left them; a reopened rendering loads.
	NativeXprDrawState draw_state;
	draw_state.first_rendering = rendering;
	draw_state.rendering       = rendering;
	for (auto& attachment: draw_state.rendering.color_attachments) attachment.is_clear = false;
	draw_state.rendering.depth_stencil_attachment.depth_clear   = false;
	draw_state.rendering.depth_stencil_attachment.stencil_clear = false;
	const auto metadata_of = [](const TextureCache::ImageDesc& desc) {
		return desc.info.metadata.kind == ImageMetadataKind::None ? uint64_t {0}
		                                                          : desc.info.metadata.range.address;
	};
	for (uint32_t i = 0; i < state.color_count; ++i) {
		const auto& desc  = state.color_info[i].desc;
		const auto& view  = desc.view_info;
		const auto& image = textures.GetImage(state.color_info[i].image_id);
		draw_state.targets.push_back({state.color_info[i].image_id,
		                              rendering.color_attachments[i].image_layout,
		                              image.binding.attachment_access,
		                              {view.base_level, view.level_count, view.base_layer, view.layer_count},
		                              metadata_of(desc), view.base_layer, image.serial});
	}
	if (state.depth_info.image_id) {
		const auto& desc  = state.depth_info.desc;
		const auto& view  = desc.view_info;
		const auto& image = textures.GetImage(state.depth_info.image_id);
		draw_state.targets.push_back({state.depth_info.image_id,
		                              rendering.depth_stencil_attachment.image_layout,
		                              image.binding.attachment_access,
		                              {view.base_level, view.level_count, view.base_layer, view.layer_count},
		                              metadata_of(desc), view.base_layer, image.serial});
	}
	draw_state.meta_epoch = 0;
	draw_state.dynamic = LocalDrawRecording::Capture(
	    buffer, state.vs_input_info, state.color_info, state.color_count, state.depth_info,
	    m_context.GetGraphics().attachment_feedback_loop_enabled,
	    rendering.depth_stencil_attachment.image_layout == vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
	        ? vk::ImageAspectFlags {vk::ImageAspectFlagBits::eDepth}
	        : vk::ImageAspectFlags {});
	// Entries stay at their address (emission tracking compares pointers); a
	// replaced state must be emitted again.
	auto& slot = cache.states[state_key];
	if (!slot) slot = std::make_unique<NativeXprDrawState>();
	*slot = std::move(draw_state);
	if (cache.emit.state == slot.get()) cache.emit.state = nullptr;

	NativeXprKey(buffer, record->key);
	record->HoldKey();
	const auto hash    = XXH3_64bits(record->key.data(), record->key.size() * sizeof(uint32_t));
	record->used_frame = m_context.FrameNumber();
	auto& pair_stats   = cache.pairs[NativeXprPairKey(record->key)];
	++pair_stats.stored;
	if (auto old = cache.records.find(hash); old != cache.records.end()) {
		// Pipelines depend on the programs and the draw state only; direct-draw offsets also on
		// the user data, which the equal key holds.
		if (old->second->vertex_program == record->vertex_program &&
		    old->second->pixel_program == record->pixel_program) {
			for (const auto& entry: old->second->pipelines)
				if (entry.first != state_key) record->pipelines.push_back(entry);
			for (const auto& entry: old->second->direct_offsets)
				if (entry.state != state_key) record->direct_offsets.push_back(entry);
		}
		cache.IndexErase(hash);
		NativeXprRetire(std::move(old->second), false);
		cache.records.erase(old);
	}
	if (runtime) {
		if (auto stale = cache.stale.find(hash); stale != cache.stale.end()) {
			if (stale->second->key == record->key) {
				NativeXprLearn(*stale->second, *record);
				if (stale->second->vertex_program == record->vertex_program &&
				    stale->second->pixel_program == record->pixel_program) {
					for (const auto& entry: stale->second->pipelines)
						if (std::ranges::find(record->pipelines, entry.first,
						                      &std::pair<uint64_t, const PipelineCache::Pipeline*>::first) ==
						    record->pipelines.end())
							record->pipelines.push_back(entry);
					for (const auto& entry: stale->second->direct_offsets)
						if (std::ranges::find(record->direct_offsets, entry.state, &NativeXprRecord::DirectOffsets::state) ==
						    record->direct_offsets.end())
							record->direct_offsets.push_back(entry);
				}
			}
			cache.stale.erase(stale);
		}
		if (record->word_patch.size() != record->words.size())
			record->word_patch.assign(record->words.size(), -1);
	}
	if (cache.records.size() >= 65536) {
		for (auto& [key, value]: cache.records) NativeXprRetire(std::move(value), false);
		cache.records.clear();
		cache.index.assign(8192, {});
		cache.index_count = 0;
	}
	cache.IndexInsert(hash, record.get());
	cache.records[hash] = std::move(record);
	LiveCounters::Add(LiveCounters::XprStored);
}

// The way the SRT evaluation reads a range: spans through the watched-page aware
// span reader, single words otherwise.
void NativeXprReadWords(uint64_t address, uint32_t count, uint32_t* out) {
	for (uint32_t done = 0; done < count;) {
		const uint32_t chunk = std::min<uint32_t>(count - done, 16);
		const auto     at    = address + 4ull * done;
		if (chunk < 2 || !Libs::LibKernel::Memory::TryReadGpuShaderSpan(at, out + done, chunk * 4ull, false))
			for (uint32_t i = 0; i < chunk; ++i) out[done + i] = NativeXprReadGuestWord(at + 4ull * i);
		done += chunk;
	}
}

// Clean (specialization) reads go through the clean backing only; false when
// the bytes are GPU-owned and a fresh evaluation would fail too.
bool NativeXprReadClean(uint64_t address, uint32_t count, uint32_t* out) {
	for (uint32_t done = 0; done < count;) {
		const uint32_t chunk = std::min<uint32_t>(count - done, 16);
		const auto     at    = address + 4ull * done;
		if (chunk < 2 || !Libs::LibKernel::Memory::TryReadGpuShaderSpan(at, out + done, chunk * 4ull, true))
			for (uint32_t i = 0; i < chunk; ++i)
				if (!Libs::LibKernel::Memory::TryReadGpuCleanBackingToHost(at + 4ull * i, out + done + i, 4))
					return false;
		done += chunk;
	}
	return true;
}

// Traces every stage again and rebuilds the gather plan; not traced on failure.
void RenderExecutor::NativeXprRetrace(NativeXprRecord& record) {
	auto& gather  = record.gather;
	gather.traced = false;
	if (record.stages[0].shader_base == 0) return;
	struct StructuralRead {
		uint64_t address;
		uint32_t value;
		uint8_t  use;
		bool     operator<(const StructuralRead& other) const { return address < other.address; }
	};
	// Spans over the non-clean structural words and the data words.
	struct Word {
		uint64_t address;
		int32_t  value;
		int32_t  data;
	};
	// Scratch kept across calls (a retrace per stored record: tens to hundreds a frame
	// while walking, each with some twenty temporary vectors otherwise).
	thread_local struct {
		std::vector<StructuralRead>                structural, clean, unique;
		std::vector<std::pair<uint64_t, uint32_t>> feeds, data, index_of;
		std::vector<uint32_t>                      feed_value, cursor;
		std::vector<Word>                          words;
		ShaderRecompiler::IR::SrtReadTrace         trace;
	} scratch;
	auto& structural = scratch.structural;
	auto& clean      = scratch.clean;
	auto& feeds      = scratch.feeds; // (address, packed feed)
	auto& data       = scratch.data;  // (address, stage << 24 | flat)
	structural.clear();
	clean.clear();
	feeds.clear();
	data.clear();
	auto& pipelines   = m_context.GetPipelineCache();
	auto& untraceable = m_native_xpr->untraceable;
	for (uint32_t st = 0; st < record.stage_count; ++st)
		if (untraceable.contains(record.stages[st].program)) return;
	for (uint32_t st = 0; st < record.stage_count; ++st) {
		const auto& stage = record.stages[st];
		auto&       trace = scratch.trace;
		if (!pipelines.TraceStage(*stage.program, stage.user_data, stage.shader_base, trace)) {
			untraceable.insert(stage.program);
			return;
		}
		if (trace.flat_words != stage.flattened.size()) {
			return;
		}
		for (const auto& [flat, address]: trace.data) data.emplace_back(address, st << 24u | flat);
		for (const auto& read: trace.structural)
			(read.clean ? clean : structural).push_back({read.address, read.value, read.use});
		for (const auto& feed: trace.feeds) {
			if (feed.index >= 0x10000 || feed.dword >= 8) continue;
			feeds.emplace_back(trace.structural[feed.read].address,
			                   st << 28u | static_cast<uint32_t>(feed.kind) << 24u | feed.index << 8u | feed.dword);
		}
		// Flat copies of structural words: kind NativeXprFlatFeed, the flat index in bits 0-23.
		for (const auto& [read, flat]: trace.flat_feeds) {
			if (flat >= 0x1000000) continue;
			feeds.emplace_back(trace.structural[read].address, st << 28u | NativeXprFlatFeed << 24u | flat);
		}
	}
	const auto runs_of = [&](std::vector<StructuralRead>& reads, bool is_clean) {
		std::sort(reads.begin(), reads.end());
		// One entry per address, with the union of its uses.
		auto& unique = scratch.unique;
		unique.clear();
		for (const auto& read: reads) {
			if (!unique.empty() && unique.back().address == read.address) unique.back().use |= read.use;
			else unique.push_back(read);
		}
		for (const auto& read: unique) {
			if (gather.structural.empty() || gather.structural.back().clean != is_clean ||
			    gather.structural.back().count >= 16 ||
			    gather.structural.back().address + 4ull * gather.structural.back().count != read.address)
				gather.structural.push_back({read.address, 0, static_cast<uint32_t>(gather.values.size()), is_clean});
			++gather.structural.back().count;
			gather.values.push_back(read.value);
			gather.uses.push_back(read.use);
		}
	};
	gather.structural.clear();
	gather.values.clear();
	gather.uses.clear();
	runs_of(structural, false);
	runs_of(clean, true);
	gather.clean_spans.clear();
	gather.clean_value.clear();
	for (const auto& run: gather.structural) {
		if (!run.clean) continue;
		for (uint32_t i = 0; i < run.count; ++i) {
			const auto address = run.address + 4ull * i;
			auto*      span    = gather.clean_spans.empty() ? nullptr : &gather.clean_spans.back();
			const auto end     = span != nullptr ? span->address + 4ull * span->count : 0;
			// The gap words count too: a span never exceeds NativeXprSpanWords (the gather's buffer).
			if (span == nullptr || address < end || address - end > 32 || address / 4096 != span->address / 4096 ||
			    span->count + (address - end) / 4 + 1 > NativeXprSpanWords) {
				gather.clean_spans.push_back({address, 0, static_cast<uint32_t>(gather.clean_value.size())});
				span = &gather.clean_spans.back();
			}
			for (; span->address + 4ull * span->count < address; ++span->count) gather.clean_value.push_back(-1);
			gather.clean_value.push_back(static_cast<int32_t>(run.first + i));
			++span->count;
		}
	}
	// Feeds per structural value (clean values never feed a patch).
	auto& index_of = scratch.index_of; // (address, value index), non-clean runs
	index_of.clear();
	for (const auto& run: gather.structural)
		if (!run.clean)
			for (uint32_t i = 0; i < run.count; ++i) index_of.emplace_back(run.address + 4ull * i, run.first + i);
	std::ranges::sort(index_of);
	std::ranges::sort(feeds);
	{
		// Grouped by value in feed order: a count per value, its prefix sums as the
		// starts, then the feeds placed at their value's cursor.
		const auto values_count = gather.values.size();
		auto&      feed_value   = scratch.feed_value;
		feed_value.clear();
		gather.feeds_at.assign(values_count + 1, 0);
		for (const auto& [address, packed]: feeds) {
			const auto     it    = std::ranges::lower_bound(index_of, std::pair {address, 0u});
			const uint32_t value = it != index_of.end() && it->first == address ? it->second : UINT32_MAX;
			feed_value.push_back(value);
			if (value != UINT32_MAX) ++gather.feeds_at[value + 1];
		}
		for (size_t v = 0; v < values_count; ++v) gather.feeds_at[v + 1] += gather.feeds_at[v];
		gather.feeds.resize(gather.feeds_at[values_count]);
		auto& cursor = scratch.cursor;
		cursor.assign(gather.feeds_at.begin(), gather.feeds_at.end() - 1);
		for (size_t f = 0; f < feeds.size(); ++f)
			if (feed_value[f] != UINT32_MAX) gather.feeds[cursor[feed_value[f]]++] = feeds[f].second;
	}
	std::ranges::sort(data);
	gather.data.clear();
	gather.targets_at.clear();
	gather.targets.clear();
	for (size_t i = 0; i < data.size();) {
		const auto address = data[i].first;
		if (gather.data.empty() || gather.data.back().count >= 16 ||
		    gather.data.back().address + 4ull * gather.data.back().count != address)
			gather.data.push_back({address, 0, static_cast<uint32_t>(gather.targets_at.size()), false});
		++gather.data.back().count;
		gather.targets_at.push_back(static_cast<uint32_t>(gather.targets.size()));
		for (; i < data.size() && data[i].first == address; ++i) gather.targets.push_back(data[i].second);
	}
	gather.targets_at.push_back(static_cast<uint32_t>(gather.targets.size()));

	auto& words = scratch.words;
	words.clear();
	for (const auto& run: gather.structural)
		if (!run.clean)
			for (uint32_t i = 0; i < run.count; ++i)
				words.push_back({run.address + 4ull * i, static_cast<int32_t>(run.first + i), -1});
	for (const auto& run: gather.data)
		for (uint32_t i = 0; i < run.count; ++i)
			words.push_back({run.address + 4ull * i, -1, static_cast<int32_t>(run.first + i)});
	std::ranges::sort(words, {}, &Word::address);
	gather.spans.clear();
	gather.words.clear();
	gather.word_value.clear();
	gather.word_data.clear();
	constexpr uint64_t Gap = 32, Page = 4096;
	for (const auto& word: words) {
		auto*          span = gather.spans.empty() ? nullptr : &gather.spans.back();
		const uint64_t end  = span != nullptr ? span->address + 4ull * span->count : 0;
		if (span != nullptr && word.address < end) {
			// Read both structurally and as data.
			const auto at = span->first + span->count - 1;
			if (word.value >= 0) gather.word_value[at] = word.value;
			if (word.data >= 0) gather.word_data[at] = word.data;
			continue;
		}
		// The gap words count too: a span never exceeds NativeXprSpanWords (the gather's buffer).
		if (span == nullptr || word.address - end > Gap || word.address / Page != span->address / Page ||
		    span->count + (word.address - end) / 4 + 1 > NativeXprSpanWords) {
			gather.spans.push_back({word.address, 0, static_cast<uint32_t>(gather.words.size())});
			span = &gather.spans.back();
		}
		for (; span->address + 4ull * span->count < word.address; ++span->count) {
			gather.words.push_back(0);
			gather.word_value.push_back(-1);
			gather.word_data.push_back(-1);
		}
		gather.words.push_back(0);
		gather.word_value.push_back(word.value);
		gather.word_data.push_back(word.data);
		++span->count;
	}
	// Start from what the record holds (the evaluation's results), not from a
	// new read: a word the guest rewrote since then shows up as a change.
	using Kind = ShaderRecompiler::IR::SrtReadTrace::Kind;
	const auto held = [&](uint32_t v) {
		for (auto f = gather.feeds_at[v]; f < gather.feeds_at[v + 1]; ++f) {
			const auto feed  = gather.feeds[f];
			const auto& stage = record.stages[feed >> 28u];
			const auto kind  = (feed >> 24u) & 0xfu;
			if (kind == NativeXprFlatFeed) {
				if ((feed & 0xffffffu) < stage.flattened.size()) return stage.flattened[feed & 0xffffffu];
				continue;
			}
			const auto& snapshot = stage.snapshot;
			const auto& values   = static_cast<Kind>(kind) == Kind::Buffer  ? snapshot.buffers
			                       : static_cast<Kind>(kind) == Kind::Image ? snapshot.images
			                                                                : snapshot.samplers;
			const auto index = (feed >> 8u) & 0xffffu;
			if (index < values.size() && (feed & 0xffu) < values[index].dword_count)
				return values[index].dwords[feed & 0xffu];
		}
		return gather.values[v];
	};
	gather.value_word.assign(gather.values.size(), 0);
	for (size_t w = 0; w < gather.words.size(); ++w) {
		if (const auto v = gather.word_value[w]; v >= 0) {
			gather.value_word[v] = static_cast<uint32_t>(w);
			const auto value     = held(static_cast<uint32_t>(v));
			gather.words[w] = gather.values[v] = value;
		} else if (const auto d = gather.word_data[w]; d >= 0 && gather.targets_at[d] != gather.targets_at[d + 1]) {
			const auto target = gather.targets[gather.targets_at[d]];
			gather.words[w]   = record.stages[target >> 24u].flattened[target & 0xffffffu];
		}
	}
	gather.traced = true;
}

// Structural words that changed feed only descriptor dwords (m_native_xpr->changed).
// T# LOD-statistics fields only move shader-data words; a new image or buffer base
// address is resolved again with a new set. Anything else is left to a full
// evaluation (false, nothing changed).
RenderExecutor::NativeXprPatch RenderExecutor::NativeXprPatchDescriptors(NativeXprRecord& record) {
	using Kind   = ShaderRecompiler::IR::SrtReadTrace::Kind;
	auto& gather = record.gather;
	const auto is_flat = [](uint32_t feed) { return ((feed >> 24u) & 0xfu) == NativeXprFlatFeed; };
	const auto target  = [&](uint32_t feed) -> ShaderRecompiler::IR::DescriptorValue* {
		auto&      snapshot = record.stages[feed >> 28u].snapshot;
		const auto kind     = static_cast<Kind>((feed >> 24u) & 0xfu);
		const auto index    = (feed >> 8u) & 0xffffu;
		auto&      values   = kind == Kind::Buffer ? snapshot.buffers : kind == Kind::Image ? snapshot.images : snapshot.samplers;
		return index < values.size() && (feed & 0xffu) < values[index].dword_count ? &values[index] : nullptr;
	};
	bool     rebind     = false;
	uint32_t lod_stages = 0;
	auto&    rebind_images  = m_native_xpr->rebind_images;
	auto&    rebind_buffers = m_native_xpr->rebind_buffers;
	rebind_images.clear();
	rebind_buffers.clear();
	for (const auto& [index, value]: m_native_xpr->changed)
		for (auto f = gather.feeds_at[index]; f < gather.feeds_at[index + 1]; ++f) {
			const auto feed  = gather.feeds[f];
			if (is_flat(feed)) continue;
			const auto kind  = static_cast<Kind>((feed >> 24u) & 0xfu);
			const auto dword = feed & 0xffu;
			auto*      desc  = target(feed);
			if (desc == nullptr) return NativeXprPatch::Evaluate;
			const auto diff = desc->dwords[dword] ^ value;
			if (kind == Kind::Image) {
				// A zeroed (invalid) image or a change beyond the base address and
				// the LOD-statistics fields needs the evaluation's validity check.
				if (std::ranges::all_of(desc->dwords, [](uint32_t w) { return w == 0; })) return NativeXprPatch::Evaluate;
				if ((diff & ~NativeXprLodStatsMask[dword]) == 0) {
					lod_stages |= 1u << (feed >> 28u);
				} else if (dword == 0 || (dword == 1 && (diff & ~0xffu) == 0)) {
					rebind = true;
					rebind_images.emplace_back(feed >> 28u, (feed >> 8u) & 0xffffu);
				} else {
					return NativeXprPatch::Evaluate;
				}
			} else if (kind == Kind::Buffer) {
				if (dword == 0 || (dword == 1 && (diff & ~0xffffu) == 0)) {
					rebind = true;
					rebind_buffers.emplace_back(feed >> 28u, (feed >> 8u) & 0xffffu);
				} else {
					return NativeXprPatch::Evaluate;
				}
			} else {
				return NativeXprPatch::Evaluate;
			}
		}
	for (const auto& [index, value]: m_native_xpr->changed) {
		for (auto f = gather.feeds_at[index]; f < gather.feeds_at[index + 1]; ++f) {
			const auto feed = gather.feeds[f];
			if (is_flat(feed)) record.stages[feed >> 28u].flattened[feed & 0xffffffu] = value;
			else target(feed)->dwords[feed & 0xffu] = value;
		}
		gather.values[index] = value;
	}
	for (uint32_t st = 0; st < record.stage_count; ++st)
		if (lod_stages & (1u << st))
			NativeXprApplyLodStats(*record.stages[st].program, record.stages[st].snapshot.images,
			                       record.stages[st].shader_data);
	if (!rebind) return NativeXprPatch::Done;
	{
		for (auto* slots: {&rebind_images, &rebind_buffers}) {
			std::ranges::sort(*slots);
			slots->erase(std::unique(slots->begin(), slots->end()), slots->end());
		}
		if (NativeXprRebindSlots(record, rebind_images, rebind_buffers)) return NativeXprPatch::Done;
	}
	std::array<ShaderRecompiler::IR::ResourceSnapshot, 2> snapshots;
	for (uint32_t st = 0; st < record.stage_count; ++st) {
		snapshots[st]               = record.stages[st].snapshot;
		snapshots[st].flattened_srt = record.stages[st].flattened;
	}
	// The snapshot already carries the new descriptors: a failed resolution
	// leaves nothing an evaluation could compare against.
	return NativeXprRebind(record, snapshots) ? NativeXprPatch::Done : NativeXprPatch::Invalid;
}

// Validation reads: a page without read watchers is loaded directly (every path
// of the evaluation's readers ends in that load for it); other pages take the
// evaluation's readers. The page answer is kept for one validation.
class NativeXprReader {
public:
	explicit NativeXprReader(GpuResourceManager& resources): m_resources(resources) {}
	void Read(uint64_t address, uint32_t count, uint32_t* out) {
		const uint64_t bytes = uint64_t {count} * 4u;
		const uint64_t first = address >> 12u, last = (address + bytes - 1u) >> 12u;
		if (first == last) {
			if (first != m_page) {
				m_page   = first;
				m_direct = !m_resources.HasReadWatchers(first << 12u, 4096);
			}
			if (m_direct) {
				std::memcpy(out, reinterpret_cast<const void*>(address), bytes);
				return;
			}
		}
		NativeXprReadWords(address, count, out);
	}

private:
	GpuResourceManager& m_resources;
	uint64_t            m_page   = UINT64_MAX;
	bool                m_direct = false;
};

// Traced records: false when a structural read changed (the caller evaluates
// again); otherwise the data reads are copied into the flattened SRT.
bool RenderExecutor::NativeXprGather(NativeXprRecord& record) {
	LiveCensus::Scope census(LiveCensus::NativeGather, 0);
	auto&           gather = record.gather;
	NativeXprReader reader(m_context.GetGpuResources());
	auto& changed = m_native_xpr->changed; // (value index, new value)
	changed.clear();
	// Clean structural reads (GPU-written memory, read as the evaluation reads
	// it) are never patched.
	std::array<uint32_t, NativeXprSpanWords> fresh;
	for (const auto& item: gather.clean_spans) {
		if (item.count > fresh.size()) return false; // never past the buffer: evaluate again
		const auto* values = gather.clean_value.data() + item.first;
		// A span that reads clean has clean words; otherwise each word is read
		// as the evaluation reads it.
		if (Libs::LibKernel::Memory::TryReadGpuCleanBackingToHost(item.address, fresh.data(), 4ull * item.count)) {
			for (uint32_t i = 0; i < item.count; ++i)
				if (values[i] >= 0 && fresh[i] != gather.values[values[i]]) return false;
			continue;
		}
		for (uint32_t i = 0; i < item.count; ++i) {
			if (values[i] < 0) continue;
			uint32_t value = 0;
			if (!NativeXprReadClean(item.address + 4ull * i, 1, &value) || value != gather.values[values[i]])
				return false;
		}
	}
	// Records are mostly used once per frame: their guest words are cold. Start
	// every line's load before comparing any.
	for (const auto& item: gather.spans)
		for (uint64_t line = item.address & ~63ull; line < item.address + 4ull * item.count; line += 64)
			__builtin_prefetch(reinterpret_cast<const void*>(line));
	for (const auto& item: gather.spans) {
		if (item.count > fresh.size()) return false; // never past the buffer: evaluate again
		reader.Read(item.address, item.count, fresh.data());
		auto* held = gather.words.data() + item.first;
		if (std::memcmp(fresh.data(), held, item.count * 4ull) == 0) continue;
		for (uint32_t i = 0; i < item.count; ++i) {
			const auto value = fresh[i];
			if (value == held[i]) continue;
			const auto at = item.first + i;
			if (const auto d = gather.word_data[at]; d >= 0)
				for (auto t = gather.targets_at[d]; t < gather.targets_at[d + 1]; ++t) {
					const auto target = gather.targets[t];
					record.stages[target >> 24u].flattened[target & 0xffffffu] = value;
				}
			if (const auto v = gather.word_value[at]; v >= 0) {
				// Only words that feed nothing but descriptor dwords may be patched;
				// anything else is evaluated again (which traces again).
				if ((gather.uses[v] & ~ShaderRecompiler::IR::SrtReadTrace::Descriptor) != 0 ||
				    gather.feeds_at[v] == gather.feeds_at[v + 1])
					return false;
				changed.emplace_back(v, value);
				continue; // held once the patch is done
			}
			held[i] = value;
		}
	}
	if (!changed.empty()) {
		const auto patched = NativeXprPatchDescriptors(record);
		if (patched == NativeXprPatch::Evaluate) return false;
		if (patched == NativeXprPatch::Invalid) {
			record.gather.broken = true;
			return false;
		}
		for (const auto& [v, value]: changed) gather.words[gather.value_word[v]] = value;
	}
	return true;
}

bool RenderExecutor::NativeXprReevaluate(NativeXprRecord& record) {
	auto& cache = *m_native_xpr;
	if (record.stages[0].shader_base == 0) return false;
	std::array<ShaderRecompiler::IR::ResourceSnapshot, 2> fresh;
	auto& reads = cache.reads;
	reads.clear();
	// Traced records need no read log: the trace is rebuilt instead.
	const bool traced = record.gather.traced;
	{
		std::optional<ShaderReadObserver> observer;
		if (!traced)
			observer.emplace(
			    [](void* userdata, uint64_t address, uint64_t size) {
				    static_cast<std::vector<std::pair<uint64_t, uint64_t>>*>(userdata)->emplace_back(address, size);
			    },
			    &reads);
		auto& pipelines = m_context.GetPipelineCache();
		for (uint32_t st = 0; st < record.stage_count; ++st) {
			const auto& stage = record.stages[st];
			if (!pipelines.RematerializeStage(*stage.program, stage.user_data, stage.shader_base, fresh[st]))
				return false;
		}
	}
	// Same structure: the descriptors up to the LOD-statistics fields, user data,
	// uniform fill and the flattened SRT size are unchanged.
	bool same_structure = true;
	for (uint32_t st = 0; st < record.stage_count && same_structure; ++st) {
		const auto& a  = record.stages[st].snapshot;
		const auto& b  = fresh[st];
		same_structure = a.buffers == b.buffers && NativeXprSameImagesButLodStats(a.images, b.images) &&
		                 a.samplers == b.samplers && a.user_data == b.user_data &&
		                 a.uniform_fill == b.uniform_fill &&
		                 b.flattened_srt.size() == record.stages[st].flattened.size();
	}
	if (traced) {
		if (!same_structure) {
			if (!NativeXprRebind(record, fresh)) return false;
		} else {
			for (uint32_t st = 0; st < record.stage_count; ++st) {
				auto& stage = record.stages[st];
				if (!(stage.snapshot.images == fresh[st].images)) {
					NativeXprApplyLodStats(*stage.program, fresh[st].images, stage.shader_data);
					stage.snapshot.images = std::move(fresh[st].images);
				}
				stage.flattened = std::move(fresh[st].flattened_srt);
			}
		}
		NativeXprRetrace(record);
		return true;
	}
	std::vector<std::pair<uint64_t, uint32_t>> new_reads;
	std::vector<uint32_t>                      new_words;
	new_reads.reserve(reads.size());
	for (const auto& [address, size]: reads) {
		if (address % 4 != 0 || size % 4 != 0 || size == 0 || size > 4096) return false;
		const auto count = static_cast<uint32_t>(size / 4);
		new_reads.emplace_back(address, count);
		const auto at = new_words.size();
		new_words.resize(at + count);
		NativeXprReadWords(address, count, new_words.data() + at);
	}
	if (!same_structure) {
		// New descriptors: resolve them and bind a new set (flattened constants,
		// shader data and proofs come with it); no patch knowledge carries over.
		if (!NativeXprRebind(record, fresh)) return false;
		record.word_patch.assign(new_words.size(), -1);
		record.reads = std::move(new_reads);
		record.words = std::move(new_words);
		NativeXprRetrace(record);
		return true;
	}
	// Same structure up to the LOD-statistics fields. With an unchanged read layout
	// the old and new evaluations teach which words are plain copies (flattened
	// constants, LOD dwords), so the next change of them is only patched.
	std::array<std::vector<uint32_t>, 2> shader_data;
	for (uint32_t st = 0; st < record.stage_count; ++st) {
		shader_data[st] = record.stages[st].shader_data;
		NativeXprApplyLodStats(*record.stages[st].program, fresh[st].images, shader_data[st]);
	}
	if (new_reads == record.reads) {
		NativeXprRecord fresh_record;
		fresh_record.reads          = new_reads;
		fresh_record.words          = new_words;
		fresh_record.stage_count    = record.stage_count;
		fresh_record.vertex_program = record.vertex_program;
		fresh_record.pixel_program  = record.pixel_program;
		for (uint32_t st = 0; st < record.stage_count; ++st) {
			fresh_record.stages[st].program     = record.stages[st].program;
			fresh_record.stages[st].snapshot    = fresh[st];
			fresh_record.stages[st].shader_data = shader_data[st];
			fresh_record.stages[st].flattened   = fresh[st].flattened_srt;
		}
		NativeXprLearn(record, fresh_record);
		record.word_patch = std::move(fresh_record.word_patch);
	} else {
		record.word_patch.assign(new_words.size(), -1);
	}
	for (uint32_t st = 0; st < record.stage_count; ++st) {
		record.stages[st].snapshot.images = fresh[st].images;
		record.stages[st].shader_data     = std::move(shader_data[st]);
	}
	for (uint32_t st = 0; st < record.stage_count; ++st) record.stages[st].flattened = std::move(fresh[st].flattened_srt);
	record.reads = std::move(new_reads);
	record.words = std::move(new_words);
	if (record.gather.traced) NativeXprRetrace(record);
	return true;
}

// Once per frame: is the record's content still what a fresh translation gives?
bool RenderExecutor::NativeXprValidate(NativeXprRecord& record, uint64_t frame) {
	auto& cache     = *m_native_xpr;
	auto& resources = m_context.GetGpuResources();
	// A guest mapping change forces the buffer and image proofs below; the words
	// are compared again anyway.
	const bool mapping_moved = resources.MappingEpoch() != record.mapping_epoch;
	// An unmap can take words the record reads (tables the SRT no longer reaches):
	// those are read directly, so none may have been unmapped since the proof.
	if (mapping_moved) {
		const auto unmapped = [&](uint64_t address, uint32_t count) {
			return resources.UnmappedSince(record.mapping_epoch, address, 4ull * count);
		};
		if (record.gather.traced
		        ? std::ranges::any_of(record.gather.spans, [&](const auto& span) { return unmapped(span.address, span.count); })
		        : std::ranges::any_of(record.reads, [&](const auto& read) { return unmapped(read.first, read.second); }))
			return false;
	}
	if (record.gather.traced) {
		if (!NativeXprGather(record) && (record.gather.broken || !NativeXprReevaluate(record))) {
			return false;
		}
		return NativeXprValidateResources(record, frame, mapping_moved);
	}
	// Every logged word, read the way the evaluation reads it: a span through the
	// watched-page aware span reader, single words otherwise.
	auto& changed = cache.changed;
	auto& span    = cache.span;
	changed.clear();
	size_t cursor = 0;
	for (const auto& [address, count]: record.reads) {
		span.resize(count);
		NativeXprReadWords(address, count, span.data());
		if (std::memcmp(span.data(), record.words.data() + cursor, count * 4ull) != 0) {
			for (uint32_t i = 0; i < count; ++i)
				if (span[i] != record.words[cursor + i])
					changed.emplace_back(static_cast<uint32_t>(cursor + i), span[i]);
		}
		cursor += count;
	}
	if (!changed.empty() &&
	    std::ranges::any_of(changed, [&](const auto& change) { return record.word_patch[change.first] == -1; })) {
		// A word no patch explains: evaluate the SRT again. Unchanged structure
		// (descriptors, user data, uniform fill, sizes) keeps the descriptor set;
		// only the flattened constants and the logged reads are replaced.
		if (NativeXprReevaluate(record)) {
			changed.clear();
		}
	}
	if (!changed.empty()) {
		for (const auto& [index, value]: changed) {
			if (record.word_patch[index] == -1) {
				return false;
			}
		}
		// Only learned words moved: patch the flattened SRT copies and the T# LOD
		// dwords (with the shader-data words derived from them).
		uint32_t lod_stages = 0;
		for (const auto& [index, value]: changed) {
			const auto patch = record.word_patch[index];
			if (patch >= 0) {
				const auto at = static_cast<uint32_t>(patch);
				record.stages[at >> 24u].flattened[at & 0xffffffu] = value;
			} else if (patch <= NativeXprImagePatchBase) {
				const auto code = static_cast<uint32_t>(NativeXprImagePatchBase - patch);
				record.stages[code >> 20u].snapshot.images[(code >> 4u) & 0xffffu].dwords[code & 0xfu] = value;
				lod_stages |= 1u << (code >> 20u);
			}
			record.words[index] = value;
		}
		for (uint32_t st = 0; st < record.stage_count; ++st)
			if (lod_stages & (1u << st))
				NativeXprApplyLodStats(*record.stages[st].program, record.stages[st].snapshot.images,
				                       record.stages[st].shader_data);
	}
	return NativeXprValidateResources(record, frame, mapping_moved);
}

// No image started at a texel dependency since the record's proof.
bool RenderExecutor::NativeXprTexelRangesClear(const NativeXprRecord& record) {
	if (std::ranges::none_of(record.buffers, &NativeXprRecord::BufferDep::texel)) return true;
	auto& cache    = *m_native_xpr;
	auto& textures = m_context.GetTextureCache();
	if (cache.starts_seen != textures.StartEpoch()) {
		const auto seen = cache.starts_seen;
		if (seen != 0 && textures.NewImageStartsSince(seen, cache.recent_starts)) {
			if (!cache.recent_starts.empty()) cache.starts_seen = std::max(seen, cache.recent_starts.back().first);
		} else {
			cache.recent_starts.clear();
			cache.starts_from = cache.starts_seen = textures.StartEpoch();
		}
		if (cache.recent_starts.size() > 4096) {
			const auto drop   = cache.recent_starts.size() - 2048;
			cache.starts_from = cache.recent_starts[drop - 1].first;
			cache.recent_starts.erase(cache.recent_starts.begin(), cache.recent_starts.begin() + static_cast<ptrdiff_t>(drop));
		}
	}
	if (record.start_epoch == 0 || record.start_epoch < cache.starts_from) {
		for (const auto& dep: record.buffers)
			if (dep.texel && textures.HasImageStartingAt(dep.address)) return false;
		return true;
	}
	const auto first = std::ranges::upper_bound(cache.recent_starts, record.start_epoch, {},
	                                            &std::pair<uint64_t, uint64_t>::first);
	for (auto it = first; it != cache.recent_starts.end(); ++it)
		for (const auto& dep: record.buffers)
			if (dep.texel && dep.address == it->second) return false;
	return true;
}

// Buffers and images: once per frame, or when the live-object epochs moved.
bool RenderExecutor::NativeXprValidateResources(NativeXprRecord& record, uint64_t frame, bool mapping_moved) {
	auto& buffers   = m_context.GetBufferCache();
	auto& textures  = m_context.GetTextureCache();
	auto& resources = m_context.GetGpuResources();
	// The rest holds for the whole frame unless the live-object epochs move, or the guest
	// wrote a bound buffer since the proof (its region's CPU epoch moved).
	if (!mapping_moved && record.validated_frame == frame &&
	    record.registration_epoch == buffers.RegistrationEpoch() &&
	    record.resolution_epoch == textures.ResolutionEpoch()) {
		uint64_t cpu_epoch = 0;
		for (const auto& dep: record.buffers) {
			const auto epoch = buffers.CpuModificationEpoch(dep.address, dep.size);
			if (epoch == 0) {
				cpu_epoch = 0;
				break;
			}
			cpu_epoch += epoch;
		}
		// The epochs cannot prove it: the ranges themselves (what the normal path's
		// ObtainBuffer checks per draw).
		const bool buffers_clean = record.buffers.empty() || (cpu_epoch != 0 && cpu_epoch == record.cpu_epoch) ||
		                           std::ranges::none_of(record.buffers, [&](const auto& dep) {
			                           return buffers.IsRegionCpuModified(dep.address, dep.size);
		                           });
		// Images: no image became CPU-dirty since the proof, or none of these did.
		const auto image_epoch  = Image::CpuDirtyEpoch();
		bool       images_clean = image_epoch == record.image_dirty_epoch;
		if (buffers_clean && !images_clean) {
			images_clean = std::ranges::none_of(record.images, [&](const auto& dep) {
				const auto* image = textures.m_slot_images.try_get(dep.id);
				return image == nullptr || image->serial != dep.serial || image->IsCpuDirty();
			});
			if (images_clean) record.image_dirty_epoch = image_epoch;
		}
		if (buffers_clean && images_clean) return true;
	}
	const auto image_epoch_at_proof = Image::CpuDirtyEpoch();
	// Buffers: live owners, CPU-clean. The region epochs prove both unchanged
	// cheaply; when they moved, check each range.
	uint64_t cpu_epoch = 0;
	for (const auto& dep: record.buffers) {
		const auto epoch = buffers.CpuModificationEpoch(dep.address, dep.size);
		if (epoch == 0) {
			cpu_epoch = 0;
			break;
		}
		cpu_epoch += epoch;
	}
	// After a mapping change, the ranges as FindBuffers gives them now: none unmapped since
	// the proof (the refresh below reads them), and one it clamped to the mappings not grown.
	const auto range_current = [&](const NativeXprRecord::BufferDep& dep) {
		const auto& descriptors = record.stages[dep.stage].snapshot.buffers;
		if (dep.slot >= descriptors.size() || resources.UnmappedSince(record.mapping_epoch, dep.address, dep.size))
			return false;
		const auto descriptor = DecodeNativeDescriptor<ShaderBufferResource>(descriptors[dep.slot]);
		const auto stride     = descriptor.Stride();
		const auto requested  = stride != 0 ? uint64_t(stride) * descriptor.NumRecords() : uint64_t(descriptor.NumRecords());
		return requested == dep.size || Libs::LibKernel::Memory::ClampRangeSize(dep.address, requested) == dep.size;
	};
	// TouchLiveBuffer also keeps the owners from the garbage collector, as the
	// normal path's per-draw ObtainBuffer would.
	const bool cpu_proven = !mapping_moved && cpu_epoch != 0 && cpu_epoch == record.cpu_epoch;
	for (const auto& dep: record.buffers) {
		if ((mapping_moved && !range_current(dep)) ||
		    !buffers.TouchLiveBuffer(dep.id, dep.handle, dep.address, dep.size)) {
			return false;
		}
		if (cpu_proven || !buffers.IsRegionCpuModified(dep.address, dep.size)) continue;
		// The guest rewrote the range: bring the owner up to date as the store
		// did (the normal path would copy the range for its draw instead).
		if (buffers.IsRegionGpuModified(dep.address, dep.size)) {
			return false;
		}
		buffers.EnsureBufferContents(dep.address, dep.size);
		const auto [served, offset] = buffers.ObtainBuffer(dep.address, dep.size, false, dep.texel, dep.id);
		if (served == nullptr || served->Handle() != dep.handle || buffers.IsRegionCpuModified(dep.address, dep.size)) {
			return false;
		}
	}
	record.cpu_epoch          = cpu_epoch;
	record.registration_epoch = buffers.RegistrationEpoch();
	// Texel buffers: still no image starting at one (checked against the image
	// starts logged since the record's proof).
	if (const auto start_epoch = textures.StartEpoch(); record.start_epoch != start_epoch) {
		if (!NativeXprTexelRangesClear(record)) {
			return false;
		}
		record.start_epoch = start_epoch;
	}
	// Images: the same objects (views alive), registered with the same backing,
	// contents current.
	const bool registrations_moved = textures.ResolutionEpoch() != record.resolution_epoch;
	// KYTY_NATIVE_IMAGE_PROOF: other images' registrations move the global epoch every
	// frame; an image still in the registration the proof saw passes the same checks.
	const bool per_image = kyty_local_native_image_proof_mode.load(std::memory_order_relaxed) != 0;
	for (const auto& dep: record.images) {
		auto* image = textures.m_slot_images.try_get(dep.id);
		if (image == nullptr || image->serial != dep.serial || image->IsCpuDirty()) return false;
		if (registrations_moved) {
			if (per_image && image->registered && image->registrations == dep.registration &&
			    !image->depth_id && !image->binding.needs_rebind) {
				image->frame_accessed_last = textures.CurrentFrame();
			} else if (!textures.IsSampledImageCurrent(dep.id, dep.desc)) {
				return false;
			}
		}
		(void)textures.GetImage(dep.id); // LRU, as FindTexture does
	}
	record.resolution_epoch = textures.ResolutionEpoch();
	record.mapping_epoch    = resources.MappingEpoch();
	record.validated_frame  = frame;
	record.image_dirty_epoch = image_epoch_at_proof;
	return true;
}

// Emits the draws of one record under one draw state. `commands` are the
// GPU-side DrawIndexedIndirectCommand addresses, all drawn with the same bindings.
struct RenderExecutor::NativeXprDirect {
	NativeXprDirectDraw draw;
	int32_t             vertex_offset  = 0;
	uint32_t            first_instance = 0;
};

bool RenderExecutor::NativeXprEmit(CommandBuffer& buffer, NativeXprRecord& record,
                                   const NativeXprDrawState& draw_state,
                                   const PipelineCache::Pipeline& pipeline,
                                   std::span<const uint64_t> commands, uint64_t index_base,
                                   uint64_t index_bytes, vk::IndexType index_type, bool keep_clears,
                                   const NativeXprDirect* direct) {
	auto&           cache = *m_native_xpr;
	auto& upload = m_context.GetBufferCache().GetShaderUploadBuffer();
	if (upload.Handle() != record.upload) {
		return false;
	}
	// Constants into the ring; dynamic offsets follow binding order (vertex
	// flattened SRT, vertex shader data, pixel ...), as the bindings are numbered.
	const auto& limits    = m_context.GetGraphics().physical_device_properties.limits;
	const auto  alignment = std::max<uint64_t>({256, limits.minUniformBufferOffsetAlignment,
	                                            limits.minStorageBufferOffsetAlignment});
	std::array<uint32_t, 4> offsets {};
	uint32_t                offset_count = 0;
	ShaderRecompiler::IR::PushData push_data;
	const auto debug = kyty_local_native_xpr_debug.load(std::memory_order_relaxed);
	for (uint32_t i = 0; i < record.stage_count; ++i) {
		const auto& stage = record.stages[i];
		if (stage.srt_binding) {
			const std::vector<uint32_t> zeroes(i == 0 && (debug & 1u) != 0 ? stage.flattened.size() : 0, 0u);
			const auto* data = zeroes.empty() ? stage.flattened.data() : zeroes.data();
			offsets[offset_count++] = static_cast<uint32_t>(
			    upload.Copy(data, stage.flattened.size() * sizeof(uint32_t), alignment));
		}
		if (stage.data_binding)
			offsets[offset_count++] = static_cast<uint32_t>(upload.Copy(
			    stage.shader_data.data(), stage.shader_data.size() * sizeof(uint32_t), alignment));
		if (stage.push_start != ShaderRecompiler::IR::PushData::NoStart) {
			EXIT_IF(stage.push_start + stage.shader_data.size() > push_data.dwords.size());
			std::ranges::copy(stage.shader_data, push_data.dwords.begin() + stage.push_start);
		}
	}

	// Indices and arguments are read by the GPU; lookups may restart the
	// scheduler, so the command buffer handle is taken after them.
	auto& buffer_cache = m_context.GetBufferCache();
	// One lookup per group of consecutive arguments: the same range the draws
	// read, synchronized as the per-draw lookups would.
	constexpr uint64_t stride = sizeof(vk::DrawIndexedIndirectCommand);
	// Only the first `groups` entries are written and read: no zero fill (1.25 KB per draw).
	std::array<std::pair<Buffer*, uint64_t>, 64> arguments;
	std::array<uint32_t, 64>                     group_sizes;
	uint32_t                                     groups = 0;
	EXIT_IF(commands.size() > arguments.size() || (direct != nullptr && !commands.empty()));
	for (size_t first = 0; first < commands.size();) {
		size_t count = 1;
		while (first + count < commands.size() && commands[first + count] == commands[first] + count * stride) ++count;
		const auto [owner, offset] = buffer_cache.ObtainBuffer(commands[first], count * stride, false);
		if (owner == nullptr) return false;
		arguments[groups]   = {owner, offset};
		group_sizes[groups] = static_cast<uint32_t>(count);
		++groups;
		first += count;
	}
	auto&      emit       = cache.emit;
	const bool same_index = emit.command == buffer.Handle() && emit.generation == buffer.GraphicsGeneration() &&
	                        emit.index_base == index_base && emit.index_bytes == index_bytes &&
	                        emit.index_type == index_type;
	std::pair<Buffer*, uint64_t> indices {};
	if (!same_index) {
		const auto [owner, offset] = buffer_cache.ObtainBuffer(index_base, index_bytes, false);
		if (owner == nullptr) return false;
		indices = {owner, offset};
	}
	if (buffer.IsInvalid()) return false;
	auto vk_buffer = buffer.Handle();

	auto& textures  = m_context.GetTextureCache();
	auto& scheduler = m_context.GetCommandScheduler();
	// What the normal path's target acquisition establishes must still hold: the
	// image is registered, GPU-owned (so CPU reads download it) and CPU-clean, and
	// no fast clear is pending in its metadata. Otherwise the normal path acquires.
	const auto meta_epoch = textures.MetaEpoch();
	for (const auto& target: draw_state.targets) {
		auto* image = textures.m_slot_images.try_get(target.id);
		if (image == nullptr || image->serial != target.serial || !image->registered || image->binding.needs_rebind ||
		    !image->SafeToDownload()) {
			return false;
		}
		if (target.metadata != 0 && draw_state.meta_epoch != meta_epoch &&
		    textures.IsMetaCleared(target.metadata, target.slice)) {
			return false;
		}
	}
	draw_state.meta_epoch = meta_epoch;
	for (const auto& target: draw_state.targets) {
		auto& image = textures.GetImage(target.id); // LRU, as acquisition does
		if (image.backing.state.layout != target.layout)
			image.Transit(target.layout, target.access, target.range, vk_buffer);
	}
	for (const auto& dep: record.images) {
		auto* image = textures.m_slot_images.try_get(dep.id);
		if (image == nullptr) return false;
		image->Transit(dep.layout, vk::AccessFlagBits2::eShaderRead, dep.range, vk_buffer);
	}
	scheduler.BeginRendering(keep_clears ? draw_state.first_rendering : draw_state.rendering);

	// Everything bound since the last native emission in this command buffer
	// stays bound unless another path bound graphics state (generation).
	const bool same_command = emit.command == vk_buffer && emit.generation == buffer.GraphicsGeneration();
	if (same_command && emit.state != &draw_state && emit.state != nullptr &&
	    std::memcmp(&emit.state->dynamic, &draw_state.dynamic, sizeof(draw_state.dynamic)) == 0) {
		// Another draw state with the same dynamic state is bound already.
		emit.state = &draw_state;
	}
	if (!same_command || emit.state != &draw_state) {
		const auto& state = draw_state.dynamic;
		SetGraphicsDynamicParamsImpl(state.buffer, vk_buffer, state.indexed_viewports, state.colors,
		                             state.color_count, state.depth, VULKAN_HPP_DEFAULT_DISPATCHER);
		if (state.feedback_enabled) vk_buffer.setAttachmentFeedbackLoopEnableEXT(state.feedback_aspects);
	}
	if (!same_command || emit.pipeline != &pipeline) {
		vk_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline);
	}
	vk_buffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.pipeline_layout, 0, 1,
	                             &record.set, offset_count, offsets.data());
	if (record.has_push)
		vk_buffer.pushConstants(pipeline.pipeline_layout, record.push_stages, 0, sizeof(push_data),
		                        push_data.dwords.data());
	if (!same_index) {
		vk_buffer.bindIndexBuffer(indices.first->Handle(), indices.second, index_type);
	}
	if ((debug & 2u) == 0) {
		if (direct != nullptr) {
			// The index buffer is bound at the draw's first index (index_base).
			vk_buffer.drawIndexed(direct->draw.index_count, direct->draw.instance_count, 0, direct->vertex_offset,
			                      direct->first_instance);
		}
		// Each group of consecutive arguments goes out as one multi-draw.
		for (uint32_t group = 0; group < groups; ++group)
			vk_buffer.drawIndexedIndirect(arguments[group].first->Handle(), arguments[group].second,
			                              group_sizes[group], static_cast<uint32_t>(stride));
	}
	emit.command     = vk_buffer;
	emit.generation  = buffer.GraphicsGeneration();
	emit.state       = &draw_state;
	emit.pipeline    = &pipeline;
	emit.index_base  = index_base;
	emit.index_bytes = index_bytes;
	emit.index_type  = index_type;
	// KYTY_FRAMEGEN: the camera, which the normal path's draw hook reads from the G-buffer vertex
	// shader's SRT. Without it frames drawn from records had no generated frames (and were not
	// capped by KYTY_FLIP_RATE).
	if (FrameGen::Enabled() && record.stage_count > 0 && record.stages[0].program != nullptr) {
		FrameGen::OnDraw(record.stages[0].program->shader_hash, record.stages[0].flattened);
	}
	return true;
}

// Offline replay: draws the object whose record matches the current registers
// under the draw state they select, without runtime proofs. `keep_clears`
// begins rendering with the stored draw's clears as well.
bool RenderExecutor::NativeXprDraw(CommandBuffer& buffer, const NativeXprDrawArgs& args) {
	if (!m_native_xpr || buffer.IsInvalid() || args.index_address == 0 || args.index_bytes == 0 ||
	    args.args_address == 0)
		return false;
	auto& cache = *m_native_xpr;
	m_context.GetCommandScheduler().PopPendingOperations();
	Common::LockGuard lock(m_context.GetMutex());

	NativeXprKey(buffer, cache.key);
	const auto hash  = XXH3_64bits(cache.key.data(), cache.key.size() * sizeof(uint32_t));
	const auto found = cache.records.find(hash);
	if (found == cache.records.end() || found->second->key != cache.key) {
		return false;
	}
	auto&      record    = *found->second;
	const auto state_key = NativeXprStateKey(buffer);
	const auto state_it  = cache.states.find(state_key);
	const auto pipeline_it =
	    std::ranges::find(record.pipelines, state_key, &std::pair<uint64_t, const PipelineCache::Pipeline*>::first);
	if (state_it == cache.states.end() || pipeline_it == record.pipelines.end()) {
		return false;
	}
	cache.emit = {};
	return NativeXprEmit(buffer, record, *state_it->second, *pipeline_it->second, {&args.args_address, 1},
	                     args.index_address, args.index_bytes, args.index_type, args.keep_clears);
}

// Runtime: a DRAW_INDEX_INDIRECT run of the current object. `clean`: no graphics
// register changed since the previous indexed indirect draw except user data.
bool RenderExecutor::NativeXprTry(CommandBuffer& buffer, bool clean, std::span<const uint64_t> commands,
                                  uint64_t index_base, uint64_t index_bytes, vk::IndexType index_type,
                                  const NativeXprDirectDraw* direct) {
	auto& cache = NativeXprState();
	if (buffer.IsInvalid() || (direct == nullptr ? commands.empty() : !commands.empty()) || commands.size() > 64 ||
	    index_base == 0 || index_bytes == 0)
		return false;
	m_context.GetCommandScheduler().PopPendingOperations();
	Common::LockGuard lock(m_context.GetMutex());
	NativeXprCollect();

	if (!clean || !cache.state_key_valid) {
		cache.state_key       = NativeXprStateKey(buffer);
		cache.state_key_valid = true;
	}
	NativeXprKey(buffer, cache.key);
	const auto hash  = XXH3_64bits(cache.key.data(), cache.key.size() * sizeof(uint32_t));
	auto* const found = cache.IndexFind(hash);
	if (found != nullptr) NativeXprPrefetchFields(*found);
	LiveCounters::Add(direct != nullptr ? LiveCounters::XprDirectTries : LiveCounters::XprTries);
	if (found == nullptr || !found->SameKey(cache.key)) {
		LiveCounters::Add(LiveCounters::XprMissKey);
		if (const auto pair = cache.pairs.find(NativeXprPairKey(cache.key));
		    pair != cache.pairs.end() && (pair->second.blocked || pair->second.unstorable))
			return false;
		if (!cache.SeenAgain(hash, m_context.FrameNumber())) return false;
		m_native_xpr_store = true;
		return false;
	}
	auto& record      = *found;
	record.used_frame = m_context.FrameNumber();
	++record.uses;
	if (kyty_local_native_xpr_predict_mode.load(std::memory_order_relaxed) != 0) {
		// Learn the order, then fetch the fields of the predicted next record while
		// this draw runs. Records are found through the index only (never a stale pointer).
		if (cache.last_hash != 0 && cache.last_hash != hash) {
			if (auto* previous = cache.IndexFind(cache.last_hash)) previous->next_hash = hash;
			if (cache.last2_hash != 0 && cache.last2_hash != hash)
				if (auto* before = cache.IndexFind(cache.last2_hash)) before->next2_hash = hash;
			cache.last2_hash = cache.last_hash;
		}
		cache.last_hash = hash;
		// Two ahead: the index line of the record after next, so the lookup below finds the
		// next record's slot in cache (the index is too large to stay there between frames).
		if (record.next2_hash != 0) __builtin_prefetch(cache.IndexSlotOf(record.next2_hash));
		if (record.next_hash != 0)
			if (auto* next = cache.IndexFind(record.next_hash)) NativeXprPrefetchFields(*next);
	}
	// States are never erased (a replaced one keeps its address): the last lookup's entry
	// stands while the state key is the same.
	if (cache.last_state == nullptr || cache.last_state_key != cache.state_key) {
		const auto state_it = cache.states.find(cache.state_key);
		if (state_it == cache.states.end()) {
			LiveCounters::Add(LiveCounters::XprMissState);
			m_native_xpr_store = true;
			return false;
		}
		cache.last_state_key = cache.state_key;
		cache.last_state     = state_it->second.get();
	}
	const auto pipeline_it = std::ranges::find(record.pipelines, cache.state_key,
	                                           &std::pair<uint64_t, const PipelineCache::Pipeline*>::first);
	if (pipeline_it == record.pipelines.end()) {
		LiveCounters::Add(LiveCounters::XprMissState);
		m_native_xpr_store = true;
		return false;
	}
	// A direct draw also needs the offsets the normal path resolved under this state.
	NativeXprDirect direct_draw;
	if (direct != nullptr) {
		const auto offsets =
		    std::ranges::find(record.direct_offsets, cache.state_key, &NativeXprRecord::DirectOffsets::state);
		if (offsets == record.direct_offsets.end()) {
			LiveCounters::Add(LiveCounters::XprMissState);
			m_native_xpr_store = true;
			return false;
		}
		direct_draw = {*direct, offsets->vertex, offsets->instance};
	}
	const auto& draw_state = *cache.last_state;
	// A deleted target image took the state's attachment views along (its slot
	// may hold another image by now): the normal path captures the state again.
	auto& textures = m_context.GetTextureCache();
	for (const auto& target: draw_state.targets) {
		const auto* image = textures.m_slot_images.try_get(target.id);
		if (image == nullptr || image->serial != target.serial) {
			LiveCounters::Add(LiveCounters::XprMissTarget);
			m_native_xpr_store = true;
			return false;
		}
	}
	// Records are mostly used once per frame, so their data is cold: load the
	// arrays together instead of missing on each in turn.
	NativeXprPrefetchArrays(record);
	// A sampled image that is also a render target takes the normal path.
	for (const auto id: record.image_ids)
		for (const auto& target: draw_state.targets)
			if (id == target.id) {
				LiveCounters::Add(LiveCounters::XprMissTarget);
				return false;
			}
	const auto frame = m_context.FrameNumber();
	// Every use reads the SRT words again: the guest may rewrite constants between
	// two draws of one frame (the normal path reads them at each draw).
	if (!NativeXprValidate(record, frame)) {
		LiveCounters::Add(LiveCounters::XprMissValidate);
		cache.IndexErase(hash);
		const auto owner = cache.records.find(hash);
		NativeXprRetire(std::move(owner->second), true);
		cache.records.erase(owner);
		m_native_xpr_store = true;
		return false;
	}
	if (kyty_local_native_xpr_mode.load(std::memory_order_relaxed) == 2) {
		m_native_xpr_verify = {&record, &draw_state};
		return false;
	}
	const bool emitted = NativeXprEmit(buffer, record, draw_state, *pipeline_it->second, commands, index_base,
	                                   index_bytes, index_type, false, direct != nullptr ? &direct_draw : nullptr);
	if (emitted) LiveCounters::Add(direct != nullptr ? LiveCounters::XprDirectHits : LiveCounters::XprHits);
	return emitted;
}

// Mode 2: the normal path prepared the draw the native path would have emitted
// from `m_native_xpr_verify`; every difference is a record the proofs kept
// although it went stale (or state they do not cover).
void RenderExecutor::NativeXprVerify(CommandBuffer& buffer, const DrawRenderState& state,
                                     const RenderState& rendering, std::span<PreparedBindings* const> stages,
                                     const DrawEmitInfo& emit) {
	const auto pending = m_native_xpr_verify;
	m_native_xpr_verify = {};
	if (pending.record == nullptr) return;
	const auto& record     = *pending.record;
	const auto& draw_state = *pending.state;
	static uint64_t                 verified = 0, mismatches = 0;
	static std::array<uint64_t, 12> kinds {};
	++verified;
	uint32_t mismatch = 0;
	if (stages.size() != record.stage_count) mismatch |= 1u;
	for (uint32_t st = 0; st < record.stage_count && st < stages.size(); ++st) {
		const auto& prepared = *stages[st];
		const auto& stage    = record.stages[st];
		const auto& fresh    = prepared.runtime->resources;
		const auto& layout   = stage.program->bindings;
		if (prepared.runtime->program != stage.program) mismatch |= 2u;
		if (!(fresh.buffers == stage.snapshot.buffers) || !(fresh.images == stage.snapshot.images) ||
		    !(fresh.samplers == stage.snapshot.samplers) || fresh.user_data != stage.snapshot.user_data ||
		    !(fresh.uniform_fill == stage.snapshot.uniform_fill))
			mismatch |= 4u;
		if (fresh.flattened_srt != stage.flattened) mismatch |= 8u;
		// Shader data, except the buffer alignment bytes (streamed slots differ).
		if (prepared.shader_data.size() != stage.shader_data.size()) {
			mismatch |= 16u;
		} else {
			const auto first = layout.memory_offset_dword;
			const auto last  = first + (static_cast<uint32_t>(stage.program->info.buffers.size()) + 3u) / 4u;
			for (uint32_t i = 0; i < prepared.shader_data.size(); ++i)
				if ((i < first || i >= last) && prepared.shader_data[i] != stage.shader_data[i]) mismatch |= 16u;
		}
		if (prepared.images.size() != stage.views.size()) mismatch |= 32u;
		else
			for (size_t i = 0; i < stage.views.size(); ++i)
				if (prepared.images[i].image_view != stage.views[i]) mismatch |= 32u;
		if (prepared.samplers != stage.samplers) mismatch |= 64u;
		for (size_t i = 0; i < prepared.buffer_sources.size(); ++i) {
			const auto& source = prepared.buffer_sources[i];
			if (source.address == 0 || source.size == 0) continue;
			if (std::ranges::none_of(record.buffers, [&](const auto& dep) {
				    return dep.address == source.address && dep.size == source.size;
			    }))
				mismatch |= 128u;
		}
	}
	const auto& a = draw_state.rendering;
	if (a.num_color_attachments != rendering.num_color_attachments || a.width != rendering.width ||
	    a.height != rendering.height || a.num_layers != rendering.num_layers ||
	    a.depth_stencil_attachment.image_view != rendering.depth_stencil_attachment.image_view)
		mismatch |= 256u;
	for (uint32_t i = 0; i < a.num_color_attachments && i < rendering.num_color_attachments; ++i)
		if (a.color_attachments[i].image_view != rendering.color_attachments[i].image_view) mismatch |= 256u;
	if (a.depth_stencil_attachment.image_layout != rendering.depth_stencil_attachment.image_layout)
		mismatch |= 512u;
	const auto dynamic = LocalDrawRecording::Capture(
	    buffer, state.vs_input_info, state.color_info, state.color_count, state.depth_info,
	    m_context.GetGraphics().attachment_feedback_loop_enabled,
	    rendering.depth_stencil_attachment.image_layout == vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
	        ? vk::ImageAspectFlags {vk::ImageAspectFlagBits::eDepth}
	        : vk::ImageAspectFlags {});
	if (std::memcmp(&dynamic, &draw_state.dynamic, sizeof(dynamic)) != 0) mismatch |= 1024u;
	// Direct draws: the offsets the record holds for this state.
	if (emit.state_offsets) {
		const auto offsets = std::ranges::find(record.direct_offsets, m_native_xpr->state_key,
		                                       &NativeXprRecord::DirectOffsets::state);
		if (offsets != record.direct_offsets.end() &&
		    (offsets->vertex != emit.vertex_offset || offsets->instance != emit.first_instance))
			mismatch |= 2048u;
	}
	if (mismatch != 0) {
		++mismatches;
		for (uint32_t bit = 0; bit < kinds.size(); ++bit)
			if (mismatch & (1u << bit)) ++kinds[bit];
		if (mismatches <= 40) {
			std::printf("NATIVE_XPR_VERIFY mismatch=0x%x vs=%016" PRIx64 " ps=%016" PRIx64 "\n", mismatch,
			            record.stages[0].program->shader_hash,
			            record.stage_count > 1 ? record.stages[1].program->shader_hash : 0);
			if (mismatch & 8u)
				for (uint32_t st = 0; st < record.stage_count && st < stages.size(); ++st) {
					const auto& fresh = stages[st]->runtime->resources.flattened_srt;
					const auto& old   = record.stages[st].flattened;
					for (size_t i = 0; i < fresh.size() && i < old.size(); ++i)
						if (fresh[i] != old[i]) {
							std::printf("  flat stage=%u index=%zu record=%08x normal=%08x\n", st, i, old[i],
							            fresh[i]);
							break;
						}
				}
			std::fflush(stdout);
		}
	}
	if (verified % 20000 == 0) {
		std::printf("NATIVE_XPR_VERIFY_SUMMARY verified=%" PRIu64 " mismatches=%" PRIu64, verified, mismatches);
		static constexpr const char* names[] = {"stages", "program", "structure", "flattened", "shader_data", "views",
		                                        "samplers", "buffers", "targets", "depth_layout", "dynamic", "offsets"};
		for (size_t i = 0; i < kinds.size(); ++i) std::printf(" %s=%" PRIu64, names[i], kinds[i]);
		std::printf("\n");
		std::fflush(stdout);
	}
}

void RenderExecutor::NativeXprForgetState() {
	if (m_native_xpr) m_native_xpr->state_key_valid = false;
}
