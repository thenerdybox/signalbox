/*
 * SignalBox - twitch/TwitchCategoryLookup.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * See TwitchCategoryLookup.h for the FIFO ordering assumption this relies
 * on.
 */

#include "TwitchCategoryLookup.h"

#include "TwitchClient.h"

namespace signalbox::twitch {

TwitchCategoryLookup::TwitchCategoryLookup(TwitchClient &client, QObject *parent) : QObject(parent), client_(client)
{
	connect(&client_, &TwitchClient::categoryFound, this, [this](std::optional<CategoryMatch> match) {
		if (pendingExact_.empty()) {
			return; // Stray/late signal - nothing waiting on it.
		}
		auto callback = std::move(pendingExact_.front());
		pendingExact_.pop_front();

		if (!match) {
			callback(std::nullopt);
			return;
		}
		callback(core::ResolvedCategory{match->gameId.toStdWString(), match->gameName.toStdWString()});
	});

	connect(&client_, &TwitchClient::categorySearchResults, this, [this](QVector<CategoryMatch> matches) {
		if (pendingSearch_.empty()) {
			return;
		}
		auto callback = std::move(pendingSearch_.front());
		pendingSearch_.pop_front();

		std::vector<core::ResolvedCategory> results;
		results.reserve(static_cast<std::size_t>(matches.size()));
		for (const CategoryMatch &match : matches) {
			results.push_back(core::ResolvedCategory{match.gameId.toStdWString(), match.gameName.toStdWString()});
		}
		callback(std::move(results));
	});

	// findCategoryByExactName()/searchCategories() only ever fail via
	// requestFailed() when Twitch itself is unreachable (frozen/backoff)
	// or a transport error - a plain "no match" is a normal, successful
	// categoryFound(nullopt)/categorySearchResults({}) per TwitchClient's
	// own doc comment, not a failure. Treat a genuine requestFailed() the
	// same as "no match" here rather than leaving the caller's callback
	// unanswered - CategoryResolver's contract is "unmapped means no
	// action" either way (CategoryResolver.h's class doc comment), so
	// this collapses cleanly onto the exact same safe outcome.
	connect(&client_, &TwitchClient::requestFailed, this, [this](QString context, QString /*reason*/) {
		if (context == QStringLiteral("findCategoryByExactName")) {
			if (pendingExact_.empty()) {
				return;
			}
			auto callback = std::move(pendingExact_.front());
			pendingExact_.pop_front();
			callback(std::nullopt);
		} else if (context == QStringLiteral("searchCategories")) {
			if (pendingSearch_.empty()) {
				return;
			}
			auto callback = std::move(pendingSearch_.front());
			pendingSearch_.pop_front();
			callback({});
		}
	});
}

TwitchCategoryLookup::~TwitchCategoryLookup() = default;

void TwitchCategoryLookup::findExact(const std::wstring &name,
				      std::function<void(std::optional<core::ResolvedCategory>)> onResult)
{
	pendingExact_.push_back(std::move(onResult));
	client_.findCategoryByExactName(QString::fromStdWString(name));
}

void TwitchCategoryLookup::search(const std::wstring &query, std::function<void(std::vector<core::ResolvedCategory>)> onResult)
{
	pendingSearch_.push_back(std::move(onResult));
	client_.searchCategories(QString::fromStdWString(query));
}

} // namespace signalbox::twitch
