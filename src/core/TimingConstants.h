/*
 * SignalBox - core/TimingConstants.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * ALL detection/automation timing lives here, as named constants, and
 * NOWHERE else. Do not hardcode a sleep, a poll interval, or a "wait N
 * seconds" anywhere in DetectionEngine, DetectionStateMachine,
 * TwitchClient, or the UI - reference these instead. That is what keeps
 * a future retune (informed by ../rate-limits.md and real-world soak
 * testing) a one-line config change instead of a code hunt.
 *
 * These are DEFAULTS. At runtime the effective values live in
 * PluginConfig (persisted, user-editable under the dock's "Advanced
 * timing" group) - this header only defines the shape and the
 * out-of-the-box numbers. See DESIGN.md Section 2.1 and the ADDENDUM
 * "Timing constants - keep these separate and named".
 *
 * Four of these map directly to the four concerns called out in the
 * design ADDENDUM - do not collapse them into one value, they answer
 * different questions:
 *   - CONFIRM_POLLS            -> "how sure are we before switching in?"
 *   - CRASH_GRACE_S            -> "how long do we wait before giving up
 *                                   on a game that just disappeared?"
 *   - MIN_PATCH_SPACING_S      -> "how often can we touch the Twitch API?"
 *   - PROMPT_TIMEOUT_S         -> "how long before an unanswered prompt
 *                                   falls through to the safe default?"
 */

#pragma once

#include <cstdint>

namespace signalbox::core {

struct TimingConstants {
	// --- Detection polling (DetectionEngine, worker thread) ---

	// Process-scan cadence on the worker thread.
	std::uint32_t pollIntervalS = 5;

	// Process-scan cadence while OBS is idle - not streaming, and no
	// candidate is currently being tracked (nothing plausibly running
	// that could become the switch-in target). Detection latency does
	// not matter when nothing is happening: no stream to get the
	// category wrong on, no confirmation window to keep tight. The
	// owner's standing requirement is that this plugin never competes
	// with a game for CPU/disk - polling four times less often while
	// idle is most of that budget for free. DetectionEngine falls back
	// to pollIntervalS the moment either condition flips (going live, or
	// a candidate starting to accumulate confirm polls) - see
	// DetectionEngine.cpp's adaptive-polling note.
	std::uint32_t idlePollIntervalS = 20;

	// Consecutive winning polls a candidate must accumulate before a
	// switch-in triggers any Twitch call (pollIntervalS * confirmPolls
	// ~= 15s with defaults). Kills flaps from launcher stubs, DRM
	// wrappers, and mis-ranked first polls. See DESIGN.md 2.2.
	std::uint32_t confirmPolls = 3;

	// Stability (in polls) required before GetForegroundWindow is
	// allowed to break a tie between two simultaneously-running
	// indexed games. Never used as the primary detection signal.
	std::uint32_t foregroundTiebreakPolls = 2;

	// --- Exit / grace handling (DetectionStateMachine) ---

	// Relaunch window after an ABNORMAL process exit (nonzero exit
	// code - crash territory). Deliberately much longer than
	// confirmPolls: acting on absence of evidence is asymmetric with
	// acting on presence of evidence. See DESIGN.md 2.2 "fast in, slow
	// out". This is also the grace window behind ADDENDUM Trigger B
	// (the "no game detected, still live" prompt).
	std::uint32_t crashGraceS = 120;

	// Relaunch window after a CLEAN process exit (exit code 0).
	// Shorter than crashGraceS - a clean quit is weaker evidence of an
	// imminent relaunch than a crash is.
	std::uint32_t cleanExitGraceS = 30;

	// --- Install index maintenance (InstallIndex, worker thread) ---

	// Long-timer fallback for providers that have no cheap on-disk
	// change signal (GOG/Ubisoft/Generic are registry-only - see
	// IGameProvider::changeSignal()'s doc comment). The per-poll hot
	// path NEVER triggers a rebuild by itself; DetectionEngine checks
	// InstallIndex::hasCheapChangeSignal() (a stat()-class call) every
	// poll and only pays for a real rebuild() when that signal fires,
	// an explicit rescan is requested, or this timer elapses - whichever
	// comes first. DESIGN.md 1.2's "every 10 minutes" default.
	std::uint32_t indexRebuildIntervalS = 600;

	// --- Twitch API pacing (TwitchClient) ---

	// Floor between successive category PATCH calls, regardless of
	// how many detection events fire in between. See DESIGN.md 3.4 and
	// rate-limits.md "Recommended Minimum Interval" (30-60s).
	std::uint32_t minPatchSpacingS = 45;

	// --- Live category verification (DetectionStateMachine) ---
	//
	// While live, the channel's category is re-read on a schedule and, if
	// a confirmed game is running and the channel shows something else,
	// the game's category is re-applied. This exists because the category
	// can be changed behind SignalBox's back at any time - multistream
	// services push their own saved category when a stream starts - and a
	// single read at go-live races exactly that.
	//
	// Cost: one GET /helix/channels per interval while live, i.e. about 30
	// requests an hour at the default - a rounding error against the
	// 800-points-a-minute bucket in rate-limits.md. Never runs while
	// offline, and the Twitch client's own 429/transport backoff still
	// gates every call (a gated call simply reports "could not verify" and
	// waits for the next slot - there is no retry loop).

	// Seconds between verifications while live. Clamped to a 30 s floor on
	// load so a hand-edited config can never turn this into a tight poll.
	std::uint32_t liveCategoryCheckIntervalS = 120;

	// Extra early verifications shortly after going live, to catch a
	// multistream relay overwriting the category in the first moments of a
	// stream. Offsets are seconds after the stream starts.
	bool goLiveQuickChecks = true;
	std::uint32_t goLiveQuickCheckFirstS = 15;
	std::uint32_t goLiveQuickCheckSecondS = 60;

	// A correction made within this many seconds of going live is
	// reported as a probable multistream/external override in the log.
	std::uint32_t goLiveOverrideWindowS = 300;

	// --- Crash-loop / flap breaker (DetectionStateMachine) ---

	// If automation performs more than flapBreakerN category changes
	// within flapBreakerWindowS, automation pauses and the dock shows
	// a notice. Bounds worst-case viewer-visible churn from a
	// crash-looping game. See DESIGN.md 2.2.
	std::uint32_t flapBreakerN = 4;
	std::uint32_t flapBreakerWindowS = 600;

	// --- Prompt UI (CategoryDock / PromptWidget) ---

	// How long an unanswered go-live-mismatch or game-closed prompt
	// stays live before falling through to the safe default (hold the
	// last category). Ignoring the prompt must always be safe - see
	// ADDENDUM "Hard constraints" #2. Value is provisional per
	// DESIGN.md ADDENDUM ("to be tuned"); not yet informed by real
	// dock-usability testing.
	std::uint32_t promptTimeoutS = 20;

	// How long the plugin sits with nothing detected before it asks what
	// the user is doing (Trigger D - the no-game idle prompt). Unlike
	// every other prompt this is NOT gated on live-ness, so the value has
	// to be long enough that merely having OBS open does not produce a
	// question: OBS with no game running is the normal state for most of
	// the day, and a prompt that fires the moment the dock loads is the
	// nagging this whole design exists to avoid. Two minutes is long
	// enough that reaching it means the user really has settled in
	// without a game, and short enough to be useful before they go live.
	// Only ever asked ONCE per idle spell - see
	// DetectionStateMachine's noGameIdleAsked_.
	std::uint32_t noGameIdlePromptS = 120;

	// "No game while live" prompt (DetectionStateMachine, PromptKind::GameClosed).
	//
	// noGameSnoozeS: how long "Waiting for a game" (or leaving the prompt
	// unanswered) stays quiet before asking again, if still nothing is
	// detected. Also the delay before the first question when a stream
	// starts with no game running, so a "Starting soon" scene is not
	// interrupted the instant it goes live.
	std::uint32_t noGameSnoozeS = 120;

	// How long that prompt waits for an answer before it counts as
	// "Waiting for a game". Longer than promptTimeoutS: this prompt is
	// answered from outside OBS (the streamer is in the game), and an
	// unanswered one never changes the category either way.
	std::uint32_t noGameLivePromptTimeoutS = 60;

	// --- Twitch token validation (TwitchAuth::validate()) ---

	// Twitch's authentication docs require validating the access token
	// on startup and hourly thereafter; non-conforming apps risk
	// punitive action against the client ID (see the project report's
	// verdict on this). No new timer for this - CategoryDock piggybacks
	// the check on tickTimer_ (already running every pollIntervalS for
	// DetectionStateMachine::onTick() - see CategoryDock.h's TIMERS
	// note), via ShouldValidateTwitchToken() below. This constant only
	// names the interval between calls.
	std::uint32_t tokenValidationIntervalS = 3600;
};

// Single shared instance of the compiled-in defaults. PluginConfig loads
// the persisted (possibly user-edited) copy over this at startup; code
// outside PluginConfig should go through PluginConfig::timing(), not this
// directly, once that wiring exists.
inline constexpr TimingConstants kDefaultTimingConstants{};

// Pure scheduling decision for the hourly-validate requirement
// (TimingConstants::tokenValidationIntervalS above) - factored out as a
// free function, rather than inlined into CategoryDock's Qt-dependent tick
// handler, specifically so it is testable without Qt/OBS (see
// tests/harness/main.cpp). lastValidatedAtUnixS <= 0 means "never
// validated this run" - covers both the documented startup-validate
// requirement and a freshly connected client - and always returns true.
// Deliberately takes/returns plain std::int64_t (Unix seconds), not a Qt
// type, so this stays includable from a Qt-free translation unit.
inline bool ShouldValidateTwitchToken(std::int64_t lastValidatedAtUnixS, std::int64_t nowUnixS,
				       std::uint32_t intervalS = kDefaultTimingConstants.tokenValidationIntervalS)
{
	if (lastValidatedAtUnixS <= 0) {
		return true;
	}
	return (nowUnixS - lastValidatedAtUnixS) >= static_cast<std::int64_t>(intervalS);
}

} // namespace signalbox::core
