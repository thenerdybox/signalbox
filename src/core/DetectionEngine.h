/*
 * SignalBox - core/DetectionEngine.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * ============================================================================
 * THREADING BOUNDARY - READ THIS BEFORE TOUCHING THIS CLASS OR ANYTHING IT OWNS
 * ============================================================================
 * DetectionEngine owns exactly one worker thread (std::thread). Everything
 * that thread touches - ProcessScanner, InstallIndex (registry/file I/O),
 * the poll loop itself - must stay OBS- and Qt-free:
 *   - No obs_* API calls from the worker thread.
 *   - No QObject, QWidget, QNetworkAccessManager, or any Qt type touched
 *     from the worker thread. QNAM in particular is documented as usable
 *     only from the thread that owns it (DESIGN.md 4.3) - that thread is
 *     the Qt main thread, always.
 *   - No synchronous network calls anywhere near this thread. Detection
 *     is 100% local (registry + Toolhelp32); Twitch calls happen only
 *     after a DetectedGame snapshot has crossed to the main thread.
 *
 * The ONLY thing that crosses the boundary is an immutable
 * DetectionResult snapshot (plain data, no pointers into worker-owned
 * state), posted to the main-thread coordinator. In the real
 * implementation that post is a Qt::QueuedConnection
 * QMetaObject::invokeMethod call (DESIGN.md 4.3) - deliberately not
 * wired up in this stub (see resultCallback_ below) so this header has
 * zero Qt dependency and can be reused/tested without pulling in
 * QtWidgets.
 *
 * No other shared mutable state exists between the two threads except:
 *   - stopRequested_, an atomic flag.
 *   - InstallIndex's internal mutex-guarded snapshot (see InstallIndex.h) -
 *     read-only from the main thread's perspective (indexedGameCount()
 *     for UI display), written only by the worker thread's rebuild().
 *
 * If a future change needs to touch OBS or Qt state from inside
 * DetectionEngine, ProcessScanner, InstallIndex, or any IGameProvider,
 * that is a sign the change belongs on the other side of
 * resultCallback_, not in this file.
 * ============================================================================
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../detection/DetectedGame.h"
#include "../detection/InstallIndex.h"
#include "../detection/ProcessScanner.h"
#include "TimingConstants.h"

namespace signalbox::detection {
// Forward-declared only - HelperDenylist.h is Qt-JSON-header-only (see its
// own doc comment) and pulled in by DetectionEngine.cpp, not here, so this
// header keeps its "zero Qt dependency" property for any translation unit
// that only wants DetectionResult/ResultCallback.
class HelperDenylist;
} // namespace signalbox::detection

namespace signalbox::core {

// Immutable snapshot posted from the worker thread to the main thread
// once per poll. Deliberately plain data - see the threading boundary
// note above.
struct DetectionResult {
	std::optional<detection::DetectedGame> detected;
	std::size_t indexedGameCount = 0;

	// A tracked pid's exit, WITH the identity of the game it belonged to
	// (see DetectionEngine.cpp's "tracked pid" note). Carrying identity
	// alongside the reason (not just a bare ExitReason) is what lets
	// DetectionStateMachine::onTrackedProcessExited() verify this exit
	// actually belongs to ITS incumbent (activeGame_) rather than
	// blindly trusting "whichever pid DetectionEngine happened to be
	// following most recently" - see the project report's item 7 fix.
	// Without an identity, a transient rival's exit during the brief
	// window DetectionEngine is tracking it (see DetectionEngine.cpp's
	// challenger-persistence note) could be misattributed to the real
	// incumbent.
	struct TrackedExit {
		detection::ExitReason reason;
		detection::InstalledGame game;
	};

	// Set when the pid DetectionEngine has been following (its own best
	// last-known winning candidate - see DetectionEngine.cpp's "tracked
	// pid" note) was observed to exit this poll. Carried alongside
	// `detected` rather than as a separate callback so both cross the
	// worker->main-thread boundary as one atomic snapshot, in the right
	// order: a caller applies the exit (GRACE sizing) before the fresh
	// poll result, matching DESIGN.md 2.2's "exit observed within one
	// poll" behavior even when a relaunch is fast enough to also appear
	// in `detected` the same cycle.
	std::optional<TrackedExit> trackedExit;

	// Set only on a poll where the index was actually rebuilt. Carried
	// so the dock can say what the Rescan button DID.
	//
	// Rescan previously logged "Requested an install-index rescan" and
	// then nothing, ever - the rebuild happened on the worker thread and
	// reported to no one. From the outside that is indistinguishable
	// from a dead button, and it was reported as one. A button that
	// performs a slow, invisible action needs to say what it found, not
	// that it was pressed.
	struct IndexRebuild {
		std::size_t gameCount = 0;
		std::size_t excludedCount = 0;
		std::size_t duplicateCount = 0;
		double durationMs = 0.0;
		bool userRequested = false; // Distinguishes the button from the periodic timer.

		// Total denylist/exclusion rules (pathFragments + helperBasenames +
		// launcherBasenames + excludedBasenames + excludedPathFragments +
		// every IndexExclusions list) in effect after this rebuild -
		// HelperDenylist::ruleCount(). Only populated when userRequested is
		// true, because helpers.json is only actually re-read from disk on
		// the button press (see workerLoop()'s "re-read helpers.json fresh"
		// note) - the periodic/cheap-change rebuild paths keep using
		// whatever denylist was already loaded, so reporting a count there
		// would imply a reload that did not happen. 0 otherwise.
		std::size_t denylistRuleCount = 0;
	};
	std::optional<IndexRebuild> indexRebuild;
};

class DetectionEngine {
public:
	// resultCallback is invoked once per poll from the worker thread in
	// this stub's current form. The real implementation MUST wrap this
	// so the callback body actually executes on the main thread (a
	// Qt::QueuedConnection hop) - see the threading boundary note. Do
	// not call directly into DetectionStateMachine or any Qt/OBS API
	// from inside a callback that might still be running on the worker
	// thread.
	using ResultCallback = std::function<void(DetectionResult)>;

	explicit DetectionEngine(ResultCallback resultCallback, const TimingConstants &timing = kDefaultTimingConstants);
	~DetectionEngine();

	DetectionEngine(const DetectionEngine &) = delete;
	DetectionEngine &operator=(const DetectionEngine &) = delete;

	// Registers the standard provider set (Steam, Epic, GOG, Ubisoft,
	// GenericUninstall) with the owned InstallIndex. Call once before
	// start(). Split out from the constructor so tests can build an
	// engine with a custom provider set instead.
	void registerDefaultProviders();

	// Resolves OBS-aware paths (obs_module_file/obs_module_config_path)
	// are the caller's job - see DetectionEngine.h's threading-boundary
	// note and HelperDenylist.h's OWNERSHIP NOTE; this class only stores
	// plain strings and does its own file I/O on the worker thread with
	// them (QFile - no obs_* calls). Call both before start(); empty
	// paths degrade gracefully (BuiltInFallback() denylist, no index
	// cache persistence) rather than failing.
	void setHelperDenylistPath(std::wstring path);
	void setIndexCachePath(std::wstring path);

	// Starts the worker thread. Safe to call once; call stop() (or let
	// the destructor do it) before a second start().
	void start();

	// Signals the worker thread to stop and joins it. Also called from
	// the destructor - obs_module_unload must not return while this
	// thread is still alive, per DESIGN.md 4.3.
	void stop();

	// Requests an out-of-cadence index rebuild (dock "Rescan" button).
	// Thread-safe: posts a wake to the worker thread rather than
	// touching InstallIndex from the calling thread.
	void requestRescan();

	// Thread-safe read for UI display.
	std::size_t indexedGameCount() const;

	// OBS_FRONTEND_EVENT_STREAMING_STARTED/_STOPPED, threaded in from the
	// main thread (see plugin-main.cpp - a second, independent frontend
	// event listener, deliberately not routed through CategoryDock's own
	// one, to avoid touching src/ui). Drives adaptive polling: the
	// normal cadence (TimingConstants::pollIntervalS) while live OR
	// while a candidate is being tracked, TimingConstants::
	// idlePollIntervalS otherwise. Thread-safe (plain atomic bool) -
	// safe to call from the main thread at any time, including before
	// start().
	void setLive(bool live);

private:
	void workerLoop(); // Runs entirely on worker_. See class threading note.

	// Per-poll hot path: scan running processes, resolve image paths,
	// apply the helper/launcher denylist, match against the install
	// index, and rank surviving candidates per DESIGN.md 1.4 step 4
	// (foreground stability > confidence tier > working set > start
	// time). No I/O beyond what ProcessScanner/InstallIndex::match()
	// already do; no index rebuild. Worker thread only.
	std::optional<detection::DetectedGame> scanAndRank(const detection::HelperDenylist &denylist,
									      const std::vector<detection::ProcessInfo> &processes);

	ResultCallback resultCallback_;
	TimingConstants timing_;

	detection::InstallIndex installIndex_;
	detection::ProcessScanner processScanner_;

	std::wstring helperDenylistPath_;
	std::wstring indexCachePath_;

	std::thread worker_;
	std::atomic<bool> stopRequested_{false};
	std::atomic<bool> rescanRequested_{false};
	std::atomic<bool> live_{false};
	std::mutex wakeMutex_;
	std::condition_variable wakeCv_;

	// --- Worker-thread-only state (never touched off worker_) ---

	// Best-effort mirror of whatever the main-thread DetectionStateMachine
	// will end up treating as ACTIVE - see DetectionEngine.cpp's
	// "tracked pid" note for why an independent proxy on this side is
	// the right call rather than sharing state across the boundary.
	std::uint32_t trackedPid_ = 0;
	bool tracking_ = false;
	std::optional<detection::InstalledGame> trackedGame_; // Identity paired with trackedPid_ - see TrackedExit above.

	// Challenger-persistence bookkeeping (item 7 fix - see
	// DetectionEngine.cpp's "CHALLENGER PERSISTENCE" note): a single
	// poll's winner different from trackedPid_ must NOT immediately
	// steal the incumbent's exit-tracking handle. It has to win
	// confirmPolls polls in a row first, same bar DetectionStateMachine
	// itself applies before treating a candidate as real.
	std::uint32_t challengerPid_ = 0;
	std::uint32_t challengerStablePolls_ = 0;

	// Foreground-stability tie-break bookkeeping (DESIGN.md 1.4 step
	// 4.1) - persists across polls within scanAndRank()'s caller.
	std::uint32_t lastForegroundCandidatePid_ = 0;
	std::uint32_t foregroundStableCount_ = 0;
};

} // namespace signalbox::core
