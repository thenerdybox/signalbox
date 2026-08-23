/*
 * SignalBox - detection/DetectedGame.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Result types shared across the detection pipeline:
 *   IGameProvider::enumerate() -> InstalledGame   (what's installed)
 *   InstallIndex + ProcessScanner -> DetectedGame (what's running, matched)
 *
 * These are plain, immutable-by-convention value types so they can be
 * posted across the worker-thread -> Qt-main-thread boundary as snapshots
 * (see DetectionEngine.h) without any shared mutable state. Nothing here
 * touches OBS or Qt headers on purpose - this header must stay includable
 * from the worker thread's translation units with zero UI dependencies.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace signalbox::detection {

// Which store/platform surfaced this install. Xbox/Game Pass is deferred
// to v1.1 (see DESIGN.md 1.2) but the enum reserves a slot so the switch
// statements that will eventually handle it don't need touching twice.
enum class Platform {
	Steam,
	Epic,
	Gog,
	Ubisoft,
	Generic, // Uninstall-registry catch-all (EA app, Battle.net, one-offs)
	Xbox,    // Reserved, not implemented in v1.0.
};

// How much we trust a running-process-to-install match. Drives ranking
// per DESIGN.md 1.4 step 4 (foreground pid > confidence tier > Steam
// RunningAppID corroboration > working set > start time).
enum class Confidence {
	High,   // Exact launchExe match (Epic, GOG), or Steam prefix match.
	Medium, // Ubisoft folder-name-derived display name.
	Low,    // Generic/Uninstall registry catch-all.
};

// One entry from a store's install metadata. Providers (SteamProvider,
// EpicProvider, ...) produce vectors of these; InstallIndex owns the
// merged, deduplicated set. Field semantics mirror DESIGN.md 1.2's
// InstalledGame struct exactly (same name there; kept here under this
// project's naming so it lives next to the rest of the detection types).
struct InstalledGame {
	std::wstring displayName;  // Platform's own name for the game.
	std::wstring installRoot;  // Absolute, normalized directory.
	std::wstring launchExe;    // Absolute path if known (Epic/GOG); else empty.
	Platform platform = Platform::Generic;
	std::wstring platformId;   // appid / catalog id / product id.
};

// A single poll's winning candidate, or std::nullopt-equivalent via
// DetectionEngine's optional wrapper (see DetectionEngine.h) when
// nothing matched. Emitted at most once per poll by InstallIndex's
// matching/ranking pass over ProcessScanner's snapshot.
struct DetectedGame {
	InstalledGame game;
	std::uint32_t pid = 0;
	Confidence confidence = Confidence::Low;
	std::chrono::steady_clock::time_point observedAt{};
};

// Platform <-> stable lowercase token, used by the index cache file's
// "platform" field and by exclusion rules keyed as "steam:431960". Lives
// here rather than in InstallIndex.cpp (where both used to be file-local
// statics) because IndexExclusions.h needs the exact same spelling: an
// exclusion that says "steam:431960" and an index that writes "Steam"
// would silently never match, and nothing would fail loudly enough to
// notice.
inline const wchar_t *PlatformToString(Platform platform)
{
	switch (platform) {
	case Platform::Steam:
		return L"steam";
	case Platform::Epic:
		return L"epic";
	case Platform::Gog:
		return L"gog";
	case Platform::Ubisoft:
		return L"ubisoft";
	case Platform::Xbox:
		return L"xbox";
	case Platform::Generic:
	default:
		return L"generic";
	}
}

inline Platform PlatformFromString(const std::wstring &s)
{
	if (s == L"steam")
		return Platform::Steam;
	if (s == L"epic")
		return Platform::Epic;
	if (s == L"gog")
		return Platform::Gog;
	if (s == L"ubisoft")
		return Platform::Ubisoft;
	if (s == L"xbox")
		return Platform::Xbox;
	return Platform::Generic;
}

} // namespace signalbox::detection
