/*
 * SignalBox - core/DetectionEngine.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Real worker loop. See DetectionEngine.h for the threading boundary this
 * must never cross, and for what crosses it (DetectionResult snapshots
 * only).
 *
 * TRACKED-PID NOTE: the real ACTIVE game lives in DetectionStateMachine,
 * on the Qt main thread - this class has no access to it, by design (no
 * shared mutable state across the boundary). DetectionEngine instead
 * keeps its own worker-thread-local notion of "the pid I'm currently
 * following" (trackedPid_/tracking_): whichever pid won the most recent
 * poll's ranking pass. In practice this converges with the state
 * machine's real ACTIVE pid almost immediately, because the state
 * machine's CONFIRM_POLLS gate just re-confirms whatever this class has
 * already been reporting consistently. The only window where they can
 * differ is the few seconds a candidate spends in PENDING before
 * CONFIRM_POLLS is reached - and DetectionStateMachine::
 * onTrackedProcessExited() already documents itself as a no-op unless
 * State::Active, so a stray/early exit signal during that window is
 * silently and correctly ignored on the receiving end. This keeps the
 * cross-thread contract exactly "immutable snapshots only" while still
 * letting ProcessScanner's trackForExit()/pollExit() (which must run on
 * the worker thread - see ProcessScanner.h) do the crash-vs-clean
 * classification DESIGN.md 2.2's GRACE window sizing depends on.
 *
 * CHALLENGER PERSISTENCE (project report item 7 fix): the paragraph above
 * used to end there, but re-targeting trackedPid_ to "whichever pid won
 * the MOST RECENT poll" has a real failure mode, not just a harmless
 * window: if the real incumbent g exits during the single poll a
 * transient rival h happens to win, this class had already called
 * stopTracking(g) and started tracking h instead - nobody is watching g's
 * handle anymore, so g's exit is never observed by anyone.
 * DetectionStateMachine restores State::Active(g) once h fails to
 * reconfirm (per its "ACTIVE seeing no detection does NOT start GRACE"
 * design), and the machine is stuck believing a dead game is ACTIVE
 * forever - no grace, no fallback, no further switches until a different
 * game confirms. Fix: track a challenger's win streak
 * (challengerPid_/challengerStablePolls_) and only retarget once it has
 * won timing_.confirmPolls polls in a row - the same bar
 * DetectionStateMachine applies before treating a candidate as real. The
 * incumbent's exit handle is never released to a single-poll fluke.
 *
 * ADAPTIVE POLLING NOTE: "while confirming a candidate" (project brief
 * item 4) is approximated by `tracking_` - this class is "tracking" a pid
 * exactly when the most recent poll produced a winning candidate, which
 * is the same condition under which the main-thread state machine is
 * either accumulating CONFIRM_POLLS or already ACTIVE. Combined with the
 * `live_` flag threaded in from plugin-main.cpp's own frontend event
 * listener, this reaches the normal cadence in every case DESIGN.md
 * ADDENDUM cares about (live, or something plausible is happening) and
 * only backs off to idlePollIntervalS when truly nothing is going on.
 */

#include "DetectionEngine.h"

#include <algorithm>
#include <chrono>

#include "../detection/HelperDenylist.h"
#include "../detection/providers/EpicProvider.h"
#include "../detection/providers/GenericUninstallProvider.h"
#include "../detection/providers/GogProvider.h"
#include "../detection/providers/SteamProvider.h"
#include "../detection/providers/UbisoftProvider.h"

namespace signalbox::core {

namespace {

// DESIGN.md 1.4 step 4.2: HIGH > MEDIUM > LOW. Confidence's enumerator
// order already matches this (High=0, Medium=1, Low=2) - a free function
// makes that assumption explicit and reviewable instead of a bare
// operator< on the enum sprinkled through scanAndRank().
int ConfidenceRank(detection::Confidence confidence)
{
	return static_cast<int>(confidence);
}

} // namespace

DetectionEngine::DetectionEngine(ResultCallback resultCallback, const TimingConstants &timing)
	: resultCallback_(std::move(resultCallback)),
	  timing_(timing)
{
}

DetectionEngine::~DetectionEngine()
{
	stop();
}

void DetectionEngine::registerDefaultProviders()
{
	using namespace detection::providers;
	installIndex_.addProvider(std::make_unique<SteamProvider>());
	installIndex_.addProvider(std::make_unique<EpicProvider>());
	installIndex_.addProvider(std::make_unique<GogProvider>());
	installIndex_.addProvider(std::make_unique<UbisoftProvider>());
	installIndex_.addProvider(std::make_unique<GenericUninstallProvider>());
}

void DetectionEngine::setHelperDenylistPath(std::wstring path)
{
	helperDenylistPath_ = std::move(path);
}

void DetectionEngine::setIndexCachePath(std::wstring path)
{
	indexCachePath_ = std::move(path);
}

void DetectionEngine::start()
{
	if (worker_.joinable()) {
		return; // Already running.
	}
	stopRequested_ = false;
	worker_ = std::thread(&DetectionEngine::workerLoop, this);
}

void DetectionEngine::stop()
{
	stopRequested_ = true;
	wakeCv_.notify_all();
	if (worker_.joinable()) {
		worker_.join();
	}
}

void DetectionEngine::requestRescan()
{
	rescanRequested_ = true;
	wakeCv_.notify_all();
}

std::size_t DetectionEngine::indexedGameCount() const
{
	return installIndex_.indexedGameCount();
}

void DetectionEngine::setLive(bool live)
{
	live_.store(live, std::memory_order_relaxed);
	wakeCv_.notify_all(); // Live going true should shorten the wait immediately, not on the next natural wake.
}

void DetectionEngine::workerLoop()
{
	// WORKER THREAD. See the threading-boundary comment block at the
	// top of DetectionEngine.h before adding anything here.

	// Startup: warm-start from the cached index if one exists (fast
	// bridge across a restart - InstallIndex.h's own performance
	// contract). Only pay for a real rebuild() when there's nothing
	// usable to load yet - this is the "never rebuild on the hot path"
	// requirement applied to plugin startup too, not just steady state.
	// The denylist is loaded FIRST, before either ingest path runs,
	// because it carries the index exclusions too - and both
	// loadFromFile() and rebuild() apply them on ingest. Loading it
	// after the index would mean the very first poll of every session
	// ranks against an unfiltered index, which is precisely the window
	// in which a browser or wallpaper animator is the only thing
	// running and therefore wins.
	// Not const: a user-requested rescan (see below, inside the loop)
	// reloads this from disk so a mid-session edit to helpers.json takes
	// effect without an OBS restart. Every other read of `denylist` -
	// scanAndRank()'s per-poll filtering - takes it by const reference,
	// so nothing about the hot path changes; only this variable's
	// lifetime does.
	detection::HelperDenylist denylist = helperDenylistPath_.empty()
						      ? detection::HelperDenylist::BuiltInFallback()
						      : detection::HelperDenylist::LoadFromFile(helperDenylistPath_);
	installIndex_.setExclusions(denylist.indexExclusions);

	bool haveIndex = false;
	if (!indexCachePath_.empty()) {
		haveIndex = installIndex_.loadFromFile(indexCachePath_);
	}
	if (!haveIndex) {
		installIndex_.rebuild();
	}
	// Saved unconditionally, not only on the rebuild path: a cache
	// loaded from an older plugin version has just been re-filtered
	// through the current exclusions, and writing it back is what stops
	// that work being redone on every launch.
	if (!indexCachePath_.empty()) {
		installIndex_.saveToFile(indexCachePath_);
	}
	auto lastIndexMaintenanceAt = std::chrono::steady_clock::now();

	while (!stopRequested_) {
		// --- Index maintenance: a cheap stat()-class check every
		// iteration; a real rebuild only on the cheap change signal, an
		// explicit rescan, or the long-timer fallback (registry-only
		// providers have no cheap signal - see IGameProvider.h). NEVER
		// on the hot path otherwise - see InstallIndex.h's performance
		// contract and project brief item 5.
		const auto now = std::chrono::steady_clock::now();
		const bool longTimerElapsed =
			std::chrono::duration_cast<std::chrono::seconds>(now - lastIndexMaintenanceAt).count() >=
			static_cast<long long>(timing_.indexRebuildIntervalS);
		const bool rescanRequested = rescanRequested_.exchange(false);

		std::optional<DetectionResult::IndexRebuild> indexRebuild;
		if (rescanRequested || longTimerElapsed || installIndex_.hasCheapChangeSignal()) {
			// Only the button re-reads helpers.json from disk. The owner
			// asked for exactly this: editing data/aliases.json or
			// data/helpers.json used to require a full OBS restart to take
			// effect, and mid-session live-testing needs a way to add a
			// denylist/exclusion rule and see it apply on the next press,
			// not on the next relaunch. The periodic timer and the cheap-
			// change-signal path both keep using whatever denylist is
			// already loaded - they exist to catch a changed INSTALL
			// (a newly launched Steam game), not a changed DATA FILE, and
			// re-reading+re-parsing a JSON file on every one of those would
			// be exactly the kind of unrequested disk I/O InstallIndex.h's
			// performance contract rules out for anything but plugin load
			// or an explicit user rescan.
			if (rescanRequested) {
				denylist = helperDenylistPath_.empty()
						   ? detection::HelperDenylist::BuiltInFallback()
						   : detection::HelperDenylist::LoadFromFile(helperDenylistPath_);
				// Re-apply before rebuild() so the freshly reloaded
				// exclusions filter this same rebuild's ingest, exactly
				// like the startup ordering above (denylist first, then
				// index) - a rebuild running against the OLD exclusions
				// would surface whatever a just-added exclusion rule was
				// meant to hide for one more rebuild cycle.
				installIndex_.setExclusions(denylist.indexExclusions);
			}

			installIndex_.rebuild();
			lastIndexMaintenanceAt = now;
			if (!indexCachePath_.empty()) {
				installIndex_.saveToFile(indexCachePath_);
			}
			const detection::RebuildStats stats = installIndex_.lastRebuildStats();
			indexRebuild = DetectionResult::IndexRebuild{stats.gameCount, stats.excludedCount,
								      stats.duplicateCount, stats.durationMs,
								      rescanRequested,
								      rescanRequested ? denylist.ruleCount() : 0};
		}

		// --- Per-poll hot path. One process enumeration serves both
		// the exit check below and the ranking pass; scanning twice
		// would double the only per-poll cost that scales with how busy
		// the machine is. ---
		const std::vector<detection::ProcessInfo> processes = processScanner_.scan();

		// --- Exit tracking for whatever pid this engine is currently
		// following (see this file's TRACKED-PID NOTE). ---
		//
		// TWO INDEPENDENT SIGNALS, and the second one is not belt and
		// braces - it is the one that works when the first cannot.
		//
		// pollExit() is the good signal: it waits on a real handle and
		// can tell a clean exit from a crash, which is what sizes the
		// grace window. But it requires trackForExit() to have opened
		// that handle, and OpenProcess is exactly the call an
		// anti-cheat-protected game is most likely to refuse. When it
		// fails there is no handle, no exit signal, and nothing to
		// notice the game closing - the state machine sits on a dead
		// ACTIVE game indefinitely, because "ACTIVE seeing no detection"
		// is deliberately NOT an exit signal (see
		// DetectionStateMachine::onPollResult).
		//
		// So the pid table gets the final say: if the pid we are
		// following is not in this poll's enumeration, the process is
		// gone, whatever the handle did or did not tell us. That costs
		// nothing extra - the enumeration already happened above - and
		// it needs no permissions at all.
		std::optional<DetectionResult::TrackedExit> trackedExit;
		if (tracking_) {
			std::optional<detection::ExitReason> reason = processScanner_.pollExit(trackedPid_);
			if (!reason) {
				const bool stillListed =
					std::any_of(processes.begin(), processes.end(),
						     [this](const detection::ProcessInfo &p) { return p.pid == trackedPid_; });
				if (!stillListed) {
					// No handle, or a handle that told us nothing, and
					// the pid is no longer running. Clean is the safe
					// assumption: it sizes the SHORTER grace window, so
					// a real crash-and-relaunch still reappears inside
					// it, while a genuine quit is not left hanging.
					reason = detection::ExitReason::Clean;
				}
			}
			if (reason) {
				trackedExit = DetectionResult::TrackedExit{
					*reason, trackedGame_.value_or(detection::InstalledGame{})};
				processScanner_.stopTracking(trackedPid_);
				tracking_ = false;
				trackedPid_ = 0;
				trackedGame_.reset();
				challengerPid_ = 0;
				challengerStablePolls_ = 0;
			}
		}

		const std::optional<detection::DetectedGame> winner = scanAndRank(denylist, processes);

		if (winner) {
			if (!tracking_) {
				// Nothing currently tracked - start immediately.
				// There is no incumbent to protect against a
				// single-poll fluke, so no need to wait out a
				// challenger streak here.
				//
				// trackForExit()'s return value is deliberately
				// ignored. It fails on precisely the processes
				// worth following - anti-cheat-protected games
				// refuse OpenProcess - and treating that as "do
				// not follow this pid" is what left the engine
				// watching nothing at all. Follow it either way;
				// the pid-table check above does not need a
				// handle to notice the process is gone.
				(void)processScanner_.trackForExit(winner->pid);
				trackedPid_ = winner->pid;
				trackedGame_ = winner->game;
				tracking_ = true;
				challengerPid_ = 0;
				challengerStablePolls_ = 0;
			} else if (trackedPid_ == winner->pid) {
				// The incumbent keeps winning - nothing to do,
				// and any past challenger streak is stale.
				challengerPid_ = 0;
				challengerStablePolls_ = 0;
			} else {
				// A DIFFERENT pid won this poll while an
				// incumbent is still being tracked - see this
				// file's CHALLENGER PERSISTENCE note. Do not
				// abandon the incumbent's exit handle on a
				// single transient win; require the challenger
				// to persist first.
				if (challengerPid_ == winner->pid) {
					++challengerStablePolls_;
				} else {
					challengerPid_ = winner->pid;
					challengerStablePolls_ = 1;
				}
				if (challengerStablePolls_ >= std::max<std::uint32_t>(1, timing_.confirmPolls)) {
					processScanner_.stopTracking(trackedPid_);
					(void)processScanner_.trackForExit(winner->pid); // See above on ignoring this.
					trackedPid_ = winner->pid;
					trackedGame_ = winner->game;
					tracking_ = true;
					challengerPid_ = 0;
					challengerStablePolls_ = 0;
				}
			}
		} else {
			// Nothing detected this poll - a transient miss must not
			// itself count as, or reset progress toward, anything;
			// it just shouldn't let a stale challenger streak carry
			// forward indefinitely across gaps.
			challengerPid_ = 0;
			challengerStablePolls_ = 0;
		}

		DetectionResult result;
		result.detected = winner;
		result.indexedGameCount = installIndex_.indexedGameCount();
		result.trackedExit = trackedExit;
		result.indexRebuild = indexRebuild;
		if (resultCallback_) {
			resultCallback_(result);
		}

		// --- Adaptive polling (project brief item 4): normal cadence
		// while live or while a candidate is being tracked; noticeably
		// slower otherwise. Detection latency does not matter when
		// nothing is happening, and this plugin must never compete with
		// a game for resources. ---
		const bool fast = live_.load(std::memory_order_relaxed) || tracking_;
		const std::uint32_t intervalS = fast ? timing_.pollIntervalS : timing_.idlePollIntervalS;

		std::unique_lock<std::mutex> lock(wakeMutex_);
		wakeCv_.wait_for(lock, std::chrono::seconds(intervalS),
				  [this] { return stopRequested_.load() || rescanRequested_.load(); });
	}

	if (tracking_) {
		processScanner_.stopTracking(trackedPid_);
		tracking_ = false;
		trackedGame_.reset();
	}
}

std::optional<detection::DetectedGame> DetectionEngine::scanAndRank(const detection::HelperDenylist &denylist,
								     const std::vector<detection::ProcessInfo> &processes)
{
	struct Candidate {
		detection::DetectedGame detected;
		std::optional<std::uint64_t> workingSet;
		std::optional<std::chrono::system_clock::time_point> startedAt;
	};

	std::vector<Candidate> candidates;
	candidates.reserve(8);

	for (const auto &proc : processes) {
		// Cheap basename-only reject first (DESIGN.md 1.4 step 3) -
		// avoids paying for OpenProcess+QueryFullProcessImageNameW on
		// every obviously-irrelevant launcher/helper process.
		if (denylist.shouldIgnoreProcess(L"", proc.exeBasename)) {
			continue;
		}

		const auto imagePath = processScanner_.resolveImagePath(proc.pid);
		if (!imagePath) {
			continue; // ACCESS_DENIED etc. on protected/system processes is normal - skip silently.
		}

		if (denylist.shouldIgnoreProcess(*imagePath, proc.exeBasename)) {
			continue;
		}

		const auto matched = installIndex_.match(*imagePath);
		if (!matched) {
			continue; // Doesn't fall under any indexed install root.
		}

		detection::DetectedGame detected;
		detected.game = matched->first;
		detected.pid = proc.pid;
		detected.confidence = matched->second;
		detected.observedAt = std::chrono::steady_clock::now();

		candidates.push_back(Candidate{std::move(detected), std::nullopt, std::nullopt});
	}

	if (candidates.empty()) {
		lastForegroundCandidatePid_ = 0;
		foregroundStableCount_ = 0;
		return std::nullopt;
	}

	// DESIGN.md 1.4 step 4.1: foreground-window stability tie-break -
	// tie-break ONLY, never the primary signal. Alt-tabbing away from a
	// still-running game must not change detection, which is exactly
	// why this only ever narrows a choice among already-matched
	// candidates and never adds one.
	const std::optional<std::uint32_t> foregroundPid = processScanner_.foregroundPid();
	bool foregroundIsCandidate = false;
	if (foregroundPid) {
		foregroundIsCandidate = std::any_of(candidates.begin(), candidates.end(), [&](const Candidate &c) {
			return c.detected.pid == *foregroundPid;
		});
	}
	if (foregroundIsCandidate && *foregroundPid == lastForegroundCandidatePid_) {
		++foregroundStableCount_;
	} else {
		lastForegroundCandidatePid_ = foregroundIsCandidate ? *foregroundPid : 0;
		foregroundStableCount_ = foregroundIsCandidate ? 1 : 0;
	}
	const bool foregroundStable =
		foregroundIsCandidate && foregroundStableCount_ >= std::max<std::uint32_t>(1, timing_.foregroundTiebreakPolls);

	if (foregroundStable) {
		const auto it = std::find_if(candidates.begin(), candidates.end(), [&](const Candidate &c) {
			return c.detected.pid == lastForegroundCandidatePid_;
		});
		if (it != candidates.end()) {
			return it->detected;
		}
	}

	// DESIGN.md 1.4 step 4.2-4.5: confidence tier, then working-set
	// size, then most-recently-started. This is also what naturally
	// bounds GenericUninstallProvider's noise (project brief item 6):
	// Generic's matches are always Confidence::Low, so a Steam/Epic/
	// GOG/Ubisoft candidate (High/Medium) wins this comparison
	// unconditionally, without any store-specific special-casing here -
	// Generic only ever wins when it is the ONLY surviving candidate.
	Candidate *best = nullptr;
	for (auto &candidate : candidates) {
		if (!best) {
			best = &candidate;
			continue;
		}

		const int candidateRank = ConfidenceRank(candidate.detected.confidence);
		const int bestRank = ConfidenceRank(best->detected.confidence);
		if (candidateRank != bestRank) {
			if (candidateRank < bestRank) { // Lower rank = higher confidence.
				best = &candidate;
			}
			continue;
		}

		if (!candidate.workingSet) {
			candidate.workingSet = processScanner_.workingSetSize(candidate.detected.pid);
		}
		if (!best->workingSet) {
			best->workingSet = processScanner_.workingSetSize(best->detected.pid);
		}
		const std::uint64_t candidateWs = candidate.workingSet.value_or(0);
		const std::uint64_t bestWs = best->workingSet.value_or(0);
		if (candidateWs != bestWs) {
			if (candidateWs > bestWs) {
				best = &candidate;
			}
			continue;
		}

		if (!candidate.startedAt) {
			candidate.startedAt = processScanner_.startTime(candidate.detected.pid);
		}
		if (!best->startedAt) {
			best->startedAt = processScanner_.startTime(best->detected.pid);
		}
		if (candidate.startedAt && best->startedAt && *candidate.startedAt > *best->startedAt) {
			best = &candidate;
		}
	}

	return best ? std::optional<detection::DetectedGame>(best->detected) : std::nullopt;
}

} // namespace signalbox::core
