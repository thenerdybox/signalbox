/*
 * SignalBox - detection/providers/GogProvider.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Real implementation. See GogProvider.h for the plan.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "GogProvider.h"

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

// Enumerates HKLM\<gamesKeyPath>\<productId> subkeys and appends one
// InstalledGame per subkey that has both gameName and path
// (DESIGN.md 1.2 "GOG provider"). samFlags selects the registry view
// (KEY_WOW64_32KEY or KEY_WOW64_64KEY) so both the 32- and 64-bit
// GOG.com\Games hives are covered regardless of this plugin's own
// bitness.
void EnumerateGamesKey(const wchar_t *gamesKeyPath, REGSAM samFlags, std::vector<InstalledGame> &out)
{
	HKEY gamesKey = nullptr;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, gamesKeyPath, 0, KEY_READ | samFlags, &gamesKey) != ERROR_SUCCESS) {
		return; // Key absent - GOG not installed (or this view has no entries). Not a fault.
	}

	DWORD index = 0;
	wchar_t subKeyName[256];

	for (;;) {
		DWORD nameLen = static_cast<DWORD>(std::size(subKeyName));
		const LONG enumResult = RegEnumKeyExW(gamesKey, index, subKeyName, &nameLen, nullptr, nullptr, nullptr, nullptr);
		if (enumResult == ERROR_NO_MORE_ITEMS) {
			break;
		}
		if (enumResult != ERROR_SUCCESS) {
			break;
		}
		++index;

		HKEY productKey = nullptr;
		if (RegOpenKeyExW(gamesKey, subKeyName, 0, KEY_READ | samFlags, &productKey) != ERROR_SUCCESS) {
			continue;
		}

		const auto gameName = ReadValueSz(productKey, L"gameName");
		const auto path = ReadValueSz(productKey, L"path");
		const auto exe = ReadValueSz(productKey, L"exe");

		RegCloseKey(productKey);

		if (!gameName || gameName->empty() || !path || path->empty()) {
			continue;
		}

		InstalledGame game;
		game.displayName = *gameName;
		game.installRoot = EnsureTrailingBackslash(*path);
		// exe (DESIGN.md 1.2) is a full path, exact-match case like
		// Epic's LaunchExecutable - confidence High in InstallIndex::match().
		game.launchExe = exe.value_or(std::wstring());
		game.platform = Platform::Gog;
		game.platformId = subKeyName;

		out.push_back(std::move(game));
	}

	RegCloseKey(gamesKey);
}

} // namespace

std::vector<InstalledGame> GogProvider::enumerate()
{
	std::vector<InstalledGame> games;

	// Both registry views (DESIGN.md 1.2: "enumerate both with
	// KEY_WOW64_32KEY / KEY_WOW64_64KEY"). Real-world GOG installs are
	// written under WOW6432Node by the (32-bit) GOG Galaxy client, but
	// checking both views is cheap and future-proof.
	EnumerateGamesKey(L"SOFTWARE\\WOW6432Node\\GOG.com\\Games", KEY_WOW64_32KEY, games);
	EnumerateGamesKey(L"SOFTWARE\\GOG.com\\Games", KEY_WOW64_64KEY, games);

	return games;
}

std::wstring GogProvider::providerId() const
{
	return L"gog";
}

} // namespace signalbox::detection::providers
