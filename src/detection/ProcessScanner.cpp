/*
 * SignalBox - detection/ProcessScanner.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Real Win32 implementation. See ProcessScanner.h for the access-rights
 * rule this whole file exists to enforce (PROCESS_QUERY_LIMITED_INFORMATION
 * only, never PROCESS_VM_READ) and DESIGN.md Section 1.3 for the API
 * choices:
 *   scan()             -> CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS) +
 *                          Process32FirstW/NextW
 *   resolveImagePath() -> OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) +
 *                          QueryFullProcessImageNameW
 *   trackForExit/pollExit -> keep the QUERY_LIMITED handle open,
 *                          WaitForSingleObject(h, 0) + GetExitCodeProcess
 *   foregroundPid()    -> GetForegroundWindow() + GetWindowThreadProcessId()
 *   workingSetSize()   -> GetProcessMemoryInfo (K32GetProcessMemoryInfo -
 *                          exported directly by kernel32.dll since Win7,
 *                          no separate Psapi.lib link needed)
 *   startTime()        -> GetProcessTimes
 *
 * Do not add a <windows.h> include to ProcessScanner.h - it stays here,
 * so nothing outside this file needs to deal with HANDLE/DWORD.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ProcessScanner.h"

#include <windows.h>

#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <iterator>

namespace signalbox::detection {

// Real definition of the incomplete type declared in the header: one
// PROCESS_QUERY_LIMITED_INFORMATION handle held open per tracked pid,
// for exit polling while that pid is the DetectionStateMachine's
// ACTIVE(g) (DESIGN.md 2.2).
struct ProcessScanner::TrackedHandle {
	std::uint32_t pid = 0;
	HANDLE handle = nullptr;
};

namespace {

// Converts a Win32 FILETIME (100ns intervals since 1601-01-01 UTC) to
// std::chrono::system_clock::time_point (epoch 1970-01-01 UTC).
std::chrono::system_clock::time_point FileTimeToSystemClock(const FILETIME &ft)
{
	ULARGE_INTEGER uli;
	uli.LowPart = ft.dwLowDateTime;
	uli.HighPart = ft.dwHighDateTime;

	// Number of 100ns intervals between the Windows epoch (1601-01-01)
	// and the Unix epoch (1970-01-01) - a well-known constant.
	constexpr std::uint64_t kWindowsToUnixEpoch100ns = 116444736000000000ULL;
	const std::uint64_t unix100ns =
		(uli.QuadPart > kWindowsToUnixEpoch100ns) ? (uli.QuadPart - kWindowsToUnixEpoch100ns) : 0;

	const auto duration = std::chrono::duration<std::uint64_t, std::ratio<1, 10000000>>(unix100ns);
	return std::chrono::system_clock::time_point(std::chrono::duration_cast<std::chrono::system_clock::duration>(duration));
}

} // namespace

ProcessScanner::ProcessScanner() = default;

ProcessScanner::~ProcessScanner()
{
	for (auto &tracked : trackedHandles_) {
		if (tracked.handle != nullptr && tracked.handle != INVALID_HANDLE_VALUE) {
			CloseHandle(tracked.handle);
		}
	}
}

ProcessScanner::ProcessScanner(ProcessScanner &&other) noexcept : trackedHandles_(std::move(other.trackedHandles_))
{
	other.trackedHandles_.clear();
}

ProcessScanner &ProcessScanner::operator=(ProcessScanner &&other) noexcept
{
	if (this != &other) {
		for (auto &tracked : trackedHandles_) {
			if (tracked.handle != nullptr && tracked.handle != INVALID_HANDLE_VALUE) {
				CloseHandle(tracked.handle);
			}
		}
		trackedHandles_ = std::move(other.trackedHandles_);
		other.trackedHandles_.clear();
	}
	return *this;
}

std::vector<ProcessInfo> ProcessScanner::scan()
{
	std::vector<ProcessInfo> result;

	const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		return result;
	}

	PROCESSENTRY32W entry{};
	entry.dwSize = sizeof(entry);

	if (Process32FirstW(snapshot, &entry)) {
		do {
			ProcessInfo info;
			info.pid = entry.th32ProcessID;
			info.parentPid = entry.th32ParentProcessID;
			info.exeBasename = entry.szExeFile;
			result.push_back(std::move(info));
		} while (Process32NextW(snapshot, &entry));
	}

	CloseHandle(snapshot);
	return result;
}

std::optional<std::wstring> ProcessScanner::resolveImagePath(std::uint32_t pid)
{
	const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (h == nullptr) {
		// ACCESS_DENIED on protected/system processes is normal - the
		// caller skips silently, never logs a warning (ProcessScanner.h).
		return std::nullopt;
	}

	// Long-path-safe buffer; QueryFullProcessImageNameW wants the size
	// in characters, including room for the terminator.
	wchar_t buffer[32768];
	DWORD size = static_cast<DWORD>(std::size(buffer));
	const BOOL ok = QueryFullProcessImageNameW(h, 0, buffer, &size);
	CloseHandle(h);

	if (!ok) {
		return std::nullopt;
	}
	return std::wstring(buffer, size);
}

bool ProcessScanner::trackForExit(std::uint32_t pid)
{
	const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (h == nullptr) {
		return false;
	}

	// Replace any stale handle already tracked for this pid (defensive -
	// callers are expected to stopTracking() before re-tracking, but a
	// leaked duplicate here would leak a handle silently otherwise).
	stopTracking(pid);

	TrackedHandle tracked;
	tracked.pid = pid;
	tracked.handle = h;
	trackedHandles_.push_back(tracked);
	return true;
}

std::optional<ExitReason> ProcessScanner::pollExit(std::uint32_t pid)
{
	const auto it = std::find_if(trackedHandles_.begin(), trackedHandles_.end(),
				      [pid](const TrackedHandle &t) { return t.pid == pid; });
	if (it == trackedHandles_.end()) {
		return std::nullopt; // Not tracked.
	}

	const DWORD waitResult = WaitForSingleObject(it->handle, 0);
	if (waitResult != WAIT_OBJECT_0) {
		// WAIT_TIMEOUT (still running) or WAIT_FAILED - either way,
		// exit hasn't been observed yet.
		return std::nullopt;
	}

	DWORD exitCode = 0;
	ExitReason reason = ExitReason::Crash;
	if (GetExitCodeProcess(it->handle, &exitCode)) {
		// DESIGN.md 2.2: 0 -> clean quit, anything else -> treat as a
		// crash for grace-window sizing purposes. Heuristic, not
		// perfect (some engines exit nonzero routinely) - it only
		// changes *when* fallback fires, never *what* happens.
		reason = (exitCode == 0) ? ExitReason::Clean : ExitReason::Crash;
	}

	CloseHandle(it->handle);
	trackedHandles_.erase(it);
	return reason;
}

void ProcessScanner::stopTracking(std::uint32_t pid)
{
	const auto it = std::find_if(trackedHandles_.begin(), trackedHandles_.end(),
				      [pid](const TrackedHandle &t) { return t.pid == pid; });
	if (it != trackedHandles_.end()) {
		if (it->handle != nullptr && it->handle != INVALID_HANDLE_VALUE) {
			CloseHandle(it->handle);
		}
		trackedHandles_.erase(it);
	}
}

std::optional<std::uint32_t> ProcessScanner::foregroundPid()
{
	const HWND fg = GetForegroundWindow();
	if (fg == nullptr) {
		return std::nullopt;
	}

	DWORD pid = 0;
	if (GetWindowThreadProcessId(fg, &pid) == 0 || pid == 0) {
		return std::nullopt;
	}
	return static_cast<std::uint32_t>(pid);
}

std::optional<std::uint64_t> ProcessScanner::workingSetSize(std::uint32_t pid)
{
	const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (h == nullptr) {
		return std::nullopt;
	}

	PROCESS_MEMORY_COUNTERS counters{};
	counters.cb = sizeof(counters);
	const BOOL ok = K32GetProcessMemoryInfo(h, &counters, sizeof(counters));
	CloseHandle(h);

	if (!ok) {
		return std::nullopt;
	}
	return static_cast<std::uint64_t>(counters.WorkingSetSize);
}

std::optional<std::chrono::system_clock::time_point> ProcessScanner::startTime(std::uint32_t pid)
{
	const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (h == nullptr) {
		return std::nullopt;
	}

	FILETIME creation{}, exit{}, kernel{}, user{};
	const BOOL ok = GetProcessTimes(h, &creation, &exit, &kernel, &user);
	CloseHandle(h);

	if (!ok) {
		return std::nullopt;
	}
	return FileTimeToSystemClock(creation);
}

} // namespace signalbox::detection
