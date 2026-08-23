/*
 * SignalBox - detection/InstallIndex.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Owns the collection of IGameProvider implementations, merges their
 * enumerate() output into one index, and performs the matching/ranking
 * pass described in DESIGN.md 1.4: given a resolved running-process image
 * path, find the best InstalledGame it belongs to (or none).
 *
 * ============================================================================
 * PERFORMANCE CONTRACT - "lightweight is the target, not the goal"
 * ============================================================================
 * rebuild() is real file/registry I/O - potentially hundreds of reads
 * across every store. It must NEVER be called on the per-poll detection
 * cadence (every few seconds); the per-poll hot path is process
 * enumeration + match() against whatever the index already holds, full
 * stop. rebuild() belongs only at plugin load, on an infrequent timer
 * (DESIGN.md: every 10 minutes), or on an explicit user-requested rescan.
 * This class provides three things specifically to keep that cheap and to
 * make the timer itself smarter than a blind interval:
 *   - hasCheapChangeSignal(): a stat()-class check (no enumeration, no
 *     parsing) the caller can poll far more often than it would ever want
 *     to actually rebuild, to decide "is this worth a real rebuild now."
 *   - saveToFile()/loadFromFile(): persists the merged index (name, root,
 *     exe, platform, id only - see InstalledGame) so a plugin restart can
 *     serve matches immediately instead of blocking on a full rebuild.
 *   - lastRebuildStats(): wall-clock duration and game count from the most
 *     recent rebuild(), so real numbers can be logged (by the OBS-aware
 *     caller - this class never calls obs_log itself, see below) and
 *     re-measured later rather than assumed.
 * None of this changes match()'s cost, which was already O(index size)
 * pointer comparisons over an in-memory vector - the thing actually worth
 * protecting is rebuild()'s call frequency, not match()'s per-call cost.
 *
 * THREADING: rebuild(), match(), saveToFile(), loadFromFile(), and
 * hasCheapChangeSignal() are only ever called from DetectionEngine's
 * worker thread. The pieces that may be read from elsewhere (dock
 * display) are exposed only as thread-safe snapshots - indexedGameCount()
 * and lastRebuildStats() - never as a live reference. See DESIGN.md 4.3.
 * This class calls no obs_* API - resolving the actual cache file path
 * (obs_module_config_path-derived) and logging rebuild stats at
 * LOG_DEBUG is the OBS-aware caller's job, consistent with
 * DetectionEngine.h's threading-boundary note.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "DetectedGame.h"
#include "IGameProvider.h"
#include "IndexExclusions.h"

namespace signalbox::detection {

// Wall-clock facts about the most recent rebuild(), for logging/
// diagnostics only - never used for matching logic itself.
struct RebuildStats {
	double durationMs = 0.0;
	std::size_t gameCount = 0;
	std::chrono::system_clock::time_point completedAt{};

	// How many entries the providers produced that never made it into
	// the index, split by why. Diagnostics only - but they are what the
	// dock's "Rescan" button reports back, which is the difference
	// between a button that visibly did something and the one that
	// looked broken because the count happened not to change.
	std::size_t excludedCount = 0;  // Dropped by IndexExclusions.
	std::size_t duplicateCount = 0; // Same install root surfaced by two providers.
};

class InstallIndex {
public:
	InstallIndex();
	~InstallIndex();

	// Registers a provider. Call once per provider at construction
	// time (see DetectionEngine's constructor for the standard set:
	// Steam, Epic, GOG, Ubisoft, GenericUninstall).
	void addProvider(std::unique_ptr<IGameProvider> provider);

	// Installs the "never a game" rules applied on ingest - see
	// IndexExclusions.h. Call before the FOLLOWING rebuild()/loadFromFile();
	// entries already in the index are not retroactively re-filtered,
	// because both ingest paths run the filter themselves and there is
	// no third way for an entry to appear.
	//
	// Worker thread only. Called once at worker startup, and once more
	// per user-requested rescan (DetectionEngine::workerLoop() re-reads
	// helpers.json on the button press specifically, so a mid-session
	// edit to its exclusions takes effect without an OBS restart - see
	// that file's rescanRequested handling) - always immediately followed
	// by a rebuild() on the same call, same thread, so this deliberately
	// takes no lock: exclusions_ is never read from the Qt main thread.
	void setExclusions(IndexExclusions exclusions);

	// Re-enumerates every registered provider and replaces the index.
	// Providers fail independently (DESIGN.md Section 7 risk 5) - one
	// provider throwing or returning {} must not prevent the others
	// from contributing. Worker thread only. Records timing/count into
	// lastRebuildStats() - see the performance-contract note above for
	// why this exists and how sparingly this should be called.
	void rebuild();

	// DESIGN.md 1.4 matching pass: given a resolved absolute image
	// path, find the InstalledGame it belongs to via exact launchExe
	// match (HIGH confidence) or longest-prefix installRoot match
	// (confidence by platform). Returns std::nullopt if the path
	// doesn't fall under any indexed install root. Does NOT apply the
	// helper/launcher denylist filter or foreground/ranking logic -
	// that's DetectionEngine's job, operating over every currently
	// running process's match() result. Worker thread only. This is
	// the per-poll hot-path call - cheap in-memory comparisons over
	// whatever rebuild() last produced, no I/O.
	std::optional<std::pair<InstalledGame, Confidence>> match(const std::wstring &imagePath) const;

	// Thread-safe read of how many games are currently indexed, for
	// UI display (e.g. dock "Rescan" button subtitle). Safe to call
	// from the Qt main thread.
	std::size_t indexedGameCount() const;

	// Cheap "is a real rebuild likely worth doing" check: true if any
	// registered provider's changeSignal() (a single stat() call - see
	// IGameProvider.h) reports a modification time newer than the
	// index's last successful rebuild. Providers with no cheap signal
	// (registry-only: GOG/Ubisoft/Generic) are silently skipped here -
	// they rely on the caller's timer instead. Returns false (not "no
	// signal available") when the index has never been built, so the
	// very first check doesn't accidentally suppress the initial load's
	// rebuild - callers always do an unconditional rebuild() at startup
	// regardless of this. Worker thread only; does no I/O beyond the
	// providers' own single stat() calls.
	bool hasCheapChangeSignal() const;

	// Most recent rebuild()'s timing/count, for logging by the OBS-aware
	// caller. {} (all-zero/default) if rebuild() has never run. Safe to
	// call from the Qt main thread.
	RebuildStats lastRebuildStats() const;

	// Serializes the current index (name/root/exe/platform/id only - see
	// InstalledGame; no manifest text or other bulk data is ever held)
	// to jsonPath so a plugin restart can serve matches immediately
	// instead of blocking on a full rebuild. Best-effort: returns false
	// on any I/O failure, and a failed save must never be treated as
	// fatal by the caller. Worker thread only.
	bool saveToFile(const std::wstring &jsonPath) const;

	// Loads a previously-saved index from jsonPath, replacing whatever
	// this InstallIndex currently holds, and seeds lastRebuildStats()'s
	// completedAt from the file's own saved timestamp (so a subsequent
	// hasCheapChangeSignal() check measures staleness from when the
	// cache was actually written, not from process start). Returns
	// false (leaving the index untouched) if the file is missing,
	// unreadable, or malformed. This is a fast bridge across a restart,
	// not a substitute for eventually calling rebuild() on the normal
	// cadence. Worker thread only.
	bool loadFromFile(const std::wstring &jsonPath);

private:
	std::vector<std::unique_ptr<IGameProvider>> providers_;

	// Applies setExclusions()' rules and drops entries two providers
	// both surfaced for the same install root (Steam and the Uninstall
	// registry routinely both find the same game). Returns the survivors
	// and reports what it dropped. Pure - takes no lock, touches no
	// member state - so both ingest paths can call it before they take
	// mutex_ to swap the index in.
	std::vector<InstalledGame> filterForIngest(std::vector<InstalledGame> candidates, std::size_t *outExcluded,
						    std::size_t *outDuplicates) const;

	IndexExclusions exclusions_; // Worker thread only - see setExclusions().

	mutable std::mutex mutex_;
	std::vector<InstalledGame> index_; // Guarded by mutex_.
	RebuildStats lastRebuildStats_;    // Guarded by mutex_.
};

} // namespace signalbox::detection
