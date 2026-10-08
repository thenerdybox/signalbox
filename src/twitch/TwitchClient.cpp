/*
 * SignalBox - twitch/TwitchClient.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Implements the Helix surface described in TwitchClient.h, verified
 * field-for-field against Twitch's own API reference
 * (dev.twitch.tv/docs/api/reference) rather than assumed - see the
 * per-endpoint comments below for exact field names.
 *
 * PERFORMANCE: see TwitchClient.h's class doc comment for the full
 * rationale. Summary of what this file actually does about it:
 *   - setChannelCategory() checks lastKnownGameId_ before touching the
 *     network at all (redundancy guard).
 *   - findCategoryByExactName()/searchCategories() check the in-memory
 *     cache (loaded from category-cache.json at construction) before
 *     touching the network, and persist every new positive resolution
 *     immediately.
 *   - No QTimer here ever fires on its own ambient schedule - patchTimer_
 *     only ever fires because something is actually queued and waiting
 *     out either the minPatchSpacingS_ floor or a backoff window, never
 *     as a "check on things" poll.
 */

#include "TwitchClient.h"

// obs-module.h (and the util/ headers it pulls in) must be included
// before plugin-support.h in any TU that needs both - see plugin-main.cpp
// for the same order. libobs' util/base.h declares blogva() with
// __declspec(dllexport); plugin-support.h re-declares it plainly (it's a
// consumer, not the exporter). MSVC accepts a plain re-declaration after
// the dllexport one but not the reverse (C2375 "different linkage").
#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include "../plugin-support.h"

#include <QDateTime>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <climits>

namespace signalbox::twitch {

namespace {

const QString kHelixBaseUrl = QStringLiteral("https://api.twitch.tv/helix");

// Exponential backoff ceiling for 429s / transport errors that don't
// carry a usable Ratelimit-Reset - never wait longer than this before
// trying again, and never less than 1s (owner directive: no tight retry
// loops, but also don't strand the user for an unbounded time).
constexpr qint64 kMinBackoffMs = 1000;
constexpr qint64 kMaxBackoffMs = 5 * 60 * 1000;

} // namespace

TwitchClient::TwitchClient(QString clientId, QString accessToken, QString broadcasterId,
			    std::uint32_t minPatchSpacingS, QObject *parent)
	: QObject(parent),
	  clientId_(std::move(clientId)),
	  accessToken_(std::move(accessToken)),
	  broadcasterId_(std::move(broadcasterId)),
	  transport_(std::make_unique<HttpTransport>(this)),
	  minPatchSpacingS_(minPatchSpacingS)
{
	patchTimer_ = new QTimer(this);
	patchTimer_->setSingleShot(true);
	connect(patchTimer_, &QTimer::timeout, this, &TwitchClient::schedulePatch);

	loadCategoryCache();
}

TwitchClient::~TwitchClient() = default;

void TwitchClient::updateAccessToken(QString accessToken)
{
	accessToken_ = std::move(accessToken);
	// A fresh token is the "reconnected" signal - unfreeze and let any
	// write that was waiting on this go out. This is what closes the
	// loop for the class doc comment's 401 HANDLING contract: the
	// coordinator only has to call this once refresh succeeds, it does
	// not have to remember to manually resubmit setChannelCategory().
	frozen_ = false;
	schedulePatch();
}

HttpRequest TwitchClient::buildHelixRequest(HttpMethod method, const QUrl &url) const
{
	HttpRequest request;
	request.method = method;
	request.url = url.toString(QUrl::FullyEncoded);
	request.headers.append(HttpHeader{"Authorization", QByteArray("Bearer ") + accessToken_.toUtf8()});
	request.headers.append(HttpHeader{"Client-Id", clientId_.toUtf8()});
	return request;
}

TwitchClient::HelixReplyOutcome TwitchClient::consumeResponse(const HttpResponse &response)
{
	HelixReplyOutcome outcome;
	outcome.body = response.body;
	outcome.httpStatus = response.httpStatus;

	// header() matches case-insensitively (see HttpTransport.h), so this
	// is robust to whatever casing Twitch actually sends.
	const QByteArray resetHeader = response.header("Ratelimit-Reset");
	if (!resetHeader.isEmpty())
		outcome.rateLimitResetUnixS = resetHeader.toLongLong();

	if (!response.transportError && outcome.httpStatus >= 200 && outcome.httpStatus < 300) {
		outcome.ok = true;
	} else if (outcome.httpStatus == 401) {
		outcome.unauthorized = true;
	} else if (outcome.httpStatus == 429) {
		outcome.rateLimited = true;
	} else if (response.transportError || outcome.httpStatus == 0) {
		// No HTTP status at all means the request never got a
		// response - DNS/connection failure, timeout, offline.
		outcome.transportError = true;
	}

	return outcome;
}

QString TwitchClient::humanReadableFailure(const HelixReplyOutcome &outcome)
{
	if (outcome.transportError)
		return QStringLiteral("Could not reach Twitch - check your internet connection.");
	if (outcome.rateLimited)
		return QStringLiteral("Twitch rate limit reached - will retry automatically.");

	const QJsonDocument doc = QJsonDocument::fromJson(outcome.body);
	const QString message = doc.object().value(QStringLiteral("message")).toString();
	if (!message.isEmpty())
		return QStringLiteral("Twitch request failed (%1): %2").arg(outcome.httpStatus).arg(message);
	return QStringLiteral("Twitch request failed (HTTP %1).").arg(outcome.httpStatus);
}

void TwitchClient::noteRequestOutcome(const HelixReplyOutcome &outcome)
{
	if (outcome.ok) {
		consecutiveFailures_ = 0;
		return;
	}
	if (!outcome.rateLimited && !outcome.transportError)
		return; // e.g. a plain 400/404 - not a capacity problem, no backoff warranted.

	++consecutiveFailures_;
	const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
	qint64 backoffMs;

	if (outcome.rateLimited && outcome.rateLimitResetUnixS > 0) {
		// Authoritative - Twitch told us exactly when the bucket refills.
		backoffMs = outcome.rateLimitResetUnixS * 1000 - nowMs;
	} else {
		// No usable header (transport error, or a 429 without the
		// header for some reason) - exponential: 1s, 2s, 4s, 8s...
		// Owner directive: never a tight retry loop against Twitch.
		const int shift = std::min(consecutiveFailures_ - 1, 8);
		backoffMs = kMinBackoffMs << shift;
	}
	backoffMs = std::clamp<qint64>(backoffMs, kMinBackoffMs, kMaxBackoffMs);
	backoffUntilUnixMs_ = nowMs + backoffMs;

	obs_log(LOG_WARNING, "twitch: backing off %lld ms after a %s (consecutive failures: %d)",
		static_cast<long long>(backoffMs), outcome.rateLimited ? "429" : "transport error",
		consecutiveFailures_);
}

bool TwitchClient::gatedByBackoff() const
{
	return QDateTime::currentMSecsSinceEpoch() < backoffUntilUnixMs_;
}

void TwitchClient::freeze()
{
	if (frozen_)
		return; // Already frozen - reauthRequired() already emitted once for this episode; never spam it.
	frozen_ = true;
	obs_log(LOG_WARNING, "twitch: 401 received - freezing automation until reconnected");
	emit reauthRequired();
}

// ---------------------------------------------------------------------
// GET /helix/users
// ---------------------------------------------------------------------
void TwitchClient::resolveCurrentUser()
{
	if (frozen_) {
		emit requestFailed(QStringLiteral("resolveCurrentUser"), QStringLiteral("Twitch reconnect required."));
		return;
	}
	if (gatedByBackoff()) {
		emit requestFailed(QStringLiteral("resolveCurrentUser"),
				    QStringLiteral("Temporarily rate-limited by Twitch."));
		return;
	}

	transport_->send(buildHelixRequest(HttpMethod::Get, QUrl(kHelixBaseUrl + QStringLiteral("/users"))),
			  [this](HttpResponse response) {
				  const HelixReplyOutcome outcome = consumeResponse(response);
				  noteRequestOutcome(outcome);
				  if (outcome.unauthorized) {
					  freeze();
					  return;
				  }
				  if (!outcome.ok) {
					  emit requestFailed(QStringLiteral("resolveCurrentUser"),
							      humanReadableFailure(outcome));
					  return;
				  }

				  const QJsonArray data = QJsonDocument::fromJson(outcome.body)
								   .object()
								   .value(QStringLiteral("data"))
								   .toArray();
				  if (data.isEmpty()) {
					  emit requestFailed(QStringLiteral("resolveCurrentUser"),
							      QStringLiteral("Twitch returned no user for this token."));
					  return;
				  }

				  const QJsonObject user = data.first().toObject();
				  broadcasterId_ = user.value(QStringLiteral("id")).toString();
				  const QString login = user.value(QStringLiteral("login")).toString();
				  emit currentUserResolved(broadcasterId_, login);
			  });
}

// ---------------------------------------------------------------------
// GET /helix/channels
// ---------------------------------------------------------------------
void TwitchClient::getChannelInfo()
{
	if (frozen_) {
		emit requestFailed(QStringLiteral("getChannelInfo"), QStringLiteral("Twitch reconnect required."));
		return;
	}
	if (broadcasterId_.isEmpty()) {
		emit requestFailed(QStringLiteral("getChannelInfo"), QStringLiteral("Broadcaster ID not resolved yet."));
		return;
	}
	if (gatedByBackoff()) {
		emit requestFailed(QStringLiteral("getChannelInfo"), QStringLiteral("Temporarily rate-limited by Twitch."));
		return;
	}

	QUrl url(kHelixBaseUrl + QStringLiteral("/channels"));
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("broadcaster_id"), broadcasterId_);
	url.setQuery(query);

	transport_->send(buildHelixRequest(HttpMethod::Get, url), [this](HttpResponse response) {
		const HelixReplyOutcome outcome = consumeResponse(response);
		noteRequestOutcome(outcome);
		if (outcome.unauthorized) {
			freeze();
			return;
		}
		if (!outcome.ok) {
			emit requestFailed(QStringLiteral("getChannelInfo"), humanReadableFailure(outcome));
			return;
		}

		const QJsonArray data = QJsonDocument::fromJson(outcome.body).object().value(QStringLiteral("data")).toArray();
		if (data.isEmpty()) {
			emit requestFailed(QStringLiteral("getChannelInfo"), QStringLiteral("Twitch returned no channel data."));
			return;
		}

		const QJsonObject ch = data.first().toObject();
		ChannelInfo info;
		info.broadcasterId = ch.value(QStringLiteral("broadcaster_id")).toString();
		info.gameId = ch.value(QStringLiteral("game_id")).toString();
		info.gameName = ch.value(QStringLiteral("game_name")).toString();
		info.title = ch.value(QStringLiteral("title")).toString();

		// Redundancy-guard cache refresh - this is what makes the
		// guard correct even before this client has ever issued a
		// PATCH itself (e.g. right after connecting mid-stream).
		lastKnownGameId_ = info.gameId;

		emit channelInfoReady(info);
	});
}

// ---------------------------------------------------------------------
// PATCH /helix/channels - write queue, pacing, redundancy guard
// ---------------------------------------------------------------------
void TwitchClient::setChannelCategory(const CategoryMatch &category)
{
	// Structural backstop (see class doc comment's "STRUCTURAL SAFETY"
	// note): per Twitch's Modify Channel Information docs, PATCHing an
	// empty game_id UNSETS the live category - the exact harm "unmapped
	// means no action" exists to prevent. Every caller (see
	// core::CategorySwitchCoordinator) is already responsible for never
	// reaching this with an empty id, but that must be true by
	// construction here too, not only by call-site discipline - refuse
	// unconditionally, before the frozen/broadcaster-id checks, so no
	// future caller can ever slip one through.
	if (category.gameId.isEmpty()) {
		obs_log(LOG_WARNING, "twitch: refusing to PATCH an empty game_id (would clear the live category)");
		emit requestFailed(QStringLiteral("setChannelCategory"),
				    QStringLiteral("Refused to clear the category (empty game_id)."));
		return;
	}
	if (frozen_) {
		emit requestFailed(QStringLiteral("setChannelCategory"), QStringLiteral("Twitch reconnect required."));
		return;
	}
	if (broadcasterId_.isEmpty()) {
		emit requestFailed(QStringLiteral("setChannelCategory"), QStringLiteral("Broadcaster ID not resolved yet."));
		return;
	}

	// Redundancy guard - zero network calls if this is already the set
	// category. This check is intentionally the very first thing that
	// happens, before the category is even queued.
	if (lastKnownGameId_.has_value() && *lastKnownGameId_ == category.gameId) {
		obs_log(LOG_DEBUG, "twitch: setChannelCategory no-op, category already %s",
			qUtf8Printable(category.gameId));
		return;
	}

	// Latest request wins - if several detection events fire before the
	// pacing floor clears, only the most recent category is ever sent;
	// stale intermediate ones are simply overwritten, never queued up
	// as separate PATCHes.
	pendingCategory_ = category;
	schedulePatch();
}

void TwitchClient::schedulePatch()
{
	if (frozen_ || patchInFlight_ || !pendingCategory_.has_value())
		return;

	const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
	qint64 earliestMs = lastPatchAtUnixMs_ == 0 ? nowMs
						     : lastPatchAtUnixMs_ + static_cast<qint64>(minPatchSpacingS_) * 1000;
	if (backoffUntilUnixMs_ > earliestMs)
		earliestMs = backoffUntilUnixMs_;

	if (earliestMs <= nowMs) {
		executePendingPatch();
	} else {
		patchTimer_->start(static_cast<int>(std::min<qint64>(earliestMs - nowMs, INT_MAX)));
	}
}

void TwitchClient::executePendingPatch()
{
	if (frozen_ || patchInFlight_ || !pendingCategory_.has_value())
		return;

	const CategoryMatch category = *pendingCategory_;
	pendingCategory_.reset();

	// Belt-and-braces re-check of the empty-game_id backstop
	// (setChannelCategory() already refuses this at the entry point,
	// but this is the literal place the PATCH body gets built - see
	// this class's doc comment's "STRUCTURAL SAFETY" note). Should be
	// unreachable; never silently proceed if it somehow isn't.
	if (category.gameId.isEmpty()) {
		obs_log(LOG_WARNING, "twitch: dropping a queued PATCH with an empty game_id (should be unreachable)");
		return;
	}

	// Re-check the guard right before sending - cheap, and closes a
	// narrow window where lastKnownGameId_ changed while this was queued.
	if (lastKnownGameId_.has_value() && *lastKnownGameId_ == category.gameId) {
		obs_log(LOG_DEBUG, "twitch: dropping now-redundant queued PATCH for %s",
			qUtf8Printable(category.gameId));
		return;
	}

	patchInFlight_ = true;
	lastPatchAtUnixMs_ = QDateTime::currentMSecsSinceEpoch();

	QUrl url(kHelixBaseUrl + QStringLiteral("/channels"));
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("broadcaster_id"), broadcasterId_);
	url.setQuery(query);

	HttpRequest request = buildHelixRequest(HttpMethod::Patch, url);
	request.headers.append(HttpHeader{"Content-Type", "application/json"});

	QJsonObject body;
	body[QStringLiteral("game_id")] = category.gameId;
	request.body = QJsonDocument(body).toJson(QJsonDocument::Compact);

	transport_->send(request, [this, category](HttpResponse response) {
		const HelixReplyOutcome outcome = consumeResponse(response);
		patchInFlight_ = false;
		noteRequestOutcome(outcome);

		if (outcome.ok) {
			// 204 No Content on success (verified against Twitch's
			// docs) - no body to parse, the request having
			// succeeded IS the information.
			lastKnownGameId_ = category.gameId;
			emit categoryChangeSucceeded(category);
		} else if (outcome.unauthorized) {
			freeze();
			// Put it back so updateAccessToken() automatically
			// resumes this exact change once reconnected - see
			// class doc comment's 401 HANDLING.
			pendingCategory_ = category;
		} else {
			emit requestFailed(QStringLiteral("setChannelCategory"), humanReadableFailure(outcome));
			if (outcome.rateLimited || outcome.transportError) {
				// Transient - put it back, schedulePatch()
				// below will honor the backoff window
				// noteRequestOutcome() just set.
				pendingCategory_ = category;
			}
		}

		schedulePatch();
	});
}

// ---------------------------------------------------------------------
// GET /helix/games?name= (exact) - permanently cached
// ---------------------------------------------------------------------
void TwitchClient::findCategoryByExactName(const QString &name)
{
	const QString key = normalizeKey(name);

	const auto cached = exactCache_.find(key);
	if (cached != exactCache_.end()) {
		// Present in the map at all means "already asked Twitch this
		// session" - the value itself (present/absent match) is the
		// answer, cached or not. Zero network calls either way.
		emit categoryFound(cached->second);
		return;
	}

	if (frozen_) {
		emit requestFailed(QStringLiteral("findCategoryByExactName"), QStringLiteral("Twitch reconnect required."));
		return;
	}
	if (gatedByBackoff()) {
		emit requestFailed(QStringLiteral("findCategoryByExactName"),
				    QStringLiteral("Temporarily rate-limited by Twitch."));
		return;
	}

	QUrl url(kHelixBaseUrl + QStringLiteral("/games"));
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("name"), name);
	url.setQuery(query);

	transport_->send(buildHelixRequest(HttpMethod::Get, url), [this, key](HttpResponse response) {
		const HelixReplyOutcome outcome = consumeResponse(response);
		noteRequestOutcome(outcome);
		if (outcome.unauthorized) {
			freeze();
			return;
		}
		if (!outcome.ok) {
			emit requestFailed(QStringLiteral("findCategoryByExactName"), humanReadableFailure(outcome));
			return;
		}

		const QJsonArray data = QJsonDocument::fromJson(outcome.body).object().value(QStringLiteral("data")).toArray();
		if (data.isEmpty()) {
			// Cached in memory only (see header doc comment) - not
			// written to disk, so a category Twitch adds later for
			// this title is picked up fresh next launch.
			exactCache_[key] = std::nullopt;
			emit categoryFound(std::nullopt);
			return;
		}

		const QJsonObject entry = data.first().toObject();
		CategoryMatch match;
		match.gameId = entry.value(QStringLiteral("id")).toString();
		match.gameName = entry.value(QStringLiteral("name")).toString();

		exactCache_[key] = match;
		saveCategoryCache(); // Once per never-before-seen game, ever - not on any hot path.
		emit categoryFound(match);
	});
}

// ---------------------------------------------------------------------
// GET /helix/search/categories?query= (fuzzy) - permanently cached
// ---------------------------------------------------------------------
void TwitchClient::searchCategories(const QString &query)
{
	const QString key = normalizeKey(query);

	const auto cached = searchCache_.find(key);
	if (cached != searchCache_.end()) {
		emit categorySearchResults(cached->second);
		return;
	}

	if (frozen_) {
		emit requestFailed(QStringLiteral("searchCategories"), QStringLiteral("Twitch reconnect required."));
		return;
	}
	if (gatedByBackoff()) {
		emit requestFailed(QStringLiteral("searchCategories"), QStringLiteral("Temporarily rate-limited by Twitch."));
		return;
	}

	QUrl url(kHelixBaseUrl + QStringLiteral("/search/categories"));
	QUrlQuery urlQuery;
	urlQuery.addQueryItem(QStringLiteral("query"), query);
	url.setQuery(urlQuery);

	transport_->send(buildHelixRequest(HttpMethod::Get, url), [this, key](HttpResponse response) {
		const HelixReplyOutcome outcome = consumeResponse(response);
		noteRequestOutcome(outcome);
		if (outcome.unauthorized) {
			freeze();
			return;
		}
		if (!outcome.ok) {
			emit requestFailed(QStringLiteral("searchCategories"), humanReadableFailure(outcome));
			return;
		}

		const QJsonArray data = QJsonDocument::fromJson(outcome.body).object().value(QStringLiteral("data")).toArray();
		QVector<CategoryMatch> matches;
		matches.reserve(data.size());
		for (const QJsonValue &v : data) {
			const QJsonObject entry = v.toObject();
			CategoryMatch match;
			match.gameId = entry.value(QStringLiteral("id")).toString();
			match.gameName = entry.value(QStringLiteral("name")).toString();
			matches.append(match);
		}

		// In-memory for this session regardless of size; only a
		// non-empty result gets written to disk (see
		// saveCategoryCache()) - same staleness reasoning as the
		// exact-match cache above.
		searchCache_[key] = matches;
		if (!matches.isEmpty())
			saveCategoryCache();
		emit categorySearchResults(matches);
	});
}

// ---------------------------------------------------------------------
// POST /helix/streams/markers - fire-and-forget, silent-with-log
// ---------------------------------------------------------------------
void TwitchClient::createStreamMarker(const QString &description)
{
	// Each branch below emits requestFailed(context="createStreamMarker", ...)
	// before returning - see this method's header doc comment for why
	// that changed from a silent return: TwitchChannelClient (the
	// coordinator's adapter) needs a signal to answer its pending
	// callback with, on every path, not just the network-reply one.
	if (frozen_) {
		obs_log(LOG_DEBUG, "twitch: skipping stream marker (frozen)");
		emit requestFailed(QStringLiteral("createStreamMarker"), QStringLiteral("Twitch reconnect required."));
		return;
	}
	if (broadcasterId_.isEmpty()) {
		obs_log(LOG_DEBUG, "twitch: skipping stream marker (no broadcaster id yet)");
		emit requestFailed(QStringLiteral("createStreamMarker"), QStringLiteral("Broadcaster ID not resolved yet."));
		return;
	}
	if (gatedByBackoff()) {
		obs_log(LOG_DEBUG, "twitch: skipping stream marker (backed off)");
		emit requestFailed(QStringLiteral("createStreamMarker"), QStringLiteral("Temporarily rate-limited by Twitch."));
		return;
	}

	QUrl url(kHelixBaseUrl + QStringLiteral("/streams/markers"));
	QUrlQuery query;
	// user_id is a QUERY parameter here, not a body field - confirmed
	// against Twitch's own curl example, which is otherwise easy to get
	// backwards since every other write endpoint here uses broadcaster_id.
	query.addQueryItem(QStringLiteral("user_id"), broadcasterId_);
	url.setQuery(query);

	HttpRequest request = buildHelixRequest(HttpMethod::Post, url);
	request.headers.append(HttpHeader{"Content-Type", "application/json"});

	QJsonObject body;
	// 140-character limit per Twitch's docs - truncate defensively
	// rather than letting a long game title fail the whole call.
	body[QStringLiteral("description")] = description.left(140);
	request.body = QJsonDocument(body).toJson(QJsonDocument::Compact);

	transport_->send(request, [this](HttpResponse response) {
		const HelixReplyOutcome outcome = consumeResponse(response);
		noteRequestOutcome(outcome);
		if (outcome.unauthorized) {
			freeze();
			return;
		}
		if (!outcome.ok) {
			// Silent-with-log per DESIGN.md Section 5 as far as any
			// dock popup/freeze goes - markers are a nice-to-have,
			// never a user-facing error on their own. Still emits
			// requestFailed() (see header doc comment) so a caller
			// that wants to know can.
			obs_log(LOG_INFO, "twitch: stream marker not created (%s)",
				qUtf8Printable(humanReadableFailure(outcome)));
			emit requestFailed(QStringLiteral("createStreamMarker"), humanReadableFailure(outcome));
			return;
		}
		emit streamMarkerCreated();
	});
}

// ---------------------------------------------------------------------
QStringList TwitchClient::cachedCategoryNames() const
{
	QStringList names;
	for (const auto &[key, match] : exactCache_) {
		if (match && !match->gameName.isEmpty() && !names.contains(match->gameName, Qt::CaseInsensitive))
			names.push_back(match->gameName);
	}
	names.sort(Qt::CaseInsensitive);
	return names;
}

// Permanent category cache - category-cache.json, plugin config dir
// ---------------------------------------------------------------------
QString TwitchClient::normalizeKey(const QString &raw)
{
	return raw.trimmed().toLower();
}

void TwitchClient::loadCategoryCache()
{
	char *utf8Path = obs_module_config_path("category-cache.json");
	if (!utf8Path)
		return;
	const QString path = QString::fromUtf8(utf8Path);
	bfree(utf8Path);

	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return; // No cache yet - the normal state on a fresh install.

	const QByteArray raw = file.readAll();
	file.close();

	QJsonParseError parseError{};
	const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
	if (parseError.error != QJsonParseError::NoError || !doc.isObject())
		return; // Corrupt file - treat as empty rather than fail; it rebuilds itself.

	const QJsonObject root = doc.object();

	const QJsonObject exactObj = root.value(QStringLiteral("exact")).toObject();
	for (auto it = exactObj.constBegin(); it != exactObj.constEnd(); ++it) {
		const QJsonObject entry = it.value().toObject();
		CategoryMatch match;
		match.gameId = entry.value(QStringLiteral("id")).toString();
		match.gameName = entry.value(QStringLiteral("name")).toString();
		if (!match.gameId.isEmpty())
			exactCache_[it.key()] = match;
	}

	const QJsonObject searchObj = root.value(QStringLiteral("search")).toObject();
	for (auto it = searchObj.constBegin(); it != searchObj.constEnd(); ++it) {
		QVector<CategoryMatch> matches;
		const QJsonArray arr = it.value().toArray();
		matches.reserve(arr.size());
		for (const QJsonValue &v : arr) {
			const QJsonObject entry = v.toObject();
			CategoryMatch match;
			match.gameId = entry.value(QStringLiteral("id")).toString();
			match.gameName = entry.value(QStringLiteral("name")).toString();
			if (!match.gameId.isEmpty())
				matches.append(match);
		}
		if (!matches.isEmpty())
			searchCache_[it.key()] = matches;
	}

	obs_log(LOG_DEBUG, "twitch: loaded category cache (%zu exact, %zu search)", exactCache_.size(),
		searchCache_.size());
}

void TwitchClient::saveCategoryCache() const
{
	char *utf8Path = obs_module_config_path("category-cache.json");
	if (!utf8Path)
		return;
	const QString path = QString::fromUtf8(utf8Path);
	bfree(utf8Path);

	char *utf8Dir = obs_module_config_path("");
	if (utf8Dir) {
		os_mkdirs(utf8Dir); // MKDIR_EXISTS is a normal, expected return here.
		bfree(utf8Dir);
	}

	QJsonObject exactObj;
	for (const auto &[key, value] : exactCache_) {
		// Negative results are never persisted - see header doc
		// comment. Only a resolved match earns a permanent entry.
		if (!value.has_value())
			continue;
		QJsonObject entry;
		entry[QStringLiteral("id")] = value->gameId;
		entry[QStringLiteral("name")] = value->gameName;
		exactObj[key] = entry;
	}

	QJsonObject searchObj;
	for (const auto &[key, matches] : searchCache_) {
		if (matches.isEmpty())
			continue; // Same staleness reasoning as the exact cache.
		QJsonArray arr;
		for (const CategoryMatch &match : matches) {
			QJsonObject entry;
			entry[QStringLiteral("id")] = match.gameId;
			entry[QStringLiteral("name")] = match.gameName;
			arr.append(entry);
		}
		searchObj[key] = arr;
	}

	QJsonObject root;
	root[QStringLiteral("exact")] = exactObj;
	root[QStringLiteral("search")] = searchObj;

	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		obs_log(LOG_WARNING, "twitch: could not write category-cache.json");
		return;
	}
	file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	file.close();
}

} // namespace signalbox::twitch
