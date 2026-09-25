#pragma once
// Switches of the native resource preparation paths; performance-switches.h
// sets them from the environment. All default to 0, the original behaviour.
#include <atomic>
#include <cstdint>

extern "C" {
// 1: SRT outputs come from the ahead-of-time compiled linear plan.
extern volatile std::atomic<uint32_t> kyty_local_srt_native_mode;
// 1: reusable preparation storage; contents are always fresh.
extern volatile std::atomic<uint32_t> kyty_local_preparation_scratch_mode;
}
