/*
 * SignalBox - detection/ProcessScanner.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Win32 process enumeration, kept deliberately minimal. See DESIGN.md 1.3.
 *
 * ACCESS RIGHTS - READ THIS BEFORE TOUCHING THE .cpp:
 *   Every OpenProcess call in this file MUST request only
 *   PROCESS_QUERY_LIMITED_INFORMATION. That is the documented minimum
 *   right needed by QueryFullProcessImageNameW, GetExitCodeProcess,
 *   GetProcessTimes, and GetProcessMemoryInfo, and it is the right that
 *   anti-cheat drivers (EAC/BattlEye/Vanguard) do NOT strip via
 *   ObRegisterCallbacks - PROCESS_VM_READ is what they deny, regardless
 *   of elevation. This plugin has no reason to ever request more.
 *
 *   NEVER call ReadProcessMemory, EnumProcessModules, CreateRemoteThread,
 *   or anything else that touches another process's memory or address
 *   space from this file, or anywhere else in this plugin. That is both
 *   the correct engineering boundary (nothing here needs it) and the
 *   anti-cheat-safe one. If a future change seems to need it, it belongs
 *   in a different design, not a patch to this class.
 *
 * THREADING: this class is only ever driven from DetectionEngine's worker
 * thread. It performs no OBS or Qt calls. scan() is synchronous and is
 * expected to cost single-digit milliseconds (a Toolhelp32 snapshot plus
 * a few hundred OpenProcess calls).
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace signalbox::detection {

// One live process, as cheaply resolved as possible. basename comes from
// the Toolhelp32 snapshot (free); fullImagePath requires one OpenProcess +
// QueryFullProcessImageNameW per pid and is only worth paying for after
// the basename has survived the helper/launcher denylist filter (see
// InstallIndex's matching pass, DESIGN.md 1.4 step 3).
struct ProcessInfo {
	std::uint32_t pid = 0;
	std::uint32_t parentPid = 0;
	std::wstring exeBasename;
	std::wstring fullImagePath; // Empty until resolved; see resolveImagePath().
};

// Why a tracked process's exit was observed. Sizes the grace window
// (DetectionStateMachine uses crashGraceS vs cleanExitGraceS) but never
// changes *what* happens on exit, only *when* the fallback fires - see
// DESIGN.md 2.2.
enum class ExitReason {
	Clean, // GetExitCodeProcess returned 0.
	Crash, // Any nonzero code. Heuristic, not perfect - see DESIGN.md 2.2.
};

class ProcessScanner {
public:
	ProcessScanner();
	// TrackedHandle is an incomplete type here (definition lives in the
	// .cpp, next to the <windows.h> HANDLE it wraps) so the special
	// member functions that touch trackedHandles_ must be declared
	// here and defined in ProcessScanner.cpp, not defaulted inline.
	~ProcessScanner();
	ProcessScanner(const ProcessScanner &) = delete;
	ProcessScanner &operator=(const ProcessScanner &) = delete;
	ProcessScanner(ProcessScanner &&) noexcept;
	ProcessScanner &operator=(ProcessScanner &&) noexcept;

	// Snapshot every running process via CreateToolhelp32Snapshot +
	// Process32FirstW/NextW. Cheap - basename only, no handles opened.
	// Worker thread only.
	std::vector<ProcessInfo> scan();

	// Resolve a pid's full image path via
	// OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, ...) +
	// QueryFullProcessImageNameW. Returns std::nullopt on failure
	// (ACCESS_DENIED on protected/system processes is normal - the
	// caller should skip silently, not log a warning). Worker thread
	// only.
	std::optional<std::wstring> resolveImagePath(std::uint32_t pid);

	// Open and hold a PROCESS_QUERY_LIMITED_INFORMATION handle on a
	// pid for exit tracking while it is the ACTIVE game (DESIGN.md
	// 2.2). Caller (DetectionStateMachine, via DetectionEngine) is
	// responsible for closing it when the state machine leaves
	// ACTIVE(g). Returns false if the handle could not be opened.
	bool trackForExit(std::uint32_t pid);

	// Non-blocking check (WaitForSingleObject(h, 0)) on a previously
	// tracked pid. Returns the exit reason once the process has
	// exited, or std::nullopt if it's still running or wasn't tracked.
	std::optional<ExitReason> pollExit(std::uint32_t pid);

	// Release a handle acquired by trackForExit(), e.g. when the state
	// machine moves on without waiting for exit (a different game won
	// during GRACE - DESIGN.md 2.2).
	void stopTracking(std::uint32_t pid);

	// Foreground-window owner pid, via GetForegroundWindow() +
	// GetWindowThreadProcessId(). Tie-breaker ONLY (DESIGN.md 1.4 step
	// 4 / 2.2 "Alt-tab / minimise") - never the primary detection
	// signal. Alt-tabbing to a browser mid-game must not change
	// detection.
	std::optional<std::uint32_t> foregroundPid();

	// Ranking signals for DESIGN.md 1.4 step 4 (DetectionEngine's job to
	// call these over the surviving candidates once the helper/launcher
	// filter has run - see InstallIndex.h's "Does NOT apply ... ranking
	// logic" note). Added alongside the rest of this class rather than
	// left for DetectionEngine to open its own handles, so every
	// PROCESS_QUERY_LIMITED_INFORMATION OpenProcess call in this plugin
	// stays in this one file - see the access-rights note above. Both
	// open and close their own short-lived handle; callers do not need
	// trackForExit() for these.

	// Working-set size in bytes via GetProcessMemoryInfo (documented to
	// accept a PROCESS_QUERY_LIMITED_INFORMATION handle - Microsoft
	// Learn). std::nullopt on any failure (protected process, pid
	// exited between scan() and this call, etc).
	std::optional<std::uint64_t> workingSetSize(std::uint32_t pid);

	// Process start time via GetProcessTimes (also QUERY_LIMITED-
	// compatible). std::nullopt on any failure.
	std::optional<std::chrono::system_clock::time_point> startTime(std::uint32_t pid);

private:
	// Opaque per-pid tracked-handle storage. Implementation detail;
	// deliberately not exposed as HANDLE in this header so callers
	// outside detection/ never need <windows.h>.
	struct TrackedHandle;
	std::vector<TrackedHandle> trackedHandles_;
};

} // namespace signalbox::detection
