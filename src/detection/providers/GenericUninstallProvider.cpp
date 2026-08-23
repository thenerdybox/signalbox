/*
 * SignalBox - detection/providers/GenericUninstallProvider.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Real implementation. See GenericUninstallProvider.h for the plan.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "GenericUninstallProvider.h"

#include <windows.h>

#include <filesystem>
#include <iterator>
#include <optional>
#include <system_error>

namespace signalbox::detection::providers {

namespace {

namespace fs = std::filesystem;

std::optional<std::wstring> ReadValueSz(HKEY key, const wchar_t *valueName)
{
	wchar_t buffer[4096];
	DWORD size = sizeof(buffer);
	DWORD type = 0;

	const LONG result = RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &size);
	if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
		return std::nullopt;
	}

	std::size_t chars = size / sizeof(wchar_t);
	while (chars > 0 && buffer[chars - 1] == L'\0') {
		--chars;
	}
	return std::wstring(buffer, chars);
}

std::wstring EnsureTrailingBackslash(std::wstring path)
{
	if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
		path.push_back(L'\\');
	}
	return path;
}

// One Uninstall hive to walk: a root (HKLM/HKCU) + subkey path + the
// registry-view flag needed to reach it deterministically regardless of
// this plugin's own bitness. DESIGN.md 1.2: "enumerate ... the WOW6432Node
// twin, and the HKCU equivalents."
struct UninstallHive {
	HKEY root;
	const wchar_t *subKey;
	REGSAM samFlags;
};

// Not constexpr: HKEY_LOCAL_MACHINE/HKEY_CURRENT_USER expand to a
// reinterpret-cast-style integer-to-pointer conversion (see winreg.h),
// which MSVC rejects in a constant expression (C2131). Plain const
// (runtime-initialized, once, at namespace scope) is the correct fit.
const UninstallHive kHives[] = {
	{HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", KEY_WOW64_64KEY},
	{HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall", KEY_WOW64_32KEY},
	{HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", KEY_WOW64_64KEY},
};

void EnumerateHive(const UninstallHive &hive, std::vector<InstalledGame> &out)
{
	HKEY uninstallKey = nullptr;
	if (RegOpenKeyExW(hive.root, hive.subKey, 0, KEY_READ | hive.samFlags, &uninstallKey) != ERROR_SUCCESS) {
		return;
	}

	DWORD index = 0;
	wchar_t subKeyName[256];

	for (;;) {
		DWORD nameLen = static_cast<DWORD>(std::size(subKeyName));
		const LONG enumResult =
			RegEnumKeyExW(uninstallKey, index, subKeyName, &nameLen, nullptr, nullptr, nullptr, nullptr);
		if (enumResult == ERROR_NO_MORE_ITEMS) {
			break;
		}
		if (enumResult != ERROR_SUCCESS) {
			break;
		}
		++index;

		HKEY entryKey = nullptr;
		if (RegOpenKeyExW(uninstallKey, subKeyName, 0, KEY_READ | hive.samFlags, &entryKey) != ERROR_SUCCESS) {
			continue;
		}

		const auto displayName = ReadValueSz(entryKey, L"DisplayName");
		const auto installLocation = ReadValueSz(entryKey, L"InstallLocation");

		RegCloseKey(entryKey);

		// DESIGN.md 1.2: include only if InstallLocation is non-empty
		// AND exists on disk - this is the low-priority catch-all, so
		// it errs toward excluding noise (uninstall entries with no
		// location, or a stale location left over after a manual
		// delete) rather than padding the index with junk.
		if (!displayName || displayName->empty() || !installLocation || installLocation->empty()) {
			continue;
		}

		std::error_code ec;
		if (!fs::exists(*installLocation, ec) || ec) {
			continue;
		}

		InstalledGame game;
		game.displayName = *displayName;
		game.installRoot = EnsureTrailingBackslash(*installLocation);
		game.launchExe.clear(); // Uninstall keys don't record the game's exe.
		game.platform = Platform::Generic;
		game.platformId = subKeyName;

		out.push_back(std::move(game));
	}

	RegCloseKey(uninstallKey);
}

} // namespace

std::vector<InstalledGame> GenericUninstallProvider::enumerate()
{
	std::vector<InstalledGame> games;

	for (const auto &hive : kHives) {
		EnumerateHive(hive, games);
	}

	return games;
}

std::wstring GenericUninstallProvider::providerId() const
{
	return L"generic";
}

} // namespace signalbox::detection::providers
