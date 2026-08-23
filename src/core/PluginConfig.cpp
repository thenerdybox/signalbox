/*
 * SignalBox - core/PluginConfig.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Real obs_data-backed load()/save(). See PluginConfig.h for what's
 * persisted and why. Field access follows the standard obs_data pattern:
 * seed a default via obs_data_set_default_*, then obs_data_get_* returns
 * that default whenever no user value is present - so load() never needs
 * a manual "is this the first run" branch, and a config.json that only
 * overrides one field is valid (every other field falls through to its
 * compiled-in default).
 */

#include "PluginConfig.h"

// obs-module.h (and the util/ headers it pulls in) must come before
// plugin-support.h in any TU that needs both - see TwitchClient.cpp's
// include-order comment for why (MSVC C2375 on blogva() otherwise).
#include <obs-data.h>
#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include "../plugin-support.h"

namespace signalbox::core {

namespace {

// bmalloc'd UTF-8 -> std::wstring, freeing the bmalloc'd intermediate.
// Mirrors the conversion pattern already used by TokenStore.cpp/
// TwitchClient.cpp for the same obs_module_config_path()/os_*_ptr() APIs.
std::wstring Utf8ToWide(const char *utf8)
{
	if (!utf8 || !*utf8) {
		return {};
	}
	std::wstring result;
	wchar_t *wide = nullptr;
	if (os_utf8_to_wcs_ptr(utf8, 0, &wide) > 0 && wide) {
		result = wide;
	}
	if (wide) {
		bfree(wide);
	}
	return result;
}

std::string WideToUtf8(const std::wstring &wide)
{
	if (wide.empty()) {
		return {};
	}
	std::string result;
	char *utf8 = nullptr;
	const std::size_t len = os_wcs_to_utf8_ptr(wide.c_str(), wide.size(), &utf8);
	if (utf8 && len > 0) {
		result.assign(utf8, len);
	}
	if (utf8) {
		bfree(utf8);
	}
	return result;
}

std::string ConfigPathUtf8(const char *file)
{
	char *path = obs_module_config_path(file);
	if (!path) {
		return {};
	}
	std::string result(path);
	bfree(path);
	return result;
}

} // namespace

PluginConfig::PluginConfig() = default;

PluginConfig::~PluginConfig()
{
	if (data_) {
		obs_data_release(data_); // struct obs_data* and obs_data_t* are the same type (see header).
	}
}

void PluginConfig::load()
{
	const std::string path = ConfigPathUtf8("config.json");
	if (path.empty()) {
		// No config dir available (e.g. a standalone test harness, or
		// OBS's module path resolution failed) - stay on compiled-in
		// defaults rather than fail; nothing here is safety-critical.
		return;
	}

	if (data_) {
		obs_data_release(data_);
		data_ = nullptr;
	}

	obs_data_t *loaded = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	obs_data_t *data = loaded ? loaded : obs_data_create(); // Fresh install: seed defaults below.
	data_ = data;

	// --- Timing (nested object; every field has an explicit default so
	// a config.json written by an older version with fewer fields still
	// loads cleanly). ---
	obs_data_t *timingObj = obs_data_get_obj(data, "timing");
	if (!timingObj) {
		timingObj = obs_data_create();
		obs_data_set_obj(data, "timing", timingObj);
	}

	const TimingConstants defaults{};
	obs_data_set_default_int(timingObj, "pollIntervalS", defaults.pollIntervalS);
	obs_data_set_default_int(timingObj, "idlePollIntervalS", defaults.idlePollIntervalS);
	obs_data_set_default_int(timingObj, "confirmPolls", defaults.confirmPolls);
	obs_data_set_default_int(timingObj, "foregroundTiebreakPolls", defaults.foregroundTiebreakPolls);
	obs_data_set_default_int(timingObj, "crashGraceS", defaults.crashGraceS);
	obs_data_set_default_int(timingObj, "cleanExitGraceS", defaults.cleanExitGraceS);
	obs_data_set_default_int(timingObj, "indexRebuildIntervalS", defaults.indexRebuildIntervalS);
	obs_data_set_default_int(timingObj, "minPatchSpacingS", defaults.minPatchSpacingS);
	obs_data_set_default_int(timingObj, "flapBreakerN", defaults.flapBreakerN);
	obs_data_set_default_int(timingObj, "flapBreakerWindowS", defaults.flapBreakerWindowS);
	obs_data_set_default_int(timingObj, "promptTimeoutS", defaults.promptTimeoutS);

	TimingConstants t;
	t.pollIntervalS = static_cast<std::uint32_t>(obs_data_get_int(timingObj, "pollIntervalS"));
	t.idlePollIntervalS = static_cast<std::uint32_t>(obs_data_get_int(timingObj, "idlePollIntervalS"));
	t.confirmPolls = static_cast<std::uint32_t>(obs_data_get_int(timingObj, "confirmPolls"));
	t.foregroundTiebreakPolls = static_cast<std::uint32_t>(obs_data_get_int(timingObj, "foregroundTiebreakPolls"));
	t.crashGraceS = static_cast<std::uint32_t>(obs_data_get_int(timingObj, "crashGraceS"));
	t.cleanExitGraceS = static_cast<std::uint32_t>(obs_data_get_int(timingObj, "cleanExitGraceS"));
	t.indexRebuildIntervalS = static_cast<std::uint32_t>(obs_data_get_int(timingObj, "indexRebuildIntervalS"));
	t.minPatchSpacingS = static_cast<std::uint32_t>(obs_data_get_int(timingObj, "minPatchSpacingS"));
	t.flapBreakerN = static_cast<std::uint32_t>(obs_data_get_int(timingObj, "flapBreakerN"));
	t.flapBreakerWindowS = static_cast<std::uint32_t>(obs_data_get_int(timingObj, "flapBreakerWindowS"));
	t.promptTimeoutS = static_cast<std::uint32_t>(obs_data_get_int(timingObj, "promptTimeoutS"));
	timing_ = t;
	obs_data_release(timingObj);

	// --- Top-level scalars. ---
	obs_data_set_default_string(data, "fallbackCategoryName", "Just Chatting");
	fallbackCategoryName_ = Utf8ToWide(obs_data_get_string(data, "fallbackCategoryName"));

	obs_data_set_default_bool(data, "onlyWhileLive", false);
	onlyWhileLive_ = obs_data_get_bool(data, "onlyWhileLive");

	obs_data_set_default_bool(data, "autoSwitchEnabled", true);
	autoSwitchEnabled_ = obs_data_get_bool(data, "autoSwitchEnabled");

	obs_data_set_default_bool(data, "promptsEnabled", true);
	promptsEnabled_ = obs_data_get_bool(data, "promptsEnabled");

	// Off by default - hold-last-category is the default even when the
	// user explicitly asks to switch, unless this is turned on
	// (project brief; see DetectionStateMachine::fallbackEnabled()).
	obs_data_set_default_bool(data, "fallbackSwitchingEnabled", false);
	fallbackSwitchingEnabled_ = obs_data_get_bool(data, "fallbackSwitchingEnabled");

	// On by default - see PluginConfig.h. An existing config.json written
	// before this key existed simply gets the default, which is what we
	// want: the prompt is a new feature, not an opt-in one.
	obs_data_set_default_bool(data, "idlePromptEnabled", true);
	idlePromptEnabled_ = obs_data_get_bool(data, "idlePromptEnabled");

	// --- Per-game user overrides (CategoryResolver tier 1). ---
	userOverrides_.clear();
	userOverrideDisplayNames_.clear();
	obs_data_array_t *overridesArray = obs_data_get_array(data, "userOverrides");
	if (overridesArray) {
		const std::size_t count = obs_data_array_count(overridesArray);
		for (std::size_t i = 0; i < count; ++i) {
			obs_data_t *entry = obs_data_array_item(overridesArray, i);
			if (!entry) {
				continue;
			}
			const std::wstring key = Utf8ToWide(obs_data_get_string(entry, "key"));
			if (!key.empty()) {
				UserOverride override;
				override.categoryId = Utf8ToWide(obs_data_get_string(entry, "categoryId"));
				override.categoryName = Utf8ToWide(obs_data_get_string(entry, "categoryName"));
				override.ignored = obs_data_get_bool(entry, "ignored");
				userOverrides_[key] = std::move(override);

				// Cosmetic only - see userOverrideDisplayName()'s doc
				// comment. Absent on an entry that predates this field
				// or was hand-edited; that's fine, display just falls
				// back to the key.
				const std::wstring displayName = Utf8ToWide(obs_data_get_string(entry, "displayName"));
				if (!displayName.empty())
					userOverrideDisplayNames_[key] = displayName;
			}
			obs_data_release(entry);
		}
		obs_data_array_release(overridesArray);
	}

	obs_log(LOG_DEBUG, "config: loaded from %s (%zu user override(s))", path.c_str(), userOverrides_.size());
}

void PluginConfig::save()
{
	const std::string path = ConfigPathUtf8("config.json");
	if (path.empty()) {
		return;
	}

	const std::string dir = ConfigPathUtf8("");
	if (!dir.empty()) {
		os_mkdirs(dir.c_str()); // MKDIR_EXISTS is a normal, expected return here.
	}

	if (!data_) {
		data_ = obs_data_create();
	}
	obs_data_t *data = data_;

	obs_data_t *timingObj = obs_data_create();
	obs_data_set_int(timingObj, "pollIntervalS", timing_.pollIntervalS);
	obs_data_set_int(timingObj, "idlePollIntervalS", timing_.idlePollIntervalS);
	obs_data_set_int(timingObj, "confirmPolls", timing_.confirmPolls);
	obs_data_set_int(timingObj, "foregroundTiebreakPolls", timing_.foregroundTiebreakPolls);
	obs_data_set_int(timingObj, "crashGraceS", timing_.crashGraceS);
	obs_data_set_int(timingObj, "cleanExitGraceS", timing_.cleanExitGraceS);
	obs_data_set_int(timingObj, "indexRebuildIntervalS", timing_.indexRebuildIntervalS);
	obs_data_set_int(timingObj, "minPatchSpacingS", timing_.minPatchSpacingS);
	obs_data_set_int(timingObj, "flapBreakerN", timing_.flapBreakerN);
	obs_data_set_int(timingObj, "flapBreakerWindowS", timing_.flapBreakerWindowS);
	obs_data_set_int(timingObj, "promptTimeoutS", timing_.promptTimeoutS);
	obs_data_set_obj(data, "timing", timingObj);
	obs_data_release(timingObj);

	obs_data_set_string(data, "fallbackCategoryName", WideToUtf8(fallbackCategoryName_).c_str());
	obs_data_set_bool(data, "onlyWhileLive", onlyWhileLive_);
	obs_data_set_bool(data, "autoSwitchEnabled", autoSwitchEnabled_);
	obs_data_set_bool(data, "promptsEnabled", promptsEnabled_);
	obs_data_set_bool(data, "fallbackSwitchingEnabled", fallbackSwitchingEnabled_);
	obs_data_set_bool(data, "idlePromptEnabled", idlePromptEnabled_);

	obs_data_array_t *overridesArray = obs_data_array_create();
	for (const auto &[key, override] : userOverrides_) {
		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, "key", WideToUtf8(key).c_str());
		obs_data_set_string(entry, "categoryId", WideToUtf8(override.categoryId).c_str());
		obs_data_set_string(entry, "categoryName", WideToUtf8(override.categoryName).c_str());
		obs_data_set_bool(entry, "ignored", override.ignored);
		// Cosmetic only - see userOverrideDisplayName()'s doc comment.
		const auto nameIt = userOverrideDisplayNames_.find(key);
		if (nameIt != userOverrideDisplayNames_.end())
			obs_data_set_string(entry, "displayName", WideToUtf8(nameIt->second).c_str());
		obs_data_array_push_back(overridesArray, entry);
		obs_data_release(entry);
	}
	obs_data_set_array(data, "userOverrides", overridesArray);
	obs_data_array_release(overridesArray);

	if (!obs_data_save_json_safe(data, path.c_str(), "tmp", "bak")) {
		obs_log(LOG_WARNING, "config: failed to save %s", path.c_str());
	}
}

const TimingConstants &PluginConfig::timing() const
{
	return timing_;
}

void PluginConfig::setTiming(const TimingConstants &timing)
{
	timing_ = timing;
}

std::wstring PluginConfig::fallbackCategoryName() const
{
	return fallbackCategoryName_;
}

void PluginConfig::setFallbackCategoryName(std::wstring name)
{
	fallbackCategoryName_ = std::move(name);
}

bool PluginConfig::onlyWhileLive() const
{
	return onlyWhileLive_;
}

void PluginConfig::setOnlyWhileLive(bool value)
{
	onlyWhileLive_ = value;
}

bool PluginConfig::autoSwitchEnabled() const
{
	return autoSwitchEnabled_;
}

void PluginConfig::setAutoSwitchEnabled(bool enabled)
{
	autoSwitchEnabled_ = enabled;
}

bool PluginConfig::promptsEnabled() const
{
	return promptsEnabled_;
}

void PluginConfig::setPromptsEnabled(bool enabled)
{
	promptsEnabled_ = enabled;
}

bool PluginConfig::fallbackSwitchingEnabled() const
{
	return fallbackSwitchingEnabled_;
}

void PluginConfig::setFallbackSwitchingEnabled(bool enabled)
{
	fallbackSwitchingEnabled_ = enabled;
}

bool PluginConfig::idlePromptEnabled() const
{
	return idlePromptEnabled_;
}

void PluginConfig::setIdlePromptEnabled(bool enabled)
{
	idlePromptEnabled_ = enabled;
}

std::optional<UserOverride> PluginConfig::findUserOverride(const std::wstring &key) const
{
	const auto it = userOverrides_.find(key);
	if (it == userOverrides_.end()) {
		return std::nullopt;
	}
	return it->second;
}

void PluginConfig::setUserOverride(const std::wstring &key, const UserOverride &value)
{
	userOverrides_[key] = value;
}

void PluginConfig::removeUserOverride(const std::wstring &key)
{
	userOverrides_.erase(key);
	userOverrideDisplayNames_.erase(key); // Cosmetic side map - see its own doc comment; no orphans left behind.
}

const std::map<std::wstring, UserOverride> &PluginConfig::userOverrides() const
{
	return userOverrides_;
}

std::wstring PluginConfig::userOverrideDisplayName(const std::wstring &key) const
{
	const auto it = userOverrideDisplayNames_.find(key);
	return it == userOverrideDisplayNames_.end() ? std::wstring() : it->second;
}

void PluginConfig::setUserOverrideDisplayName(const std::wstring &key, const std::wstring &displayName)
{
	if (displayName.empty()) {
		userOverrideDisplayNames_.erase(key);
		return;
	}
	userOverrideDisplayNames_[key] = displayName;
}

} // namespace signalbox::core
