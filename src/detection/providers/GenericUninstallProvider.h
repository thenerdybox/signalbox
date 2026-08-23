/*
 * SignalBox - detection/providers/GenericUninstallProvider.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Real implementation plan (DESIGN.md 1.2 "Generic Uninstall provider"):
 *   Enumerate HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*,
 *   its WOW6432Node twin, and the HKCU equivalents. Use DisplayName and
 *   InstallLocation (Microsoft Learn: "Uninstall Registry Key"). Include
 *   an entry only if InstallLocation is non-empty and exists on disk.
 *
 *   LOW priority / LOW confidence by design (DESIGN.md 1.4 step 2) - this
 *   is the catch-all for EA app, Battle.net, and one-off installers that
 *   don't have their own provider. No Publisher/DisplayName denylist
 *   filtering is attempted here on purpose: matching is driven by which
 *   running process's path falls under an indexed installRoot, so a
 *   Visual C++ Redistributable entry sitting in the index is harmless -
 *   nothing ever runs from its InstallLocation (DESIGN.md 1.2).
 */

#pragma once

#include "../IGameProvider.h"

namespace signalbox::detection::providers {

class GenericUninstallProvider final : public IGameProvider {
public:
	std::vector<InstalledGame> enumerate() override;
	std::wstring providerId() const override;
};

} // namespace signalbox::detection::providers
