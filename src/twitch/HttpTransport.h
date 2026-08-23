/*
 * SignalBox - twitch/HttpTransport.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Minimal async HTTPS transport for TwitchAuth/TwitchClient, backed by
 * libcurl instead of Qt Network.
 *
 * WHY NOT QNetworkAccessManager: Qt's network stack needs its own TLS
 * backend plugin (qschannelbackend.dll/qopensslbackend.dll) to do HTTPS at
 * all. OBS's Qt distribution does not ship one - Qt only does UI duty in
 * OBS, never networking, so OBS has no reason to carry that plugin - which
 * is exactly why every Twitch call used to fail instantly with "No
 * functional TLS backend was found". OBS itself ships libcurl.dll
 * (bin/64bit) and uses it for all of its own HTTPS traffic (update checks,
 * service info, etc.) via Windows' native Secure Channel, with no OpenSSL
 * involved. This class links against that same libcurl and rides the same
 * already-proven TLS path OBS depends on for itself, rather than shipping
 * (and staying version-locked to) a Qt plugin DLL.
 *
 * THREADING: unlike QNetworkAccessManager (main-thread-owned, non-blocking
 * by construction), libcurl's easy interface is blocking. To keep that
 * blocking call off the OBS UI thread, this class runs a single worker
 * thread (the same std::thread + mutex/condition-variable pattern
 * DetectionEngine already uses - see core/DetectionEngine.h) that drains a
 * small FIFO queue of requests one at a time with curl_easy_perform().
 * Every request's completion callback is handed back to the thread that
 * called send() via QMetaObject::invokeMethod(this, ..., Qt::QueuedConnection)
 * - Qt guarantees a pending invocation like that is silently dropped, never
 * delivered, if `this` (the context object) is destroyed first, so a
 * HttpTransport going away mid-request can never call back into a
 * half-destroyed TwitchAuth/TwitchClient. The destructor stops and joins
 * the worker thread before returning, so no callback can fire after this
 * object is gone.
 *
 * This is a transport swap only - callers still get one callback per
 * request, on their own thread, exactly as QNAM gave them via
 * QNetworkReply::finished. Nothing about TwitchAuth/TwitchClient's public
 * shape changes because of this class.
 */

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace signalbox::twitch {

enum class HttpMethod { Get, Post, Patch };

struct HttpHeader {
	QByteArray name;
	QByteArray value;
};

struct HttpRequest {
	HttpMethod method = HttpMethod::Get;
	QString url;
	QVector<HttpHeader> headers;
	QByteArray body; // Ignored for Get.
};

struct HttpResponse {
	// True when curl never got an HTTP response at all - DNS failure,
	// connection refused, TLS handshake failure, timeout. Mirrors the
	// old QNetworkReply-based outcome.httpStatus == 0 case exactly, so
	// callers built against that meaning need no changes.
	bool transportError = false;
	QString transportErrorString; // curl_easy_strerror() text - logs only.
	int httpStatus = 0;
	QByteArray body;
	QVector<HttpHeader> headers; // As received; use header() for lookup.

	// Case-insensitive, first-match lookup (HTTP header names are
	// case-insensitive; Twitch is consistent about casing but this
	// matches QNetworkReply::rawHeader()'s documented behavior exactly,
	// which the code that used to call it already relies on).
	QByteArray header(const QByteArray &name) const;
};

// One HttpTransport per owner (TwitchAuth, TwitchClient each have their
// own); not shared, not a singleton. Cheap to own - the worker thread sits
// blocked on a condition variable when idle, per DESIGN.md's "no ambient
// polling" philosophy.
class HttpTransport : public QObject {
	Q_OBJECT

public:
	explicit HttpTransport(QObject *parent = nullptr);
	~HttpTransport() override;

	HttpTransport(const HttpTransport &) = delete;
	HttpTransport &operator=(const HttpTransport &) = delete;

	// Enqueues request and returns immediately. callback fires exactly
	// once, on the thread that called send() (must have a running Qt
	// event loop - true for both TwitchAuth and TwitchClient, which are
	// Qt-main-thread-only per their own class doc comments), never
	// synchronously from inside send() itself.
	void send(HttpRequest request, std::function<void(HttpResponse)> callback);

private:
	struct QueuedRequest {
		HttpRequest request;
		std::function<void(HttpResponse)> callback;
	};

	void workerLoop();
	HttpResponse perform(const HttpRequest &request);

	std::thread worker_;
	std::mutex queueMutex_;
	std::condition_variable queueCv_;
	std::deque<QueuedRequest> queue_;
	std::atomic<bool> stopRequested_{false};
};

} // namespace signalbox::twitch
