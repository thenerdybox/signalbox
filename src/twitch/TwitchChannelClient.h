/*
 * SignalBox - twitch/TwitchChannelClient.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * The concrete core::IChannelClient adapter CategorySwitchCoordinator.h's
 * class doc comment describes - wraps twitch::TwitchClient's async,
 * signal-based getChannelInfo()/setChannelCategory()/createStreamMarker()
 * behind IChannelClient's callback interface, and mirrors TwitchClient's
 * own 401 handling (TwitchClient.h's "401 HANDLING" section) as a plain
 * reauthRequired() bool the coordinator can check before every call.
 *
 * ORDERING ASSUMPTION: same as TwitchCategoryLookup - channelInfoReady()/
 * categoryChangeSucceeded()/streamMarkerCreated()/requestFailed() do not
 * carry a request id, so each call kind is answered FIFO in issue order.
 * CategorySwitchCoordinator never has more than one getChannelInfo() or
 * setChannelCategory() outstanding at a time per switch (it awaits each
 * step before starting the next - see its class doc comment's numbered
 * sequence), so this holds in practice; documented rather than assumed.
 *
 * requestFailed() IS SHARED ACROSS EVERY TwitchClient CALL KIND (see
 * TwitchClient.h - one signal, a `context` string names which call
 * failed). This adapter only routes a requestFailed() to the matching
 * pending queue when its `context` names a call kind this adapter
 * actually issues, so a getChannelInfo() failure can never be mistaken
 * for (or accidentally answer) a setChannelCategory() failure.
 *
 * REAUTH LATCH LIFECYCLE: reauthRequired_ is set true by TwitchClient's
 * reauthRequired signal (a 401) and is NOT self-clearing - TwitchClient's
 * own frozen_ flag is cleared by updateAccessToken(), but that is a
 * different object's state entirely, so something must explicitly tell
 * this adapter the freeze is over. clearReauthRequired() is that signal:
 * the owning caller (ui::CategoryDock) must call it once, right after
 * the same TwitchClient::updateAccessToken() call that unfreezes the
 * underlying client - see CategoryDock.cpp's TwitchAuth::tokensRefreshed
 * handler. Without this, every switch after the FIRST 401 of a session
 * is refused forever with "Twitch needs to be reconnected", even though
 * TwitchClient itself resumed and the dock shows "Connected".
 *
 * WHY setChannelCategory() PRE-CHECKS lastKnownGameId(): TwitchClient's
 * own redundancy guard silently no-ops (no signal at all - see
 * TwitchClient.h) when the requested category already matches. Without
 * pre-empting that here, a redundant call would leave this adapter's
 * callback permanently unanswered. See TwitchClient::lastKnownGameId()'s
 * doc comment - added specifically to make this adapter possible.
 *
 * THREADING: Qt main thread only, same as TwitchClient.
 */

#pragma once

#include <deque>
#include <functional>
#include <optional>

#include <QObject>

#include "../core/CategorySwitchCoordinator.h"

namespace signalbox::twitch {

class TwitchClient;

class TwitchChannelClient : public QObject, public core::IChannelClient {
	Q_OBJECT

public:
	// client must outlive this object - not owned, same pattern as
	// TwitchCategoryLookup.
	explicit TwitchChannelClient(TwitchClient &client, QObject *parent = nullptr);
	~TwitchChannelClient() override;

	bool reauthRequired() const override;

	// Clears the latch set by TwitchClient::reauthRequired() - see class
	// doc comment's "REAUTH LATCH LIFECYCLE" note. The caller is
	// responsible for only calling this once the underlying TwitchClient
	// has actually been unfrozen (i.e. right after
	// TwitchClient::updateAccessToken() with a fresh token); calling it
	// any earlier would let the coordinator attempt calls TwitchClient
	// itself would still refuse.
	void clearReauthRequired();

	void getChannelInfo(std::function<void(std::optional<core::ChannelSnapshot>)> onResult) override;
	void setChannelCategory(const std::wstring &categoryId, const std::wstring &categoryName,
				 std::function<void(bool, std::wstring)> onResult) override;
	void createStreamMarker(const std::wstring &description, std::function<void(bool)> onResult) override;

private:
	TwitchClient &client_;
	bool reauthRequired_ = false;

	std::deque<std::function<void(std::optional<core::ChannelSnapshot>)>> pendingChannelInfo_;
	std::deque<std::function<void(bool, std::wstring)>> pendingSetCategory_;
	std::deque<std::function<void(bool)>> pendingMarker_;
};

} // namespace signalbox::twitch
