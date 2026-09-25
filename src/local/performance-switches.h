#pragma once
// Runtime switches of the performance paths. Every path keeps the original
// behaviour unless its environment variable is set; the GPU thread applies them
// once, before it consumes commands. Values outside a switch's range abort.

#include "native-buffer-residency.h"
#include "native-resource-state.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <string>

extern "C" {
extern volatile std::atomic_uint32_t kyty_local_binding_scratch_mode;
extern volatile std::atomic_uint32_t kyty_local_draw_run_ranges_mode;
}

inline void InitializePerformanceSwitches() {
	struct Switch {
		const char*                     environment;
		volatile std::atomic_uint32_t* value;
		uint32_t                        minimum = 0;
		uint32_t                        maximum = 1;
	};
	const std::array switches {
	    // Shader resource preparation.
	    Switch {"KYTY_SRT_NATIVE", &kyty_local_srt_native_mode},
	    Switch {"KYTY_SRT_PREDICATES", &kyty_local_srt_predicate_mode},
	    Switch {"KYTY_PREPARATION_SCRATCH", &kyty_local_preparation_scratch_mode},
	    Switch {"KYTY_PREPARATION_LOOKUP", &kyty_local_preparation_lookup_mode},
	    Switch {"KYTY_PREPARATION_TRIM", &kyty_local_preparation_trim_mode},
	    Switch {"KYTY_SPECIALIZATION_GUARD", &kyty_local_specialization_guard_mode},
	    Switch {"KYTY_PIPELINE_INDEX", &kyty_local_pipeline_index_mode},
	    Switch {"KYTY_BINDING_SCRATCH", &kyty_local_binding_scratch_mode},
	    Switch {"KYTY_DRAW_RUN_RANGES", &kyty_local_draw_run_ranges_mode},
	    // Buffers and guest memory.
	    Switch {"KYTY_BUFFER_RESIDENCY", &kyty_local_buffer_residency_mode},
	    Switch {"KYTY_COPY_FEEDBACK", &kyty_local_copy_feedback_mode},
	};
	std::string enabled;
	for (const auto& setting: switches) {
		const auto* text = std::getenv(setting.environment);
		if (text == nullptr) {
			continue;
		}
		char*      end   = nullptr;
		const auto value = std::strtoul(text, &end, 10);
		if (*text == '\0' || *end != '\0' || value < setting.minimum || value > setting.maximum) {
			std::fprintf(stderr, "%s must be a value from %u to %u\n", setting.environment,
			             setting.minimum, setting.maximum);
			std::abort();
		}
		setting.value->store(static_cast<uint32_t>(value), std::memory_order_relaxed);
		enabled += std::string(enabled.empty() ? "" : " ") + setting.environment + "=" + text;
	}
	// External tools find the render thread by this name (for example to pin it).
	(void)pthread_setname_np(pthread_self(), "Kyty.Gpu");
	if (!enabled.empty()) {
		std::printf("Performance switches: %s\n", enabled.c_str());
		std::fflush(stdout);
	}
}
