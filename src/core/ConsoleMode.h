/*
 * SignalBox - core/ConsoleMode.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * "Console mode": for a stream where the game is not on this PC. The streamer
 * turns it on, SignalBox stops scanning for games, and a small box lets them
 * type the category themselves. Turning it off hands control back to the
 * detector; it never reverts the category that was set.
 *
 * Header-only and free of OBS and Qt so the standalone harness can drive it
 * with fake hooks. The real wiring (engine pause, dialog, Twitch call) lives
 * in CategoryDock and plugin-main.cpp.
 *
 * THREADING: main thread only. The OBS hotkey thread only queues a toggle.
 */

#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace signalbox::core {

// --- The OBS hotkey ---------------------------------------------------------
inline constexpr const char *kConsoleModeHotkeyName = "signalbox.console_mode";
inline constexpr const char *kConsoleModeHotkeyLabel = "SignalBox: Console mode (set category manually)";
inline constexpr const char *kConsoleModeHotkeySaveKey = "signalbox_console_mode_hotkey";

// Registers the hotkey through `registrar` (obs_hotkey_register_frontend in the
// plugin, a fake in the harness) and returns whatever id it hands back.
using HotkeyRegistrar = std::function<std::size_t(const char *name, const char *description)>;
inline std::size_t RegisterConsoleModeHotkey(const HotkeyRegistrar &registrar)
{
	return registrar(kConsoleModeHotkeyName, kConsoleModeHotkeyLabel);
}

// --- Typing debounce --------------------------------------------------------
// Pure timing logic with the clock passed in, so it can be tested without
// waiting. The dialog drives it from a single-shot timer.
class SearchDebounce {
public:
	static constexpr std::int64_t kDelayMs = 250;
	static constexpr std::size_t kMinChars = 2;

	void onTextChanged(const std::wstring &text, std::int64_t nowMs)
	{
		text_ = Trim(text);
		changedAtMs_ = nowMs;
		armed_ = text_.size() >= kMinChars;
	}

	// The query to send now, if the text has been still for the full delay.
	// Each burst of typing yields at most one query.
	std::optional<std::wstring> due(std::int64_t nowMs)
	{
		if (!armed_ || nowMs - changedAtMs_ < kDelayMs)
			return std::nullopt;
		armed_ = false;
		return text_;
	}

	// Enter pressed before the delay ran out: send what is there right now.
	std::optional<std::wstring> flush()
	{
		if (!armed_)
			return std::nullopt;
		armed_ = false;
		return text_;
	}

	bool armed() const { return armed_; }

	static std::wstring Trim(const std::wstring &s)
	{
		std::size_t b = 0, e = s.size();
		while (b < e && std::iswspace(s[b]))
			++b;
		while (e > b && std::iswspace(s[e - 1]))
			--e;
		return s.substr(b, e - b);
	}

private:
	std::wstring text_;
	std::int64_t changedAtMs_ = 0;
	bool armed_ = false;
};

// --- Enter before the results are back ------------------------------------
// Shared by every category search box (Console mode's popup and the dock's
// "Search Twitch categories" field), so they cannot drift apart. Typing fast
// and pressing Enter must not be a dead key: remember that Enter was pressed,
// make sure a search for that text is on its way, and pick the top suggestion
// when the answer for exactly that text lands.
class EnterBeforeResults {
public:
	// Enter was pressed on `query` and Twitch has not answered it yet.
	// `needSearch` is true when no search for that text has been sent.
	// Returns true when the caller must send one now.
	bool onEnter(const std::wstring &query, bool needSearch)
	{
		waiting_ = true;
		query_ = SearchDebounce::Trim(query);
		return needSearch;
	}

	// An answer arrived for `answered`. Returns true when the caller should
	// pick the top suggestion now. An answer to some other text is ignored;
	// an empty answer to the awaited text ends the wait without a pick.
	bool onResults(const std::wstring &answered, bool anyResults)
	{
		if (!waiting_ || SearchDebounce::Trim(answered) != query_)
			return false;
		waiting_ = false;
		return anyResults;
	}

	bool waiting() const { return waiting_; }
	void reset() { waiting_ = false; }

private:
	bool waiting_ = false;
	std::wstring query_;
};

// --- The mode itself --------------------------------------------------------
struct ConsoleModeHooks {
	std::function<void(bool paused)> setDetectionPaused; // Stops/starts the process scanner.
	std::function<void()> onEntered;                     // Open the dialog, drop stale detection state.
	std::function<void()> onExited;                      // Close the dialog, hand back to the detector.
	std::function<void(const std::wstring &id, const std::wstring &name)> applyCategory; // The set-category path.
	std::function<void()> onChanged;                     // Refresh the dock.
};

class ConsoleMode {
public:
	explicit ConsoleMode(ConsoleModeHooks hooks) : hooks_(std::move(hooks)) {}

	bool active() const { return active_; }
	// The game picked in this session of console mode; empty until one is.
	const std::wstring &game() const { return game_; }

	void toggle()
	{
		if (active_)
			exit();
		else
			enter();
	}

	void enter()
	{
		if (active_)
			return;
		active_ = true;
		game_.clear();
		call(hooks_.setDetectionPaused, true);
		call(hooks_.onEntered);
		call(hooks_.onChanged);
	}

	// Does NOT touch the category: the detector takes over on its next match.
	void exit()
	{
		if (!active_)
			return;
		active_ = false;
		call(hooks_.setDetectionPaused, false);
		call(hooks_.onExited);
		call(hooks_.onChanged);
	}

	// Sets the category and stays in console mode. False if not in the mode or
	// nothing was named.
	bool pick(const std::wstring &id, const std::wstring &name)
	{
		if (!active_ || name.empty())
			return false;
		game_ = name;
		if (hooks_.applyCategory)
			hooks_.applyCategory(id, name);
		call(hooks_.onChanged);
		return true;
	}

private:
	template <class F, class... A> static void call(const F &f, A... a)
	{
		if (f)
			f(a...);
	}

	ConsoleModeHooks hooks_;
	bool active_ = false;
	std::wstring game_;
};

} // namespace signalbox::core
