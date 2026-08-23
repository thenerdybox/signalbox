/*
 * SignalBox - twitch/TokenStore.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Persists {access_token, refresh_token, user_id, login, obtained_at,
 * expires_in} to tokens.bin under the plugin config directory, encrypted
 * with DPAPI (CryptProtectData with CRYPTPROTECT_UI_FORBIDDEN - Microsoft
 * Learn), machine+user bound, no key management (DESIGN.md 3.3).
 *
 * NEVER in config.json (see PluginConfig.h), never anywhere a scene
 * collection export could carry it. This class owns the ONLY code path
 * in the plugin allowed to read or write tokens.bin.
 *
 * THREADING: file + DPAPI I/O is synchronous but local and fast (single
 * small file). Intended to be called from the Qt main thread alongside
 * TwitchAuth/TwitchClient - not performance-sensitive enough to need a
 * worker-thread hop, and doing so would just add cross-thread complexity
 * for no benefit. Do not call from DetectionEngine's worker thread
 * regardless (see DetectionEngine.h's threading boundary) - tokens have
 * nothing to do with detection.
 */

#pragma once

#include <optional>
#include <string>

namespace signalbox::twitch {

struct StoredTokens {
	std::wstring accessToken;
	std::wstring refreshToken;
	std::wstring userId;
	std::wstring login;
	long long obtainedAtUnixS = 0;
	int expiresInS = 0;
};

class TokenStore {
public:
	TokenStore();
	~TokenStore();

	// Reads and DPAPI-decrypts tokens.bin. std::nullopt if the file
	// doesn't exist or fails to decrypt (e.g. moved to a different
	// machine/user - DPAPI blobs don't travel). Caller's response to a
	// nullopt is "show the DCG re-auth button", never an error dialog.
	std::optional<StoredTokens> load() const;

	// DPAPI-encrypts and writes tokens.bin, replacing any existing
	// contents. Called after every successful auth or refresh.
	void save(const StoredTokens &tokens) const;

	// Removes tokens.bin (explicit "disconnect Twitch account" action).
	void clear() const;

private:
	std::wstring path_; // obs_module_config_path()-derived; set in .cpp.
};

} // namespace signalbox::twitch
