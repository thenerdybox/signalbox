/*
 * SignalBox - twitch/TwitchChannelClient.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * See TwitchChannelClient.h for the FIFO ordering assumption, the
 * requestFailed() routing rule, and why setChannelCategory() pre-checks
 * lastKnownGameId().
 */

#include "TwitchChannelClient.h"

#include "TwitchClient.h"

namespace signalbox::twitch {

TwitchChannelClient::TwitchChannelClient(TwitchClient &client, QObject *parent) : QObject(parent), client_(client)
{
	connect(&client_, &TwitchClient::reauthRequired, this, [this]() {
		reauthRequired_ = true;
		// Every call outstanding at the moment of a freeze will never
		// get its matching success/failure signal (TwitchClient stops
		// issuing the request entirely once frozen - see its 401
		// HANDLING note) - answer them all now with a failure rather
		// than leaving callers waiting forever.
		while (!pendingChannelInfo_.empty()) {
			auto callback = std::move(pendingChannelInfo_.front());
			pendingChannelInfo_.pop_front();
			callback(std::nullopt);
		}
		while (!pendingSetCategory_.empty()) {
			auto callback = std::move(pendingSetCategory_.front());
			pendingSetCategory_.pop_front();
			callback(false, QStringLiteral("Twitch reconnect required.").toStdWString());
		}
		while (!pendingMarker_.empty()) {
			auto callback = std::move(pendingMarker_.front());
			pendingMarker_.pop_front();
			callback(false);
		}
	});

	connect(&client_, &TwitchClient::channelInfoReady, this, [this](ChannelInfo info) {
		if (pendingChannelInfo_.empty()) {
			return;
		}
		auto callback = std::move(pendingChannelInfo_.front());
		pendingChannelInfo_.pop_front();
		callback(core::ChannelSnapshot{info.gameId.toStdWString(), info.gameName.toStdWString()});
	});

	connect(&client_, &TwitchClient::categoryChangeSucceeded, this, [this](CategoryMatch /*category*/) {
		if (pendingSetCategory_.empty()) {
			return;
		}
		auto callback = std::move(pendingSetCategory_.front());
		pendingSetCategory_.pop_front();
		callback(true, std::wstring());
	});

	connect(&client_, &TwitchClient::streamMarkerCreated, this, [this]() {
		if (pendingMarker_.empty()) {
			return;
		}
		auto callback = std::move(pendingMarker_.front());
		pendingMarker_.pop_front();
		callback(true);
	});

	connect(&client_, &TwitchClient::requestFailed, this, [this](QString context, QString reason) {
		if (context == QStringLiteral("getChannelInfo")) {
			if (pendingChannelInfo_.empty()) {
				return;
			}
			auto callback = std::move(pendingChannelInfo_.front());
			pendingChannelInfo_.pop_front();
			callback(std::nullopt);
		} else if (context == QStringLiteral("setChannelCategory")) {
			if (pendingSetCategory_.empty()) {
				return;
			}
			auto callback = std::move(pendingSetCategory_.front());
			pendingSetCategory_.pop_front();
			callback(false, reason.toStdWString());
		} else if (context == QStringLiteral("createStreamMarker")) {
			if (pendingMarker_.empty()) {
				return;
			}
			auto callback = std::move(pendingMarker_.front());
			pendingMarker_.pop_front();
			callback(false);
		}
	});
}

TwitchChannelClient::~TwitchChannelClient() = default;

bool TwitchChannelClient::reauthRequired() const
{
	return reauthRequired_;
}

void TwitchChannelClient::clearReauthRequired()
{
	reauthRequired_ = false;
}

void TwitchChannelClient::getChannelInfo(std::function<void(std::optional<core::ChannelSnapshot>)> onResult)
{
	if (reauthRequired_) {
		onResult(std::nullopt);
		return;
	}
	pendingChannelInfo_.push_back(std::move(onResult));
	client_.getChannelInfo();
}

void TwitchChannelClient::setChannelCategory(const std::wstring &categoryId, const std::wstring &categoryName,
					      std::function<void(bool, std::wstring)> onResult)
{
	if (reauthRequired_) {
		onResult(false, QStringLiteral("Twitch reconnect required.").toStdWString());
		return;
	}

	const QString qCategoryId = QString::fromStdWString(categoryId);

	// Pre-empt TwitchClient's own redundancy guard - see class doc
	// comment. Reports the same success outcome the caller would have
	// seen anyway, just without waiting on a signal that will never come.
	if (const auto known = client_.lastKnownGameId(); known && *known == qCategoryId) {
		onResult(true, std::wstring());
		return;
	}

	pendingSetCategory_.push_back(std::move(onResult));

	CategoryMatch match;
	match.gameId = qCategoryId;
	match.gameName = QString::fromStdWString(categoryName);
	client_.setChannelCategory(match);
}

void TwitchChannelClient::createStreamMarker(const std::wstring &description, std::function<void(bool)> onResult)
{
	if (reauthRequired_) {
		onResult(false);
		return;
	}
	pendingMarker_.push_back(std::move(onResult));
	client_.createStreamMarker(QString::fromStdWString(description));
}

} // namespace signalbox::twitch
