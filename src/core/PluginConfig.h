/*
 * SignalBox - core/PluginConfig.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Wraps obs_data for the plugin's persisted, human-editable config
 * (DESIGN.md 4.4). Lives at obs_module_config_path("config.json"),
 * which resolves under %APPDATA%\obs-studio\plugin_config\signalbox\.
 *
 * DELIBERATELY SEPARATE FROM TOKEN STORAGE. Twitch tokens live in
 * tokens.bin (DPAPI-encrypted, see twitch/TokenStore.h) in the same
 * directory - NEVER in this file, and never in anything a scene
 * collection export could carry. See DESIGN.md 3.3 and 4.4.
 *
 * Contents (DESIGN.md 4.4): poll/confirm/grace timings (a persisted
 * override of TimingConstants, surfaced under the dock's "Advanced
 * timing" group), only-while-live flag, fallback-category choice,
 * auto-switch/prompts/fallback-switching feature toggles (SettingsDialog's
 * three top checkboxes - previously live-only on DetectionStateMachine and
 * lost on restart; now persisted here and re-applied by whichever
 * OBS-aware caller constructs the state machine), and per-game user
 * overrides (CategoryResolver tier 1, via IUserOverrideStore).
 *
 * NOT YET HERE: the CategoryResolver tier-3 learned-mappings cache - that
 * lives in TwitchClient's own category-cache.json (see TwitchClient.h's
 * class doc comment), which already implements the "resolve once, ever"
 * persistence this file's own doc comment used to describe. Duplicating
 * it here would just be a second cache to keep in sync for no benefit.
 *
 * THREADING: obs_data is not documented as thread-safe for concurrent
 * read/write. Treat PluginConfig as main-thread-owned, matching every
 * other Qt/OBS-facing piece of this plugin (DESIGN.md 4.3). If
 * DetectionEngine's worker thread ever needs a config value, it must be a
 * value snapshotted at start() (as timing() already is - see
 * DetectionEngine's constructor call sites), never a live PluginConfig
 * reference.
 */

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "RecentCategories.h"
#include "TimingConstants.h"
#include "UserOverrideStore.h"

// Forward-declared only - obs-data.h is included in PluginConfig.cpp, not
// here, so translation units that only want timing()/fallbackCategoryName()
// don't pay for pulling in the OBS data API.
struct obs_data;

namespace signalbox::core {

class PluginConfig : public IUserOverrideStore {
public:
	PluginConfig();
	~PluginConfig() override;

	PluginConfig(const PluginConfig &) = delete;
	PluginConfig &operator=(const PluginConfig &) = delete;

	// Loads config.json from obs_module_config_path(), or seeds
	// defaults if it doesn't exist yet. Call once from
	// obs_module_load(). Safe to call again later (e.g. to discard
	// unsaved in-memory edits) - fully replaces in-memory state from
	// disk.
	void load();

	// Writes the current in-memory state back to config.json via
	// obs_data's atomic safe-save (obs_data_save_json_safe). Call after
	// any change made through the dock/settings dialog - SettingsDialog
	// calls this exactly once, from its Save button (lightweight-by-
	// design: persist on explicit change, never on a timer).
	void save();

	// Effective timing constants: compiled-in defaults with any
	// user-persisted overrides applied.
	const TimingConstants &timing() const;
	void setTiming(const TimingConstants &timing);

	// Fallback category shown/used by Trigger B's "Switch to X" option
	// and applied on prompt timeout is explicitly NOT this - the
	// timeout default is always "hold the last category" per the
	// ADDENDUM. This is only the *labelled* fallback choice offered in
	// the prompt and in the dock's manual controls (default "Just
	// Chatting").
	std::wstring fallbackCategoryName() const;
	void setFallbackCategoryName(std::wstring name);

	// Restrict automatic changes to the OBS_FRONTEND_EVENT_STREAMING_
	// STARTED..STOPPED window. DESIGN.md 2.2 "Only-while-live option".
	bool onlyWhileLive() const;
	void setOnlyWhileLive(bool value);

	// --- Persisted mirrors of SettingsDialog's behavior toggles ---
	// These were previously applied directly (and only) to the live
	// DetectionStateMachine, so a restart silently reset every one of
	// them. The dock/coordinator is responsible for calling
	// stateMachine.setManualLock(!autoSwitchEnabled()) etc. once at
	// startup after load() - this class only stores the values.

	// Whether automatic category switching runs at all (dock's manual-
	// lock toggle, inverted: true = automation enabled). Default true.
	bool autoSwitchEnabled() const;
	void setAutoSwitchEnabled(bool enabled);

	// Whether the two ADDENDUM prompts (go-live mismatch / game closed)
	// are ever raised. Default true.
	bool promptsEnabled() const;
	void setPromptsEnabled(bool enabled);

	// Whether GameClosed's "Switch to <fallback>" response is allowed to
	// actually apply the fallback category. OFF BY DEFAULT per the
	// project brief - hold-last-category is the default outcome even
	// when the user explicitly asks to switch, unless this is on.
	bool fallbackSwitchingEnabled() const;
	void setFallbackSwitchingEnabled(bool enabled);

	// Whether Trigger D - the "no game running, what are you up to?"
	// prompt - may be raised at all. Default ON: it is the only prompt
	// that appears without a game or a stream to attach itself to, so it
	// is also the only one a user might want gone entirely rather than
	// just answered, and it needs a permanent off switch that isn't
	// "turn off all prompts". Independent of promptsEnabled(), which
	// still gates it too (all prompts off means all prompts off).
	bool idlePromptEnabled() const;
	void setIdlePromptEnabled(bool enabled);

	// --- CategoryResolver tier 1 (DESIGN.md 1.5) ---
	// Keyed by CategoryResolver::overrideKeyFor() - "platform:platformId"
	// (preferred) or an absolute exe path fallback.
	std::optional<UserOverride> findUserOverride(const std::wstring &key) const override;
	void setUserOverride(const std::wstring &key, const UserOverride &value);
	void removeUserOverride(const std::wstring &key);
	const std::map<std::wstring, UserOverride> &userOverrides() const;

	// Friendly display name to show next to a tier-1 override in the UI
	// (SettingsDialog's override list, GameOverrideDialog's own header) -
	// purely cosmetic, and DELIBERATELY not a field on UserOverride
	// itself: UserOverride/IUserOverrideStore (UserOverrideStore.h) is
	// CategoryResolver's tiny, OBS/Qt-free interface (DESIGN.md 1.5), and
	// resolve() has no use for a display name at all - only the key.
	// Keeping it a separate side map here means the resolver's contract
	// never changes shape for a UI-only need. Best-effort: only ever set
	// by the one dock action that already knows the InstalledGame's
	// displayName at the moment an override is created
	// (GameOverrideDialog); a hand-edited config.json entry with no
	// matching name here just falls back to showing the key alone - see
	// SettingsDialog.cpp. Returns an empty string if none is recorded.
	std::wstring userOverrideDisplayName(const std::wstring &key) const;
	void setUserOverrideDisplayName(const std::wstring &key, const std::wstring &displayName);

	// --- Recently applied categories (RecentCategories.h for the rules) ---
	// Persisted so "the same few games, nearly every stream" survives a
	// restart. Fed only from a CONFIRMED category change, never from a
	// request, so the list holds names Twitch actually accepted.
	const std::vector<std::wstring> &recentCategories() const;

	// Returns true if the list changed, i.e. the caller should save().
	// The configured fallback category is deliberately not recorded - it has
	// its own button.
	bool noteRecentCategory(std::wstring name);

private:
	TimingConstants timing_{};
	std::wstring fallbackCategoryName_ = L"Just Chatting";
	bool onlyWhileLive_ = false;
	bool autoSwitchEnabled_ = true;
	bool promptsEnabled_ = true;
	bool fallbackSwitchingEnabled_ = false; // Off by default - see project brief.
	bool idlePromptEnabled_ = true;
	std::map<std::wstring, UserOverride> userOverrides_;
	std::map<std::wstring, std::wstring> userOverrideDisplayNames_; // Cosmetic only - see userOverrideDisplayName().
	RecentCategories recentCategories_;

	obs_data *data_ = nullptr; // Owned; released in ~PluginConfig() and on every load().
};

} // namespace signalbox::core
