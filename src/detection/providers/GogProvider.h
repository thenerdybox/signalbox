/*
 * SignalBox - detection/providers/GogProvider.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Real implementation plan (DESIGN.md 1.2 "GOG provider"):
 *   Enumerate HKLM\SOFTWARE\WOW6432Node\GOG.com\Games\<productId>
 *   (and the 64-bit view without WOW6432Node - enumerate both with
 *   KEY_WOW64_32KEY / KEY_WOW64_64KEY). Per game key:
 *     gameName -> displayName
 *     path     -> installRoot
 *     exe      -> launchExe (full path, exact-match like Epic)
 */

#pragma once

#include "../IGameProvider.h"

namespace signalbox::detection::providers {

class GogProvider final : public IGameProvider {
public:
	std::vector<InstalledGame> enumerate() override;
	std::wstring providerId() const override;
};

} // namespace signalbox::detection::providers
