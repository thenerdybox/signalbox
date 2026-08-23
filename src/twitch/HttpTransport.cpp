/*
 * SignalBox - twitch/HttpTransport.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * See HttpTransport.h for the why (no Qt TLS backend in OBS's Qt) and the
 * threading contract (worker thread does the blocking curl call; the
 * caller's callback always runs back on the caller's own thread via a
 * QueuedConnection tied to this object's lifetime).
 */

#include "HttpTransport.h"

// obs-module.h must come before plugin-support.h in any TU that needs
// both - see TwitchClient.cpp's include-order comment for why (MSVC C2375
// on blogva() otherwise).
#include <obs-module.h>

#include "../plugin-support.h"

#include <curl/curl.h>

#include <QMetaObject>

#include <cstring>
#include <mutex>

namespace signalbox::twitch {

namespace {

// curl_global_init()/curl_global_cleanup() must each run exactly once per
// process (curl's own docs: not thread-safe, do it once at startup, once
// at shutdown). TwitchAuth and TwitchClient each own an independent
// HttpTransport, so this is refcounted rather than tied to any one
// instance.
std::mutex g_curlGlobalMutex;
int g_curlGlobalRefCount = 0;

void curlGlobalRef()
{
	std::lock_guard<std::mutex> lock(g_curlGlobalMutex);
	if (g_curlGlobalRefCount++ == 0)
		curl_global_init(CURL_GLOBAL_DEFAULT);
}

void curlGlobalUnref()
{
	std::lock_guard<std::mutex> lock(g_curlGlobalMutex);
	if (--g_curlGlobalRefCount == 0)
		curl_global_cleanup();
}

size_t writeBodyCallback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	const size_t bytes = size * nmemb;
	static_cast<QByteArray *>(userdata)->append(ptr, static_cast<int>(bytes));
	return bytes;
}

size_t writeHeaderCallback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	const size_t bytes = size * nmemb;
	auto *headers = static_cast<QVector<HttpHeader> *>(userdata);

	// Status line ("HTTP/1.1 200 OK") and the blank line terminating the
	// header block both lack a colon - skip anything that isn't a real
	// "Name: value" header field.
	const char *colon = static_cast<const char *>(std::memchr(ptr, ':', bytes));
	if (!colon)
		return bytes;

	const int nameLen = static_cast<int>(colon - ptr);
	int valueStart = static_cast<int>(colon - ptr) + 1;
	int valueLen = static_cast<int>(bytes) - valueStart;

	// Trim leading/trailing whitespace (including the trailing \r\n) off
	// the value, same as QNetworkReply::rawHeader() effectively hands
	// back.
	while (valueLen > 0 && (ptr[valueStart] == ' ' || ptr[valueStart] == '\t')) {
		++valueStart;
		--valueLen;
	}
	while (valueLen > 0 && (ptr[valueStart + valueLen - 1] == '\r' || ptr[valueStart + valueLen - 1] == '\n')) {
		--valueLen;
	}

	if (nameLen > 0)
		headers->append(HttpHeader{QByteArray(ptr, nameLen), QByteArray(ptr + valueStart, valueLen)});

	return bytes;
}

// Lets HttpTransport's destructor abort a blocked-in-progress transfer
// promptly on plugin unload instead of waiting out curl's own timeout.
int abortIfStoppingCallback(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
	const auto *stopRequested = static_cast<const std::atomic<bool> *>(clientp);
	return stopRequested->load() ? 1 : 0; // Non-zero aborts the transfer.
}

const char *methodName(HttpMethod method)
{
	switch (method) {
	case HttpMethod::Get:
		return "GET";
	case HttpMethod::Post:
		return "POST";
	case HttpMethod::Patch:
		return "PATCH";
	}
	return "GET";
}

} // namespace

QByteArray HttpResponse::header(const QByteArray &name) const
{
	for (const HttpHeader &h : headers) {
		if (h.name.compare(name, Qt::CaseInsensitive) == 0)
			return h.value;
	}
	return {};
}

HttpTransport::HttpTransport(QObject *parent) : QObject(parent)
{
	curlGlobalRef();
	worker_ = std::thread(&HttpTransport::workerLoop, this);
}

HttpTransport::~HttpTransport()
{
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		stopRequested_ = true;
	}
	queueCv_.notify_all();
	if (worker_.joinable())
		worker_.join(); // Bounded by abortIfStoppingCallback() above - never hangs on a stuck transfer.
	curlGlobalUnref();
}

void HttpTransport::send(HttpRequest request, std::function<void(HttpResponse)> callback)
{
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		queue_.push_back(QueuedRequest{std::move(request), std::move(callback)});
	}
	queueCv_.notify_one();
}

void HttpTransport::workerLoop()
{
	for (;;) {
		QueuedRequest item;
		{
			std::unique_lock<std::mutex> lock(queueMutex_);
			queueCv_.wait(lock, [this] { return stopRequested_ || !queue_.empty(); });
			if (stopRequested_ && queue_.empty())
				break;
			item = std::move(queue_.front());
			queue_.pop_front();
		}

		HttpResponse response = perform(item.request);

		// Hand the result back to whatever thread called send() (always
		// the Qt main thread in this plugin - see class doc comment).
		// `this` is also the context object: if this HttpTransport is
		// destroyed before the queued call is delivered, Qt drops it
		// instead of calling into a dead object - see HttpTransport.h.
		auto callback = std::move(item.callback);
		QMetaObject::invokeMethod(
			this, [callback = std::move(callback), response = std::move(response)]() { callback(response); },
			Qt::QueuedConnection);
	}
}

HttpResponse HttpTransport::perform(const HttpRequest &request)
{
	HttpResponse response;

	CURL *curl = curl_easy_init();
	if (!curl) {
		response.transportError = true;
		response.transportErrorString = QStringLiteral("curl_easy_init failed");
		return response;
	}

	curl_slist *headerList = nullptr;
	for (const HttpHeader &h : request.headers) {
		const QByteArray line = h.name + ": " + h.value;
		headerList = curl_slist_append(headerList, line.constData());
	}

	const QByteArray urlUtf8 = request.url.toUtf8();
	curl_easy_setopt(curl, CURLOPT_URL, urlUtf8.constData());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "SignalBox-OBS-Plugin/1.0");

	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abortIfStoppingCallback);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &stopRequested_);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBodyCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, writeHeaderCallback);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);

	switch (request.method) {
	case HttpMethod::Get:
		curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
		break;
	case HttpMethod::Post:
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.constData());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
		break;
	case HttpMethod::Patch:
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, methodName(HttpMethod::Patch));
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.constData());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
		break;
	}

	const CURLcode result = curl_easy_perform(curl);

	if (result == CURLE_OK) {
		long httpStatus = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
		response.httpStatus = static_cast<int>(httpStatus);
	} else {
		// No usable HTTP response at all - DNS/connect/TLS/timeout/
		// aborted. Matches the old outcome.httpStatus == 0 "transport
		// error" case exactly; callers built against that meaning
		// need no changes.
		response.transportError = true;
		response.transportErrorString = QString::fromUtf8(curl_easy_strerror(result));
		obs_log(LOG_DEBUG, "twitch: curl transport error: %s", curl_easy_strerror(result));
	}

	curl_slist_free_all(headerList);
	curl_easy_cleanup(curl);
	return response;
}

} // namespace signalbox::twitch
