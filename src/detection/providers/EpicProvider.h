/*
 * SignalBox - detection/providers/EpicProvider.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Real implementation plan (DESIGN.md 1.2 "Epic Games provider"):
 *   1. Read every *.item JSON manifest under
 *      %PROGRAMDATA%\Epic\EpicGamesLauncher\Data\Manifests\.
 *   2. Pull DisplayName, InstallLocation, LaunchExecutable (relative to
 *      InstallLocation - this is the gold case: exact-path match, not
 *      prefix), AppName / MainGameAppName.
 *   3. Skip entries where AppName != MainGameAppName (DLC).
 *   4. Cross-check only (not primary) against
 *      %PROGRAMDATA%\Epic\UnrealEngineLauncher\LauncherInstalled.dat.
 *
 * Needs JSON parsing - see THIRD-PARTY-NOTICES.md's note on vendoring
 * nlohmann/json when this stub becomes real.
 */

#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../IGameProvider.h"

namespace signalbox::detection::providers {

class EpicProvider final : public IGameProvider {
public:
	std::vector<InstalledGame> enumerate() override;
	std::wstring providerId() const override;

	// Last-write-time of %PROGRAMDATA%\Epic\EpicGamesLauncher\Data\
	// Manifests (a single stat() call). Each installed app gets its own
	// .item file directly in that directory, so installing/uninstalling
	// updates the directory's own mtime - see IGameProvider.h.
	std::optional<std::chrono::system_clock::time_point> changeSignal() const override;
};

// Parses one *.item manifest's JSON content into an InstalledGame.
// Exposed here (rather than kept file-static in EpicProvider.cpp) so it
// can be exercised directly against literal or real manifest text
// without touching the filesystem or OBS/Qt's application object -
// project brief testability requirement. Returns std::nullopt for a DLC
// entry (AppName != MainGameAppName, when both are present) or a
// manifest missing DisplayName/InstallLocation/LaunchExecutable.
std::optional<InstalledGame> ParseEpicManifest(std::string_view jsonText);

} // namespace signalbox::detection::providers
