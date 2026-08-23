/*
 * SignalBox - twitch/TwitchAuth.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Implements the Device Code Grant flow exactly as described in
 * TwitchAuth.h and DESIGN.md 3.1-3.3, verified field-for-field against
 * Twitch's own published docs (dev.twitch.tv/docs/authentication) rather
 * than assumed:
 *
 *   POST /oauth2/device   body: client_id, scopes  (plural - initial request)
 *     -> { device_code, user_code, verification_uri, expires_in, interval }
 *   POST /oauth2/token    body: client_id, scope, device_code, grant_type
 *     (singular "scope" here - Twitch's own documented curl example for
 *     this step uses the singular field name, unlike step 1's "scopes";
 *     this is Twitch's docs being internally inconsistent, not a typo
 *     introduced here)
 *     -> success: { access_token, expires_in, refresh_token, scope[], token_type }
 *     -> pending/error: { status, message } - Twitch's own doc example
 *        shows exactly {"status":400,"message":"authorization_pending"}.
 *        The exact message strings for slow_down/expired_token/
 *        access_denied are NOT shown in Twitch's docs (only
 *        authorization_pending is), so those three are matched by
 *        case-insensitive substring against the RFC 8628 standard error
 *        codes, which Twitch's one confirmed example already follows
 *        verbatim. THIS IS THE ONE PIECE THAT NEEDS LIVE-FLOW
 *        VERIFICATION - see the report back to the coordinator.
 *
 * NO POLLING LOOPS BEYOND WHAT THE PROTOCOL REQUIRES: the poll timer
 * here only exists while a device-code flow the user explicitly started
 * is in flight, and it self-terminates on success, expiry, denial, or
 * cancelAuthorization() - never runs ambiently in the background. This
 * is the one exception the owner's "no polling" directive already
 * carves out by definition (it's not a background timer "checking on
 * things"; it's the mandatory mechanic of the OAuth flow the user is
 * actively waiting on, bounded by expires_in and paced by Twitch's own
 * interval/slow_down signal).
 */

#include "TwitchAuth.h"

// obs-module.h must come before plugin-support.h in any TU that needs
// both - see TwitchClient.cpp's include-order comment for why (MSVC
// C2375 on blogva() otherwise).
#include <obs-module.h>

#include "../plugin-support.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QDateTime>

namespace signalbox::twitch {

namespace {

const QString kDeviceCodeUrl = QStringLiteral("https://id.twitch.tv/oauth2/device");
const QString kTokenUrl = QStringLiteral("https://id.twitch.tv/oauth2/token");
const QString kValidateUrl = QStringLiteral("https://id.twitch.tv/oauth2/validate");

// Exactly one scope, per DESIGN.md 3.2: covers Modify Channel Information
// (category + title) and Create Stream Marker; Get Channel Information
// and Search Categories need no scope at all.
const QString kScope = QStringLiteral("channel:manage:broadcast");

HttpRequest formUrlEncodedRequest(const QString &url, const QUrlQuery &query)
{
	HttpRequest request;
	request.method = HttpMethod::Post;
	request.url = url;
	request.headers.append(HttpHeader{"Content-Type", "application/x-www-form-urlencoded"});
	request.body = query.toString(QUrl::FullyEncoded).toUtf8();
	return request;
}

// Twitch's own error shape for id.twitch.tv failures, per docs:
// {"status": 400, "message": "..."}. Returns an empty string if the body
// doesn't parse as that shape (network-level failure, HTML error page
// from an intermediary, etc.) rather than throwing - callers treat an
// empty message as "transient, keep going" where that's safe.
QString extractMessage(const QByteArray &body)
{
	QJsonParseError parseError{};
	const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
	if (parseError.error != QJsonParseError::NoError || !doc.isObject())
		return {};
	return doc.object().value(QStringLiteral("message")).toString();
}

qint64 nowUnixS()
{
	return QDateTime::currentSecsSinceEpoch();
}

} // namespace

TwitchAuth::TwitchAuth(QString clientId, QObject *parent)
	: QObject(parent),
	  clientId_(std::move(clientId)),
	  transport_(std::make_unique<HttpTransport>(this))
{
	pollTimer_ = new QTimer(this);
	pollTimer_->setSingleShot(false);
	connect(pollTimer_, &QTimer::timeout, this, &TwitchAuth::pollOnce);
}

TwitchAuth::~TwitchAuth() = default;

void TwitchAuth::beginDeviceCodeFlow()
{
	if (clientId_.isEmpty()) {
		emit authFailed(QStringLiteral(
			"Twitch Client ID is not configured. See src/twitch/TwitchClientId.h - the plugin owner "
			"needs to register a free app at dev.twitch.tv/console/apps."));
		return;
	}

	// A fresh request always wins over whatever was in flight - dock
	// reopened, user hit "connect" again, etc.
	cancelAuthorization();
	flowActive_ = true;

	QUrlQuery query;
	query.addQueryItem(QStringLiteral("client_id"), clientId_);
	query.addQueryItem(QStringLiteral("scopes"), kScope);

	transport_->send(formUrlEncodedRequest(kDeviceCodeUrl, query), [this](HttpResponse response) {
		if (!flowActive_)
			return; // Cancelled while this was in flight.
		handleDeviceCodeReply(response);
	});
}

void TwitchAuth::handleDeviceCodeReply(const HttpResponse &response)
{
	const QByteArray &body = response.body;

	QJsonParseError parseError{};
	const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);

	if (response.transportError || response.httpStatus != 200 || parseError.error != QJsonParseError::NoError ||
	    !doc.isObject() || !doc.object().contains(QStringLiteral("device_code"))) {
		flowActive_ = false;
		const QString message = extractMessage(body);
		emit authFailed(message.isEmpty() ? QStringLiteral("Could not start Twitch authorization. Check "
								     "your internet connection and try again.")
						   : QStringLiteral("Twitch authorization could not start: %1").arg(message));
		return;
	}

	const QJsonObject obj = doc.object();
	DeviceCodeInfo info;
	info.deviceCode = obj.value(QStringLiteral("device_code")).toString();
	info.userCode = obj.value(QStringLiteral("user_code")).toString();
	info.verificationUri = obj.value(QStringLiteral("verification_uri")).toString();
	info.expiresInS = obj.value(QStringLiteral("expires_in")).toInt();
	info.pollIntervalS = obj.value(QStringLiteral("interval")).toInt(5);

	pendingDeviceCode_ = info.deviceCode;
	pollIntervalS_ = info.pollIntervalS > 0 ? info.pollIntervalS : 5;
	deviceCodeExpiresAtUnixS_ = nowUnixS() + info.expiresInS;

	emit deviceCodeReady(info);
	startPolling();
}

void TwitchAuth::startPolling()
{
	pollTimer_->start(pollIntervalS_ * 1000);
}

void TwitchAuth::stopPolling()
{
	pollTimer_->stop();
}

void TwitchAuth::cancelAuthorization()
{
	flowActive_ = false;
	stopPolling();
	// No transport-level abort - the poll callback below checks
	// flowActive_ and no-ops if it's already false (see TwitchAuth.h's
	// pollInFlight_ doc comment). The in-flight request, if any, is left
	// to finish naturally; its result is simply discarded.
	pendingDeviceCode_.clear();
	deviceCodeExpiresAtUnixS_ = 0;
}

void TwitchAuth::pollOnce()
{
	if (!flowActive_ || pendingDeviceCode_.isEmpty())
		return;

	if (nowUnixS() >= deviceCodeExpiresAtUnixS_) {
		flowActive_ = false;
		stopPolling();
		emit authFailed(QStringLiteral(
			"The Twitch device code expired before authorization was completed. Please try connecting again."));
		return;
	}

	// Never let two polls overlap - if the previous one is still in
	// flight (slow network), skip this tick rather than piling up
	// requests.
	if (pollInFlight_)
		return;

	QUrlQuery query;
	query.addQueryItem(QStringLiteral("client_id"), clientId_);
	query.addQueryItem(QStringLiteral("scope"), kScope);
	query.addQueryItem(QStringLiteral("device_code"), pendingDeviceCode_);
	query.addQueryItem(QStringLiteral("grant_type"),
			    QStringLiteral("urn:ietf:params:oauth:grant-type:device_code"));

	pollInFlight_ = true;
	transport_->send(formUrlEncodedRequest(kTokenUrl, query), [this](HttpResponse response) {
		pollInFlight_ = false;
		if (!flowActive_)
			return; // Cancelled while this was in flight.
		handlePollReply(response);
	});
}

void TwitchAuth::handlePollReply(const HttpResponse &response)
{
	const QByteArray &body = response.body;

	QJsonParseError parseError{};
	const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
	const bool parsedOk = parseError.error == QJsonParseError::NoError && doc.isObject();

	if (!response.transportError && response.httpStatus == 200 && parsedOk &&
	    doc.object().contains(QStringLiteral("access_token"))) {
		const QJsonObject obj = doc.object();

		TokenSet tokens;
		tokens.accessToken = obj.value(QStringLiteral("access_token")).toString();
		tokens.refreshToken = obj.value(QStringLiteral("refresh_token")).toString();
		tokens.obtainedAtUnixS = nowUnixS();
		tokens.expiresInS = obj.value(QStringLiteral("expires_in")).toInt();
		// user_id/login are not part of this response (verified against
		// Twitch's docs) - the caller resolves those via validate() or
		// TwitchClient::getUsers() immediately after, per TwitchAuth.h.

		flowActive_ = false;
		stopPolling();
		pendingDeviceCode_.clear();
		emit tokensAcquired(tokens);
		return;
	}

	const QString message = parsedOk ? doc.object().value(QStringLiteral("message")).toString() : QString();
	const QString lowerMessage = message.toLower();

	if (lowerMessage.contains(QStringLiteral("authorization_pending"))) {
		return; // Expected/normal - user hasn't approved yet. Keep polling.
	}

	if (lowerMessage.contains(QStringLiteral("slow_down"))) {
		// RFC 8628: back off by increasing the interval, do not just
		// keep polling at the old rate.
		pollIntervalS_ += 5;
		pollTimer_->setInterval(pollIntervalS_ * 1000);
		return;
	}

	if (lowerMessage.contains(QStringLiteral("expired"))) {
		flowActive_ = false;
		stopPolling();
		emit authFailed(QStringLiteral(
			"The Twitch device code expired before authorization was completed. Please try connecting again."));
		return;
	}

	if (lowerMessage.contains(QStringLiteral("denied"))) {
		flowActive_ = false;
		stopPolling();
		emit authFailed(QStringLiteral("Twitch authorization was denied."));
		return;
	}

	if (!message.isEmpty()) {
		// Some other terminal, Twitch-reported error (e.g. "invalid
		// device code") - stop rather than loop against a request
		// that will never succeed.
		flowActive_ = false;
		stopPolling();
		emit authFailed(QStringLiteral("Twitch authorization failed: %1").arg(message));
		return;
	}

	// No parseable {status,message} body at all - a transient
	// network-level hiccup (dropped connection, proxy error page),
	// not a protocol-level answer from Twitch. Log and let the next
	// scheduled tick retry; do not treat this as fatal and do not
	// retry immediately (that would be the tight-loop behavior the
	// owner's directive explicitly rules out).
	obs_log(LOG_WARNING, "Twitch device code poll: transient failure (HTTP %d), will retry on next scheduled poll",
		response.httpStatus);
}

void TwitchAuth::refreshTokens(const TokenSet &current)
{
	if (current.refreshToken.isEmpty()) {
		emit authFailed(QStringLiteral("No Twitch refresh token available - reconnect required."));
		return;
	}
	if (clientId_.isEmpty()) {
		emit authFailed(QStringLiteral("Twitch Client ID is not configured."));
		return;
	}

	QUrlQuery query;
	query.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
	query.addQueryItem(QStringLiteral("refresh_token"), current.refreshToken);
	query.addQueryItem(QStringLiteral("client_id"), clientId_);
	// No client_secret - public client (DESIGN.md 3.1).

	const QString fallbackUserId = current.userId;
	const QString fallbackLogin = current.login;
	const QString fallbackRefreshToken = current.refreshToken;
	transport_->send(formUrlEncodedRequest(kTokenUrl, query),
			  [this, fallbackUserId, fallbackLogin, fallbackRefreshToken](HttpResponse response) {
				  handleRefreshReply(response, fallbackUserId, fallbackLogin, fallbackRefreshToken);
			  });
}

void TwitchAuth::handleRefreshReply(const HttpResponse &response, QString fallbackUserId, QString fallbackLogin,
				     QString fallbackRefreshToken)
{
	const QByteArray &body = response.body;

	QJsonParseError parseError{};
	const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);

	if (response.transportError || response.httpStatus != 200 || parseError.error != QJsonParseError::NoError ||
	    !doc.isObject() || !doc.object().contains(QStringLiteral("access_token"))) {
		const QString message = extractMessage(body);
		// Deliberately ONE attempt, no internal retry loop here - a
		// failed refresh means "reconnect required", surfaced by the
		// caller freezing automation (DESIGN.md 3.3). Never hammer
		// this endpoint.
		emit authFailed(message.isEmpty() ? QStringLiteral("Twitch token refresh failed - reconnect required.")
						   : QStringLiteral("Twitch token refresh failed: %1").arg(message));
		return;
	}

	const QJsonObject obj = doc.object();
	TokenSet tokens;
	tokens.accessToken = obj.value(QStringLiteral("access_token")).toString();
	// Twitch may or may not rotate the refresh token on refresh; if the
	// response omits it, keep using the one that got us here rather than
	// clobbering it with an empty string.
	const QString newRefreshToken = obj.value(QStringLiteral("refresh_token")).toString();
	tokens.refreshToken = newRefreshToken.isEmpty() ? fallbackRefreshToken : newRefreshToken;
	tokens.userId = fallbackUserId;
	tokens.login = fallbackLogin;
	tokens.obtainedAtUnixS = nowUnixS();
	tokens.expiresInS = obj.value(QStringLiteral("expires_in")).toInt();

	emit tokensRefreshed(tokens);
}

void TwitchAuth::validate(const QString &accessToken)
{
	if (accessToken.isEmpty()) {
		emit authFailed(QStringLiteral("No Twitch access token to validate - reconnect required."));
		return;
	}

	HttpRequest request;
	request.method = HttpMethod::Get;
	request.url = kValidateUrl;
	// /oauth2/validate specifically wants the "OAuth" scheme, not
	// "Bearer" - confirmed against Twitch's own docs/examples; this is
	// a documented quirk unique to this one endpoint (every Helix call
	// uses Bearer).
	request.headers.append(HttpHeader{"Authorization", QByteArray("OAuth ") + accessToken.toUtf8()});

	transport_->send(request, [this](HttpResponse response) { handleValidateReply(response); });
}

void TwitchAuth::handleValidateReply(const HttpResponse &response)
{
	const QByteArray &body = response.body;

	QJsonParseError parseError{};
	const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);

	if (response.transportError || response.httpStatus != 200 || parseError.error != QJsonParseError::NoError ||
	    !doc.isObject() || !doc.object().contains(QStringLiteral("user_id"))) {
		// A single failed validate is exactly the mid-stream-expiry
		// signal DESIGN.md 3.3 describes - report it and let the
		// caller drive the freeze/reconnect flow. No retry loop here.
		emit authFailed(QStringLiteral("Twitch token validation failed - reconnect required."));
		return;
	}

	const QJsonObject obj = doc.object();
	emit validated(obj.value(QStringLiteral("user_id")).toString(), obj.value(QStringLiteral("login")).toString());
}

} // namespace signalbox::twitch
