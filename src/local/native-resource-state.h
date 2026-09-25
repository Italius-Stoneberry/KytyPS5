#pragma once
// Switches of the native resource preparation paths; performance-switches.h
// sets them from the environment. All default to 0, the original behaviour.
#include <atomic>
#include <cstdint>

extern "C" {
// 1: SRT outputs come from the ahead-of-time compiled linear plan.
extern volatile std::atomic<uint32_t> kyty_local_srt_native_mode;
// 1: a plan with one control-flow condition selects its variant through the
// compiled predicate instead of the interpreter.
extern volatile std::atomic<uint32_t> kyty_local_srt_predicate_mode;
// 1: reusable preparation storage; contents are always fresh.
extern volatile std::atomic<uint32_t> kyty_local_preparation_scratch_mode;
// 1: bounded lookup shortcuts (the first image page, single-mapping backing copies).
extern volatile std::atomic<uint32_t> kyty_local_preparation_lookup_mode;
// 1: remove redundant attachment clearing and the temporary snapshot remap index.
extern volatile std::atomic<uint32_t> kyty_local_preparation_trim_mode;
// 1: semantic shape guard for resource specialization, with reusable miss
// storage and the guarded compiled-permutation shortcut.
extern volatile std::atomic<uint32_t> kyty_local_specialization_guard_mode;
// 1: graphics pipelines through a block-hashed index of the full key.
extern volatile std::atomic<uint32_t> kyty_local_pipeline_index_mode;
// 1: exact copy readbacks served from completed GPU mirrors.
extern volatile std::atomic<uint32_t> kyty_local_copy_feedback_mode;
// 1: a CPU write-window fault hands the image lock over once it holds the region lock,
// so the page protection change runs outside the image lock.
extern volatile std::atomic<uint32_t> kyty_local_write_window_handoff_mode;
}
