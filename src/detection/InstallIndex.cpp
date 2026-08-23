/*
 * SignalBox - detection/InstallIndex.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Real rebuild()/match() per DESIGN.md 1.4, plus the caching/persistence/
 * staleness-check machinery described in InstallIndex.h's performance
 * contract:
 *   1. Exact launchExe match (Epic/GOG gold case) -> Confidence::High.
 *   2. Else longest-prefix installRoot match -> confidence by platform
 *      (Steam/Epic/Gog High, Ubisoft Medium, Generic/Xbox Low).
 * This is purely the *identity* match against the install index. The
 * helper/launcher denylist filter and the foreground/working-set/start-
 * time ranking across multiple simultaneously-matched processes are
 * DetectionEngine's job, operating over every running process's match()
 * result (see this header's class comment and DetectionEngine.h) -
 * neither belongs here.
 *
 * JSON persistence uses Qt6::Core's QJsonDocument, the same choice as
 * HelperDenylist.h and EpicProvider.cpp - already a required dependency,
 * no new vendoring, and safe off the Qt main thread as a plain value-type
 * parser/serializer (see HelperDenylist.h's note on this).
 */

#include "InstallIndex.h"

#include <map>

#include <algorithm>
#include <cwctype>
#include <iterator>

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace signalbox::detection {

namespace {

std::wstring ToLowerCopy(std::wstring s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
	return s;
}

std::wstring NormalizeSeparators(std::wstring s)
{
	std::replace(s.begin(), s.end(), L'/', L'\\');
	return s;
}

std::wstring NormalizePath(const std::wstring &s)
{
	return ToLowerCopy(NormalizeSeparators(s));
}

// True if normalizedPath falls under normalizedRoot (a directory).
// Both arguments must already be normalized (lowercased, backslash
// separators) via NormalizePath(). A root without a trailing separator
// is treated as a directory boundary regardless (e.g. root "c:\games\foo"
// matches "c:\games\foo\bar.exe" but not "c:\games\foobar.exe").
bool PathStartsWithRoot(const std::wstring &normalizedPath, const std::wstring &normalizedRoot)
{
	if (normalizedRoot.empty()) {
		return false;
	}

	std::wstring root = normalizedRoot;
	if (root.back() != L'\\') {
		root.push_back(L'\\');
	}

	if (normalizedPath.size() < root.size()) {
		return false;
	}

	return normalizedPath.compare(0, root.size(), root) == 0;
}

// DESIGN.md 1.4 step 2: confidence by platform for a prefix (non-exact)
// match. Steam has no per-app launchExe (ACF doesn't record it), so its
// installRoot match is still its primary, High-confidence signal - its
// steamapps\common\<installdir> roots are game-specific, not a shared
// parent. Epic/GOG normally match exactly via launchExe (see match()
// step 1); a prefix match for them here only happens if that provider's
// launchExe was empty for some reason, so it's still treated as High
// rather than penalized for a data gap that isn't the matching logic's
// fault.
Confidence ConfidenceForPlatform(Platform platform)
{
	switch (platform) {
	case Platform::Steam:
	case Platform::Epic:
	case Platform::Gog:
		return Confidence::High;
	case Platform::Ubisoft:
		return Confidence::Medium;
	case Platform::Generic:
	case Platform::Xbox:
	default:
		return Confidence::Low;
	}
}

} // namespace

InstallIndex::InstallIndex() = default;
InstallIndex::~InstallIndex() = default;

void InstallIndex::addProvider(std::unique_ptr<IGameProvider> provider)
{
	providers_.push_back(std::move(provider));
}

void InstallIndex::setExclusions(IndexExclusions exclusions)
{
	exclusions_ = std::move(exclusions);
}

std::vector<InstalledGame> InstallIndex::filterForIngest(std::vector<InstalledGame> candidates, std::size_t *outExcluded,
							  std::size_t *outDuplicates) const
{
	std::size_t excluded = 0;
	std::size_t duplicates = 0;

	std::vector<InstalledGame> survivors;
	survivors.reserve(candidates.size());

	// Install root (normalized) -> index into survivors. Entries with no
	// install root are never deduplicated against each other: an empty
	// root is "unknown", not "the same place".
	std::map<std::wstring, std::size_t> byRoot;

	for (auto &game : candidates) {
		if (exclusions_.excludes(game)) {
			++excluded;
			continue;
		}

		if (game.installRoot.empty()) {
			survivors.push_back(std::move(game));
			continue;
		}

		const std::wstring key = NormalizePath(game.installRoot);
		const auto existing = byRoot.find(key);
		if (existing == byRoot.end()) {
			byRoot.emplace(key, survivors.size());
			survivors.push_back(std::move(game));
			continue;
		}

		// Two providers found the same install. Keep the more
		// trustworthy one: a Steam entry carries a real appid, while
		// the Uninstall-registry entry for the same game carries
		// "Steam App 2767030" and only ever resolves to Confidence::Low.
		// Keeping both is not merely wasteful - it makes the index's
		// count lie, and it leaves a Low-confidence twin that can win a
		// ranking pass if the real entry is ever dropped.
		++duplicates;
		InstalledGame &incumbent = survivors[existing->second];
		const bool challengerIsBetter =
			static_cast<int>(ConfidenceForPlatform(game.platform)) <
			static_cast<int>(ConfidenceForPlatform(incumbent.platform));
		if (challengerIsBetter) {
			incumbent = std::move(game);
		} else if (incumbent.launchExe.empty() && !game.launchExe.empty()) {
			// Same confidence tier, but the loser knows the exact
			// binary and the winner does not. That is match()'s
			// step-1 gold case, so take it rather than discard it.
			incumbent.launchExe = std::move(game.launchExe);
		}
	}

	if (outExcluded != nullptr) {
		*outExcluded = excluded;
	}
	if (outDuplicates != nullptr) {
		*outDuplicates = duplicates;
	}
	return survivors;
}

void InstallIndex::rebuild()
{
	const auto startedAt = std::chrono::steady_clock::now();

	std::vector<InstalledGame> next;

	for (auto &provider : providers_) {
		// Providers fail independently (DESIGN.md Section 7 risk 5):
		// one provider's exception must never take down the others'
		// contributions. Providers are documented (IGameProvider.h) to
		// return {} rather than throw on a missing/corrupt source, but
		// this catch is the hard backstop for anything that slips
		// through that contract (e.g. an unexpected filesystem/registry
		// error type).
		try {
			auto games = provider->enumerate();
			next.insert(next.end(), std::make_move_iterator(games.begin()), std::make_move_iterator(games.end()));
		} catch (...) {
			continue;
		}
	}

	std::size_t excluded = 0;
	std::size_t duplicates = 0;
	next = filterForIngest(std::move(next), &excluded, &duplicates);

	const auto finishedAt = std::chrono::steady_clock::now();
	const double durationMs = std::chrono::duration<double, std::milli>(finishedAt - startedAt).count();

	std::lock_guard<std::mutex> lock(mutex_);
	index_ = std::move(next);
	lastRebuildStats_.durationMs = durationMs;
	lastRebuildStats_.gameCount = index_.size();
	lastRebuildStats_.excludedCount = excluded;
	lastRebuildStats_.duplicateCount = duplicates;
	lastRebuildStats_.completedAt = std::chrono::system_clock::now();
}

std::optional<std::pair<InstalledGame, Confidence>> InstallIndex::match(const std::wstring &imagePath) const
{
	if (imagePath.empty()) {
		return std::nullopt;
	}

	const std::wstring normalizedImage = NormalizePath(imagePath);

	std::lock_guard<std::mutex> lock(mutex_);

	// Step 1 (DESIGN.md 1.4): exact launchExe match. Epic/GOG's gold
	// case - the platform told us the exact binary, so this beats any
	// prefix match unconditionally.
	for (const auto &game : index_) {
		if (game.launchExe.empty()) {
			continue;
		}
		if (NormalizePath(game.launchExe) == normalizedImage) {
			return std::make_pair(game, Confidence::High);
		}
	}

	// Step 2: longest-prefix installRoot match. "Longest" matters when
	// one indexed root is a subdirectory of another (rare, but cheap to
	// get right) - the more specific root wins.
	const InstalledGame *best = nullptr;
	std::size_t bestRootLength = 0;

	for (const auto &game : index_) {
		if (game.installRoot.empty()) {
			continue;
		}
		const std::wstring normalizedRoot = NormalizePath(game.installRoot);
		if (!PathStartsWithRoot(normalizedImage, normalizedRoot)) {
			continue;
		}
		if (normalizedRoot.size() > bestRootLength) {
			bestRootLength = normalizedRoot.size();
			best = &game;
		}
	}

	if (best == nullptr) {
		return std::nullopt;
	}

	return std::make_pair(*best, ConfidenceForPlatform(best->platform));
}

std::size_t InstallIndex::indexedGameCount() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return index_.size();
}

bool InstallIndex::hasCheapChangeSignal() const
{
	std::chrono::system_clock::time_point lastBuild;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (lastRebuildStats_.completedAt.time_since_epoch().count() == 0) {
			// Never built yet - caller always does an unconditional
			// rebuild() at startup regardless, so this deliberately
			// does not try to justify that first call.
			return false;
		}
		lastBuild = lastRebuildStats_.completedAt;
	}

	for (const auto &provider : providers_) {
		const auto signal = provider->changeSignal();
		if (signal.has_value() && *signal > lastBuild) {
			return true;
		}
	}

	return false;
}

RebuildStats InstallIndex::lastRebuildStats() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return lastRebuildStats_;
}

bool InstallIndex::saveToFile(const std::wstring &jsonPath) const
{
	QJsonArray gamesArray;

	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (const auto &game : index_) {
			QJsonObject obj;
			obj[QStringLiteral("displayName")] = QString::fromStdWString(game.displayName);
			obj[QStringLiteral("installRoot")] = QString::fromStdWString(game.installRoot);
			obj[QStringLiteral("launchExe")] = QString::fromStdWString(game.launchExe);
			obj[QStringLiteral("platform")] = QString::fromWCharArray(PlatformToString(game.platform));
			obj[QStringLiteral("platformId")] = QString::fromStdWString(game.platformId);
			gamesArray.append(obj);
		}
	}

	QJsonObject root;
	root[QStringLiteral("savedAtUnixMs")] =
		static_cast<qint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
					     std::chrono::system_clock::now().time_since_epoch())
					     .count());
	root[QStringLiteral("games")] = gamesArray;

	QFile file(QString::fromStdWString(jsonPath));
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return false;
	}

	const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
	const bool ok = file.write(bytes) == bytes.size();
	file.close();
	return ok;
}

bool InstallIndex::loadFromFile(const std::wstring &jsonPath)
{
	QFile file(QString::fromStdWString(jsonPath));
	if (!file.open(QIODevice::ReadOnly)) {
		return false;
	}

	const QByteArray bytes = file.readAll();
	file.close();

	QJsonParseError parseError{};
	const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
	if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
		return false;
	}

	const QJsonObject root = doc.object();
	const QJsonValue gamesValue = root.value(QStringLiteral("games"));
	if (!gamesValue.isArray()) {
		return false;
	}

	std::vector<InstalledGame> loaded;
	for (const QJsonValue &entryValue : gamesValue.toArray()) {
		if (!entryValue.isObject()) {
			continue;
		}
		const QJsonObject entry = entryValue.toObject();

		InstalledGame game;
		game.displayName = entry.value(QStringLiteral("displayName")).toString().toStdWString();
		game.installRoot = entry.value(QStringLiteral("installRoot")).toString().toStdWString();
		game.launchExe = entry.value(QStringLiteral("launchExe")).toString().toStdWString();
		game.platform = PlatformFromString(entry.value(QStringLiteral("platform")).toString().toStdWString());
		game.platformId = entry.value(QStringLiteral("platformId")).toString().toStdWString();

		if (game.displayName.empty()) {
			continue; // Malformed entry - drop it rather than fail the whole load.
		}

		loaded.push_back(std::move(game));
	}

	// The cache on disk was written by whatever exclusion rules were in
	// force when it was saved - which, for any cache predating this
	// filter, is none at all. Filtering on load as well as on rebuild is
	// what stops a stale cache from quietly reintroducing Wallpaper
	// Engine on the next launch and making the fix look intermittent.
	std::size_t excluded = 0;
	std::size_t duplicates = 0;
	loaded = filterForIngest(std::move(loaded), &excluded, &duplicates);

	const qint64 savedAtUnixMs = root.value(QStringLiteral("savedAtUnixMs")).toVariant().toLongLong();
	const auto savedAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(savedAtUnixMs));

	std::lock_guard<std::mutex> lock(mutex_);
	index_ = std::move(loaded);
	lastRebuildStats_.durationMs = 0.0; // Loaded from cache, not measured this run.
	lastRebuildStats_.gameCount = index_.size();
	lastRebuildStats_.excludedCount = excluded;
	lastRebuildStats_.duplicateCount = duplicates;
	lastRebuildStats_.completedAt = savedAtUnixMs > 0 ? savedAt : std::chrono::system_clock::now();

	return true;
}

} // namespace signalbox::detection
