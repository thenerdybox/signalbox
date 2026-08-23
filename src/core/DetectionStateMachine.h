/*
 * SignalBox - core/DetectionStateMachine.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * The state machine from DESIGN.md Section 2 and the ADDENDUM. Pure logic:
 * takes DetectedGame snapshots and timer ticks as input, and calls back
 * through Listener to request actions (a Twitch PATCH, a dock prompt, a
 * log entry). It does not perform any I/O itself and does not know about
 * Qt, OBS, or Twitch - that separation is what lets this be unit tested
 * without a running OBS process, and is what the "threading boundary"
 * requirement in the project brief is protecting.
 *
 * STATES (DESIGN.md 2.2):
 *   IDLE -> PENDING(g) -> ACTIVE(g) -> GRACE(g) -> ACTIVE(g) | ACTIVE(h) | FALLBACK
 * Plus EXTERNAL_OVERRIDE (DESIGN.md 3.5: another writer changed the
 * channel; automation stands down until the user resumes it) and a
 * manual-lock flag that suspends automatic changes independent of state.
 * ExternalOverride is a hard freeze: onPollResult/onTick/
 * onTrackedProcessExited are all no-ops while in it - only resume() exits
 * it. manualLock_ and the flap-breaker's automationPaused_ are the softer
 * kind: internal PENDING/ACTIVE/GRACE tracking keeps running (so the dock
 * can still show "would switch to X"), but the two Twitch-affecting
 * Listener calls (onSwitchIn/onApplyFallback) are suppressed - see
 * suspended().
 *
 * THE PROMPT (DESIGN.md ADDENDUM) supersedes "auto-revert to fallback" as
 * the *default* behavior: the state machine does not silently apply a
 * fallback category on its own initiative. Two triggers surface through
 * Listener instead:
 *   - Trigger A (go-live mismatch): fired once per stream, at the
 *     Idle->Live transition (or as soon afterward as both a confirmed
 *     ACTIVE game and a known live category exist), if they disagree.
 *   - Trigger B (game closed while live): fired when GRACE expires with
 *     nothing detected and the stream is still live.
 * Both are NEVER MODAL and NEVER STEAL FOCUS (ADDENDUM hard constraint
 * #1) - Listener::onPrompt* must only ever result in passive dock/toast
 * state, never a blocking dialog. An unanswered prompt times out
 * (TimingConstants::promptTimeoutS) to the safe default: hold the last
 * category (ADDENDUM hard constraint #2). Concretely: this class calls
 * Listener::onApplyFallback() from exactly ONE place - an explicit accept
 * response to a GameClosed prompt (respondToPrompt(true, ...)). It is
 * NEVER called from onTick()'s timeout handling, never from a disabled-
 * prompts path, and never from a "don't ask again" path. Ignoring the
 * prompt - or never seeing it because prompts/live-ness say don't show
 * it - always means "hold", by construction, not by convention.
 *
 * TRIGGER A GATES THE SWITCH, IT DOES NOT FOLLOW IT: the first confirmed
 * game since going live (or since the live category last became known)
 * calls Listener::onSwitchIn() ONLY if goLiveMismatchApplies() is false
 * (not live, prompts off, already asked/suppressed this stream, live
 * category unknown yet, or it already matches) - a real mismatch defers
 * onSwitchIn entirely until an explicit "Set to <game>" response
 * (respondToPrompt(true, false)). The category the ADDENDUM's worked
 * example shows ("You're live in 'Just Chatting' but Path of Exile 2 is
 * running") is still "Just Chatting" at the moment the prompt appears -
 * switching first and prompting after would make that message a lie.
 * activeGame_/State::Active ARE still set immediately either way (Active
 * means "confirmed as running", a detection fact, independent of whether
 * the Twitch category has been touched yet) - only the Listener call that
 * actually changes the channel is what's deferred.
 *
 * TRIGGER D (NoGameIdle) IS THE ONE PROMPT THAT IS NOT GATED ON
 * LIVE-NESS, and that is a deliberate, eyes-open departure from ADDENDUM
 * hard constraint #4 ("ONLY WHEN LIVE"). The constraint exists because a
 * prompt with no audience is pure interruption - but the situation this
 * trigger covers is the one the constraint gets wrong: sitting in OBS
 * with no game running is exactly when the category is most likely to be
 * stale from the last session, and the useful moment to fix it is BEFORE
 * going live, not after. Everything else about the constraint is kept:
 * it is still non-modal, still times out to a hold, still answerable with
 * one click that silences it. What replaces live-ness as the guard is
 * scarcity - three independent things all have to be true for this to
 * fire, and any one of them turns it off:
 *   - noGameIdleEnabled_ (the caller's own gate; the real plugin ties it
 *     to "Twitch is connected AND the user hasn't turned it off in
 *     Settings", because a question whose answer cannot be carried out
 *     is worse than no question),
 *   - a FULL noGameIdlePromptS of continuous nothing-detected, restarted
 *     from zero by any activity at all, and
 *   - noGameIdleAsked_ being false, which it only is once per idle
 *     spell: it is set when the prompt is raised (answered, timed out,
 *     or otherwise) and cleared only when a game actually confirms.
 * So the worst case is one question per "a game ran and then stopped
 * running", plus one at startup - never a repeating nag, and never a
 * second one while the user is deciding what to do about the first.
 *
 * STREAM-ENDING HOLD (beginStreamEndingHold()) is a self-clearing manual
 * lock. Its whole reason to exist is that the ordinary manual lock is a
 * trap for the case it covers: at the end of a stream the user sets their
 * own outro category, and needs automation to stop touching it - but they
 * are ending a stream, which is exactly when they will not remember to go
 * and switch automation back on, and they will find out next session by
 * streaming a game under last night's outro category. So this hold
 * clears itself on the two events that mean it is no longer wanted:
 * setLive(false) (the stream ended - what they were holding for) and a
 * DIFFERENT game confirming (they are plainly not ending anything). It
 * behaves like manualLock_ in every other respect - see suspended() -
 * and additionally suppresses every prompt, because an outro is not a
 * moment to be asked questions. A prompt-only/creative app confirming
 * deliberately does NOT clear it: opening a browser or an editor to wrap
 * up is part of ending a stream, not a signal that a new one started.
 *
 * PER-STREAM SUPPRESSION ("don't ask again"/"don't ask this stream"):
 * both triggers carry a per-stream suppression flag, reset by setLive()
 * on every Idle/offline -> live transition (a fresh stream is a fresh
 * chance to ask). See respondToPrompt()'s doc comment for how the three-
 * button UI in PromptWidget maps onto the two-argument response.
 *
 * COMPARING "what's running" TO "what's live" (Trigger A): this class
 * compares the detected game's raw InstalledGame::displayName against the
 * live category name via CategoryResolver::normalize() (strips (tm)/(r)/
 * (c), collapses whitespace/light punctuation, casefolds) rather than a
 * bare trim+lowercase - Steam/store display names routinely carry
 * trademark glyphs Twitch's own category names don't ("Tom Clancy's
 * Rainbow Six(R) Siege" vs "Tom Clancy's Rainbow Six Siege"), and a plain
 * trim+lowercase compares those unequal, firing a false mismatch prompt
 * on the happy path. This is still a documented simplification, though a
 * smaller one now: normalizedEquals() compares the raw display name, not
 * the *resolved* Twitch category name CategoryResolver would produce for
 * the candidate - ideally Trigger A would compare resolved-to-resolved.
 * currentLiveCategory_ itself is fed exclusively by onLiveCategoryKnown()
 * (an explicit GET sync, or - since the coordinator's feedback channel
 * closed this gap - the real name a switch/fallback PATCH actually
 * applied); it is never set optimistically from a raw display name
 * in-line with a switch request, and it is cleared on every
 * live -> offline transition (setLive(false)) so a stale basis can never
 * carry into the next stream and spend that stream's once-per-stream
 * prompt budget on last stream's data.
 *
 * THREADING: instances of this class are driven by DetectionEngine and
 * are intended to live on the Qt main thread (all Listener callbacks
 * execute actions that ultimately touch TwitchClient/dock state, which
 * are main-thread-only). DetectionEngine's worker thread only ever hands
 * this class immutable DetectedGame snapshots via a queued cross-thread
 * post - see DetectionEngine.h.
 *
 * PERFORMANCE (lightweight-by-design requirement): onTick() and
 * onPollResult() are O(1) comparisons/counters on the non-transition
 * path - no heap allocation, no string formatting, no I/O. Allocation
 * (building a std::wstring for a log line) happens ONLY inside the
 * branches that already represent a real state transition (a switch, a
 * fallback, a prompt raised/resolved), i.e. work is proportional to
 * events, never to elapsed time. Callers should drive onTick() from a
 * heartbeat that already exists for another reason (e.g. once per
 * DetectionEngine poll) rather than a dedicated timer - see
 * ui/CategoryDock.h for how this is done in practice.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>

#include "../detection/DetectedGame.h"
#include "../detection/ProcessScanner.h" // ExitReason
#include "TimingConstants.h"

namespace signalbox::core {

enum class State {
	Idle,
	Pending,          // Candidate seen, accumulating confirmPolls.
	Active,           // Confirmed; this is the game the state machine believes is running.
	Grace,            // Active game's process vanished; waiting out the grace window
	                  // (and, once the window elapses, waiting out an outstanding
	                  // GameClosed prompt - see promptOutstanding() below).
	Fallback,         // No confirmed game. Reached either by an explicit "switch to
	                  // fallback" response, or by holding after a prompt timeout /
	                  // suppressed prompt / not-live grace expiry. In all cases this
	                  // just means "nothing active, watching for the next candidate" -
	                  // it behaves identically to Idle for accepting a new detection.
	ExternalOverride, // Another writer changed the channel; automation paused (DESIGN.md 3.5).
};

// What kind of unanswered-prompt situation the state machine is asking
// the UI layer to surface. See PromptWidget.h for the actual widget.
enum class PromptKind {
	GoLiveMismatch, // Trigger A: category didn't match what's running at stream start.
	GameClosed,     // Trigger B: grace expired, nothing detected, still live.
	CreativeApp,    // Trigger C: a "prompt": true alias confirmed - offer it, never apply it.
	NoGameIdle,     // Trigger D: nothing detected for noGameIdlePromptS. NOT gated on live-ness.
};

// Listener is how the state machine asks for side effects without
// performing them itself. All calls happen on the thread that owns the
// DetectionStateMachine instance (see class comment - intended to be the
// Qt main thread). Implementations must not block.
class DetectionStateMachine {
public:
	class Listener {
	public:
		virtual ~Listener() = default;

		// Request a category change to the given game. The listener
		// (typically a coordinator wrapping TwitchClient) is
		// responsible for CategoryResolver mapping, MIN_PATCH_SPACING_S
		// pacing, and the redundancy guard (DESIGN.md 2.2, 3.4).
		virtual void onSwitchIn(const detection::InstalledGame &game) = 0;

		// Request the fallback category be applied. Called from exactly
		// one place: an explicit accept response to a GameClosed prompt
		// (see class comment) - never as a silent default.
		virtual void onApplyFallback() = 0;

		// Ask the UI layer to surface a non-modal prompt. The state
		// machine does not wait synchronously - the caller answers
		// later via respondToPrompt(), or the state machine times out
		// on its own per TimingConstants::promptTimeoutS.
		virtual void onPrompt(PromptKind kind, const detection::InstalledGame &relevantGame) = 0;

		// Automation has paused itself (flap breaker tripped, or an
		// external writer was detected). reasonForDock is informational
		// text for the dock notice.
		virtual void onAutomationPaused(const std::wstring &reasonForDock) = 0;

		// Append a line to the dock's activity log with Undo support
		// (DESIGN.md 2.2 "Manual lock"). previousCategory is what Undo
		// should restore. Fired only for events that actually happened
		// (or were explicitly, safely no-ops worth surfacing) - never
		// speculative "would have" text.
		virtual void onLogEntry(const std::wstring &message, const std::wstring &previousCategory) = 0;
	};

	explicit DetectionStateMachine(Listener &listener, const TimingConstants &timing = kDefaultTimingConstants);

	// Live re-tune (e.g. after the user edits "Advanced timing" in
	// SettingsDialog and saves). Takes effect for whatever transition
	// happens next - nothing is retroactively rescheduled (an
	// already-outstanding grace/prompt deadline keeps the deadline it
	// was given; only future ones use the new values).
	void setTiming(const TimingConstants &timing);

	// Feed one poll's result from DetectionEngine. std::nullopt means
	// "nothing detected this poll".
	void onPollResult(std::optional<detection::DetectedGame> result);

	// Feed a tracked process's exit, so GRACE can size its window per
	// DESIGN.md 2.2 (crashGraceS vs cleanExitGraceS). exitedGame is the
	// identity of whatever actually exited (DetectionEngine's own
	// tracked pid - see DetectionEngine.cpp's TRACKED-PID/CHALLENGER
	// PERSISTENCE notes); this is accepted whenever it matches
	// activeGame_ (via sameGame()), REGARDLESS of whether state_ is
	// currently Active or Pending-with-an-incumbent - a transient rival
	// winning a single poll moves state_ to Pending while activeGame_
	// (the true incumbent) stays set (see onPollResult()'s "alt-tab /
	// already-confirmed no-op" comment and confirmPendingCandidate()) -
	// "Pending with a live activeGame_" still means "g is the
	// incumbent," and its exit must not be swallowed while a transient
	// challenger is being evaluated. Anything else (no activeGame_, or
	// an identity that doesn't match it - e.g. a transient rival's own
	// exit) is a stray/unrelated signal and is ignored.
	void onTrackedProcessExited(detection::ExitReason reason, const detection::InstalledGame &exitedGame);

	// Drives all time-based transitions (grace expiry, prompt timeout,
	// go-live-mismatch check once both inputs are known). Call at least
	// as often as TimingConstants::pollIntervalS from the owning
	// thread's timer - in practice, piggyback this on whatever already
	// delivers onPollResult() rather than running a second timer (see
	// class comment's PERFORMANCE note).
	void onTick();

	// User answered a prompt raised via Listener::onPrompt. Maps
	// PromptWidget's three buttons onto this two-argument signature as
	// follows (see PromptWidget.h for the exact button wiring):
	//   GoLiveMismatch: "Set to <game>" -> (true, false)
	//                   "Keep <category>" -> (false, false)
	//                   "Don't ask this stream" -> (false, true)
	//   GameClosed:     "Wait for a new game" -> (false, false)
	//                   "Switch to <fallback>" -> (true, false)
	//                   "Ignore this change" -> (false, checkbox-state)
	//   NoGameIdle:     "Set to <fallback>" -> (true, false)
	//                   "Wait for a game" -> (false, false)
	//                   "Just recording" -> (false, true)
	// acceptAction=true is the only path that can call a Twitch-facing
	// Listener method (onSwitchIn for GoLiveMismatch, onApplyFallback -
	// if fallback is enabled, see setFallbackEnabled - for GameClosed).
	//
	// NoGameIdle's accept is the ONE fallback path that is deliberately
	// NOT gated on fallbackEnabled(). That toggle exists to stop
	// AUTOMATION applying a fallback behind the user's back; here the
	// user has just read a question naming the category and pressed the
	// button that names it again. Degrading that to a silent hold - the
	// correct outcome for GameClosed, whose accept can be reached by
	// habit mid-game - would make the button a lie, and the setting is
	// off by default, so the feature would appear broken out of the box
	// to everyone who never opened Settings.
	// dontAskAgainThisStream suppresses that PromptKind for the rest of
	// the current stream (reset on the next setLive(true)). A call with
	// no prompt outstanding is ignored.
	void respondToPrompt(bool acceptAction, bool dontAskAgainThisStream = false);

	// DESIGN.md 3.5: external writer detected (GET /helix/channels
	// disagreed with what we last set). Suspends automation.
	void onExternalChangeDetected();

	// Dock "resume" action after ExternalOverride or a flap-breaker pause.
	void resume();

	// Dock manual-lock toggle. When true, onPollResult keeps updating
	// internal state (so the dock can still show "would switch to X")
	// but Listener::onSwitchIn/onApplyFallback are never invoked.
	void setManualLock(bool locked);

	// --- Stream-ending hold (see the class comment) ---------------------
	// A manual lock that knows when it is no longer wanted. Suspends
	// every Twitch-facing action and every prompt, and clears itself on
	// setLive(false) or on a different (non-prompt-only) game confirming.
	// Pressing it again - clearStreamEndingHold() - is always available;
	// self-clearing is the safety net, not the only way out. Calling
	// begin twice is harmless (the second call re-affirms the hold and
	// does not re-log).
	void beginStreamEndingHold();
	void clearStreamEndingHold();
	bool streamEndingHold() const;

	// --- Trigger D, the no-game idle prompt (see the class comment) -----
	// The caller's gate on whether Trigger D may fire at all. OFF until
	// somebody turns it on, deliberately: this is the only prompt that
	// can appear while offline, so it does not get to be opt-out at the
	// state-machine layer. The real plugin sets this from "Twitch is
	// connected AND the Settings toggle is on AND we are not in a state
	// where applying a category would be skipped anyway" - see
	// CategoryDock::refreshIdlePromptEnabled(). promptsEnabled() gates it
	// too, like every other prompt.
	void setNoGameIdleEnabled(bool enabled);
	bool noGameIdleEnabled() const;

	// OBS_FRONTEND_EVENT_STREAMING_STARTED/_STOPPED, per requirement 7 -
	// gates both prompts (ADDENDUM hard constraint #4, "ONLY WHEN LIVE")
	// and resets each trigger's per-stream "don't ask again" suppression
	// on every offline->live transition. Does NOT gate onSwitchIn for
	// ordinary confirmed switches - DESIGN.md 2.2's "only-while-live" is
	// an opt-in *policy* toggle owned by PluginConfig/the coordinator,
	// layered on top of (not inside) this class.
	void setLive(bool live);
	bool isLive() const;

	// Coordinator calls this whenever it learns the channel's current
	// category (initial GET /helix/channels sync, or confirmation after
	// a PATCH) so Trigger A has something to compare against. See the
	// class comment's "COMPARING" note for the current simplification.
	void onLiveCategoryKnown(std::wstring categoryName);

	// Dock/settings toggle: whether either prompt is ever raised. When
	// false, a grace expiry or go-live mismatch resolves straight to
	// "hold" with a log entry instead of asking. Default true.
	// Installs the "is this app one we must ask about rather than apply"
	// test - CategoryResolver::isPromptOnly() in the real plugin, a
	// trivial lambda in the harness. Left as a settable predicate rather
	// than a resolver reference so this class keeps its standing
	// property of being exercisable with no resolver, no Qt and no OBS
	// process (see the class doc comment).
	//
	// With no predicate installed, nothing is prompt-only and behavior
	// is exactly what it was before Trigger C existed.
	using PromptOnlyPredicate = std::function<bool(const detection::InstalledGame &)>;
	void setPromptOnlyPredicate(PromptOnlyPredicate predicate);

	void setPromptsEnabled(bool enabled);
	bool promptsEnabled() const;

	// Settings toggle: whether GameClosed's accept response ("Switch to
	// <fallback>") is allowed to call Listener::onApplyFallback() at
	// all. Off by default per the project brief - hold-last-category is
	// the default outcome even when the user explicitly asks to switch
	// to the fallback, unless this has been turned on. When off, that
	// response degrades to a hold (with a log entry saying so).
	void setFallbackEnabled(bool enabled);
	bool fallbackEnabled() const;

	State state() const;
	bool isManuallyLocked() const;
	bool isAutomationPaused() const; // Flap breaker tripped; distinct from manual lock/ExternalOverride.

	// Dock display accessors. Cheap (no formatting) - the dock decides
	// whether/how to render these, and per the lightweight requirement
	// should skip doing so entirely while not visible.
	std::optional<detection::InstalledGame> activeGame() const;
	std::optional<detection::InstalledGame> pendingCandidate() const;
	std::optional<detection::InstalledGame> graceGame() const;
	bool promptOutstanding() const;
	PromptKind outstandingPromptKind() const; // Only valid if promptOutstanding().
	std::optional<detection::InstalledGame> promptRelevantGame() const;

	// Seconds remaining until the next self-driven deadline (grace
	// expiry or prompt timeout), or std::nullopt if nothing is pending -
	// this is the dock's "pending-action countdown" source. Cheap
	// (subtraction + clamp, no formatting).
	std::optional<std::uint32_t> secondsUntilNextDeadline() const;

private:
	using Clock = std::chrono::steady_clock;

	static bool sameGame(const detection::InstalledGame &a, const detection::InstalledGame &b);
	static bool normalizedEquals(const std::wstring &a, const std::wstring &b);

	void confirmPendingCandidate();
	bool goLiveMismatchApplies(const detection::InstalledGame &candidate) const;
	bool isPromptOnly(const detection::InstalledGame &game) const;
	void raiseCreativeAppPrompt(const detection::InstalledGame &game);
	void raiseGoLiveMismatch(const detection::InstalledGame &game);
	void maybeRaiseGoLiveMismatch();
	void recordAutomatedChange();
	void pruneOldChanges(Clock::time_point now);
	bool suspended() const; // manualLock_ || automationPaused_ || streamEndingHold_ (NOT ExternalOverride).

	// True when there is genuinely nothing going on: no confirmed game,
	// no candidate accumulating polls, no grace window, no outstanding
	// prompt, and automation not suspended or frozen. The precondition
	// for Trigger D's idle clock to run at all - see onTick().
	bool nothingIsHappening() const;
	void serviceIdlePrompt(Clock::time_point now);

	Listener &listener_;
	TimingConstants timing_;
	State state_ = State::Idle;
	bool manualLock_ = false;
	bool automationPaused_ = false; // Flap breaker.
	bool streamEndingHold_ = false; // Self-clearing manual lock - see class comment.
	bool live_ = false;
	bool promptsEnabled_ = true;
	bool fallbackEnabled_ = false; // Off by default: hold-last-category is the default (project brief).

	// Pending confirmation (IDLE/GRACE/FALLBACK -> ACTIVE).
	std::optional<detection::InstalledGame> pendingCandidate_;
	std::uint32_t pendingConfirmPolls_ = 0;
	bool pendingCameFromGrace_ = false;

	// Confirmed / grace-tracked games.
	std::optional<detection::InstalledGame> activeGame_;
	std::optional<detection::InstalledGame> graceGame_;
	detection::ExitReason graceReason_ = detection::ExitReason::Clean;
	Clock::time_point graceDeadline_{};

	// Outstanding prompt (at most one at a time - single PromptWidget).
	bool promptOutstanding_ = false;
	PromptKind promptKind_ = PromptKind::GameClosed;
	PromptOnlyPredicate promptOnlyPredicate_; // Empty = nothing is prompt-only.
	std::optional<detection::InstalledGame> promptRelevantGame_;
	Clock::time_point promptDeadline_{};

	// Per-stream "don't ask again" suppression (ADDENDUM hard constraint #3).
	bool goLiveMismatchAskedThisStream_ = false;
	bool goLiveMismatchSuppressed_ = false;
	bool gameClosedSuppressed_ = false;
	bool creativeAppSuppressed_ = false;

	// --- Trigger D state (see the class comment) ---
	bool noGameIdleEnabled_ = false;    // Caller's gate; off until switched on.
	bool noGameIdleSuppressed_ = false; // "Just recording" - reset on the next go-live.
	bool noGameIdleAsked_ = false;      // Once per idle spell; cleared when a game confirms.
	// When the current run of nothing-happening began, or nullopt if
	// something is happening. Reset to nullopt (not just re-stamped) by
	// any activity, so the full noGameIdlePromptS has to elapse again.
	std::optional<Clock::time_point> idleSince_;

	// What we believe is currently live on the channel - see class
	// comment's "COMPARING" note. Empty until onLiveCategoryKnown() is
	// called at least once.
	std::wstring currentLiveCategory_;

	// Flap breaker rolling window (DESIGN.md 2.2). Bounded by
	// flapBreakerWindowS via pruneOldChanges(); never grows unbounded.
	std::deque<Clock::time_point> changeTimestamps_;
};

} // namespace signalbox::core
