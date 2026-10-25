/*
 * SignalBox - src/core/DetectionFilter.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * "Never switch for this app" has to mean the app is not treated as the
 * detected game at all, not merely that its category switch is skipped.
 * Before this existed the dock still listed an ignored app as
 * "Detected: ...": the poll result went to the state machine untouched and
 * only the final category switch was refused, so the dock said one thing and
 * the activity log said "Ignoring ..." about it.
 *
 * Pure and Qt/OBS free so the harness can exercise it.
 */

#pragma once

#include <optional>
#include <utility>

#include "CategoryResolver.h"
#include "UserOverrideStore.h"
#include "../detection/DetectedGame.h"

namespace signalbox::core {

inline bool IsNeverSwitchApp(const IUserOverrideStore &overrides, const detection::InstalledGame &game)
{
	const auto override = overrides.findUserOverride(CategoryResolver::overrideKeyFor(game));
	return override && override->ignored;
}

// What the rest of the plugin should see for this poll: the detected game, or
// nothing if the user told SignalBox never to switch for it.
inline std::optional<detection::DetectedGame> WithoutNeverSwitchApps(std::optional<detection::DetectedGame> detected,
								     const IUserOverrideStore &overrides)
{
	if (detected && IsNeverSwitchApp(overrides, detected->game))
		return std::nullopt;
	return detected;
}

} // namespace signalbox::core
