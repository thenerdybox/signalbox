/*
 * SignalBox - detection/providers/SteamProvider.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Real implementation. See SteamProvider.h for the plan and for the
 * ParseLibraryFolders/ParseAppManifest declarations that make the parsing
 * half of this testable independent of the registry/filesystem calls.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "SteamProvider.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "KeyValuesParser.h"

namespace signalbox::detection::providers {

namespace {

namespace fs = std::filesystem;

std::wstring ToLowerCopy(std::wstring s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
	return s;
}

// Normalizes '/' to '\\' and strips a trailing separator. Real
// libraryfolders.vdf files on this machine were observed to list the
// same physical library twice with different separator styles for the
// same path (e.g. "C:/Program Files (x86)/Steam" alongside the registry-
// derived "C:\Program Files (x86)\Steam") - a lowercase-only comparison
// does not catch that, so the dedup key needs this too.
std::wstring NormalizeForDedup(std::wstring s)
{
	std::replace(s.begin(), s.end(), L'/', L'\\');
	while (!s.empty() && s.back() == L'\\') {
		s.pop_back();
	}
	return ToLowerCopy(s);
}

std::wstring Utf8ToWide(const std::string &utf8)
{
	if (utf8.empty()) {
		return {};
	}
	const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
	if (len <= 0) {
		return {};
	}
	std::wstring wide(static_cast<std::size_t>(len), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), len);
	return wide;
}

std::wstring JoinPath(const std::wstring &base, const std::wstring &leaf)
{
	std::wstring b = base;
	while (!b.empty() && (b.back() == L'\\' || b.back() == L'/')) {
		b.pop_back();
	}
	return b + L"\\" + leaf;
}

// RRF_RT_REG_SZ via RegGetValueW - handles both HKCU\...\SteamPath and
// the HKLM WOW6432Node fallback with the same helper (DESIGN.md 1.2).
std::optional<std::wstring> ReadRegistrySz(HKEY root, const std::wstring &subKey, const std::wstring &valueName)
{
	wchar_t buffer[4096];
	DWORD size = sizeof(buffer);
	DWORD type = 0;

	const LONG result = RegGetValueW(root, subKey.c_str(), valueName.c_str(), RRF_RT_REG_SZ, &type, buffer, &size);
	if (result != ERROR_SUCCESS) {
		return std::nullopt;
	}

	std::size_t chars = size / sizeof(wchar_t);
	while (chars > 0 && buffer[chars - 1] == L'\0') {
		--chars;
	}
	return std::wstring(buffer, chars);
}

std::optional<std::wstring> FindSteamPath()
{
	if (auto p = ReadRegistrySz(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath")) {
		return p;
	}
	if (auto p = ReadRegistrySz(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath")) {
		return p;
	}
	return std::nullopt;
}

std::string ReadFileUtf8(const std::wstring &path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return {};
	}

	std::ostringstream buffer;
	buffer << file.rdbuf();
	std::string content = buffer.str();

	// Strip a UTF-8 BOM if present; Steam doesn't normally write one but
	// tolerate it rather than let it corrupt the first token.
	if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
	    static_cast<unsigned char>(content[1]) == 0xBB && static_cast<unsigned char>(content[2]) == 0xBF) {
		content.erase(0, 3);
	}

	return content;
}

} // namespace

std::vector<std::wstring> ParseLibraryFolders(std::string_view vdfText)
{
	std::vector<std::wstring> roots;

	const VdfNode root = ParseKeyValues(vdfText);
	const VdfNode *libraryFolders = root.findChild("libraryfolders");
	if (libraryFolders == nullptr) {
		// Tolerate a document whose top-level IS the libraryfolders
		// contents directly - normal files always wrap it, but this
		// keeps an odd/truncated file from yielding nothing at all.
		libraryFolders = &root;
	}

	for (const auto &entry : libraryFolders->children) {
		if (!entry.isBlock) {
			continue;
		}
		const VdfNode *pathNode = entry.findChild("path");
		if (pathNode == nullptr || pathNode->isBlock || pathNode->value.empty()) {
			continue;
		}
		roots.push_back(Utf8ToWide(pathNode->value));
	}

	return roots;
}

std::optional<InstalledGame> ParseAppManifest(std::string_view acfText, const std::wstring &libraryRoot)
{
	const VdfNode root = ParseKeyValues(acfText);
	const VdfNode *appState = root.findChild("AppState");
	if (appState == nullptr) {
		appState = &root;
	}

	const VdfNode *appidNode = appState->findChild("appid");
	const VdfNode *nameNode = appState->findChild("name");
	const VdfNode *installDirNode = appState->findChild("installdir");
	const VdfNode *stateFlagsNode = appState->findChild("StateFlags");

	if (nameNode == nullptr || nameNode->isBlock || nameNode->value.empty()) {
		return std::nullopt;
	}
	if (installDirNode == nullptr || installDirNode->isBlock || installDirNode->value.empty()) {
		return std::nullopt;
	}
	if (stateFlagsNode == nullptr || stateFlagsNode->isBlock) {
		return std::nullopt;
	}

	unsigned long flags = 0;
	try {
		flags = std::stoul(stateFlagsNode->value);
	} catch (...) {
		return std::nullopt;
	}

	// DESIGN.md 1.2: bit 4 = fully installed. Skip mid-download/
	// validating/update-pending apps - "never guess" extends to not
	// surfacing a half-installed game as a match candidate.
	if ((flags & 4ul) == 0) {
		return std::nullopt;
	}

	InstalledGame game;
	game.displayName = Utf8ToWide(nameNode->value);
	game.installRoot = JoinPath(JoinPath(libraryRoot, L"steamapps\\common"), Utf8ToWide(installDirNode->value)) + L"\\";
	// ACF doesn't record the launch executable - Steam matches by
	// directory prefix in InstallIndex::match(), confidence High.
	game.launchExe.clear();
	game.platform = Platform::Steam;
	game.platformId = (appidNode != nullptr && !appidNode->isBlock) ? Utf8ToWide(appidNode->value) : std::wstring();

	return game;
}

std::vector<InstalledGame> SteamProvider::enumerate()
{
	std::vector<InstalledGame> games;

	const auto steamPath = FindSteamPath();
	if (!steamPath) {
		return games; // Steam not installed - normal, not a fault (IGameProvider.h).
	}

	std::vector<std::wstring> libraryRoots;
	libraryRoots.push_back(*steamPath);

	const std::wstring libraryFoldersVdf = JoinPath(JoinPath(*steamPath, L"steamapps"), L"libraryfolders.vdf");
	const std::string vdfText = ReadFileUtf8(libraryFoldersVdf);
	if (!vdfText.empty()) {
		for (auto &lib : ParseLibraryFolders(vdfText)) {
			libraryRoots.push_back(std::move(lib));
		}
	}

	// De-dupe case-insensitively - libraryfolders.vdf normally lists the
	// main Steam install itself as one of its own numbered entries.
	std::vector<std::wstring> uniqueRoots;
	for (auto &candidate : libraryRoots) {
		const std::wstring key = NormalizeForDedup(candidate);
		const bool alreadyPresent =
			std::any_of(uniqueRoots.begin(), uniqueRoots.end(),
				    [&key](const std::wstring &existing) { return NormalizeForDedup(existing) == key; });
		if (!alreadyPresent) {
			uniqueRoots.push_back(candidate);
		}
	}

	for (const auto &library : uniqueRoots) {
		const std::wstring steamappsDir = JoinPath(library, L"steamapps");

		std::error_code ec;
		if (!fs::exists(steamappsDir, ec) || ec) {
			continue;
		}

		fs::directory_iterator it(steamappsDir, fs::directory_options::skip_permission_denied, ec);
		if (ec) {
			continue;
		}

		for (const auto &entry : it) {
			std::error_code entryEc;
			if (!entry.is_regular_file(entryEc) || entryEc) {
				continue;
			}

			const std::wstring lowerName = ToLowerCopy(entry.path().filename().wstring());
			if (lowerName.rfind(L"appmanifest_", 0) != 0) {
				continue;
			}
			if (lowerName.size() < 4 || lowerName.compare(lowerName.size() - 4, 4, L".acf") != 0) {
				continue;
			}

			const std::string acfText = ReadFileUtf8(entry.path().wstring());
			if (acfText.empty()) {
				continue;
			}

			if (auto game = ParseAppManifest(acfText, library)) {
				games.push_back(std::move(*game));
			}
		}
	}

	return games;
}

std::wstring SteamProvider::providerId() const
{
	return L"steam";
}

std::optional<std::chrono::system_clock::time_point> SteamProvider::changeSignal() const
{
	const auto steamPath = FindSteamPath();
	if (!steamPath) {
		return std::nullopt;
	}

	const std::wstring steamappsDir = JoinPath(*steamPath, L"steamapps");

	std::error_code ec;
	const auto fsTime = fs::last_write_time(steamappsDir, ec);
	if (ec) {
		return std::nullopt;
	}

	// std::filesystem::file_time_type -> system_clock::time_point.
	// clock_cast is C++20; C++17 needs the manual duration-based
	// conversion below (this project targets C++17 - see .clang-format).
	const auto sysNow = std::chrono::system_clock::now();
	const auto fsNow = fs::file_time_type::clock::now();
	const auto delta = fsTime - fsNow;
	return sysNow + std::chrono::duration_cast<std::chrono::system_clock::duration>(delta);
}

} // namespace signalbox::detection::providers
