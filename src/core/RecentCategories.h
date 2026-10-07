/*
 * SignalBox - core/RecentCategories.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * The short most-recent-first list of categories SignalBox has confirmed
 * applying, so the dock can offer them as one-click choices. Most streams
 * are the same handful of games, and a streamer who is "done gaming" should
 * not have to type a category name to say so.
 *
 * Header-only and free of OBS and Qt on purpose: PluginConfig owns and
 * persists one of these, but the ordering rules live here so the standalone
 * harness (which links no libobs) can exercise them directly.
 *
 * RULES
 *   - Most recent first, at most kMax entries.
 *   - De-duplicated case-insensitively. Re-noting a name moves it to the
 *     front and keeps the NEWEST spelling, since that is the one Twitch
 *     most recently reported back.
 *   - Empty names are never recorded.
 *   - The configured fallback category (e.g. "Just Chatting") is never
 *     recorded: it already has its own button, and listing it again would
 *     spend one of five slots on something always on screen.
 *
 * THREADING: plain value type, no synchronisation. Main thread only, like
 * PluginConfig.
 */

#pragma once

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace signalbox::core {

class RecentCategories {
public:
	static constexpr std::size_t kMax = 5;

	const std::vector<std::wstring> &list() const { return names_; }

	// Records `name` as the most recent category. Returns true only if the
	// list actually changed (so callers can skip a pointless config save).
	bool note(const std::wstring &name, const std::wstring &fallbackCategoryName)
	{
		if (name.empty() || equalsIgnoreCase(name, fallbackCategoryName))
			return false;

		const auto existing = std::find_if(names_.begin(), names_.end(),
						   [&](const std::wstring &n) { return equalsIgnoreCase(n, name); });
		if (existing == names_.begin() && existing != names_.end() && *existing == name)
			return false; // Already first, same spelling - nothing to do.
		if (existing != names_.end())
			names_.erase(existing);

		names_.insert(names_.begin(), name);
		if (names_.size() > kMax)
			names_.resize(kMax);
		return true;
	}

	// Replaces the list from persisted data, applying the same rules so a
	// hand-edited or older file can never leave it oversized or duplicated.
	// `ordered` is most-recent-first, as saved. The fallback is not filtered
	// here: it is only excluded at note() time, and a user who later changes
	// their fallback should not have entries vanish from a list they built.
	void assign(const std::vector<std::wstring> &ordered)
	{
		names_.clear();
		for (const auto &name : ordered) {
			if (name.empty() || names_.size() >= kMax)
				continue;
			const bool duplicate = std::any_of(names_.begin(), names_.end(), [&](const std::wstring &n) {
				return equalsIgnoreCase(n, name);
			});
			if (!duplicate)
				names_.push_back(name);
		}
	}

	static bool equalsIgnoreCase(const std::wstring &a, const std::wstring &b)
	{
		return a.size() == b.size() &&
		       std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
			       return std::towlower(x) == std::towlower(y);
		       });
	}

private:
	std::vector<std::wstring> names_;
};

} // namespace signalbox::core
