/*
 * SignalBox - core/DetectionStateMachine.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Implements the transition table from DESIGN.md Section 2.2 and the
 * ADDENDUM. See DetectionStateMachine.h for the full contract; the
 * comments here focus on *why* each branch exists, not what it does.
 */

#include "DetectionStateMachine.h"

#include "CategoryResolver.h" // CategoryResolver::normalize() - see normalizedEquals()'s doc comment (item 8 fix).

namespace signalbox::core {

DetectionStateMachine::DetectionStateMachine(Listener &listener, const TimingConstants &timing)
	: listener_(listener),
	  timing_(timing)
{
}

void DetectionStateMachine::setTiming(const TimingConstants &timing)
{
	timing_ = timing;
}

bool DetectionStateMachine::sameGame(const detection::InstalledGame &a, const detection::InstalledGame &b)
{
	// Prefer the platform's own identifier (stable across path/casing
	// noise); fall back to the install directory; last resort is the
	// display name. Mirrors CategoryResolver's tier-1 key shape
	// (DESIGN.md 1.5) without depending on it.
	if (!a.platformId.empty() && !b.platformId.empty())
		return a.platform == b.platform && a.platformId == b.platformId;
	if (!a.installRoot.empty() && !b.installRoot.empty())
		return a.installRoot == b.installRoot;
	return a.displayName == b.displayName;
}

bool DetectionStateMachine::normalizedEquals(const std::wstring &a, const std::wstring &b)
{
	// Item 8 fix: was a bare trim+lowercase, which compares a Steam-style
	// display name carrying (tm)/(r)/(c) glyphs unequal to Twitch's own
	// (glyph-free) category name for the exact same game - a false
	// Trigger A mismatch prompt on the happy path. CategoryResolver::
	// normalize() is the same normalization the resolver itself uses, so
	// this and CategoryResolver agree on what "the same name" means. See
	// the class doc comment's "COMPARING" note.
	return CategoryResolver::normalize(a) == CategoryResolver::normalize(b);
}

bool DetectionStateMachine::suspended() const
{
	// Deliberately excludes ExternalOverride: that state freezes the
	// whole machine at the call sites in onPollResult/onTick/
	// onTrackedProcessExited, so by the time suspended() could matter
	// state_ is never ExternalOverride here.
	return manualLock_ || automationPaused_ || streamEndingHold_;
}

bool DetectionStateMachine::nothingIsHappening() const
{
	// Grace is excluded on purpose even though nothing is detected during
	// it: a game that just closed is Trigger B's territory, and having
	// two prompts race for the same silence is how you get asked the same
	// question twice in different words. Once grace resolves (to Fallback)
	// the idle clock starts from there, which is the right moment - the
	// user closed a game and then did nothing for two minutes.
	if (state_ != State::Idle && state_ != State::Fallback)
		return false;
	if (activeGame_ || pendingCandidate_ || graceGame_)
		return false;
	if (promptOutstanding_)
		return false;
	return !suspended();
}

void DetectionStateMachine::pruneOldChanges(Clock::time_point now)
{
	const auto window = std::chrono::seconds(timing_.flapBreakerWindowS);
	while (!changeTimestamps_.empty() && (now - changeTimestamps_.front()) > window)
		changeTimestamps_.pop_front();
}

void DetectionStateMachine::recordAutomatedChange()
{
	const auto now = Clock::now();
	changeTimestamps_.push_back(now);
	pruneOldChanges(now);
	if (changeTimestamps_.size() > timing_.flapBreakerN && !automationPaused_) {
		automationPaused_ = true;
		listener_.onAutomationPaused(L"Category changes paused — this game appears unstable. Resume?");
	}
}

void DetectionStateMachine::onPollResult(std::optional<detection::DetectedGame> result)
{
	if (state_ == State::ExternalOverride)
		return; // Hard freeze - only resume() moves out of this state.

	if (!result.has_value()) {
		// Nothing detected this poll. A candidate that hasn't finished
		// confirming yet must win CONSECUTIVE polls (DESIGN.md 2.2) -
		// drop it and fall back to whatever we were doing before.
		if (state_ == State::Pending) {
			pendingCandidate_.reset();
			pendingConfirmPolls_ = 0;
			pendingCameFromGrace_ = false;
			state_ = activeGame_ ? State::Active : (graceGame_ ? State::Grace : State::Idle);
		}
		// ACTIVE(g) seeing no detection does NOT by itself start GRACE -
		// that transition is driven exclusively by
		// onTrackedProcessExited(), which is how the crash-vs-clean exit
		// code is known and the correct grace window gets sized. A
		// momentary miss here (e.g. a single dropped poll) must not
		// itself act as an exit signal.
		return;
	}

	const detection::InstalledGame &game = result->game;

	// Alt-tab / already-confirmed no-op: the same game keeps winning
	// polls while ACTIVE. Detection keys on process existence, not
	// focus, so this is the common case while the user is playing -
	// nothing to do, no counters move, no Listener call.
	if (state_ == State::Active && activeGame_ && sameGame(*activeGame_, game))
		return;

	// Reappearance during GRACE (or while a GameClosed prompt raised by
	// that grace's expiry is still outstanding) - the anti-flap core of
	// DESIGN.md 2.2. Route through the normal CONFIRM_POLLS gate (a
	// crash-relaunch can itself be flaky for the first poll or two) but
	// remember it came from grace so confirmPendingCandidate() applies
	// zero Twitch calls.
	const bool reappearingFromGrace = (state_ == State::Grace) && graceGame_ && sameGame(*graceGame_, game);

	if (pendingCandidate_ && sameGame(*pendingCandidate_, game)) {
		++pendingConfirmPolls_;
	} else {
		pendingCandidate_ = game;
		pendingConfirmPolls_ = 1;
		pendingCameFromGrace_ = reappearingFromGrace;
	}
	// If a DIFFERENT game confirms while GRACE is waiting on its own
	// exited game, that's a normal switch-in candidate - it falls
	// through to confirmPendingCandidate()'s "changingGame" path below
	// once CONFIRM_POLLS is reached, same as any other new candidate.
	state_ = State::Pending;

	if (pendingConfirmPolls_ >= std::max<std::uint32_t>(1, timing_.confirmPolls))
		confirmPendingCandidate();
}

void DetectionStateMachine::confirmPendingCandidate()
{
	detection::InstalledGame game = *pendingCandidate_;
	const bool cameFromGraceSameGame = pendingCameFromGrace_ && graceGame_ && sameGame(*graceGame_, game);
	pendingCandidate_.reset();
	pendingConfirmPolls_ = 0;
	pendingCameFromGrace_ = false;

	// A game confirming ends the idle spell, so Trigger D re-arms: the
	// next stretch of nothing-detected is a NEW silence and worth one
	// question of its own. Without this, the idle prompt would be a
	// once-per-OBS-session event, which is not what "you have been
	// sitting here with no game for two minutes" means.
	noGameIdleAsked_ = false;
	idleSince_.reset();

	if (cameFromGraceSameGame) {
		// Crash-then-relaunch inside the grace window: the category was
		// never touched while GRACE was waiting, so returning to ACTIVE
		// here is a pure bookkeeping move - zero Twitch calls, no log
		// entry, and any GameClosed prompt that grace expiry had already
		// raised is now moot.
		activeGame_ = game;
		graceGame_.reset();
		promptOutstanding_ = false;
		state_ = State::Active;
		return;
	}

	const bool changingGame = !activeGame_ || !sameGame(*activeGame_, game);
	const State previousState = state_;

	// TRIGGER C - creative/dev apps are offered, never applied.
	//
	// ORDER MATTERS: this is evaluated BEFORE Trigger A, and the harness
	// (SECTION 2d) fails if that is ever reversed. Going live with your
	// editor already open is the single likeliest way to reach here, and
	// with Trigger A first the user is asked "you're live in Just
	// Chatting but Visual Studio Code is running - set to Visual Studio
	// Code?" and answering yes APPLIES it. That is precisely the
	// automatic switch Trigger C exists to prevent, reintroduced with an
	// extra click in front of it. Trigger C asks the more specific and
	// more useful question ("streaming it, or is it just open?"), so it
	// wins; a prompt-only app never produces a go-live mismatch prompt.
	//
	// This also sits ahead of the automatic switch rather than inside it,
	// because "don't switch" is the whole point: a prompt-only app must
	// not change the category even when the prompt itself can't be shown
	// (not live, prompts turned off, already declined this stream). The
	// fallthrough for those cases is to hold the current category, which
	// is the same safe default every other unanswered prompt resolves to.
	//
	// activeGame_ is still set to the app either way. That is deliberate:
	// it is what stops the next poll re-entering here and asking again
	// every few seconds, and a real game arriving later is simply a
	// different game, so it confirms and switches through the normal
	// path with no special casing.
	if (changingGame && isPromptOnly(game)) {
		activeGame_ = game;
		graceGame_.reset();
		state_ = State::Active;

		const bool canAsk = live_ && promptsEnabled_ && !creativeAppSuppressed_ && !suspended();
		if (canAsk) {
			raiseCreativeAppPrompt(game);
		} else {
			promptOutstanding_ = false;
			listener_.onLogEntry(L"Detected \"" + game.displayName +
						      L"\" - not switching on its own, it needs your say-so",
					      currentLiveCategory_);
		}
		return;
	}

	// A DIFFERENT GAME ENDS A STREAM-ENDING HOLD. Sitting here below the
	// prompt-only branch above is the whole point: launching a real game
	// is proof the user is not wrapping up, but opening a browser or an
	// editor during an outro is not, and a creative app confirming has
	// already returned by now. Cleared BEFORE the switch decision below
	// so this poll's switch actually goes through rather than being
	// suppressed by a hold we have just decided is over.
	if (changingGame && streamEndingHold_) {
		streamEndingHold_ = false;
		listener_.onLogEntry(L"\"" + game.displayName + L"\" started — stream-ending hold lifted", L"");
	}

	// ADDENDUM Trigger A gates the switch rather than following it - see
	// this class's header doc comment ("TRIGGER A GATES THE SWITCH").
	// Only evaluated on a genuine game change arriving from a non-Active
	// state; an ordinary in-stream game-to-game switch (previousState
	// already Active) is never subject to it - the go-live check is a
	// once-per-stream thing, not a gate on every automatic switch.
	if (changingGame && previousState != State::Active && goLiveMismatchApplies(game)) {
		activeGame_ = game;
		graceGame_.reset();
		promptOutstanding_ = false;
		state_ = State::Active;
		raiseGoLiveMismatch(game);
		return;
	}

	activeGame_ = game;
	graceGame_.reset();
	promptOutstanding_ = false;
	state_ = State::Active;

	if (changingGame) {
		if (!suspended()) {
			listener_.onSwitchIn(game);
			recordAutomatedChange();

			// NO LOG ENTRY HERE, deliberately. onSwitchIn() is
			// fire-and-forget - the switch can still fail on a
			// disconnected Twitch, an expired token, an unmappable
			// game, or an external writer - and this class never
			// learns which. It used to log "Switched to X" anyway,
			// immediately after the coordinator had logged "Twitch is
			// not connected - couldn't switch to X". Both lines, in
			// that order, in the same log. The second one was simply
			// false.
			//
			// The coordinator already reports every outcome it
			// actually observes, success and failure alike, and its
			// feedback channel fires on real success with the name
			// that was really applied. That is where the switch gets
			// recorded, and where Undo attaches - see CategoryDock's
			// categoryApplied callback.
		}
	}
}

void DetectionStateMachine::onTrackedProcessExited(detection::ExitReason reason, const detection::InstalledGame &exitedGame)
{
	if (state_ == State::ExternalOverride)
		return;
	// Item 7a fix: accept whenever the exit identity matches activeGame_
	// (our real incumbent), REGARDLESS of state_ being Active vs Pending-
	// with-an-incumbent - see this method's header doc comment. The old
	// `state_ != State::Active` check silently swallowed the incumbent's
	// exit any time a transient rival had won a single poll (moving
	// state_ to Pending while activeGame_ stayed set), which meant a game
	// could exit and never leave ACTIVE - no Grace, no fallback, no
	// further switches until a different game happened to confirm.
	if (!activeGame_ || !sameGame(*activeGame_, exitedGame))
		return; // No incumbent, or this exit belongs to someone else (e.g. a transient rival) - ignore.

	graceGame_ = activeGame_;
	activeGame_.reset();
	graceReason_ = reason;
	const auto graceSeconds =
		(reason == detection::ExitReason::Crash) ? timing_.crashGraceS : timing_.cleanExitGraceS;
	graceDeadline_ = Clock::now() + std::chrono::seconds(graceSeconds);
	promptOutstanding_ = false;
	pendingCandidate_.reset();
	pendingConfirmPolls_ = 0;
	pendingCameFromGrace_ = false;
	state_ = State::Grace;
}

void DetectionStateMachine::onTick()
{
	if (state_ == State::ExternalOverride)
		return;

	const auto now = Clock::now();

	if (state_ == State::Grace) {
		if (promptOutstanding_) {
			if (now >= promptDeadline_) {
				// TIMEOUT -> HOLD, unconditionally (ADDENDUM hard
				// constraint #2). This is the only outcome of a
				// GameClosed timeout, regardless of any other setting.
				promptOutstanding_ = false;
				listener_.onLogEntry(L"No response — holding \"" + currentLiveCategory_ + L"\"",
						      currentLiveCategory_);
				state_ = State::Fallback;
				graceGame_.reset();
			}
		} else if (now >= graceDeadline_) {
			// !streamEndingHold_ and not !suspended(): a manual lock
			// deliberately still asks (the answer is useful even when
			// the action is suppressed), but a stream-ending hold is a
			// statement that the user is done being asked things.
			if (live_ && promptsEnabled_ && !gameClosedSuppressed_ && !streamEndingHold_ && graceGame_) {
				promptOutstanding_ = true;
				promptKind_ = PromptKind::GameClosed;
				promptRelevantGame_ = graceGame_;
				promptDeadline_ = now + std::chrono::seconds(timing_.promptTimeoutS);
				listener_.onPrompt(PromptKind::GameClosed, *graceGame_);
			} else {
				// Not live, prompts disabled, or "don't ask again this
				// stream" was set on a previous GameClosed response -
				// hold silently. Never onApplyFallback() here.
				listener_.onLogEntry(L"No game detected — holding \"" + currentLiveCategory_ + L"\"",
						      currentLiveCategory_);
				state_ = State::Fallback;
				graceGame_.reset();
			}
		}
		return;
	}

	if (promptOutstanding_ && promptKind_ == PromptKind::GoLiveMismatch && now >= promptDeadline_) {
		// GoLiveMismatch timeout: also always holds - keep whatever is
		// currently live, do not switch.
		promptOutstanding_ = false;
		listener_.onLogEntry(L"No response — keeping \"" + currentLiveCategory_ + L"\"", currentLiveCategory_);
	}

	if (promptOutstanding_ && promptKind_ == PromptKind::CreativeApp && now >= promptDeadline_) {
		// Trigger C timeout holds too, and holding here is not a
		// consolation outcome - it is the correct one. An unanswered
		// creative-app prompt most likely means the app is open and the
		// user is doing something else entirely, which is exactly the
		// situation where switching the category would be wrong.
		promptOutstanding_ = false;
		listener_.onLogEntry(L"No response — keeping \"" + currentLiveCategory_ + L"\", still watching for a game",
				      currentLiveCategory_);
	}

	if (promptOutstanding_ && promptKind_ == PromptKind::NoGameIdle && now >= promptDeadline_) {
		// Trigger D times out to a hold like everything else. It says
		// nothing about the category here because, unlike the other
		// three, this one can fire while offline, where
		// currentLiveCategory_ is deliberately empty (see setLive) -
		// "holding \"\"" is worse than saying what actually happened.
		promptOutstanding_ = false;
		listener_.onLogEntry(L"No response — leaving your category alone, still watching for a game", L"");
	}

	serviceIdlePrompt(now);
}

void DetectionStateMachine::serviceIdlePrompt(Clock::time_point now)
{
	if (!nothingIsHappening()) {
		idleSince_.reset(); // Any activity restarts the full window, never resumes it.
		return;
	}

	if (!idleSince_) {
		idleSince_ = now;
		return;
	}

	// Every one of these can turn the prompt off, and none of them stops
	// the clock: the user can enable the setting, or connect Twitch, part
	// way through an idle spell and get asked at the right moment rather
	// than two minutes after flipping a switch.
	if (!noGameIdleEnabled_ || !promptsEnabled_ || noGameIdleSuppressed_ || noGameIdleAsked_)
		return;

	if ((now - *idleSince_) < std::chrono::seconds(timing_.noGameIdlePromptS))
		return;

	// Set BEFORE raising, not in the response handler: every way this
	// prompt can end (answered, ignored until timeout, dismissed by a
	// game confirming) has to count as "already asked this spell", and
	// the only place that catches all three is here.
	noGameIdleAsked_ = true;
	promptOutstanding_ = true;
	promptKind_ = PromptKind::NoGameIdle;
	// Trigger D is about the ABSENCE of a game, so there is no relevant
	// game to name - and leaving a stale one here would put the wrong
	// name under "Fix This!" (see CategoryDock::currentGameForFix(),
	// which asks promptRelevantGame() first).
	promptRelevantGame_.reset();
	promptDeadline_ = now + std::chrono::seconds(timing_.promptTimeoutS);
	listener_.onPrompt(PromptKind::NoGameIdle, detection::InstalledGame{});
}

void DetectionStateMachine::setPromptOnlyPredicate(PromptOnlyPredicate predicate)
{
	promptOnlyPredicate_ = std::move(predicate);
}

bool DetectionStateMachine::isPromptOnly(const detection::InstalledGame &game) const
{
	return promptOnlyPredicate_ && promptOnlyPredicate_(game);
}

void DetectionStateMachine::raiseCreativeAppPrompt(const detection::InstalledGame &game)
{
	promptOutstanding_ = true;
	promptKind_ = PromptKind::CreativeApp;
	promptRelevantGame_ = game;
	promptDeadline_ = Clock::now() + std::chrono::seconds(timing_.promptTimeoutS);
	listener_.onPrompt(PromptKind::CreativeApp, game);
}

bool DetectionStateMachine::goLiveMismatchApplies(const detection::InstalledGame &candidate) const
{
	if (!live_ || !promptsEnabled_ || goLiveMismatchSuppressed_ || goLiveMismatchAskedThisStream_)
		return false;
	if (streamEndingHold_)
		return false; // An outro is not a moment to be asked questions - see the class comment.
	if (currentLiveCategory_.empty())
		return false; // Haven't learned the live category yet; re-checked from onLiveCategoryKnown().
	return !normalizedEquals(candidate.displayName, currentLiveCategory_);
}

void DetectionStateMachine::raiseGoLiveMismatch(const detection::InstalledGame &game)
{
	goLiveMismatchAskedThisStream_ = true;
	promptOutstanding_ = true;
	promptKind_ = PromptKind::GoLiveMismatch;
	promptRelevantGame_ = game;
	promptDeadline_ = Clock::now() + std::chrono::seconds(timing_.promptTimeoutS);
	listener_.onPrompt(PromptKind::GoLiveMismatch, game);
}

void DetectionStateMachine::maybeRaiseGoLiveMismatch()
{
	// Post-hoc path: covers the switch having already happened silently
	// (not live at confirm time, or the live category only became known
	// afterward via onLiveCategoryKnown()) - called from setLive(true)
	// and onLiveCategoryKnown(). confirmPendingCandidate() itself no
	// longer calls this; it gates the switch up front instead (see
	// header doc comment).
	if (state_ != State::Active || !activeGame_)
		return; // Nothing confirmed yet to offer as "Set to <game>".
	if (!goLiveMismatchApplies(*activeGame_))
		return;
	raiseGoLiveMismatch(*activeGame_);
}

void DetectionStateMachine::respondToPrompt(bool acceptAction, bool dontAskAgainThisStream)
{
	if (!promptOutstanding_)
		return; // Nothing to answer (already timed out, or state machine moved on).

	const PromptKind kind = promptKind_;
	const std::optional<detection::InstalledGame> game = promptRelevantGame_;
	promptOutstanding_ = false;

	if (kind == PromptKind::GoLiveMismatch) {
		if (dontAskAgainThisStream)
			goLiveMismatchSuppressed_ = true;
		if (acceptAction && game) {
			if (!suspended()) {
				listener_.onSwitchIn(*game);
				recordAutomatedChange();
				// No log entry - see confirmPendingCandidate()'s
				// note on why this class must not report an outcome
				// it never learns.
				
			}
			activeGame_ = game;
			state_ = State::Active;
		} else {
			listener_.onLogEntry(L"Kept \"" + currentLiveCategory_ + L"\" at go-live", currentLiveCategory_);
		}
		return;
	}

	if (kind == PromptKind::CreativeApp) {
		if (dontAskAgainThisStream)
			creativeAppSuppressed_ = true;

		if (acceptAction && game) {
			if (!suspended()) {
				listener_.onSwitchIn(*game);
				recordAutomatedChange();
				// No log entry here either - same reason.
			}
			// activeGame_ is already this app (set when the prompt
			// was raised) and state_ is already Active, so there is
			// no bookkeeping left to do here - only the switch that
			// was deliberately withheld until now.
			return;
		}

		// Declined: keep watching. activeGame_ stays pointed at the app
		// so this does not re-ask on the next poll, and detection keeps
		// running underneath - a game launching later is a different
		// game and confirms normally.
		listener_.onLogEntry(L"Keeping \"" + currentLiveCategory_ + L"\" - still watching for a game",
				      currentLiveCategory_);
		return;
	}

	if (kind == PromptKind::NoGameIdle) {
		if (dontAskAgainThisStream)
			noGameIdleSuppressed_ = true;

		if (acceptAction) {
			if (!suspended()) {
				// NOT gated on fallbackEnabled_ - see
				// respondToPrompt()'s doc comment for why this one
				// case differs from GameClosed's identical-looking
				// accept.
				listener_.onApplyFallback();
				recordAutomatedChange();
				// No log entry - onApplyFallback() is
				// fire-and-forget and the coordinator reports the
				// real outcome, same as every other switch path.
			}
			return;
		}

		listener_.onLogEntry(dontAskAgainThisStream
					      ? L"Recording, not streaming — leaving your category alone"
					      : L"Waiting for a game — your category is untouched",
				      L"");
		return;
	}

	// PromptKind::GameClosed
	if (dontAskAgainThisStream)
		gameClosedSuppressed_ = true;

	if (acceptAction && fallbackEnabled_) {
		if (!suspended()) {
			listener_.onApplyFallback();
			recordAutomatedChange();
			// No log entry - applyFallback() is fire-and-forget too.
		}
	} else if (acceptAction && !fallbackEnabled_) {
		// Fallback switching is off by default (project brief) - the
		// explicit "Switch to <fallback>" response still degrades to a
		// hold, visibly, rather than silently doing nothing.
		listener_.onLogEntry(L"Fallback switching is off — holding \"" + currentLiveCategory_ + L"\"",
				      currentLiveCategory_);
	} else {
		listener_.onLogEntry(L"Holding \"" + currentLiveCategory_ + L"\"", currentLiveCategory_);
	}
	state_ = State::Fallback;
	graceGame_.reset();
}

void DetectionStateMachine::onExternalChangeDetected()
{
	state_ = State::ExternalOverride;
	promptOutstanding_ = false;
	pendingCandidate_.reset();
	pendingConfirmPolls_ = 0;

	// automationPaused_ is set here as well as state_, and that is not
	// redundant bookkeeping - it is the ONLY thing the dock can see.
	// isAutomationPaused() reads this flag, and without it the dock
	// showed a running, healthy-looking plugin while onPollResult() was
	// returning immediately on every poll. That combination produced a
	// silent 45-minute dead zone in testing: detection appeared frozen
	// on a game that had been closed for three quarters of an hour, the
	// Rescan button rebuilt the index and then dropped the result on the
	// floor, and nothing anywhere said why.
	//
	// Note the asymmetry with the coordinator that calls this: it logs
	// "standing down for this switch". This state is NOT per-switch -
	// only resume() leaves it. Respecting a human's manual change is
	// right; doing so permanently and invisibly is not, which is why
	// resume() now has a button attached to it.
	automationPaused_ = true;

	listener_.onAutomationPaused(L"Category was changed outside SignalBox — automation paused");
}

void DetectionStateMachine::resume()
{
	// Re-detect from scratch rather than assuming the previous ACTIVE
	// game still holds (DESIGN.md 3.5) - whatever caused the pause may
	// have invalidated our picture of the world.
	state_ = State::Idle;
	automationPaused_ = false;
	streamEndingHold_ = false; // "Resume automatic switching" means all of it, not most of it.
	activeGame_.reset();
	graceGame_.reset();
	pendingCandidate_.reset();
	pendingConfirmPolls_ = 0;
	promptOutstanding_ = false;

	// A fresh start is a fresh idle spell: whatever was asked (or
	// suppressed) belonged to the picture of the world we just threw
	// away.
	noGameIdleAsked_ = false;
	idleSince_.reset();
}

void DetectionStateMachine::setManualLock(bool locked)
{
	manualLock_ = locked;
}

void DetectionStateMachine::beginStreamEndingHold()
{
	if (streamEndingHold_)
		return; // Already held - don't log the same thing twice.
	streamEndingHold_ = true;
	// Any outstanding prompt goes with it. The user has just told us what
	// they are doing, which answers whatever was being asked more
	// directly than any of the buttons would have.
	promptOutstanding_ = false;
	listener_.onLogEntry(L"Stream ending — holding your category until the stream stops or a game starts", L"");
}

void DetectionStateMachine::clearStreamEndingHold()
{
	if (!streamEndingHold_)
		return;
	streamEndingHold_ = false;
	listener_.onLogEntry(L"Stream-ending hold released — automatic switching is back on", L"");
}

bool DetectionStateMachine::streamEndingHold() const
{
	return streamEndingHold_;
}

void DetectionStateMachine::setNoGameIdleEnabled(bool enabled)
{
	noGameIdleEnabled_ = enabled;
}

bool DetectionStateMachine::noGameIdleEnabled() const
{
	return noGameIdleEnabled_;
}

void DetectionStateMachine::setLive(bool live)
{
	if (live == live_)
		return;
	live_ = live;
	if (live_) {
		// Fresh stream: per-stream suppression flags reset (ADDENDUM
		// hard constraint #3's scope is explicitly "this stream").
		goLiveMismatchSuppressed_ = false;
		gameClosedSuppressed_ = false;
		creativeAppSuppressed_ = false;
		noGameIdleSuppressed_ = false;
		goLiveMismatchAskedThisStream_ = false;

		// Going live while a stream-ending hold is on means the hold is
		// stale - it belongs to the stream that already finished (or to
		// a press the user has since thought better of). Clearing it
		// silently rather than through clearStreamEndingHold() because
		// "hold released" alongside "you just went live" is two lines
		// for one event.
		streamEndingHold_ = false;

		maybeRaiseGoLiveMismatch();
	} else {
		// Going offline: nothing left to prompt about (ADDENDUM hard
		// constraint #4, "ONLY WHEN LIVE") - drop any outstanding
		// prompt without side effects. Detection/state tracking is
		// untouched; only the prompt is dismissed.
		promptOutstanding_ = false;

		// Item 8.3 fix: currentLiveCategory_ must not survive into the
		// next stream. Left alone, the next setLive(true) would run
		// maybeRaiseGoLiveMismatch() SYNCHRONOUSLY against THIS
		// stream's stale value, before syncLiveCategoryOnGoLive()'s
		// fresh GET can return - a stale-basis prompt would then set
		// goLiveMismatchAskedThisStream_ and burn the new stream's
		// once-per-stream budget before the correct data even arrives.
		// goLiveMismatchApplies() already treats an empty
		// currentLiveCategory_ as "nothing to compare yet" (see its own
		// doc comment), so clearing this is always safe - never a false
		// negative, only ever prevents a false positive.
		currentLiveCategory_.clear();

		// THE STREAM ENDED - which is the thing the hold was waiting
		// for. This is the clear that matters: without it, "Stream
		// Ending" would be a lock the user has to remember to undo, and
		// they would find out they hadn't next session, live, under the
		// wrong category. Logged (unlike the go-live clear above)
		// because nothing else on screen changes at this moment, so the
		// activity log is the only place it can be seen to have
		// happened.
		if (streamEndingHold_) {
			streamEndingHold_ = false;
			listener_.onLogEntry(L"Stream stopped — stream-ending hold cleared, automatic switching is back on",
					      L"");
		}
	}
}

bool DetectionStateMachine::isLive() const
{
	return live_;
}

void DetectionStateMachine::onLiveCategoryKnown(std::wstring categoryName)
{
	currentLiveCategory_ = std::move(categoryName);
	maybeRaiseGoLiveMismatch(); // Handles the case where this arrives after setLive(true).
}

void DetectionStateMachine::setPromptsEnabled(bool enabled)
{
	promptsEnabled_ = enabled;
}

void DetectionStateMachine::setFallbackEnabled(bool enabled)
{
	fallbackEnabled_ = enabled;
}

bool DetectionStateMachine::promptsEnabled() const
{
	return promptsEnabled_;
}

bool DetectionStateMachine::fallbackEnabled() const
{
	return fallbackEnabled_;
}

State DetectionStateMachine::state() const
{
	return state_;
}

bool DetectionStateMachine::isManuallyLocked() const
{
	return manualLock_;
}

bool DetectionStateMachine::isAutomationPaused() const
{
	return automationPaused_;
}

std::optional<detection::InstalledGame> DetectionStateMachine::activeGame() const
{
	return activeGame_;
}

std::optional<detection::InstalledGame> DetectionStateMachine::pendingCandidate() const
{
	return pendingCandidate_;
}

std::optional<detection::InstalledGame> DetectionStateMachine::graceGame() const
{
	return graceGame_;
}

bool DetectionStateMachine::promptOutstanding() const
{
	return promptOutstanding_;
}

PromptKind DetectionStateMachine::outstandingPromptKind() const
{
	return promptKind_;
}

std::optional<detection::InstalledGame> DetectionStateMachine::promptRelevantGame() const
{
	return promptRelevantGame_;
}

std::optional<std::uint32_t> DetectionStateMachine::secondsUntilNextDeadline() const
{
	std::optional<Clock::time_point> deadline;
	if (promptOutstanding_)
		deadline = promptDeadline_;
	else if (state_ == State::Grace)
		deadline = graceDeadline_;

	if (!deadline)
		return std::nullopt;

	const auto now = Clock::now();
	if (*deadline <= now)
		return std::uint32_t{0};
	return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(*deadline - now).count());
}

} // namespace signalbox::core
