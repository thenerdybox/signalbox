/*
 * SignalBox - tests/harness/main.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Standalone proof that CategoryResolver and DetectionStateMachine work
 * together, without an OBS process, a live Twitch connection, or a Qt
 * event loop. Everything here is a plain console program: synthetic
 * DetectedGame/InstalledGame values go in, and the resolver's tier
 * decisions and the state machine's Listener callbacks come out on
 * stdout - see CMakeLists.txt's signalbox-harness target for how
 * this is built (Qt6::Core only, no libobs, no obs-frontend-api).
 *
 * Exit code is nonzero if any scenario's actual outcome didn't match what
 * it asserts, so this doubles as a real (if manual-run) regression check,
 * not just a demo.
 */

#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/CategoryResolver.h"
#include "core/CategorySwitchCoordinator.h"
#include "core/DetectionStateMachine.h"
#include "core/TimingConstants.h"
#include "detection/DetectedGame.h"
#include "detection/IGameProvider.h"
#include "detection/HelperDenylist.h"
#include "detection/InstallIndex.h"

using signalbox::core::CategoryResolver;
using signalbox::core::CategorySwitchCoordinator;
using signalbox::core::ChannelSnapshot;
using signalbox::core::DetectionStateMachine;
using signalbox::core::IChannelClient;
using signalbox::core::ICategoryLookup;
using signalbox::core::IUserOverrideStore;
using signalbox::core::PromptKind;
using signalbox::core::ResolvedCategory;
using signalbox::core::ShouldValidateTwitchToken;
using signalbox::core::State;
using signalbox::core::TimingConstants;
using signalbox::core::UserOverride;
using signalbox::detection::Confidence;
using signalbox::detection::DetectedGame;
using signalbox::detection::ExitReason;
using signalbox::detection::IGameProvider;
using signalbox::detection::InstalledGame;
using signalbox::detection::InstallIndex;
using signalbox::detection::Platform;

namespace {

int g_failures = 0;

std::string Narrow(const std::wstring &w)
{
	std::string s;
	s.reserve(w.size());
	for (wchar_t c : w) {
		s.push_back(c < 128 ? static_cast<char>(c) : '?');
	}
	return s;
}

// Counterpart to Narrow(), for feeding SIGNALBOX_SOURCE_DIR (a narrow
// string literal from CMake) into the wide-string file APIs. ASCII-only,
// which is all a build path needs to be here.
std::wstring Widen(const char *s)
{
	std::wstring w;
	for (const char *p = s; *p != 0; ++p) {
		w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
	}
	return w;
}

void Check(bool condition, const char *what)
{
	if (condition) {
		std::printf("  [PASS] %s\n", what);
	} else {
		std::printf("  [FAIL] %s\n", what);
		++g_failures;
	}
}

InstalledGame MakeGame(const wchar_t *displayName, Platform platform, const wchar_t *platformId,
			const wchar_t *installRoot = L"")
{
	InstalledGame game;
	game.displayName = displayName;
	game.platform = platform;
	game.platformId = platformId;
	game.installRoot = installRoot;
	return game;
}

// ---------------------------------------------------------------------
// SECTION 1: CategoryResolver
// ---------------------------------------------------------------------

// In-memory IUserOverrideStore stand-in for PluginConfig - proves
// CategoryResolver needs nothing OBS-specific for tier 1.
class FakeOverrideStore : public IUserOverrideStore {
public:
	void set(const std::wstring &key, UserOverride value) { overrides_[key] = std::move(value); }

	std::optional<UserOverride> findUserOverride(const std::wstring &key) const override
	{
		const auto it = overrides_.find(key);
		if (it == overrides_.end()) {
			return std::nullopt;
		}
		return it->second;
	}

private:
	std::map<std::wstring, UserOverride> overrides_;
};

// Canned tier-3 lookup - proves the fuzzy-match scoring/threshold logic
// without a live Twitch connection.
class FakeCategoryLookup : public ICategoryLookup {
public:
	void seedExact(const std::wstring &name, ResolvedCategory category) { exact_[name] = std::move(category); }
	void seedSearch(const std::wstring &query, std::vector<ResolvedCategory> results)
	{
		search_[query] = std::move(results);
	}

	void findExact(const std::wstring &name, std::function<void(std::optional<ResolvedCategory>)> onResult) override
	{
		const auto it = exact_.find(name);
		onResult(it != exact_.end() ? std::optional<ResolvedCategory>(it->second) : std::nullopt);
	}

	void search(const std::wstring &query, std::function<void(std::vector<ResolvedCategory>)> onResult) override
	{
		const auto it = search_.find(query);
		onResult(it != search_.end() ? it->second : std::vector<ResolvedCategory>{});
	}

private:
	std::map<std::wstring, ResolvedCategory> exact_;
	std::map<std::wstring, std::vector<ResolvedCategory>> search_;
};

void RunResolverScenarios()
{
	std::printf("\n=== SECTION 1: CategoryResolver ===\n");

	FakeOverrideStore overrides;
	CategoryResolver resolver(overrides);
	resolver.setAliasTableForTesting({
		{CategoryResolver::normalize(L"Photoshop"), L"Art"},
		{CategoryResolver::normalize(L"Ableton Live 12"), L"Music"},
		{CategoryResolver::normalize(L"Visual Studio Code"), L"Software and Game Development"},
	});

	// --- Tier 1: persistent user override wins outright, no tier 2/3. ---
	{
		std::printf("-- Tier 1: user override --\n");
		const InstalledGame game = MakeGame(L"Some Roguelike", Platform::Steam, L"123456");
		overrides.set(CategoryResolver::overrideKeyFor(game), UserOverride{L"999", L"Overridden Category", false});

		std::optional<ResolvedCategory> outcome;
		resolver.resolve(game, [&](std::optional<ResolvedCategory> r) { outcome = r; });

		Check(outcome.has_value() && outcome->categoryName == L"Overridden Category",
		      "override resolves to the persisted category, ignoring alias/Twitch");
		std::printf("     resolved -> %s\n", outcome ? Narrow(outcome->categoryName).c_str() : "(nullopt)");
	}

	// --- Tier 1: "ignore this exe entirely" resolves to nothing. ---
	{
		std::printf("-- Tier 1: ignored override --\n");
		const InstalledGame game = MakeGame(L"Discord", Platform::Generic, L"", L"C:\\Apps\\Discord\\");
		overrides.set(CategoryResolver::overrideKeyFor(game), UserOverride{L"", L"", true});

		std::optional<ResolvedCategory> outcome;
		bool called = false;
		resolver.resolve(game, [&](std::optional<ResolvedCategory> r) {
			outcome = r;
			called = true;
		});

		Check(called && !outcome.has_value(), "\"ignore this exe\" resolves to nothing (never guesses)");
	}

	// --- Tier 2: bundled alias table (the non-game mappings). ---
	{
		std::printf("-- Tier 2: alias table --\n");
		const InstalledGame game = MakeGame(L"Photoshop", Platform::Generic, L"");
		std::optional<ResolvedCategory> outcome;
		resolver.resolve(game, [&](std::optional<ResolvedCategory> r) { outcome = r; });

		Check(outcome.has_value() && outcome->categoryName == L"Art", "Photoshop -> Art via data/aliases.json seed");
		std::printf("     resolved -> %s\n", outcome ? Narrow(outcome->categoryName).c_str() : "(nullopt)");
	}

	// --- Tier 3: no lookup configured - unmapped, never guesses. ---
	{
		std::printf("-- Tier 3: no ICategoryLookup configured --\n");
		const InstalledGame game = MakeGame(L"Totally Unknown Title", Platform::Generic, L"");
		std::optional<ResolvedCategory> outcome;
		bool called = false;
		resolver.resolve(game, [&](std::optional<ResolvedCategory> r) {
			outcome = r;
			called = true;
		});

		Check(called && !outcome.has_value(), "unmapped game with no lookup resolves to nothing, not a guess");
	}

	// --- Tier 3: exact Helix match. ---
	{
		std::printf("-- Tier 3: exact Twitch match --\n");
		FakeOverrideStore emptyOverrides;
		FakeCategoryLookup lookup;
		lookup.seedExact(L"Path of Exile 2", ResolvedCategory{L"12345", L"Path of Exile 2"});
		CategoryResolver r(emptyOverrides, &lookup);

		const InstalledGame game = MakeGame(L"Path of Exile 2", Platform::Steam, L"999");
		std::optional<ResolvedCategory> outcome;
		r.resolve(game, [&](std::optional<ResolvedCategory> res) { outcome = res; });

		Check(outcome.has_value() && outcome->categoryId == L"12345", "exact Helix match resolves immediately");
	}

	// --- Tier 3: fuzzy match above threshold. ---
	{
		std::printf("-- Tier 3: fuzzy match above threshold --\n");
		FakeOverrideStore emptyOverrides;
		FakeCategoryLookup lookup;
		// No exact match; search returns a title sharing every token but
		// one extra edition suffix word - token-set overlap should clear
		// kFuzzyMatchThreshold (0.8).
		lookup.seedSearch(L"Elden Ring Shadow of the Erdtree",
				   {ResolvedCategory{L"777", L"Elden Ring: Shadow of the Erdtree"}});

		CategoryResolver r(emptyOverrides, &lookup);
		const InstalledGame game = MakeGame(L"Elden Ring Shadow of the Erdtree", Platform::Steam, L"1245620");
		std::optional<ResolvedCategory> outcome;
		r.resolve(game, [&](std::optional<ResolvedCategory> res) { outcome = res; });

		Check(outcome.has_value() && outcome->categoryId == L"777", "fuzzy match at/above threshold resolves");
	}

	// --- Tier 3: fuzzy match below threshold - unmapped, let the UI ask. ---
	{
		std::printf("-- Tier 3: fuzzy match below threshold --\n");
		FakeOverrideStore emptyOverrides;
		FakeCategoryLookup lookup;
		lookup.seedSearch(L"Some Indie Game", {ResolvedCategory{L"1", L"A Completely Different Title"}});

		CategoryResolver r(emptyOverrides, &lookup);
		const InstalledGame game = MakeGame(L"Some Indie Game", Platform::Generic, L"");
		std::optional<ResolvedCategory> outcome;
		bool called = false;
		r.resolve(game, [&](std::optional<ResolvedCategory> res) {
			outcome = res;
			called = true;
		});

		Check(called && !outcome.has_value(), "below-threshold fuzzy match resolves to nothing, not a bad guess");
	}
}

// ---------------------------------------------------------------------
// SECTION 2: DetectionStateMachine
// ---------------------------------------------------------------------

class RecordingListener : public DetectionStateMachine::Listener {
public:
	int switchInCount = 0;
	int applyFallbackCount = 0;
	int promptCount = 0;
	int automationPausedCount = 0;
	std::vector<std::wstring> logMessages;
	std::optional<PromptKind> lastPromptKind;
	InstalledGame lastPromptGame;

	void onSwitchIn(const InstalledGame &game) override
	{
		++switchInCount;
		std::printf("  [LISTENER] onSwitchIn(%s)  <-- this is the Twitch PATCH trigger\n",
			    Narrow(game.displayName).c_str());
	}

	void onApplyFallback() override
	{
		++applyFallbackCount;
		std::printf("  [LISTENER] onApplyFallback()\n");
	}

	void onPrompt(PromptKind kind, const InstalledGame &relevantGame) override
	{
		++promptCount;
		lastPromptKind = kind;
		lastPromptGame = relevantGame;
		// Named properly rather than as a two-way ternary: that version
		// printed "GameClosed" for a CreativeApp prompt, which is a
		// debugging trap in the one file whose job is to say what happened.
		const char *kindName = "?";
		switch (kind) {
		case PromptKind::GoLiveMismatch:
			kindName = "GoLiveMismatch";
			break;
		case PromptKind::GameClosed:
			kindName = "GameClosed";
			break;
		case PromptKind::CreativeApp:
			kindName = "CreativeApp";
			break;
		case PromptKind::NoGameIdle:
			kindName = "NoGameIdle";
			break;
		}
		// Trigger D names no game at all, so this has to cope with an
		// empty identity rather than printing a blank and looking broken.
		std::printf("  [LISTENER] onPrompt(%s, %s)\n", kindName,
			    relevantGame.displayName.empty() ? "(no game)"
			                                     : Narrow(relevantGame.displayName).c_str());
	}

	void onAutomationPaused(const std::wstring &reasonForDock) override
	{
		++automationPausedCount;
		std::printf("  [LISTENER] onAutomationPaused(\"%s\")\n", Narrow(reasonForDock).c_str());
	}

	void onLogEntry(const std::wstring &message, const std::wstring & /*previousCategory*/) override
	{
		logMessages.push_back(message);
		std::printf("  [LISTENER] onLogEntry(\"%s\")\n", Narrow(message).c_str());
	}
};

const char *StateName(State s)
{
	switch (s) {
	case State::Idle:
		return "Idle";
	case State::Pending:
		return "Pending";
	case State::Active:
		return "Active";
	case State::Grace:
		return "Grace";
	case State::Fallback:
		return "Fallback";
	case State::ExternalOverride:
		return "ExternalOverride";
	}
	return "?";
}

void RunStateMachineScenarios()
{
	std::printf("\n=== SECTION 2: DetectionStateMachine ===\n");

	// Short timings so the harness can observe real deadline expiry
	// (grace/prompt timeout) without waiting the production defaults
	// (120s/20s) - the state machine's logic is identical either way,
	// only the wall-clock wait shrinks.
	TimingConstants timing;
	timing.confirmPolls = 2;
	timing.crashGraceS = 1;
	timing.cleanExitGraceS = 1;
	timing.promptTimeoutS = 1;
	timing.flapBreakerN = 2;
	timing.flapBreakerWindowS = 30;

	RecordingListener listener;
	DetectionStateMachine sm(listener, timing);

	const InstalledGame gameA = MakeGame(L"Path of Exile 2", Platform::Steam, L"2669320");
	const InstalledGame gameB = MakeGame(L"Hollow Knight: Silksong", Platform::Steam, L"1030300");

	DetectedGame detectedA;
	detectedA.game = gameA;
	detectedA.pid = 4242;
	detectedA.confidence = Confidence::High;

	// --- Go-live mismatch (Trigger A), fired as a side effect of the
	// first confirmed switch-in once the live category is known. ---
	std::printf("-- Confirm + go-live mismatch prompt --\n");
	sm.setLive(true);
	sm.onLiveCategoryKnown(L"Just Chatting");
	for (std::uint32_t i = 0; i < timing.confirmPolls; ++i) {
		sm.onPollResult(detectedA);
	}
	Check(sm.promptOutstanding() && sm.outstandingPromptKind() == PromptKind::GoLiveMismatch,
	      "confirming a game while live-in-mismatch raises GoLiveMismatch, not a silent switch");
	Check(listener.switchInCount == 0, "no PATCH fires until the prompt is answered");

	sm.respondToPrompt(/*acceptAction=*/true, /*dontAskAgainThisStream=*/false);
	Check(listener.switchInCount == 1, "\"Set to <game>\" response fires exactly one onSwitchIn");
	Check(sm.state() == State::Active, "state machine is now Active(Path of Exile 2)");

	// --- Crash, then relaunch inside the grace window: anti-flap core -
	// zero further Twitch calls. ---
	std::printf("-- Crash + relaunch within grace: anti-flap core --\n");
	const int switchInBefore = listener.switchInCount;
	sm.onTrackedProcessExited(ExitReason::Crash, gameA);
	Check(sm.state() == State::Grace, "crash moves Active -> Grace, not straight to Fallback");

	for (std::uint32_t i = 0; i < timing.confirmPolls; ++i) {
		sm.onPollResult(detectedA); // Same game reappears - a crash-relaunch.
	}
	Check(sm.state() == State::Active, "relaunch inside the grace window returns to Active");
	Check(listener.switchInCount == switchInBefore,
	      "relaunch-during-grace calls onSwitchIn ZERO times - the category was never touched");

	// --- Crash, no relaunch: grace expires, GameClosed prompt raised,
	// no response -> falls through to hold (never a silent fallback). ---
	std::printf("-- Crash, no relaunch: grace -> prompt -> timeout -> hold --\n");
	sm.onTrackedProcessExited(ExitReason::Crash, gameA);
	std::this_thread::sleep_for(std::chrono::milliseconds(1200)); // Past crashGraceS (1s).
	sm.onTick();
	Check(sm.promptOutstanding() && sm.outstandingPromptKind() == PromptKind::GameClosed,
	      "grace expiry with nothing detected raises GameClosed, never a silent fallback");

	const int applyFallbackBefore = listener.applyFallbackCount;
	std::this_thread::sleep_for(std::chrono::milliseconds(1200)); // Past promptTimeoutS (1s).
	sm.onTick();
	Check(!sm.promptOutstanding(), "unanswered prompt times out");
	Check(sm.state() == State::Fallback, "timeout lands in Fallback (\"hold\"), never re-applies a category");
	Check(listener.applyFallbackCount == applyFallbackBefore,
	      "onApplyFallback() is NEVER called from a timeout - only an explicit accept can call it");

	// --- Flap breaker: repeated rapid switches trip automation pause. ---
	std::printf("-- Flap breaker: crash-looping game trips automation pause --\n");
	Check(!sm.isAutomationPaused(), "automation not paused yet");
	// Two switch-ins already happened above (gameA go-live mismatch
	// accept, once); flapBreakerN=2 means a 3rd automated change within
	// the window should trip the breaker. Force a couple more confirmed
	// switches by alternating games (each is a changingGame switch-in).
	DetectedGame detectedB;
	detectedB.game = gameB;
	detectedB.pid = 4343;
	detectedB.confidence = Confidence::High;
	for (int round = 0; round < 3 && !sm.isAutomationPaused(); ++round) {
		const InstalledGame &game = (round % 2 == 0) ? gameB : gameA;
		DetectedGame d;
		d.game = game;
		d.pid = detectedA.pid + static_cast<std::uint32_t>(round) + 100;
		d.confidence = Confidence::High;
		for (std::uint32_t i = 0; i < timing.confirmPolls; ++i) {
			sm.onPollResult(d);
		}
	}
	Check(sm.isAutomationPaused(), "repeated rapid switches trip the flap breaker");
	Check(listener.automationPausedCount == 1, "onAutomationPaused() fires exactly once (never spammed)");

	std::printf("\nFinal state: %s | switchInCount=%d applyFallbackCount=%d promptCount=%d "
		    "automationPausedCount=%d logEntries=%zu\n",
		    StateName(sm.state()), listener.switchInCount, listener.applyFallbackCount, listener.promptCount,
		    listener.automationPausedCount, listener.logMessages.size());
}

// ---------------------------------------------------------------------
// SECTION 2b: onTrackedProcessExited identity + lifetime (item 7).
// DetectionEngine.cpp's own re-targeting fix (never abandoning the
// incumbent's exit handle to a single-poll transient rival) is NOT
// exercised here - ProcessScanner.cpp/DetectionEngine.cpp are Windows-API-
// dependent and are not linked into this harness (see CMakeLists.txt).
// What IS fully testable at this layer, and is the state-machine half of
// the same bug, is that onTrackedProcessExited() now carries and checks
// identity rather than trusting "whichever pid engine most recently
// followed", and accepts the incumbent's exit even while Pending on a
// transient rival.
// ---------------------------------------------------------------------

void RunExitSignalIdentityScenarios()
{
	std::printf("\n=== SECTION 2b: onTrackedProcessExited identity + lifetime (item 7) ===\n");

	TimingConstants timing;
	timing.confirmPolls = 2;
	timing.crashGraceS = 1;
	timing.cleanExitGraceS = 1;
	timing.promptTimeoutS = 1;

	const InstalledGame gameA = MakeGame(L"Path of Exile 2", Platform::Steam, L"2669320");
	const InstalledGame gameB = MakeGame(L"Hollow Knight: Silksong", Platform::Steam, L"1030300");

	// --- A stray exit signal for a game that ISN'T the incumbent is ignored. ---
	{
		std::printf("-- Stray exit for a non-incumbent game is ignored --\n");
		RecordingListener listener;
		DetectionStateMachine sm(listener, timing);
		DetectedGame detectedA;
		detectedA.game = gameA;
		detectedA.pid = 100;
		detectedA.confidence = Confidence::High;
		for (std::uint32_t i = 0; i < timing.confirmPolls; ++i)
			sm.onPollResult(detectedA);
		Check(sm.state() == State::Active, "gameA confirmed Active");

		sm.onTrackedProcessExited(ExitReason::Crash, gameB); // Identity mismatch - gameB was never the incumbent.
		Check(sm.state() == State::Active,
		      "an exit signal for a DIFFERENT game than the incumbent is ignored - no Grace");
	}

	// --- Exit accepted while Pending on a transient rival (item 7a: the
	// swallowed-exit bug). Reproduces exactly the scenario the project
	// report describes: the active game g exits while a transient rival
	// h has just won a single poll (state_ == Pending, but activeGame_
	// is still g - the true incumbent). Pre-fix, onTrackedProcessExited()
	// required state_ == State::Active and silently dropped this. ---
	{
		std::printf("-- Exit for the incumbent is accepted even while Pending on a transient rival --\n");
		RecordingListener listener;
		DetectionStateMachine sm(listener, timing);
		DetectedGame detectedA;
		detectedA.game = gameA;
		detectedA.pid = 100;
		detectedA.confidence = Confidence::High;
		for (std::uint32_t i = 0; i < timing.confirmPolls; ++i)
			sm.onPollResult(detectedA);
		Check(sm.state() == State::Active, "gameA confirmed Active");

		// A transient rival h wins ONE poll - moves state_ to Pending
		// while activeGame_ (gameA) stays untouched (only a DIFFERENT
		// game starts a fresh pendingCandidate_ streak - see
		// onPollResult()'s "alt-tab / already-confirmed no-op" comment).
		DetectedGame detectedB;
		detectedB.game = gameB;
		detectedB.pid = 200;
		detectedB.confidence = Confidence::High;
		sm.onPollResult(detectedB);
		Check(sm.state() == State::Pending, "a single-poll rival win moves state_ to Pending, not Active(h)");

		// gameA (the real incumbent, per activeGame_) exits right now.
		sm.onTrackedProcessExited(ExitReason::Crash, gameA);
		Check(sm.state() == State::Grace, "the incumbent's exit is observed even while Pending on a transient rival");
		Check(sm.graceGame().has_value() && sm.graceGame()->displayName == gameA.displayName,
		      "Grace tracks the game that actually exited (gameA), not the transient rival (gameB)");
	}
}

// ---------------------------------------------------------------------
// SECTION 2c: Trigger A comparison-basis fixes (item 8).
// ---------------------------------------------------------------------

void RunGoLiveMismatchScenarios()
{
	std::printf("\n=== SECTION 2c: Trigger A comparison-basis fixes (item 8) ===\n");

	TimingConstants timing;
	timing.confirmPolls = 2;
	timing.crashGraceS = 1;
	timing.cleanExitGraceS = 1;
	timing.promptTimeoutS = 1;

	// --- Item 8.1: normalizedEquals uses CategoryResolver::normalize(),
	// which strips (tm)/(r)/(c) - a Steam-style display name carrying a
	// (R) glyph must not false-fire Trigger A against Twitch's
	// glyph-free category name for the exact same game. ---
	{
		std::printf("-- A (R)/(tm)/(c)-only difference does not false-fire Trigger A --\n");
		RecordingListener listener;
		DetectionStateMachine sm(listener, timing);
		sm.setLive(true);
		sm.onLiveCategoryKnown(L"Tom Clancy's Rainbow Six Siege"); // Twitch's canonical name - no glyph.
		const InstalledGame steamGame =
			MakeGame(L"Tom Clancy's Rainbow Six® Siege", Platform::Steam, L"359550"); // Steam's name - has (R).
		DetectedGame detected;
		detected.game = steamGame;
		detected.pid = 321;
		detected.confidence = Confidence::High;
		for (std::uint32_t i = 0; i < timing.confirmPolls; ++i)
			sm.onPollResult(detected);

		Check(!sm.promptOutstanding(), "a (R)-only difference does not raise a false go-live-mismatch prompt");
		Check(listener.switchInCount == 1, "the switch proceeds normally (not gated) once names normalize-equal");
	}

	// --- Item 8.3: currentLiveCategory_ does not survive setLive(false) -
	// a stale basis must never burn the NEXT stream's once-per-stream
	// prompt budget before the fresh go-live sync arrives. ---
	{
		std::printf("-- currentLiveCategory_ does not survive setLive(false) (stale-basis fix) --\n");
		RecordingListener listener;
		DetectionStateMachine sm(listener, timing);
		sm.setLive(true);
		sm.onLiveCategoryKnown(L"Just Chatting");

		const InstalledGame game = MakeGame(L"Some Game", Platform::Steam, L"1");
		DetectedGame detected;
		detected.game = game;
		detected.pid = 1;
		detected.confidence = Confidence::High;
		for (std::uint32_t i = 0; i < timing.confirmPolls; ++i)
			sm.onPollResult(detected);
		Check(sm.promptOutstanding() && sm.outstandingPromptKind() == PromptKind::GoLiveMismatch,
		      "mismatch prompt raised this stream (\"Just Chatting\" vs \"Some Game\")");

		sm.setLive(false); // Stream ends with the prompt still unanswered.
		Check(!sm.promptOutstanding(), "going offline drops the outstanding prompt");

		sm.setLive(true); // New stream. If the OLD stale basis ("Just Chatting") survived, this alone
				   // would immediately re-raise a mismatch prompt against last stream's data,
				   // before this stream's real sync has even arrived.
		Check(!sm.promptOutstanding(),
		      "no stale-basis prompt fires at the next go-live, before the fresh sync arrives");

		// The fresh sync arrives with a REAL mismatch this stream. If the
		// stale-basis prompt above had wrongly fired (pre-fix), it would
		// have already burned goLiveMismatchAskedThisStream_ and this
		// would incorrectly stay silent.
		sm.onLiveCategoryKnown(L"Something Else Entirely");
		Check(sm.promptOutstanding() && sm.outstandingPromptKind() == PromptKind::GoLiveMismatch,
		      "once the fresh sync arrives with a REAL mismatch, the prompt fires normally - the "
		      "once-per-stream budget was not pre-burned by stale data");
	}
}

// ---------------------------------------------------------------------
// SECTION 2d: Trigger C - creative/dev apps are offered, never applied.
//
// The whole point of this trigger is a NEGATIVE: a prompt-only app must
// not move the category by itself under ANY combination of settings. So
// these checks assert on what did NOT happen (switchInCount) at least as
// hard as on what did, and they walk the paths where the prompt cannot
// be shown at all - not live, prompts off, suppressed - because those
// are exactly where a naive implementation quietly falls through to the
// automatic switch it was meant to replace.
// ---------------------------------------------------------------------

void RunCreativeAppScenarios()
{
	std::printf("\n=== SECTION 2d: Trigger C - creative apps are offered, never applied ===\n");

	TimingConstants timing;
	timing.confirmPolls = 2;
	timing.crashGraceS = 1;
	timing.cleanExitGraceS = 1;
	timing.promptTimeoutS = 1;
	timing.flapBreakerN = 5;
	timing.flapBreakerWindowS = 30;

	const InstalledGame vscode = MakeGame(L"Visual Studio Code", Platform::Generic, L"VSCode");
	const InstalledGame game = MakeGame(L"Marvel Rivals", Platform::Steam, L"2767030");

	auto promptOnlyIsVsCode = [vscode](const InstalledGame &g) { return g.displayName == vscode.displayName; };

	DetectedGame detectedVsCode;
	detectedVsCode.game = vscode;
	detectedVsCode.pid = 900;
	detectedVsCode.confidence = Confidence::Low;

	DetectedGame detectedGame;
	detectedGame.game = game;
	detectedGame.pid = 901;
	detectedGame.confidence = Confidence::High;

	auto confirm = [&](DetectionStateMachine &sm, const DetectedGame &d) {
		for (std::uint32_t i = 0; i < timing.confirmPolls; ++i)
			sm.onPollResult(d);
	};

	// --- Confirming a prompt-only app asks instead of switching. ---
	{
		std::printf("-- Live, prompts on: confirming VS Code asks --\n");
		RecordingListener listener;
		DetectionStateMachine sm(listener, timing);
		sm.setPromptOnlyPredicate(promptOnlyIsVsCode);
		sm.setLive(true);
		sm.onLiveCategoryKnown(L"Just Chatting");

		confirm(sm, detectedVsCode);
		Check(sm.promptOutstanding() && sm.outstandingPromptKind() == PromptKind::CreativeApp,
		      "a confirmed prompt-only app raises CreativeApp");
		Check(listener.switchInCount == 0, "and fires no PATCH while the prompt is outstanding");

		sm.respondToPrompt(/*acceptAction=*/true, /*dontAskAgainThisStream=*/false);
		Check(listener.switchInCount == 1, "accepting is the ONLY path that applies a creative category");
	}

	// --- Declining keeps game detection alive underneath. ---
	{
		std::printf("-- Declining, then a real game launches --\n");
		RecordingListener listener;
		DetectionStateMachine sm(listener, timing);
		sm.setPromptOnlyPredicate(promptOnlyIsVsCode);
		sm.setLive(true);
		sm.onLiveCategoryKnown(L"Just Chatting");

		confirm(sm, detectedVsCode);
		sm.respondToPrompt(/*acceptAction=*/false, /*dontAskAgainThisStream=*/false);
		Check(listener.switchInCount == 0, "declining applies nothing");

		// Re-polling the same app must not re-ask every poll - that
		// nagging is the failure mode this whole design exists to avoid.
		confirm(sm, detectedVsCode);
		Check(!sm.promptOutstanding(), "and the same app re-winning polls does not re-ask");

		// Trigger C deliberately does NOT spend the once-per-stream
		// go-live budget, so the first real game of the stream still
		// meets Trigger A. That is the pre-existing design, and this
		// asserts the whole chain rather than pretending the creative
		// prompt bypasses it.
		confirm(sm, detectedGame);
		Check(sm.promptOutstanding() && sm.outstandingPromptKind() == PromptKind::GoLiveMismatch,
		      "a real game afterwards meets the still-unspent go-live check");
		sm.respondToPrompt(/*acceptAction=*/true, /*dontAskAgainThisStream=*/false);
		Check(listener.switchInCount == 1, "and switches once that is answered");
	}

	// --- The paths where the prompt CANNOT be shown must still not switch. ---
	{
		std::printf("-- Not live: no prompt, and still no switch --\n");
		RecordingListener listener;
		DetectionStateMachine sm(listener, timing);
		sm.setPromptOnlyPredicate(promptOnlyIsVsCode);
		sm.setLive(false);

		confirm(sm, detectedVsCode);
		Check(!sm.promptOutstanding(), "offline raises no prompt");
		Check(listener.switchInCount == 0, "and offline does NOT fall through to an automatic switch");
	}

	{
		std::printf("-- Prompts disabled: no prompt, and still no switch --\n");
		RecordingListener listener;
		DetectionStateMachine sm(listener, timing);
		sm.setPromptOnlyPredicate(promptOnlyIsVsCode);
		sm.setPromptsEnabled(false);
		sm.setLive(true);
		sm.onLiveCategoryKnown(L"Just Chatting");

		confirm(sm, detectedVsCode);
		Check(!sm.promptOutstanding(), "prompts-off raises no prompt");
		Check(listener.switchInCount == 0,
		      "and prompts-off does NOT fall through to an automatic switch either");
	}

	{
		std::printf("-- \"Don't ask this stream\" suppresses, without ever switching --\n");
		RecordingListener listener;
		DetectionStateMachine sm(listener, timing);
		sm.setPromptOnlyPredicate(promptOnlyIsVsCode);
		sm.setLive(true);
		sm.onLiveCategoryKnown(L"Just Chatting");

		confirm(sm, detectedVsCode);
		sm.respondToPrompt(/*acceptAction=*/false, /*dontAskAgainThisStream=*/true);

		// Force the app to look new again so the trigger would fire a
		// second time if suppression were not honored.
		confirm(sm, detectedGame);
		sm.respondToPrompt(/*acceptAction=*/true, /*dontAskAgainThisStream=*/false); // Trigger A, as above.
		Check(listener.switchInCount == 1, "the real game switches normally");

		listener.promptCount = 0;
		const int switchesBefore = listener.switchInCount;
		confirm(sm, detectedVsCode);
		Check(listener.promptCount == 0, "suppressed for the rest of the stream");
		Check(listener.switchInCount == switchesBefore,
		      "suppression holds the category rather than silently switching to the app");
	}

	// --- Timeout holds, like every other prompt. ---
	{
		std::printf("-- Unanswered prompt times out to a hold --\n");
		RecordingListener listener;
		DetectionStateMachine sm(listener, timing);
		sm.setPromptOnlyPredicate(promptOnlyIsVsCode);
		sm.setLive(true);
		sm.onLiveCategoryKnown(L"Just Chatting");

		confirm(sm, detectedVsCode);
		Check(sm.promptOutstanding(), "prompt is outstanding before the timeout");
		std::this_thread::sleep_for(std::chrono::milliseconds(1200));
		sm.onTick();
		Check(!sm.promptOutstanding(), "prompt clears on timeout");
		Check(listener.switchInCount == 0, "and a timed-out creative prompt applies nothing");
	}

	// --- With no predicate installed, nothing changes. ---
	{
		std::printf("-- No predicate: pre-Trigger-C behavior is unchanged --\n");
		RecordingListener listener;
		DetectionStateMachine sm(listener, timing);
		sm.setLive(true);
		// Live category already matches the candidate, so Trigger A has
		// nothing to say. That isolates the one thing under test: with
		// no predicate installed, nothing is prompt-only.
		sm.onLiveCategoryKnown(vscode.displayName);

		confirm(sm, detectedVsCode);
		Check(listener.promptCount == 0, "no predicate means no creative prompt");
		Check(listener.switchInCount == 1, "an app nobody marked prompt-only still switches automatically");
	}
}

// ---------------------------------------------------------------------
// SECTION 2e: the external-override freeze, and getting out of it.
//
// This is the bug that produced a 45-minute dead zone in a real session:
// a category changed outside SignalBox froze the state machine, every
// subsequent poll was discarded, the Rescan button rebuilt the index into
// the void, and the dock showed a healthy-looking plugin the whole time
// because isAutomationPaused() still reported false.
//
// So the first check here is not about the freeze - it is about whether
// the freeze is VISIBLE. A pause nobody can see is the actual defect; the
// pause itself is correct behavior.
// ---------------------------------------------------------------------

void RunExternalOverrideScenarios()
{
	std::printf("\n=== SECTION 2e: external override is visible and recoverable ===\n");

	TimingConstants timing;
	timing.confirmPolls = 2;
	timing.promptTimeoutS = 1;
	timing.flapBreakerN = 5;
	timing.flapBreakerWindowS = 30;

	RecordingListener listener;
	DetectionStateMachine sm(listener, timing);

	const InstalledGame game = MakeGame(L"Marvel Rivals", Platform::Steam, L"2767030");
	const InstalledGame other = MakeGame(L"Fortnite", Platform::Epic, L"Fortnite");

	DetectedGame detected;
	detected.game = game;
	detected.pid = 500;
	detected.confidence = Confidence::High;

	DetectedGame detectedOther;
	detectedOther.game = other;
	detectedOther.pid = 501;
	detectedOther.confidence = Confidence::High;

	sm.setLive(true);
	sm.onLiveCategoryKnown(game.displayName); // Matches, so Trigger A stays out of the way.
	for (std::uint32_t i = 0; i < timing.confirmPolls; ++i)
		sm.onPollResult(detected);
	Check(sm.activeGame().has_value(), "a game is active before the external change");

	sm.onExternalChangeDetected();
	Check(sm.isAutomationPaused(),
	      "an external change reports itself as paused - this is what the dock reads, and it used to say false");
	Check(listener.automationPausedCount == 1, "and tells the listener exactly once");

	// The freeze itself: polls are genuinely discarded.
	const int switchesBefore = listener.switchInCount;
	for (std::uint32_t i = 0; i < timing.confirmPolls * 3; ++i)
		sm.onPollResult(detectedOther);
	Check(listener.switchInCount == switchesBefore, "polls are discarded while frozen");
	Check(sm.isAutomationPaused(), "and it stays paused - no amount of polling clears it");

	// Recovery. Before this existed, restarting OBS was the only way out.
	sm.resume();
	Check(!sm.isAutomationPaused(), "resume() clears the pause");
	Check(!sm.activeGame().has_value(), "and re-detects from scratch rather than trusting the stale active game");

	// The first game confirmed after a resume still meets the
	// once-per-stream go-live check, because nothing had spent it yet.
	// That is the right outcome and worth pinning down rather than
	// asserting around: the user has just re-enabled automation after
	// something changed the category behind SignalBox's back, so
	// confirming the very next change with them is exactly the moment to
	// ask.
	for (std::uint32_t i = 0; i < timing.confirmPolls; ++i)
		sm.onPollResult(detectedOther);
	Check(sm.promptOutstanding() && sm.outstandingPromptKind() == PromptKind::GoLiveMismatch,
	      "the first game after a resume asks before switching");
	sm.respondToPrompt(/*acceptAction=*/true, /*dontAskAgainThisStream=*/false);
	Check(listener.switchInCount == switchesBefore + 1, "detection actually works again afterwards");
}

// ---------------------------------------------------------------------
// SECTION 2f: the stream-ending hold, and Trigger D (the no-game idle
// prompt).
//
// Both come from the same gap found by sitting in front of the finished
// 0.2.3 build: SignalBox has exactly one opinion about what a streamer is
// doing - a game is running, or a game is not running - and no way to be
// told anything else. Wrapping up a stream and sitting in an outro looks
// identical to a game having crashed, and idling in OBS with no game at
// all produced no reaction whatsoever, because every prompt was gated on
// being live.
//
// The two features answer opposite halves of that: one lets the user say
// "stop acting, I'm finishing", the other makes the plugin ask "there's
// no game, what are you doing?" instead of sitting silent. What follows
// pins down the parts that are easy to get wrong - the self-clearing, the
// once-per-spell scarcity, and the deliberate departure from ADDENDUM
// hard constraint #4.
// ---------------------------------------------------------------------

void RunStreamEndingHoldScenarios()
{
	std::printf("\n=== SECTION 2f-1: the stream-ending hold clears itself ===\n");

	TimingConstants timing;
	timing.confirmPolls = 2;
	timing.crashGraceS = 1;
	timing.cleanExitGraceS = 1;
	timing.promptTimeoutS = 5;
	timing.noGameIdlePromptS = 1;
	timing.flapBreakerN = 10;
	timing.flapBreakerWindowS = 30;

	const InstalledGame rivals = MakeGame(L"Marvel Rivals", Platform::Steam, L"2767030");
	const InstalledGame fortnite = MakeGame(L"Fortnite", Platform::Epic, L"Fortnite");

	DetectedGame dRivals;
	dRivals.game = rivals;
	dRivals.pid = 600;
	dRivals.confidence = Confidence::High;

	DetectedGame dFortnite;
	dFortnite.game = fortnite;
	dFortnite.pid = 601;
	dFortnite.confidence = Confidence::High;

	// --- The outro: a game closing while held must not ask or act. ---
	{
		RecordingListener listener;
		DetectionStateMachine sm(listener, timing);

		sm.setLive(true);
		sm.onLiveCategoryKnown(rivals.displayName); // Matches, so Trigger A stays out of the way.
		for (std::uint32_t i = 0; i < timing.confirmPolls; ++i)
			sm.onPollResult(dRivals);
		Check(sm.state() == State::Active, "a game is running before the user presses Stream Ending");

		sm.beginStreamEndingHold();
		Check(sm.streamEndingHold(), "the hold is on");

		// This is the whole point of the feature. Quitting the game at
		// the end of a stream is indistinguishable, to everything below
		// the UI, from a crash mid-session - and the normal answer to
		// that is Trigger B, a prompt, while live, about a category the
		// user has already decided they are done with.
		const int promptsBefore = listener.promptCount;
		sm.onTrackedProcessExited(ExitReason::Clean, rivals);
		std::this_thread::sleep_for(std::chrono::milliseconds(1200)); // Past cleanExitGraceS (1s).
		sm.onTick();
		Check(listener.promptCount == promptsBefore,
		      "closing the game during an outro raises no prompt - the user already said what they're doing");
		Check(listener.applyFallbackCount == 0, "and nothing is applied to the channel");
		Check(sm.streamEndingHold(), "and the hold survives the game closing - that was expected, not a surprise");

		// A DIFFERENT game is the second self-clearing condition. Note
		// what happens next and why it is right: the hold lifts, and
		// because this stream has not spent its go-live check yet, the
		// very next thing is Trigger A asking before switching. The
		// hold is not allowed to swallow that question - it was raised
		// about a game that had not started when the hold was pressed.
		for (std::uint32_t i = 0; i < timing.confirmPolls; ++i)
			sm.onPollResult(dFortnite);
		Check(!sm.streamEndingHold(),
		      "a different game lifts the hold - someone starting a game is plainly not ending a stream");
		Check(sm.promptOutstanding() && sm.outstandingPromptKind() == PromptKind::GoLiveMismatch,
		      "and the new game is offered normally, not swallowed by the hold that just lifted");

		const int switchesBefore = listener.switchInCount;
		sm.respondToPrompt(/*acceptAction=*/true, /*dontAskAgainThisStream=*/false);
		Check(listener.switchInCount == switchesBefore + 1, "automation really is working again afterwards");
	}

	// --- The stream stopping: the clear that actually matters. ---
	{
		RecordingListener listener;
		DetectionStateMachine sm(listener, timing);

		sm.setLive(true);
		for (std::uint32_t i = 0; i < timing.confirmPolls; ++i)
			sm.onPollResult(dRivals);
		sm.beginStreamEndingHold();
		Check(sm.streamEndingHold(), "held while live");

		sm.setLive(false);
		Check(!sm.streamEndingHold(),
		      "stopping the stream clears the hold - otherwise it is a lock you find out about next session, live");
	}

	// --- Manual escape hatches still work. ---
	{
		RecordingListener listener;
		DetectionStateMachine sm(listener, timing);

		sm.beginStreamEndingHold();
		sm.clearStreamEndingHold();
		Check(!sm.streamEndingHold(), "pressing the button again releases it - self-clearing is the net, not the only way out");

		sm.beginStreamEndingHold();
		sm.resume();
		Check(!sm.streamEndingHold(), "\"Resume automatic switching\" means all of it, including this");
	}

	// --- A creative app is NOT a new game. ---
	{
		RecordingListener listener;
		DetectionStateMachine sm(listener, timing);
		const InstalledGame vscode = MakeGame(L"Visual Studio Code", Platform::Generic, L"VS Code");
		sm.setPromptOnlyPredicate([&vscode](const InstalledGame &g) { return g.displayName == vscode.displayName; });

		DetectedGame dVscode;
		dVscode.game = vscode;
		dVscode.pid = 602;
		dVscode.confidence = Confidence::High;

		sm.setLive(true);
		for (std::uint32_t i = 0; i < timing.confirmPolls; ++i)
			sm.onPollResult(dRivals);
		sm.beginStreamEndingHold();

		const int switchesBefore = listener.switchInCount;
		for (std::uint32_t i = 0; i < timing.confirmPolls; ++i)
			sm.onPollResult(dVscode);
		Check(sm.streamEndingHold(),
		      "opening an editor during an outro does not lift the hold - wrapping up is not starting a stream");
		Check(!sm.promptOutstanding(), "and it does not get to ask about it either");
		Check(listener.switchInCount == switchesBefore, "and nothing is switched");
	}
}

// Trigger D's idle clock is stamped BY a tick, not by wall time alone,
// so waiting out the window always means tick, wait, tick. A sleep
// BEFORE the first tick is invisible to the state machine (there is no
// clock running yet to advance), and two ticks in a row after a sleep
// only ever start one. Getting this wrong makes a working feature look
// broken, which is exactly what it did on the first run of this section.
void ElapseIdleWindow(DetectionStateMachine &sm)
{
	sm.onTick();
	std::this_thread::sleep_for(std::chrono::milliseconds(1200)); // Past noGameIdlePromptS (1s).
	sm.onTick();
}

void RunIdlePromptScenarios()
{
	std::printf("\n=== SECTION 2f-2: Trigger D - the no-game idle prompt ===\n");

	TimingConstants timing;
	timing.confirmPolls = 2;
	timing.crashGraceS = 1;
	timing.cleanExitGraceS = 1;
	timing.promptTimeoutS = 5; // Long, so the prompt survives the assertions below rather than timing out mid-check.
	timing.noGameIdlePromptS = 1;
	timing.flapBreakerN = 10;
	timing.flapBreakerWindowS = 30;

	const InstalledGame rivals = MakeGame(L"Marvel Rivals", Platform::Steam, L"2767030");
	DetectedGame dRivals;
	dRivals.game = rivals;
	dRivals.pid = 700;
	dRivals.confidence = Confidence::High;

	// --- Off unless the caller turns it on, and the clock runs anyway. ---
	{
		RecordingListener listener;
		DetectionStateMachine sm(listener, timing);
		Check(!sm.isLive(), "offline for this entire scenario - Trigger D is the one prompt that does not need a stream");
		Check(!sm.noGameIdleEnabled(), "and it is OFF until somebody switches it on");

		sm.onTick();
		std::this_thread::sleep_for(std::chrono::milliseconds(1200)); // Past noGameIdlePromptS (1s).
		sm.onTick();
		Check(!sm.promptOutstanding(), "so a full idle window passes in silence");

		// The clock is NOT gated with the prompt, deliberately: someone
		// who connects Twitch (or ticks the setting) five minutes into
		// an idle spell gets asked at that moment, not two minutes
		// later.
		sm.setNoGameIdleEnabled(true);
		sm.onTick();
		Check(sm.promptOutstanding() && sm.outstandingPromptKind() == PromptKind::NoGameIdle,
		      "enabling it part-way through an idle spell asks immediately - the window had already elapsed");
		Check(!sm.promptRelevantGame().has_value(),
		      "and names no game, because there isn't one - a stale name here would mis-aim \"Fix This!\"");
	}

	// --- A full window really is required. ---
	{
		RecordingListener listener;
		DetectionStateMachine sm(listener, timing);
		sm.setNoGameIdleEnabled(true);

		sm.onTick(); // Starts the clock.
		sm.onTick(); // Immediately after - nowhere near the window.
		Check(!sm.promptOutstanding(), "it does not fire the moment the plugin loads");

		std::this_thread::sleep_for(std::chrono::milliseconds(1200));
		sm.onTick();
		Check(sm.promptOutstanding(), "it fires once the window has actually elapsed");

		// THE DIVERGENCE, PINNED DOWN. Everywhere else, an accept that
		// would apply the fallback is gated on fallbackEnabled() - off
		// by default. Here it is not, because this prompt's primary
		// button names the category out loud and the user just pressed
		// it. If this assertion ever flips, the feature is silently
		// dead for every user who never opened Settings.
		Check(!sm.fallbackEnabled(), "fallback switching is off, as it is by default");
		sm.respondToPrompt(/*acceptAction=*/true, /*dontAskAgainThisStream=*/false);
		Check(listener.applyFallbackCount == 1,
		      "\"Set to Just Chatting\" applies it anyway - an explicit answer is not automation");

		// Scarcity: this is the property that keeps a prompt which can
		// fire offline from becoming a nag. One question per idle
		// spell, however long the spell runs.
		std::this_thread::sleep_for(std::chrono::milliseconds(1200));
		sm.onTick();
		std::this_thread::sleep_for(std::chrono::milliseconds(1200));
		sm.onTick();
		Check(!sm.promptOutstanding(), "and it does not ask twice in the same idle spell");
	}

	// --- A game running and stopping is a new spell. ---
	{
		RecordingListener listener;
		DetectionStateMachine sm(listener, timing);
		sm.setNoGameIdleEnabled(true);

		ElapseIdleWindow(sm);
		Check(sm.promptOutstanding(), "asked once");
		sm.respondToPrompt(/*acceptAction=*/false, /*dontAskAgainThisStream=*/false); // "Wait for a game".

		for (std::uint32_t i = 0; i < timing.confirmPolls; ++i)
			sm.onPollResult(dRivals);
		Check(sm.state() == State::Active, "a game runs");

		sm.onTrackedProcessExited(ExitReason::Clean, rivals);
		std::this_thread::sleep_for(std::chrono::milliseconds(1200));
		sm.onTick(); // Grace expires; offline, so it holds rather than prompting.
		Check(sm.state() == State::Fallback, "and stops");

		// Note this needs a full tick-wait-tick of its own: onTick() returns
		// early out of the Grace branch, so the tick that RESOLVED grace never
		// reached the idle clock to start it.
		ElapseIdleWindow(sm);
		Check(sm.promptOutstanding() && sm.outstandingPromptKind() == PromptKind::NoGameIdle,
		      "the silence after it is a NEW spell, and gets its own question");
	}

	// --- "Just recording" means gone, not deferred. ---
	{
		RecordingListener listener;
		DetectionStateMachine sm(listener, timing);
		sm.setNoGameIdleEnabled(true);

		ElapseIdleWindow(sm);
		Check(sm.promptOutstanding(), "asked once");
		sm.respondToPrompt(/*acceptAction=*/false, /*dontAskAgainThisStream=*/true);

		// Re-arm the once-per-spell flag the only way that does it - by
		// running a game - and prove the suppression outlives it.
		for (std::uint32_t i = 0; i < timing.confirmPolls; ++i)
			sm.onPollResult(dRivals);
		sm.onTrackedProcessExited(ExitReason::Clean, rivals);
		std::this_thread::sleep_for(std::chrono::milliseconds(1200));
		sm.onTick(); // Resolves grace.
		ElapseIdleWindow(sm);
		Check(!sm.promptOutstanding(), "a new spell is not enough to bring it back - \"just recording\" meant it");

		// Going live is what resets it, same as every other per-stream
		// suppression: a stream is a new context, and someone who was
		// recording an hour ago may well want the question now.
		sm.setLive(true);
		sm.setLive(false);
		ElapseIdleWindow(sm);
		Check(sm.promptOutstanding(), "starting a stream resets the suppression, like every other one");
	}

	// --- The three states that are not idle. ---
	{
		RecordingListener listener;
		DetectionStateMachine sm(listener, timing);
		sm.setNoGameIdleEnabled(true);

		// 1. Activity restarts the window from zero rather than
		//    resuming it - otherwise a single stray poll two minutes in
		//    would still produce a prompt one tick later.
		std::this_thread::sleep_for(std::chrono::milliseconds(800));
		sm.onTick();
		sm.onPollResult(dRivals); // One winning poll: Pending, not confirmed.
		Check(sm.state() == State::Pending, "something is happening");
		sm.onPollResult(std::nullopt); // Candidate dropped; back to nothing.
		sm.onTick();
		Check(!sm.promptOutstanding(), "the clock restarted, so the old partial window does not count");

		// 2. Grace belongs to Trigger B, even offline where Trigger B
		//    will not fire - two prompts racing for the same silence is
		//    how you get asked the same thing twice in different words.
		for (std::uint32_t i = 0; i < timing.confirmPolls; ++i)
			sm.onPollResult(dRivals);
		sm.onTrackedProcessExited(ExitReason::Crash, rivals);
		Check(sm.state() == State::Grace, "a game just vanished");
		std::this_thread::sleep_for(std::chrono::milliseconds(1200));
		Check(!sm.promptOutstanding(), "and the idle prompt stays out of grace's way");

		// 3. A stream-ending hold suspends it along with everything
		//    else - being asked what you are doing right after telling
		//    it what you are doing is the definition of a nag.
		sm.onTick(); // Resolves grace (offline -> hold) and lands in Fallback.
		sm.beginStreamEndingHold();
		std::this_thread::sleep_for(std::chrono::milliseconds(1200));
		sm.onTick();
		sm.onTick();
		Check(!sm.promptOutstanding(), "and a stream-ending hold silences it too");
	}
}

// ---------------------------------------------------------------------
// SECTION 3: CategorySwitchCoordinator - the full chain this project's
// last gap needed: a state machine decision -> CategoryResolver ->
// CategorySwitchCoordinator -> IChannelClient PATCH+marker, proved with a
// mock IChannelClient that records CALL ORDER (not just outcome) so a
// bug like "PATCH fires before the external-writer GET" or "marker
// failure rolls back the switch" would actually be caught, the same
// standard Section 2 above already holds itself to.
// ---------------------------------------------------------------------

// Records every call AND its ordering relative to every other call -
// this is what lets the scenarios below assert "GET happened before
// PATCH happened before marker POST", not just "all three eventually
// happened."
class FakeChannelClient : public IChannelClient {
public:
	bool reauthRequiredFlag = false;
	std::optional<ChannelSnapshot> channelInfoToReturn; // std::nullopt simulates a failed GET.
	bool setCategorySucceeds = true;
	std::wstring setCategoryFailureReason = L"Twitch returned HTTP 500.";
	bool markerSucceeds = true;

	std::vector<std::string> callLog;
	std::optional<std::wstring> lastSuccessfullySetCategoryId;

	bool reauthRequired() const override { return reauthRequiredFlag; }

	void getChannelInfo(std::function<void(std::optional<ChannelSnapshot>)> onResult) override
	{
		callLog.push_back("getChannelInfo");
		onResult(channelInfoToReturn);
	}

	void setChannelCategory(const std::wstring &categoryId, const std::wstring & /*categoryName*/,
				 std::function<void(bool, std::wstring)> onResult) override
	{
		callLog.push_back("setChannelCategory:" + Narrow(categoryId));
		if (setCategorySucceeds) {
			lastSuccessfullySetCategoryId = categoryId;
			onResult(true, std::wstring());
		} else {
			onResult(false, setCategoryFailureReason);
		}
	}

	void createStreamMarker(const std::wstring &description, std::function<void(bool)> onResult) override
	{
		callLog.push_back("createStreamMarker:" + Narrow(description));
		onResult(markerSucceeds);
	}
};

// Bridges DetectionStateMachine::Listener straight to a
// CategorySwitchCoordinator - exactly the delegation
// ui::CategoryDock::onSwitchIn()/onApplyFallback() now do in the real
// plugin (see ui/CategoryDock.cpp). Proves the chain end to end: a
// confirmed detection drives the state machine's own decision, which
// drives the coordinator, which drives the (fake) Twitch calls - no step
// is faked away.
class CoordinatingListener : public DetectionStateMachine::Listener {
public:
	CoordinatingListener(CategorySwitchCoordinator &coordinator, bool live) : coordinator_(coordinator), live_(live) {}

	std::vector<std::wstring> logMessages;

	void onSwitchIn(const InstalledGame &game) override { coordinator_.switchIn(game, live_); }
	void onApplyFallback() override { coordinator_.applyFallback(L"Just Chatting", live_); }
	void onPrompt(PromptKind, const InstalledGame &) override {}
	void onAutomationPaused(const std::wstring &) override {}
	void onLogEntry(const std::wstring &message, const std::wstring & /*previousCategory*/) override
	{
		logMessages.push_back(message);
	}

private:
	CategorySwitchCoordinator &coordinator_;
	bool live_;
};

bool AnyMessageContains(const std::vector<std::wstring> &messages, const wchar_t *needle)
{
	const std::wstring wneedle(needle);
	for (const auto &m : messages) {
		if (m.find(wneedle) != std::wstring::npos) {
			return true;
		}
	}
	return false;
}

std::size_t CountCallsStartingWith(const std::vector<std::string> &callLog, const char *prefix)
{
	const std::string sprefix(prefix);
	std::size_t count = 0;
	for (const auto &call : callLog) {
		if (call.compare(0, sprefix.size(), sprefix) == 0) {
			++count;
		}
	}
	return count;
}

void RunCoordinatorScenarios()
{
	std::printf("\n=== SECTION 3: CategorySwitchCoordinator (full chain) ===\n");

	TimingConstants timing;
	timing.confirmPolls = 1; // Section 2 already proves confirm-poll mechanics; keep this section focused on the coordinator.

	// --- Scenario 1: full success chain, ordering asserted. ---
	{
		std::printf("-- Full chain: detection -> resolver -> state machine -> coordinator -> PATCH+marker --\n");
		FakeOverrideStore overrides;
		FakeCategoryLookup lookup;
		lookup.seedExact(L"Path of Exile 2", ResolvedCategory{L"2669320", L"Path of Exile 2"});
		CategoryResolver resolver(overrides, &lookup);

		std::vector<std::wstring> activity;
		int externalWriterCalls = 0;
		CategorySwitchCoordinator coordinator(
			resolver, [&](const std::wstring &m) { activity.push_back(m); },
			[&]() { ++externalWriterCalls; });

		FakeChannelClient channelClient;
		channelClient.channelInfoToReturn = ChannelSnapshot{L"", L"Just Chatting"}; // Nothing set by us yet.
		coordinator.setChannelClient(&channelClient);

		CoordinatingListener listener(coordinator, /*live=*/true);
		DetectionStateMachine sm(listener, timing);
		sm.setLive(true); // currentLiveCategory_ stays empty (no onLiveCategoryKnown call) - Trigger A can't fire.

		DetectedGame detected;
		detected.game = MakeGame(L"Path of Exile 2", Platform::Steam, L"2669320");
		detected.pid = 1234;
		detected.confidence = Confidence::High;
		sm.onPollResult(detected);

		const std::vector<std::string> expectedOrder = {"getChannelInfo", "setChannelCategory:2669320",
								  "createStreamMarker:Now playing: Path of Exile 2"};
		Check(channelClient.callLog == expectedOrder,
		      "GET (external-writer guard) -> PATCH -> marker POST, in that exact order");
		Check(channelClient.lastSuccessfullySetCategoryId.has_value() &&
			      *channelClient.lastSuccessfullySetCategoryId == L"2669320",
		      "PATCH actually carried the resolved category id");
		Check(AnyMessageContains(activity, L"Set Twitch category to \"Path of Exile 2\""),
		      "activity log reports the successful switch");
		Check(AnyMessageContains(activity, L"Added a stream marker"), "activity log reports the marker");
		Check(externalWriterCalls == 0, "no external-writer callback on a clean first switch");
	}

	// --- Scenario 2: resolution below threshold issues NO PATCH. ---
	{
		std::printf("-- Unmapped resolution issues NO PATCH --\n");
		FakeOverrideStore overrides;
		FakeCategoryLookup lookup; // Nothing seeded - every lookup misses, same as Section 1's "no lookup configured" case.
		CategoryResolver resolver(overrides, &lookup);

		std::vector<std::wstring> activity;
		CategorySwitchCoordinator coordinator(
			resolver, [&](const std::wstring &m) { activity.push_back(m); }, []() {});

		FakeChannelClient channelClient;
		coordinator.setChannelClient(&channelClient);

		coordinator.switchIn(MakeGame(L"Totally Unknown Title", Platform::Generic, L""), /*live=*/true);

		Check(channelClient.callLog.empty(), "unmapped game -> zero calls to the channel client, not even a GET");
		Check(AnyMessageContains(activity, L"Couldn't map"), "activity log explains why nothing changed");
	}

	// --- Scenario 3: marker failure does NOT fail the switch. ---
	{
		std::printf("-- Marker failure does not fail (or roll back) the switch --\n");
		FakeOverrideStore overrides;
		FakeCategoryLookup lookup;
		lookup.seedExact(L"Hollow Knight: Silksong", ResolvedCategory{L"1030300", L"Hollow Knight: Silksong"});
		CategoryResolver resolver(overrides, &lookup);

		std::vector<std::wstring> activity;
		CategorySwitchCoordinator coordinator(
			resolver, [&](const std::wstring &m) { activity.push_back(m); }, []() {});

		FakeChannelClient channelClient;
		channelClient.markerSucceeds = false;
		coordinator.setChannelClient(&channelClient);

		coordinator.switchIn(MakeGame(L"Hollow Knight: Silksong", Platform::Steam, L"1030300"), /*live=*/true);

		const std::vector<std::string> expectedOrder = {"getChannelInfo", "setChannelCategory:1030300",
								  "createStreamMarker:Now playing: Hollow Knight: Silksong"};
		Check(channelClient.callLog == expectedOrder, "marker is still attempted, after the PATCH, in order");
		Check(channelClient.lastSuccessfullySetCategoryId.has_value() &&
			      *channelClient.lastSuccessfullySetCategoryId == L"1030300",
		      "the PATCH itself succeeded and is recorded regardless of the marker outcome");
		Check(AnyMessageContains(activity, L"Set Twitch category to \"Hollow Knight: Silksong\""),
		      "success is logged");
		Check(AnyMessageContains(activity, L"Couldn't add a stream marker"), "marker failure is logged separately");
		Check(!AnyMessageContains(activity, L"Couldn't set category"),
		      "no failure message for the category change itself - only the marker failed");
	}

	// --- Scenario 4: external-writer mismatch is reported, never fought. ---
	{
		std::printf("-- External-writer mismatch is reported, not silently overwritten --\n");
		FakeOverrideStore overrides;
		FakeCategoryLookup lookup;
		lookup.seedExact(L"Game A", ResolvedCategory{L"111", L"Game A"});
		lookup.seedExact(L"Game B", ResolvedCategory{L"222", L"Game B"});
		CategoryResolver resolver(overrides, &lookup);

		std::vector<std::wstring> activity;
		int externalWriterCalls = 0;
		CategorySwitchCoordinator coordinator(
			resolver, [&](const std::wstring &m) { activity.push_back(m); },
			[&]() { ++externalWriterCalls; });

		FakeChannelClient channelClient;
		channelClient.channelInfoToReturn = ChannelSnapshot{L"", L""}; // No comparison basis yet - first switch proceeds.
		coordinator.setChannelClient(&channelClient);

		// First switch establishes lastAppliedCategoryId_ = "111".
		coordinator.switchIn(MakeGame(L"Game A", Platform::Steam, L"1"), /*live=*/false);
		Check(CountCallsStartingWith(channelClient.callLog, "setChannelCategory") == 1,
		      "first switch of the session PATCHes normally (nothing to compare against yet)");

		// Now simulate Restream/the dashboard/a phone app changing the
		// category to something that is neither what we last set NOR
		// what we're about to set.
		channelClient.channelInfoToReturn = ChannelSnapshot{L"999", L"Some Other Game"};
		coordinator.switchIn(MakeGame(L"Game B", Platform::Steam, L"2"), /*live=*/false);

		Check(CountCallsStartingWith(channelClient.callLog, "setChannelCategory") == 1,
		      "mismatch detected -> NO second PATCH issued (never fights the external writer)");
		Check(CountCallsStartingWith(channelClient.callLog, "getChannelInfo") == 2,
		      "the guard's GET still ran for the second switch attempt");
		Check(externalWriterCalls == 1, "external-writer callback fired exactly once");
		Check(AnyMessageContains(activity, L"standing down for this switch"),
		      "activity log explains the standdown in plain language");
	}

	// --- Scenario 5: frozen auth (reauthRequired) blocks every call. ---
	{
		std::printf("-- reauthRequired() blocks every call, no attempt made --\n");
		FakeOverrideStore overrides;
		FakeCategoryLookup lookup;
		lookup.seedExact(L"Path of Exile 2", ResolvedCategory{L"2669320", L"Path of Exile 2"});
		CategoryResolver resolver(overrides, &lookup);

		std::vector<std::wstring> activity;
		CategorySwitchCoordinator coordinator(
			resolver, [&](const std::wstring &m) { activity.push_back(m); }, []() {});

		FakeChannelClient channelClient;
		channelClient.reauthRequiredFlag = true;
		coordinator.setChannelClient(&channelClient);

		coordinator.switchIn(MakeGame(L"Path of Exile 2", Platform::Steam, L"2669320"), /*live=*/true);

		Check(channelClient.callLog.empty(), "reauthRequired() -> zero calls attempted, not even resolution's GET");
		Check(AnyMessageContains(activity, L"reconnected"), "activity log tells the user to reconnect");
	}

	// --- Scenario 6 (item 1 fix): a tier-2 alias hit (empty categoryId -
	// data/aliases.json stores names, not ids) resolves its NAME to a
	// real Twitch id via categoryLookup_ BEFORE any PATCH. This is B2:
	// the coordinator used to hand the alias table's empty categoryId
	// straight to setChannelCategory(), which PATCHes game_id:"" and
	// UNSETS the live category on Twitch - a "correct" detection (e.g.
	// Photoshop -> Art) silently clearing the channel's category. ---
	{
		std::printf("-- Tier-2 alias hit resolves id via categoryLookup_ before PATCHing (never PATCHes empty game_id) --\n");
		FakeOverrideStore overrides;
		FakeCategoryLookup lookup;
		lookup.seedExact(L"Art", ResolvedCategory{L"509658", L"Art"}); // categoryLookup_'s exact-match tier.
		CategoryResolver resolver(overrides, &lookup);
		resolver.setAliasTableForTesting({{CategoryResolver::normalize(L"Adobe Photoshop"), L"Art"}});

		std::vector<std::wstring> activity;
		CategorySwitchCoordinator coordinator(
			resolver, [&](const std::wstring &m) { activity.push_back(m); }, []() {});
		coordinator.setCategoryLookup(&lookup);

		FakeChannelClient channelClient;
		coordinator.setChannelClient(&channelClient);

		coordinator.switchIn(MakeGame(L"Adobe Photoshop", Platform::Generic, L""), /*live=*/false);

		Check(channelClient.lastSuccessfullySetCategoryId.has_value() &&
			      *channelClient.lastSuccessfullySetCategoryId == L"509658",
		      "alias hit PATCHes the RESOLVED id (509658), never the alias table's empty categoryId");
		bool anyEmptyIdPatch = false;
		for (const auto &call : channelClient.callLog) {
			if (call == "setChannelCategory:") // An empty categoryId would render as exactly this.
				anyEmptyIdPatch = true;
		}
		Check(!anyEmptyIdPatch, "no PATCH was ever issued with an empty game_id");
	}

	// --- Scenario 7 (item 1 fix, negative case): a tier-2 alias hit
	// whose name categoryLookup_ CANNOT resolve (e.g. no matching Twitch
	// category, or lookup not attached yet) issues NO PATCH at all -
	// "resolve to nothing and let the UI ask", never an empty-id PATCH. ---
	{
		std::printf("-- Tier-2 alias hit that categoryLookup_ can't resolve issues NO PATCH --\n");
		FakeOverrideStore overrides;
		FakeCategoryLookup lookup; // "Art" deliberately NOT seeded - simulates a lookup miss.
		CategoryResolver resolver(overrides, &lookup);
		resolver.setAliasTableForTesting({{CategoryResolver::normalize(L"Adobe Photoshop"), L"Art"}});

		std::vector<std::wstring> activity;
		CategorySwitchCoordinator coordinator(
			resolver, [&](const std::wstring &m) { activity.push_back(m); }, []() {});
		coordinator.setCategoryLookup(&lookup);

		FakeChannelClient channelClient;
		coordinator.setChannelClient(&channelClient);

		coordinator.switchIn(MakeGame(L"Adobe Photoshop", Platform::Generic, L""), /*live=*/false);

		Check(channelClient.callLog.empty(), "unresolvable alias -> zero calls to the channel client, never a PATCH");
		Check(AnyMessageContains(activity, L"Couldn't resolve"),
		      "activity log explains the alias couldn't be resolved to an id");
	}

	// --- Scenario 8 (item 3 fix): the coordinator's onCategoryApplied
	// feedback channel fires exactly once, with the ACTUALLY-APPLIED
	// category name, and ONLY on a real PATCH success - never on an
	// unmapped resolution or a failed PATCH. The real caller
	// (ui::CategoryDock) wires this straight to DetectionStateMachine::
	// onLiveCategoryKnown(), replacing the state machine's old
	// optimistic self-update (B7.2) with the real outcome. ---
	{
		std::printf("-- onCategoryApplied fires exactly once, with the resolved name, only on real PATCH success --\n");
		FakeOverrideStore overrides;
		FakeCategoryLookup lookup;
		lookup.seedExact(L"Path of Exile 2", ResolvedCategory{L"2669320", L"Path of Exile 2"});
		CategoryResolver resolver(overrides, &lookup);

		std::vector<std::wstring> appliedNames;
		CategorySwitchCoordinator coordinator(
			resolver, [](const std::wstring &) {}, []() {},
			[&](const std::wstring &name) { appliedNames.push_back(name); });

		FakeChannelClient channelClient;
		coordinator.setChannelClient(&channelClient);
		coordinator.switchIn(MakeGame(L"Path of Exile 2", Platform::Steam, L"2669320"), /*live=*/false);

		Check(appliedNames.size() == 1 && appliedNames[0] == L"Path of Exile 2",
		      "onCategoryApplied fires exactly once with the resolved category name on success");
	}
	{
		std::printf("-- onCategoryApplied does NOT fire when the resolver couldn't map anything --\n");
		FakeOverrideStore overrides;
		FakeCategoryLookup lookup; // Nothing seeded - every lookup misses.
		CategoryResolver resolver(overrides, &lookup);

		int appliedCount = 0;
		CategorySwitchCoordinator coordinator(
			resolver, [](const std::wstring &) {}, []() {}, [&](const std::wstring &) { ++appliedCount; });

		FakeChannelClient channelClient;
		coordinator.setChannelClient(&channelClient);
		coordinator.switchIn(MakeGame(L"Totally Unknown Title", Platform::Generic, L""), /*live=*/false);

		Check(appliedCount == 0, "onCategoryApplied never fires when nothing was mapped (no PATCH was ever issued)");
	}
	{
		std::printf("-- onCategoryApplied does NOT fire when the PATCH itself fails --\n");
		FakeOverrideStore overrides;
		FakeCategoryLookup lookup;
		lookup.seedExact(L"Path of Exile 2", ResolvedCategory{L"2669320", L"Path of Exile 2"});
		CategoryResolver resolver(overrides, &lookup);

		int appliedCount = 0;
		CategorySwitchCoordinator coordinator(
			resolver, [](const std::wstring &) {}, []() {}, [&](const std::wstring &) { ++appliedCount; });

		FakeChannelClient channelClient;
		channelClient.setCategorySucceeds = false;
		coordinator.setChannelClient(&channelClient);
		coordinator.switchIn(MakeGame(L"Path of Exile 2", Platform::Steam, L"2669320"), /*live=*/false);

		Check(appliedCount == 0, "onCategoryApplied never fires on a failed PATCH");
	}

	// --- Scenario 9 (item 6b fix): Undo is a real revert, via
	// applyFallback()'s CompletionCallback - it must succeed before
	// anything marks itself undone, and it must fire onCategoryApplied
	// like any other successful switch. ---
	{
		std::printf("-- Undo (applyFallback's CompletionCallback) only reports success once the PATCH actually succeeds --\n");
		FakeOverrideStore overrides;
		FakeCategoryLookup lookup;
		lookup.seedExact(L"Just Chatting", ResolvedCategory{L"509660", L"Just Chatting"});
		CategoryResolver resolver(overrides, &lookup);

		CategorySwitchCoordinator coordinator(
			resolver, [](const std::wstring &) {}, []() {});
		coordinator.setCategoryLookup(&lookup);

		FakeChannelClient channelClient;
		coordinator.setChannelClient(&channelClient);

		bool completionCalled = false;
		bool completionSuccess = false;
		coordinator.applyFallback(L"Just Chatting", /*live=*/false, [&](bool success) {
			completionCalled = true;
			completionSuccess = success;
		});
		Check(completionCalled && completionSuccess, "Undo's completion callback fires true once the revert PATCH succeeds");
		Check(channelClient.lastSuccessfullySetCategoryId.has_value() &&
			      *channelClient.lastSuccessfullySetCategoryId == L"509660",
		      "the revert PATCH actually carried the previous category's resolved id");
	}
	{
		std::printf("-- Undo's completion callback reports failure (never marks undone) when the category can't be found --\n");
		FakeOverrideStore overrides;
		FakeCategoryLookup lookup; // Nothing seeded - the "previous category" name can't be resolved.
		CategoryResolver resolver(overrides, &lookup);

		CategorySwitchCoordinator coordinator(
			resolver, [](const std::wstring &) {}, []() {});
		coordinator.setCategoryLookup(&lookup);

		FakeChannelClient channelClient;
		coordinator.setChannelClient(&channelClient);

		bool completionCalled = false;
		bool completionSuccess = true;
		coordinator.applyFallback(L"Some Vanished Category", /*live=*/false, [&](bool success) {
			completionCalled = true;
			completionSuccess = success;
		});
		Check(completionCalled && !completionSuccess, "Undo's completion callback reports failure, not silence, on an unresolvable category");
	}
}

// ---------------------------------------------------------------------
// SECTION 4: InstallIndex rebuild gating - proves the exact mechanism
// DetectionEngine::workerLoop() relies on for "never rebuild on the hot
// path": a real InstallIndex, a counting IGameProvider test double (no
// registry/file I/O, no OBS), and the same hasCheapChangeSignal() calls
// workerLoop() makes every poll iteration.
// ---------------------------------------------------------------------

class CountingProvider : public IGameProvider {
public:
	int enumerateCalls = 0;
	std::optional<std::chrono::system_clock::time_point> signal;

	std::vector<InstalledGame> enumerate() override
	{
		++enumerateCalls;
		InstalledGame game;
		game.displayName = L"Fake Game " + std::to_wstring(enumerateCalls);
		game.installRoot = L"C:\\FakeGames\\Fake" + std::to_wstring(enumerateCalls) + L"\\";
		game.platform = Platform::Steam;
		game.platformId = std::to_wstring(enumerateCalls);
		return {game};
	}

	std::wstring providerId() const override { return L"fake"; }

	std::optional<std::chrono::system_clock::time_point> changeSignal() const override { return signal; }
};

void RunInstallIndexScenario()
{
	std::printf("\n=== SECTION 4: InstallIndex rebuild gating (never-on-hot-path proof) ===\n");

	InstallIndex index;
	auto ownedProvider = std::make_unique<CountingProvider>();
	CountingProvider *provider = ownedProvider.get();
	index.addProvider(std::move(ownedProvider));

	// Startup: DetectionEngine::workerLoop() always does one unconditional
	// rebuild() when there's no usable cache to load (InstallIndex.h's
	// own documented contract - hasCheapChangeSignal() is never consulted
	// for this first call).
	index.rebuild();
	Check(provider->enumerateCalls == 1, "startup rebuild() enumerates exactly once");

	// Simulate 5 poll iterations with nothing on disk having changed -
	// this is EXACTLY the check DetectionEngine::workerLoop() makes every
	// iteration (see its "installIndex_.hasCheapChangeSignal()" branch)
	// before ever considering a rebuild.
	for (int i = 0; i < 5; ++i) {
		Check(!index.hasCheapChangeSignal(), "hasCheapChangeSignal() false with nothing changed");
	}
	Check(provider->enumerateCalls == 1,
	      "5 simulated poll iterations with no change signal -> enumerate() still called exactly ONCE "
	      "(the index was NOT rebuilt on the hot path)");

	// Now the provider's on-disk source changes (a real install/uninstall
	// happened) - hasCheapChangeSignal() must flip true, which is
	// DetectionEngine's cue to pay for a real rebuild().
	provider->signal = std::chrono::system_clock::now() + std::chrono::seconds(1);
	Check(index.hasCheapChangeSignal(), "hasCheapChangeSignal() true after a simulated on-disk change");

	index.rebuild(); // What DetectionEngine::workerLoop() does when the signal fires.
	Check(provider->enumerateCalls == 2, "rebuild() only runs a second time once the change signal actually fired");
}

// ---------------------------------------------------------------------
// SECTION 5: hourly Twitch token validation timing (item 5). Pure
// scheduling logic only - see TimingConstants.h's ShouldValidateTwitchToken()
// doc comment. The actual wiring (CategoryDock::maybeValidateTwitchToken(),
// piggybacked on tickTimer_) lives in the Qt-dependent dock and is NOT
// exercised here - see the project report.
// ---------------------------------------------------------------------

void RunTokenValidationTimingScenario()
{
	std::printf("\n=== SECTION 5: hourly Twitch token validation timing (item 5) ===\n");

	Check(ShouldValidateTwitchToken(/*lastValidatedAtUnixS=*/0, /*nowUnixS=*/1000),
	      "never validated this run (0) -> validate now (covers both startup and a fresh connect)");
	Check(!ShouldValidateTwitchToken(/*lastValidatedAtUnixS=*/1000, /*nowUnixS=*/1010, /*intervalS=*/3600),
	      "validated 10s ago, interval 3600s -> not due yet");
	Check(ShouldValidateTwitchToken(/*lastValidatedAtUnixS=*/1000, /*nowUnixS=*/1000 + 3600, /*intervalS=*/3600),
	      "exactly one interval elapsed -> due");
	Check(ShouldValidateTwitchToken(/*lastValidatedAtUnixS=*/1000, /*nowUnixS=*/1000 + 7200, /*intervalS=*/3600),
	      "well past the interval -> due");
}

} // namespace

// ---------------------------------------------------------------------
// SECTION 6: the SHIPPED data files.
//
// data/aliases.json and data/helpers.json stopped being inert lookup
// tables the moment they started carrying policy - "prompt": true decides
// whether an app can change someone's category on its own, and the
// exclusions list decides what is eligible to be detected at all. A typo
// in either changes runtime behavior while every other test in this file
// still passes, because every other test builds its tables in C++.
//
// So these load the real files, by absolute path, through the real
// parsers.
// ---------------------------------------------------------------------

void RunShippedDataFileScenarios()
{
	std::printf("\n=== SECTION 6: shipped data files ===\n");

	const std::wstring root = Widen(SIGNALBOX_SOURCE_DIR);

	// --- aliases.json ---
	FakeOverrideStore overrides;
	CategoryResolver resolver(overrides);
	resolver.loadAliasTable(root + L"/data/aliases.json");

	const InstalledGame vscode = MakeGame(L"Visual Studio Code", Platform::Generic, L"VSCode");
	const InstalledGame photoshop = MakeGame(L"Adobe Photoshop", Platform::Generic, L"PS");
	const InstalledGame game = MakeGame(L"Marvel Rivals", Platform::Steam, L"2767030");

	// The exact string the live OBS log showed being auto-switched to.
	// It was absent from the table, so it was not prompt-only, so it
	// silently changed the category - which is the whole failure this
	// feature exists to prevent, reached through a data gap rather than
	// a logic bug.
	const InstalledGame afterEffects = MakeGame(L"Adobe After Effects 2020", Platform::Generic, L"AEFT_17_0_4");
	Check(resolver.isPromptOnly(afterEffects), "Adobe After Effects 2020 is prompt-only");
	Check(resolver.aliasCategoryFor(afterEffects) == L"Art", "and resolves to a real category");

	// Version suffixes are exactly what an exact-match table cannot keep
	// up with, so the pattern has to survive years it has never seen.
	const InstalledGame futureAe = MakeGame(L"Adobe After Effects 2031", Platform::Generic, L"AEFT_28");
	Check(resolver.isPromptOnly(futureAe), "and so is a year nobody has written down yet");

	const InstalledGame ps2026 = MakeGame(L"Adobe Photoshop 2026", Platform::Generic, L"PS26");
	Check(resolver.isPromptOnly(ps2026), "Photoshop with a year is prompt-only");
	Check(resolver.aliasCategoryFor(ps2026) == L"Art", "and still maps to Art");

	Check(resolver.aliasCategoryFor(vscode) == L"Software and Game Development",
	      "shipped aliases.json still maps VS Code to a real category");
	Check(resolver.isPromptOnly(vscode), "and marks it prompt-only, so it can never switch on its own");
	Check(resolver.isPromptOnly(photoshop), "Photoshop is prompt-only too");
	Check(!resolver.isPromptOnly(game), "a STEAM game that is not in the table still switches automatically");
	Check(resolver.aliasCategoryFor(game).empty(), "and has no alias category");

	// The inverted default. Every one of these is a real entry from a
	// real machine's Uninstall registry, and every one of them was
	// eligible to become a live streamer's Twitch category.
	for (const wchar_t *name : {L"Microsoft 365 Apps for business - en-us", L"AMD PCI Driver",
				     L"Realtek Audio Driver", L"Wireshark 4.6.7 x64", L"7-Zip 26.02 (x64)",
				     L"TeamViewer", L"Samsung Magician", L"Some Unknown Indie Thing"}) {
		const InstalledGame junk = MakeGame(name, Platform::Generic, L"whatever");
		Check(resolver.isPromptOnly(junk), Narrow(std::wstring(L"uncurated registry entry asks first: ") + name).c_str());
	}

	// ...but curation still wins, in both directions. A plain string
	// alias is a human saying "this really is a game", and it keeps
	// switching automatically even though it is a Generic entry.
	{
		FakeOverrideStore o;
		CategoryResolver curated(o);
		curated.setAliasTableForTesting({{CategoryResolver::normalize(L"My Itch Game"), L"My Itch Game"}});
		const InstalledGame itchGame = MakeGame(L"My Itch Game", Platform::Generic, L"itch123");
		Check(!curated.isPromptOnly(itchGame), "an explicitly aliased Generic entry still switches automatically");
	}

	// --- helpers.json ---
	const signalbox::detection::HelperDenylist denylist =
		signalbox::detection::HelperDenylist::LoadFromFile(root + L"/data/helpers.json");

	Check(!denylist.indexExclusions.empty(), "shipped helpers.json parses an exclusions section at all");

	// The two entries that caused the original misdetections. Wallpaper
	// Engine is keyed by appid because it is a real Steam app; Opera GX
	// by name because its Uninstall entry carries a version that changes.
	const InstalledGame wallpaperEngine = MakeGame(L"Wallpaper Engine", Platform::Steam, L"431960");
	const InstalledGame operaGx = MakeGame(L"Opera GX Stable 134.0.5954.44", Platform::Generic, L"Opera GX");
	const InstalledGame operaGxLater = MakeGame(L"Opera GX Stable 999.0.1.2", Platform::Generic, L"Opera GX");

	Check(denylist.indexExclusions.excludes(wallpaperEngine), "Wallpaper Engine is excluded from the index");
	Check(denylist.indexExclusions.excludes(operaGx), "Opera GX is excluded from the index");
	Check(denylist.indexExclusions.excludes(operaGxLater),
	      "and stays excluded after a version bump - the rule is not pinned to today's version string");
	Check(!denylist.indexExclusions.excludes(game), "a real game is not excluded");

	// Creative apps must survive the exclusions - they are deliberately
	// detectable, just prompt-only. Excluding one would silently delete
	// the entire Trigger C feature for that app.
	Check(!denylist.indexExclusions.excludes(photoshop), "Photoshop is NOT excluded - it is prompt-only, not ignored");
	Check(!denylist.indexExclusions.excludes(vscode), "VS Code is NOT excluded either");

	Check(denylist.shouldIgnoreProcess(L"", L"chrome.exe"), "browsers are rejected by process basename");
	Check(denylist.shouldIgnoreProcess(L"", L"opera_gx.exe"), "including Opera GX");
	Check(!denylist.shouldIgnoreProcess(L"", L"MarvelRivals_Launcher.exe"), "a game process is not rejected");
}

int main()
{
	std::printf("SignalBox - standalone harness (no OBS, no live Twitch)\n");

	RunResolverScenarios();
	RunStateMachineScenarios();
	RunExitSignalIdentityScenarios();
	RunGoLiveMismatchScenarios();
	RunCreativeAppScenarios();
	RunExternalOverrideScenarios();
	RunStreamEndingHoldScenarios();
	RunIdlePromptScenarios();
	RunCoordinatorScenarios();
	RunInstallIndexScenario();
	RunTokenValidationTimingScenario();
	RunShippedDataFileScenarios();

	std::printf("\n=== RESULT: %s (%d failing check%s) ===\n", g_failures == 0 ? "PASS" : "FAIL", g_failures,
		    g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
