/*
 * SignalBox - detection/IndexExclusions.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * The "this is installed, but it is never what you are streaming" list.
 *
 * WHY THIS EXISTS AS A SEPARATE CONCEPT FROM data/aliases.json:
 * the alias table can only ever RENAME an entry - it maps a platform's
 * display name onto a Twitch category. It has no way to say "ignore this
 * entirely", so anything installed that the index surfaces is permanently
 * eligible to win a poll. That is fine for a mis-named game and actively
 * harmful for everything else the providers legitimately find: browsers,
 * wallpaper animators, overlays, voice chat, launchers' own bundled junk.
 *
 * The case that forced this: Wallpaper Engine is a real Steam app, so
 * GenericUninstallProvider is not what surfaces it - SteamProvider is,
 * with the same Confidence tier a real game gets. It runs permanently in
 * the background on a great many streamers' machines. With nothing to
 * exclude it, it does not merely add noise - it competes with the actual
 * game on working-set size and can win, so the category flips to
 * Wallpaper Engine mid-session. Opera GX (Generic/Low) has the same
 * problem one tier down: whenever no real game is running it is the only
 * surviving candidate, so it becomes "the detected game" by default.
 *
 * WHERE THIS IS APPLIED: on ingest into InstallIndex - both rebuild()
 * and loadFromFile() - never per-poll. An excluded entry is not in the
 * index at all, so it cannot be matched, cannot be ranked, cannot be
 * aliased, and cannot reach the category-set path. Filtering here rather
 * than at match() time also means the exclusion cost is paid once per
 * rebuild instead of once per process per poll, which matters: this
 * plugin must never compete with a game for resources.
 *
 * Rules are data (data/helpers.json's "exclusions" object), so a
 * false positive - some genuinely streamable app we wrongly excluded -
 * is fixable with a data PR rather than a plugin release.
 *
 * Deliberately Qt- and OBS-free: InstallIndex.h includes this, and
 * InstallIndex.h is included by DetectionEngine.h, which must stay Qt-free
 * (see its threading-boundary note). HelperDenylist.h owns the JSON
 * parsing that produces these vectors and hands them over as plain
 * strings.
 */

#pragma once

#include <string>
#include <vector>

#include "DetectedGame.h"
#include "TextMatch.h"

namespace signalbox::detection {

struct IndexExclusions {
	// Exact-ish platform keys, formatted "<platform>:<platformId>" and
	// matched with '*' globs - e.g. "steam:431960" (Wallpaper Engine),
	// "steam:228980" (Steamworks Common Redistributables).
	//
	// This is the PREFERRED way to exclude something. A Steam appid is
	// stable forever; a display name is not. "Opera GX Stable
	// 134.0.5954.44" carries a version that changes on every update,
	// which is exactly the kind of rule that silently stops working two
	// weeks after it was written.
	std::vector<std::wstring> platformIds;

	// Glob patterns against the platform's display name, for entries
	// with no stable id to key on - most of the Uninstall-registry
	// catch-alls. Case-insensitive. Use a trailing '*' for anything
	// that carries a version number.
	std::vector<std::wstring> displayNames;

	// Case-insensitive substrings of the install root. The blunt
	// instrument, for whole trees that are never a game no matter what
	// enumerated them (e.g. "\windows\system32\\").
	std::vector<std::wstring> installRootFragments;

	bool empty() const
	{
		return platformIds.empty() && displayNames.empty() && installRootFragments.empty();
	}

	// True if this entry should never enter the index.
	bool excludes(const InstalledGame &game) const
	{
		if (!platformIds.empty() && !game.platformId.empty()) {
			const std::wstring key =
				text::ToLower(std::wstring(PlatformToString(game.platform)) + L":" + game.platformId);
			for (const auto &pattern : platformIds) {
				if (text::WildcardMatch(text::ToLower(pattern), key)) {
					return true;
				}
			}
		}

		if (!displayNames.empty() && !game.displayName.empty()) {
			const std::wstring name = text::ToLower(game.displayName);
			for (const auto &pattern : displayNames) {
				if (text::WildcardMatch(text::ToLower(pattern), name)) {
					return true;
				}
			}
		}

		if (!installRootFragments.empty() && !game.installRoot.empty()) {
			const std::wstring root = text::ToLower(game.installRoot);
			for (const auto &fragment : installRootFragments) {
				if (root.find(text::ToLower(fragment)) != std::wstring::npos) {
					return true;
				}
			}
		}

		return false;
	}
};

} // namespace signalbox::detection
