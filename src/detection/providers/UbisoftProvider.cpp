/*
 * SignalBox - detection/providers/UbisoftProvider.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Real implementation. See UbisoftProvider.h for the plan.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "UbisoftProvider.h"

#include <windows.h>

#include <iterator>
#include <optional>

namespace signalbox::detection::providers {

namespace {

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

// DESIGN.md 1.2 "Ubisoft Connect provider": InstallDir has no name field
// alongside it, so the last non-empty path component becomes the
// fallback displayName (e.g. "...\games\Anno 1800\" -> "Anno 1800").
// Ubisoft install folders are human-named in practice, so this is
// acceptable input to the resolver; the user-override tier is the
// documented backstop for misses.
std::wstring LastPathComponent(const std::wstring &path)
{
	std::wstring trimmed = path;
	while (!trimmed.empty() && (trimmed.back() == L'\\' || trimmed.back() == L'/')) {
		trimmed.pop_back();
	}

	const std::size_t sep = trimmed.find_last_of(L"\\/");
	if (sep == std::wstring::npos) {
		return trimmed;
	}
	return trimmed.substr(sep + 1);
}

} // namespace

std::vector<InstalledGame> UbisoftProvider::enumerate()
{
	std::vector<InstalledGame> games;

	HKEY installsKey = nullptr;
	const LONG openResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Ubisoft\\Launcher\\Installs", 0,
					       KEY_READ | KEY_WOW64_32KEY, &installsKey);
	if (openResult != ERROR_SUCCESS) {
		return games; // Ubisoft Connect not installed - normal, not a fault.
	}

	DWORD index = 0;
	wchar_t subKeyName[256];

	for (;;) {
		DWORD nameLen = static_cast<DWORD>(std::size(subKeyName));
		const LONG enumResult =
			RegEnumKeyExW(installsKey, index, subKeyName, &nameLen, nullptr, nullptr, nullptr, nullptr);
		if (enumResult == ERROR_NO_MORE_ITEMS) {
			break;
		}
		if (enumResult != ERROR_SUCCESS) {
			break;
		}
		++index;

		HKEY installKey = nullptr;
		if (RegOpenKeyExW(installsKey, subKeyName, 0, KEY_READ | KEY_WOW64_32KEY, &installKey) != ERROR_SUCCESS) {
			continue;
		}

		const auto installDir = ReadValueSz(installKey, L"InstallDir");
		RegCloseKey(installKey);

		if (!installDir || installDir->empty()) {
			continue;
		}

		InstalledGame game;
		game.installRoot = EnsureTrailingBackslash(*installDir);
		game.displayName = LastPathComponent(*installDir);
		game.launchExe.clear(); // Not available from this registry key.
		game.platform = Platform::Ubisoft;
		game.platformId = subKeyName;

		games.push_back(std::move(game));
	}

	RegCloseKey(installsKey);
	return games;
}

std::wstring UbisoftProvider::providerId() const
{
	return L"ubisoft";
}

} // namespace signalbox::detection::providers
