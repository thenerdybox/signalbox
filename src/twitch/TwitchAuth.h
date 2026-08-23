/*
 * SignalBox - twitch/TwitchAuth.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Device Code Grant (DCG) - the only OAuth flow this plugin uses, and
 * deliberately so (DESIGN.md 3.1): no client secret (this is a public
 * client), no redirect endpoint, no localhost listener, and it still
 * yields a refresh token. See DESIGN.md's flow-comparison table for why
 * the other two options (authorization code, implicit grant) were
 * rejected.
 *
 * Scope: exactly channel:manage:broadcast (DESIGN.md 3.2) - covers
 * Modify Channel Information and Create Stream Marker; Get Channel
 * Information and Search Categories need no scope at all.
 *
 * FLOW (DESIGN.md 3.1):
 *   1. POST https://id.twitch.tv/oauth2/device with client_id + scopes
 *      -> {device_code, user_code, verification_uri, expires_in, interval}
 *   2. UI shows user_code + a copy-to-clipboard "twitch.tv/activate" link
 *      (never auto-opens a browser - no-focus-steal rule, see
 *      ui/PromptWidget.h and CategoryDock.h).
 *   3. Poll POST https://id.twitch.tv/oauth2/token with
 *      grant_type=urn:ietf:params:oauth:grant-type:device_code at the
 *      server-specified interval until success/expiry.
 *   4. Response carries access_token AND refresh_token. Hand both to
 *      TokenStore immediately - this class never persists anything
 *      itself.
 *
 * THREADING: this class is Qt-main-thread only - it owns an HttpTransport
 * (twitch/HttpTransport.h; libcurl-backed, see that header for why it
 * replaced QNetworkAccessManager) and must never be constructed or called
 * from DetectionEngine's worker thread. All requests are async: send() to
 * HttpTransport returns immediately and its completion callback always
 * fires back on this thread, never blocking this thread on network I/O.
 */

#pragma once

#include <QObject>
#include <QString>

#include <memory>

#include "HttpTransport.h"

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace signalbox::twitch {

struct DeviceCodeInfo {
	QString deviceCode;
	QString userCode;
	QString verificationUri;
	int expiresInS = 0;
	int pollIntervalS = 5;
};

struct TokenSet {
	QString accessToken;
	QString refreshToken;
	QString userId;
	QString login;
	qint64 obtainedAtUnixS = 0;
	int expiresInS = 0;
};

// Async, signal-driven. No blocking calls anywhere in this interface -
// see the threading note above.
class TwitchAuth : public QObject {
	Q_OBJECT

public:
	explicit TwitchAuth(QString clientId, QObject *parent = nullptr);
	~TwitchAuth() override;

	// Kicks off step 1 of the DCG flow. Emits deviceCodeReady() on
	// success, authFailed() on failure. Internally starts polling step
	// 3 once deviceCodeReady() fires; polling stops on success,
	// expiry, or cancelAuthorization().
	void beginDeviceCodeFlow();

	// Cancels an in-flight device code flow (e.g. dock closed, user
	// backed out). Safe to call even if no flow is in progress.
	void cancelAuthorization();

	// Refreshes an existing token set. Emits tokensRefreshed() or
	// authFailed(). Used both proactively (at 75% of expires_in,
	// DESIGN.md 3.3) and reactively (on a 401, retried once).
	void refreshTokens(const TokenSet &current);

	// GET https://id.twitch.tv/oauth2/validate. Twitch requires this
	// on startup and hourly (DESIGN.md 3.3). Emits validated() or
	// authFailed().
	void validate(const QString &accessToken);

signals:
	void deviceCodeReady(DeviceCodeInfo info);
	void tokensAcquired(TokenSet tokens);
	void tokensRefreshed(TokenSet tokens);
	void validated(QString userId, QString login);
	// human-readable, dock-safe message; never a raw exception/HTTP dump.
	void authFailed(QString reason);

private:
	void startPolling();
	void stopPolling();
	void pollOnce();
	void handleDeviceCodeReply(const HttpResponse &response);
	void handlePollReply(const HttpResponse &response);
	void handleRefreshReply(const HttpResponse &response, QString fallbackUserId, QString fallbackLogin,
				 QString fallbackRefreshToken);
	void handleValidateReply(const HttpResponse &response);

	QString clientId_;
	std::unique_ptr<HttpTransport> transport_;
	QTimer *pollTimer_ = nullptr;

	// In-flight device code flow state (DESIGN.md 3.1 steps 1-3). Reset
	// by both a fresh beginDeviceCodeFlow() call and cancelAuthorization().
	bool flowActive_ = false;
	QString pendingDeviceCode_;
	int pollIntervalS_ = 5;
	qint64 deviceCodeExpiresAtUnixS_ = 0;

	// True while a device-code poll is in flight - HttpTransport does not
	// hand back a cancellable request handle (see its class doc comment:
	// callers gate on their own state instead), so cancelAuthorization()
	// clears flowActive_ and this flag's callback checks that flag rather
	// than aborting the transfer itself. "Never let two polls overlap"
	// (pollOnce()'s own rule) still holds - this is what enforces it now.
	bool pollInFlight_ = false;
};

} // namespace signalbox::twitch
