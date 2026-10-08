/*
 * SignalBox - twitch/TwitchClient.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Helix API surface this plugin uses (DESIGN.md 3.4), and nothing else:
 *   GET   /helix/users                    - resolve broadcaster_id after auth
 *   GET   /helix/channels                 - initial state sync + external-
 *                                            writer detection (DESIGN.md 3.5)
 *   PATCH /helix/channels                 - the core action (game_id, title)
 *   GET   /helix/games?name=              - CategoryResolver tier 3 exact match
 *   GET   /helix/search/categories?query= - CategoryResolver tier 3 fuzzy search
 *   POST  /helix/streams/markers          - "Now playing: <game>" (DESIGN.md 5)
 *
 * RATE LIMITING (DESIGN.md 3.4): Twitch's bucket is 800 points/min per
 * client; worst realistic usage here is under 10 requests/min. Still:
 *   - Respect Ratelimit-Remaining/Ratelimit-Reset response headers and
 *     back off (exponentially on repeated failures, not just once) on 429
 *     and on transport-level errors alike - see the owner's "lightweight"
 *     directive: no tight retry loops against Twitch, ever.
 *   - Serialize ALL Helix writes through one queue so retries can never
 *     reorder a PATCH against itself.
 *   - Enforce TimingConstants::minPatchSpacingS between category PATCHes
 *     regardless of how often the caller asks - that floor belongs here,
 *     not in the caller, so nothing can accidentally bypass it. Read the
 *     constant from core/TimingConstants.h; never redefine it here.
 *
 * PERFORMANCE (owner directive, added mid-implementation - "lightweight
 * is the target, not the goal"):
 *   - REDUNDANCY GUARD: setChannelCategory() no-ops immediately, with no
 *     network call at all, if the requested game_id matches the last
 *     known channel state (kept in memory, refreshed by every successful
 *     getChannelInfo()/setChannelCategory()). This is a performance
 *     requirement as much as a correctness one.
 *   - CATEGORY LOOKUP CACHE: findCategoryByExactName() and
 *     searchCategories() persist every successful resolution to
 *     category-cache.json (plugin config dir) and never re-query Helix
 *     for a name/query already resolved on this install - a game name to
 *     Twitch category id mapping does not change, so Helix should be hit
 *     once per game, ever, not once per detection. Negative (not-found)
 *     results are cached in memory for the current OBS session only -
 *     not persisted - so a title that gets a Twitch category added later
 *     is picked up on the next OBS launch instead of staying permanently
 *     "unmapped".
 *   - No connection is ever held open waiting for work: every request is
 *     created, completed (or failed), and released. Nothing here polls
 *     Twitch on an ambient timer - every call is triggered by a confirmed
 *     detection event or an explicit user/caller action.
 *
 * STRUCTURAL SAFETY: setChannelCategory() refuses an empty
 * CategoryMatch::gameId unconditionally, before any other check -
 * per Twitch's Modify Channel Information docs, PATCHing an empty
 * game_id UNSETS the live category, which is the exact harm the
 * product's "unmapped means no action" premise exists to prevent
 * (DESIGN.md's ADDENDUM). Callers (core::CategorySwitchCoordinator) are
 * already responsible for never resolving to an empty id before
 * PATCHing, but that must be true by construction here too - this class
 * is the last line of defense, not the first.
 *
 * 401 HANDLING (DESIGN.md 3.3): TwitchClient only ever holds an access
 * token, never a refresh token (see constructor), so it cannot perform
 * the refresh itself. On any 401 it freezes (further setChannelCategory/
 * createStreamMarker/getChannelInfo/etc. calls no-op until
 * updateAccessToken() is called with a fresh token) and emits
 * reauthRequired() exactly once per freeze. The owning coordinator is
 * responsible for: on reauthRequired(), call TwitchAuth::refreshTokens()
 * with the full token set (which it holds, e.g. via TokenStore) exactly
 * once; on TwitchAuth::tokensRefreshed(), call updateAccessToken() here
 * and re-issue whatever action failed; on TwitchAuth::authFailed(), leave
 * this client frozen and show "Reconnect to Twitch". This client never
 * retries a 401 in a loop by construction - it has nothing to retry with
 * until updateAccessToken() unfreezes it.
 *
 * THREADING: Qt-main-thread only, same rule as TwitchAuth - owns an
 * HttpTransport (twitch/HttpTransport.h; libcurl-backed, see that header
 * for why it replaced QNetworkAccessManager), all calls async via signals.
 */

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>

#include "../core/TimingConstants.h"
#include "HttpTransport.h"

QT_BEGIN_NAMESPACE
class QTimer;
class QUrl;
QT_END_NAMESPACE

namespace signalbox::twitch {

struct ChannelInfo {
	QString broadcasterId;
	QString gameId;
	QString gameName;
	QString title;
};

struct CategoryMatch {
	QString gameId;
	QString gameName;
};

// Async, signal-driven; see class doc comment for the write-queue and
// rate-limit rules every implementation of these methods must honor.
class TwitchClient : public QObject {
	Q_OBJECT

public:
	// minPatchSpacingS defaults to the compiled-in named constant
	// (core::kDefaultTimingConstants.minPatchSpacingS) so nothing needs
	// to change at call sites that don't care to override it; pass the
	// PluginConfig-effective value once that wiring exists to honor a
	// user override instead of the default.
	explicit TwitchClient(QString clientId, QString accessToken, QString broadcasterId,
			      std::uint32_t minPatchSpacingS = core::kDefaultTimingConstants.minPatchSpacingS,
			      QObject *parent = nullptr);
	~TwitchClient() override;

	// Hands the client a fresh access token - both for the normal
	// proactive-refresh path and to unfreeze after a 401 (see class doc
	// comment's 401 HANDLING section). Clears the frozen state.
	void updateAccessToken(QString accessToken);

	// GET /helix/users with no query params - resolves the token's own
	// identity. Twitch's device-code token response does not include
	// user_id/login (verified against Twitch's docs), so this is the
	// step that fills in broadcasterId_ after a fresh login; on a warm
	// start where broadcaster_id was already persisted (TokenStore's
	// StoredTokens::userId), callers do not need to call this again.
	void resolveCurrentUser();

	// GET /helix/channels. Used for initial sync and, while live, for
	// the external-writer detection poll (DESIGN.md 3.5) - callers
	// should compare the returned ChannelInfo::gameId against the last
	// value they set themselves, not assume every mismatch is us. Note
	// per the owner's "no polling" directive: this method itself does
	// not start any timer - if a caller wires this to a recurring
	// check, that recurring check must stay event-driven (e.g. run
	// immediately before a PATCH, not on an ambient interval).
	void getChannelInfo();

	// PATCH /helix/channels. Goes through the internal write queue,
	// TimingConstants::minPatchSpacingS pacing, and the redundancy
	// guard (no-ops with zero network calls if category.gameId already
	// matches the last known channel state) - callers do not need to
	// (and should not try to) rate-limit or de-duplicate this
	// themselves.
	void setChannelCategory(const CategoryMatch &category);

	// GET /helix/games?name=<exact>. CategoryResolver tier 3, first
	// half (DESIGN.md 1.5). Answers from the persistent on-disk cache
	// without any network call when this exact name has been resolved
	// before on this install.
	void findCategoryByExactName(const QString &name);

	// Names of every category this install has ever resolved by exact name
	// (the persistent cache), sorted. Used to give an empty recent-category
	// list a useful starting point; never touches the network.
	QStringList cachedCategoryNames() const;

	// GET /helix/search/categories?query=<normalized>. CategoryResolver
	// tier 3, second half - caller applies the scoring/threshold logic
	// from DESIGN.md 1.5 to the results. Same caching behavior as
	// findCategoryByExactName().
	void searchCategories(const QString &query);

	// POST /helix/streams/markers. Fire-and-forget from the caller's
	// perspective: failures (not live, VODs disabled) are silent-with-
	// log per DESIGN.md Section 5, never surfaced as a user-facing error
	// through anything THIS class does on its own initiative (no dock
	// popup, no freeze). It still emits requestFailed(context="createStreamMarker", ...)
	// on every failure path (including the frozen/no-broadcaster-id/
	// backoff guards, which used to return silently) precisely so a
	// caller that DOES want to observe the outcome (see
	// twitch::TwitchChannelClient, the coordinator's adapter) always
	// gets an answer instead of an outstanding callback nothing will
	// ever complete - nothing about the "never a user-facing error"
	// contract changes just because the signal now always fires; no
	// caller before TwitchChannelClient ever subscribed to
	// requestFailed() for this context.
	void createStreamMarker(const QString &description);

	// Best-effort mirror of the redundancy guard's own state
	// (lastKnownGameId_ above) - lets a caller pre-empt a
	// setChannelCategory() call this class would otherwise silently
	// no-op (no signal at all - see setChannelCategory()'s doc comment)
	// so it never ends up with an outstanding callback nothing will
	// answer. std::nullopt until the first successful
	// getChannelInfo()/setChannelCategory().
	std::optional<QString> lastKnownGameId() const { return lastKnownGameId_; }

signals:
	void currentUserResolved(QString userId, QString login);
	void channelInfoReady(ChannelInfo info);
	void categoryChangeSucceeded(CategoryMatch category);
	void categoryFound(std::optional<CategoryMatch> exactMatch);
	void categorySearchResults(QVector<CategoryMatch> matches);
	void streamMarkerCreated();
	// human-readable, dock-safe; never a raw HTTP/JSON dump.
	void requestFailed(QString context, QString reason);
	// Reconnect required - this client is now frozen after a 401 with
	// no way to refresh itself. DESIGN.md 3.3.
	void reauthRequired();

private:
	struct HelixReplyOutcome {
		bool ok = false;
		bool unauthorized = false;  // HTTP 401
		bool rateLimited = false;   // HTTP 429
		bool transportError = false; // no HTTP response at all
		int httpStatus = 0;
		qint64 rateLimitResetUnixS = 0; // 0 = header absent
		QByteArray body;
	};

	HttpRequest buildHelixRequest(HttpMethod method, const QUrl &url) const;
	HelixReplyOutcome consumeResponse(const HttpResponse &response);
	static QString humanReadableFailure(const HelixReplyOutcome &outcome);
	void noteRequestOutcome(const HelixReplyOutcome &outcome);
	bool gatedByBackoff() const;
	void freeze();

	void schedulePatch();
	void executePendingPatch();

	void loadCategoryCache();
	void saveCategoryCache() const;
	static QString normalizeKey(const QString &raw);

	QString clientId_;
	QString accessToken_;
	QString broadcasterId_;
	std::unique_ptr<HttpTransport> transport_;

	// --- write queue / redundancy guard / pacing ---
	QTimer *patchTimer_ = nullptr;
	std::optional<CategoryMatch> pendingCategory_;
	bool patchInFlight_ = false;
	qint64 lastPatchAtUnixMs_ = 0;
	std::uint32_t minPatchSpacingS_;
	std::optional<QString> lastKnownGameId_;

	// --- 401 / freeze state ---
	bool frozen_ = false;

	// --- 429 / transport-error backoff (shared gate across all calls) ---
	int consecutiveFailures_ = 0;
	qint64 backoffUntilUnixMs_ = 0;

	// --- permanent category-lookup cache (owner directive: hit Helix
	// once per game, ever) ---
	// Map VALUE doubles as the "not found" marker: std::nullopt / an
	// empty QVector means "queried this session, Twitch had no match" -
	// kept in memory only and never written to category-cache.json (see
	// saveCategoryCache()), so a title with no category today gets a
	// clean re-check next OBS launch instead of staying "unmapped"
	// forever. A present, non-empty value is the permanent positive
	// resolution and IS persisted - that's the actual once-per-game-ever
	// saving the owner asked for.
	std::map<QString, std::optional<CategoryMatch>> exactCache_;
	std::map<QString, QVector<CategoryMatch>> searchCache_;
};

} // namespace signalbox::twitch
