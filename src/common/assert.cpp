#include "common/assert.h"

#include "common/logging/log.h"
#include "common/subsystems.h"
#include "kytyGitVersion.h"

#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <string>

#if defined(__linux__)
#include <execinfo.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Common {

// RecoverableExitScope: the message of the pending EXIT on this thread.
static thread_local bool        t_recoverable = false;
static thread_local std::string t_recoverable_message;

RecoverableExitScope::RecoverableExitScope() {
	t_recoverable = true;
}

RecoverableExitScope::~RecoverableExitScope() {
	t_recoverable = false;
}

// The raw return addresses of a fatal error (resolve with addr2line against the executable;
// Windows: llvm-symbolizer --obj=kyty_emulator.exe with the build's PDB next to it).
static void WriteFatalBacktrace() {
#if defined(__linux__)
	void*     frames[48];
	const int count = backtrace(frames, 48);
	std::fflush(nullptr);
	(void)!write(STDOUT_FILENO, "--- Backtrace ---\n", 18);
	backtrace_symbols_fd(frames, count, STDOUT_FILENO);
#elif defined(_WIN32)
	void*      frames[48];
	const auto count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
	std::printf("--- Backtrace --- (exe base %p)\n", static_cast<void*>(GetModuleHandleW(nullptr)));
	for (USHORT i = 0; i < count; ++i) std::printf("  %p\n", frames[i]);
	std::fflush(nullptr);
#endif
}

static std::string BuildFatalReport(const char* title, std::string_view text, const char* file,
                                    int line) {
	return fmt::format("--- Build ---\n{}\n{}\n{} in {}:{}\n", KYTY_BUILD_LABEL, title, text, file,
	                   line);
}

static int DbgReport(const char* title, std::string_view text, const char* file, int line) {
	if (t_recoverable) {
		t_recoverable_message = fmt::format("{} in {}:{}", text, file, line);
		return 1;
	}
	Log::WriteFatal(BuildFatalReport(title, text, file, line));
	WriteFatalBacktrace();
	Subsystems::EmergencyShutdownActive();
	return 1;
}

int DbgExitIfHandler(const char* expr, const char* file, int line) {
	return DbgReport("--- Fatal Error ---", fmt::format("Error: condition ({}) is true", expr),
	                 file, line);
}

int DbgNotImplementedHandler(const char* expr, const char* file, int line) {
	return DbgReport("--- Fatal Error ---", fmt::format("Not implemented ({})", expr), file, line);
}

int DbgExitHandler(const char* file, int line, std::string_view text) {
	if (t_recoverable) {
		t_recoverable_message = fmt::format("{} in {}:{}", text, file, line);
		return 1;
	}
	Log::WriteFatal(BuildFatalReport("--- Error ---", text, file, line));
	WriteFatalBacktrace();
	return 1;
}

int DbgExitHandler(const char* file, int line, fmt::text_style style, std::string_view text) {
	if (t_recoverable) {
		t_recoverable_message = fmt::format("{} in {}:{}", text, file, line);
		return 1;
	}
	Log::WriteFatal(style, BuildFatalReport("--- Error ---", text, file, line));
	WriteFatalBacktrace();
	return 1;
}

void DbgExit(int status) {
	if (t_recoverable) {
		throw RecoverableExit {std::move(t_recoverable_message)};
	}
	Subsystems::EmergencyShutdownActive();
	std::fflush(nullptr);
#if defined(_WIN32)
	// Not ExitProcess (std::_Exit): it kills the other threads wherever they are and then runs the DLLs'
	// detach routines on this one, where a driver's waited forever on what a killed thread held (a
	// crashed emulator stayed a one-thread process, unkillable, holding its memory).
	TerminateProcess(GetCurrentProcess(), static_cast<UINT>(status));
#endif
	std::_Exit(status);
}

} // namespace Common
