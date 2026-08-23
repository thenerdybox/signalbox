/*
 * SignalBox - detection/IGameProvider.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * One interface, one implementation per store: SteamProvider, EpicProvider,
 * GogProvider, UbisoftProvider, GenericUninstallProvider (registry
 * Uninstall-key catch-all). InstallIndex owns a collection of these and
 * merges their output; see InstallIndex.h.
 *
 * THREADING: enumerate() does registry/file I/O. It is only ever called
 * from DetectionEngine's worker thread (index rebuild at load, every 10
 * minutes, and on demand from the dock's "Rescan" button, which posts a
 * request to the worker thread rather than calling this directly - see
 * DetectionEngine.h). Implementations must not touch OBS or Qt objects.
 *
 * FAILURE MODE: per DESIGN.md Section 7 risk 5, providers must fail
 * independently. If a provider's enumerate() cannot read its source
 * (missing registry key, corrupt manifest, store not installed), it
 * returns an empty vector - never throws, never brings down the index
 * rebuild for other providers. Log via obs_log at LOG_DEBUG, not
 * LOG_WARNING/LOG_ERROR - "GOG not installed" is normal, not a fault.
 */

#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "DetectedGame.h"

namespace signalbox::detection {

class IGameProvider {
public:
	virtual ~IGameProvider() = default;

	// Enumerate every installed game this provider knows about right
	// now. Must be tolerant of a missing/absent store (return {}, not
	// an error) and of partially-installed entries (e.g. Steam apps
	// mid-download - see SteamProvider's StateFlags check). Must not
	// block on network I/O; everything here is local registry/file
	// reads only.
	//
	// PERFORMANCE: this is real file/registry I/O - potentially
	// hundreds of small reads (every appmanifest_*.acf, every Epic
	// .item, walking the Uninstall registry). It is NOT meant to be
	// called on every detection poll; InstallIndex::rebuild() (which
	// calls this once per provider) is only invoked at plugin load, on
	// an infrequent timer, or on an explicit user-requested rescan -
	// never from the per-poll hot path. See InstallIndex.h's caching/
	// staleness-check machinery, which exists specifically so a rebuild
	// only happens when something is actually likely to have changed.
	virtual std::vector<InstalledGame> enumerate() = 0;

	// Short stable identifier for logging and for the "platform:id"
	// key used by CategoryResolver's user-override tier (DESIGN.md
	// 1.5 tier 1), e.g. "steam", "epic", "gog", "ubisoft", "generic".
	virtual std::wstring providerId() const = 0;

	// Cheap on-disk change signal: the latest last-write-time across
	// the directory/directories this provider's enumerate() reads from
	// (e.g. Steam's steamapps folder, Epic's Manifests folder), obtained
	// via a plain stat()-class call - never a directory walk or file
	// parse. InstallIndex uses this to skip a full rebuild when nothing
	// has changed since the index was last built (the "prefer a cheap
	// change signal over a timer" requirement).
	//
	// Returns std::nullopt when no cheap signal exists for this
	// provider's source (e.g. registry-only providers like GOG/Ubisoft/
	// Generic, where "last modified" isn't a single cheap stat call) -
	// InstallIndex falls back to its timer-based refresh for those.
	// Default implementation returns std::nullopt so this is additive
	// and does not force every provider to implement it.
	virtual std::optional<std::chrono::system_clock::time_point> changeSignal() const { return std::nullopt; }
};

} // namespace signalbox::detection
