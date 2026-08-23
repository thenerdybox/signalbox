/*
 * SignalBox - detection/providers/SteamProvider.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Real implementation plan (DESIGN.md 1.2 "Steam provider"):
 *   1. Find Steam root: HKCU\Software\Valve\Steam value SteamPath
 *      (fallback HKLM\SOFTWARE\WOW6432Node\Valve\Steam value InstallPath).
 *   2. Parse <SteamPath>\steamapps\libraryfolders.vdf (Valve KeyValues
 *      text format) for every library root.
 *   3. For each library, read steamapps\appmanifest_<appid>.acf (same
 *      KeyValues format). Pull "name", "installdir", "StateFlags" from
 *      the "AppState" block; treat StateFlags == 4 as fully installed.
 *   4. installRoot = <library>\steamapps\common\<installdir>\. launchExe
 *      is unknown from the ACF - Steam matches by directory prefix, not
 *      exact exe (confidence HIGH in InstallIndex::match, per DESIGN.md
 *      1.4 step 2).
 *   5. Optional corroborating signal (never primary): HKCU's
 *      RunningAppID DWORD - see DESIGN.md 1.2 "Corroborating signal".
 *
 * A tolerant ~100-line hand-written VDF/ACF (KeyValues) parser is the
 * planned approach (DESIGN.md 1.2) - no third-party dependency for this.
 * It belongs in this .cpp (or a small SteamVdf.h/.cpp pair alongside it
 * if it grows large enough to be worth reusing), not in InstallIndex.
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../IGameProvider.h"

namespace signalbox::detection::providers {

class SteamProvider final : public IGameProvider {
public:
	std::vector<InstalledGame> enumerate() override;
	std::wstring providerId() const override;

	// Last-write-time of <SteamPath>\steamapps (a single stat() call, not
	// a directory walk). Installing/uninstalling a game adds or removes
	// an appmanifest_*.acf directly in that directory, which NTFS
	// reflects in the directory's own mtime, so this is a genuinely
	// useful "did anything change" signal for the primary library.
	// Additional Steam libraries on other drives are not covered by this
	// single check (each would need its own stat) - InstallIndex's
	// timer-based refresh is the backstop for those. See IGameProvider.h.
	std::optional<std::chrono::system_clock::time_point> changeSignal() const override;
};

// Parsing helpers, split out and exposed here (rather than kept
// file-static in SteamProvider.cpp) specifically so they can be
// exercised directly against literal or real on-disk VDF/ACF text
// without touching the registry or OBS - project brief testability
// requirement. enumerate() is the only caller in normal operation.

// Parses <SteamPath>\steamapps\libraryfolders.vdf content and returns
// every library root path it lists (DESIGN.md 1.2). Returns {} (not an
// error) if the document has no recognizable "libraryfolders" block or
// no entries.
std::vector<std::wstring> ParseLibraryFolders(std::string_view vdfText);

// Parses one steamapps\appmanifest_<appid>.acf's content into an
// InstalledGame rooted at libraryRoot (the library folder that contains
// this manifest's steamapps\ directory). Returns std::nullopt if the
// manifest is missing required fields (name/installdir/StateFlags) or
// StateFlags doesn't have the fully-installed bit (4) set - DESIGN.md
// 1.2's "skip apps mid-download" rule.
std::optional<InstalledGame> ParseAppManifest(std::string_view acfText, const std::wstring &libraryRoot);

} // namespace signalbox::detection::providers
