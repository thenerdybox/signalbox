/*
 * SignalBox - core/UserOverrideStore.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * DESIGN.md 1.5 tier 1: the highest-priority, persistent per-game category
 * override a streamer can set from the dock ("this game is always X", or
 * "ignore this exe entirely"). This tiny header exists purely to give
 * CategoryResolver something to depend on other than the concrete
 * PluginConfig class - PluginConfig (the real, obs_data-backed
 * implementation) needs libobs to load/save; CategoryResolver itself must
 * not, so it can be exercised by a standalone test harness without an OBS
 * process, the same way DetectionStateMachine already is. See
 * CategoryResolver.h's class doc comment.
 */

#pragma once

#include <optional>
#include <string>

namespace signalbox::core {

// One persisted override, keyed by CategoryResolver::overrideKeyFor()'s
// "platform:platformId" (preferred) or absolute exe path. ignored=true is
// the "ignore this exe entirely" case from DESIGN.md 1.5 - it resolves to
// nothing, exactly like an unmapped game, rather than being a distinct
// third outcome callers need to special-case.
struct UserOverride {
	std::wstring categoryId;
	std::wstring categoryName;
	bool ignored = false;
};

// Read-only view CategoryResolver needs from whatever owns the persisted
// override map. PluginConfig is the real implementation (see
// PluginConfig.h); a harness can supply a trivial in-memory stand-in.
class IUserOverrideStore {
public:
	virtual ~IUserOverrideStore() = default;

	virtual std::optional<UserOverride> findUserOverride(const std::wstring &key) const = 0;
};

} // namespace signalbox::core
