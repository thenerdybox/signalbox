/*
 * SignalBox - detection/providers/UbisoftProvider.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Real implementation plan (DESIGN.md 1.2 "Ubisoft Connect provider"):
 *   HKLM\SOFTWARE\WOW6432Node\Ubisoft\Launcher\Installs\<installId>
 *   value InstallDir -> installRoot only; no name, no exe in the
 *   registry. displayName falls back to the last path component of
 *   InstallDir (e.g. ...\games\Anno 1800\ -> "Anno 1800"). Confidence
 *   MEDIUM in InstallIndex::match (DESIGN.md 1.4 step 2) - the
 *   user-override tier (CategoryResolver, DESIGN.md 1.5 tier 1) is the
 *   intended backstop for misses here.
 */

#pragma once

#include "../IGameProvider.h"

namespace signalbox::detection::providers {

class UbisoftProvider final : public IGameProvider {
public:
	std::vector<InstalledGame> enumerate() override;
	std::wstring providerId() const override;
};

} // namespace signalbox::detection::providers
