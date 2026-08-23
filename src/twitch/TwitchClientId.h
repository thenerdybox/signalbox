/*
 * SignalBox - twitch/TwitchClientId.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * The Client ID of the Twitch application this plugin authenticates as.
 *
 * This is not a secret. Client IDs are public by design - they identify the
 * application, they do not authorise it, and every desktop client that talks
 * to Twitch ships one. What must never appear here is a client SECRET, and
 * there is none: the Device Code Grant flow this plugin uses (DESIGN.md 3.1)
 * is a public-client flow specifically so that no secret has to exist. If the
 * Twitch console ever hands out a secret for this application, it was
 * registered as Confidential by mistake - nothing here needs it.
 *
 * The registered redirect URL is a formality. Device Code Grant never
 * redirects anywhere, so whatever placeholder the console accepted is never
 * contacted.
 *
 * This constant is the only place the client ID lives. Do not duplicate it.
 */

#pragma once

#include <string_view>

namespace signalbox::twitch {

inline constexpr std::string_view kTwitchClientId = "fzliieirwntdfbfr8gzctgssjrpghp";

// Read the client ID through this accessor rather than the constant, so a
// future revision can source it from somewhere else - a build-time define,
// say - without touching every call site.
inline constexpr std::string_view twitchClientId()
{
	return kTwitchClientId;
}

} // namespace signalbox::twitch
