/*
 * SignalBox - detection/HelperDenylist.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Loads and evaluates the helper/launcher denylist described in
 * DESIGN.md 1.4 step 3 and the project brief's item 3: a running process
 * survives past the install-index match only to be discarded here if it
 * is a documented crash handler, installer/uninstaller, redistributable,
 * anti-cheat service, or store launcher rather than the game itself.
 *
 * The denylist ships as data (data/helpers.json), not hardcoded, so a
 * false-positive/false-negative report can be fixed with a data PR
 * instead of a plugin rebuild - see that file's _comment and README.md.
 * This class is the thing that consumes it at runtime, with a compiled-in
 * fallback (BuiltInFallback(), mirroring data/helpers.json's seed
 * content exactly) for when the file is missing, unreadable, or
 * malformed - the plugin must never fail to filter helpers just because
 * a data file got corrupted or deleted.
 *
 * OWNERSHIP NOTE: this class only loads and evaluates the list. Nothing
 * in src/detection/ calls obs_module_file() or touches obs_log - resolving
 * data/helpers.json's actual on-disk path (obs_module_file("helpers.json"))
 * and handing the resulting path to LoadFromFile() is the job of whichever
 * OBS-aware layer owns that (see DetectionEngine.h's threading-boundary
 * note: no obs_* calls from the worker thread's translation units except
 * plugin-main.cpp). This header stays OBS- and UI-free by design.
 *
 * Deliberately header-only for the same reason as KeyValuesParser.h: it
 * avoids adding a new translation unit to the shared CMakeLists.txt
 * target_sources list while other agents may be concurrently editing
 * that same file for src/twitch and src/ui additions.
 *
 * JSON parsing uses Qt6::Core's QJsonDocument/QJsonObject/QJsonArray -
 * already a required dependency of this plugin (CMakeLists.txt links
 * Qt6::Core unconditionally), so this introduces no new third-party
 * dependency. QJsonDocument is a plain value-type parser with no event
 * loop or GUI requirement, so it is safe to use from a worker thread or
 * a standalone test program - it is not one of the QObject/QWidget/QNAM
 * types DetectionEngine.h's threading boundary warns about.
 *
 * TESTABLE: ParseJson() takes a string and returns a value; no filesystem
 * access. LoadFromFile() is a thin wrapper that does the one file read
 * and falls back to BuiltInFallback() on any failure - exercise ParseJson()
 * directly in tests, LoadFromFile() only needs a real file on disk to
 * prove the fallback path.
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include "IndexExclusions.h"
#include "TextMatch.h"

namespace signalbox::detection {

class HelperDenylist {
public:
	// pathFragments: substrings (case-insensitive) that mark a helper
	// when found anywhere in the full image path, e.g. "\_CommonRedist\".
	// helperBasenames / launcherBasenames: exe basenames (case-insensitive),
	// each optionally containing '*' wildcards matching any run of
	// characters (e.g. "UnityCrashHandler*.exe", "*setup*.exe") - the
	// same glob syntax data/helpers.json's seed entries use.
	std::vector<std::wstring> pathFragments;
	std::vector<std::wstring> helperBasenames;
	std::vector<std::wstring> launcherBasenames;

	// The "installed, but never the thing being streamed" rules - see
	// IndexExclusions.h for why this is a separate concept from the
	// alias table. Two halves, applied at two different points:
	//
	//   excludedBasenames / excludedPathFragments reject a PROCESS in
	//   scanAndRank()'s cheap basename-only pass, before the poll pays
	//   for OpenProcess + QueryFullProcessImageNameW. This half exists
	//   for cost, not correctness: a browser is already excluded from
	//   the index, but Chrome alone spawns a dozen-plus processes and
	//   opening a handle to every one of them, every poll, forever, is
	//   exactly the kind of ambient cost this plugin has promised not
	//   to impose on a machine that is also running a game.
	//
	//   indexExclusions rejects an INDEX ENTRY at rebuild/load time, so
	//   the excluded app cannot be matched, ranked, aliased, or set as
	//   a category at all. That half is the correctness one.
	std::vector<std::wstring> excludedBasenames;
	std::vector<std::wstring> excludedPathFragments;
	IndexExclusions indexExclusions;

	// The same seed content as data/helpers.json's initial version,
	// compiled in. Used whenever the data file can't be read - see the
	// class comment. Keep this in sync with data/helpers.json's seed
	// entries; drift between the two only matters for the fallback path,
	// so it is not a correctness bug, just worth noticing in review.
	static HelperDenylist BuiltInFallback()
	{
		HelperDenylist list;

		list.pathFragments = {
			L"\\_commonredist\\",
			L"\\redist\\",
			L"\\directx\\",
			L"\\vcredist",
		};

		list.helperBasenames = {
			L"unitycrashhandler*.exe", L"crashreportclient.exe", L"crashpad_handler.exe",
			L"easyanticheat*.exe",     L"beservice*.exe",        L"unins*.exe",
			L"*setup*.exe",            L"*installer*.exe",       L"dxsetup.exe",
			L"vconsole*.exe",
		};

		list.launcherBasenames = {
			L"steam.exe",     L"steamwebhelper.exe", L"epicgameslauncher.exe", L"upc.exe",
			L"ubisoftconnect.exe", L"galaxyclient.exe",   L"eadesktop.exe",         L"battle.net.exe",
			L"riotclientservices.exe",
		};

		// Browsers first, and every one of them, because the owner's
		// rule is "any browser" - a browser is never the thing being
		// streamed, and browsers are the worst offenders for process
		// count. The rest are the always-on background apps that share
		// the same property: wallpaper animators, overlays, voice chat,
		// hardware vendor tooling, and OBS itself.
		list.excludedBasenames = {
			L"chrome.exe",         L"msedge.exe",       L"msedgewebview2.exe",
			L"firefox.exe",        L"opera.exe",        L"opera_gx.exe",
			L"opera_crashreporter.exe", L"brave.exe",   L"vivaldi.exe",
			L"arc.exe",            L"iexplore.exe",     L"chromium.exe",
			L"librewolf.exe",      L"waterfox.exe",     L"zen.exe",
			L"tor.exe",            L"duckduckgo.exe",

			L"wallpaper32.exe",    L"wallpaper64.exe",  L"wallpaperservice32*.exe",
			L"lively.exe",         L"livelyservice.exe",

			L"discord.exe",        L"discordptb.exe",   L"discordcanary.exe",
			L"slack.exe",          L"teams.exe",        L"ms-teams.exe",
			L"zoom.exe",           L"telegram.exe",     L"whatsapp.exe",
			L"signal.exe",

			L"obs64.exe",          L"obs32.exe",        L"streamlabs obs.exe",
			L"streamdeck.exe",     L"voicemeeter*.exe", L"vtubestudio.exe",

			L"spotify.exe",        L"vlc.exe",          L"mpc-hc64.exe",
			L"itunes.exe",         L"applemusic.exe",

			L"msiafterburner.exe", L"rtss.exe",         L"rtsshooks*.exe",
			L"nvcontainer.exe",    L"nvidia app.exe",   L"nvidia share.exe",
			L"radeonsoftware.exe", L"amdow.exe",
			L"logioverlay*.exe",   L"lghub.exe",        L"razer*.exe",
			L"icue.exe",           L"corsair*.exe",     L"synapse*.exe",

			L"explorer.exe",       L"searchhost.exe",   L"shellexperiencehost.exe",
			L"startmenuexperiencehost.exe",             L"textinputhost.exe",
			L"applicationframehost.exe",                L"systemsettings.exe",
			L"taskmgr.exe",        L"dwm.exe",          L"csrss.exe",
		};

		list.excludedPathFragments = {
			L"\\windows\\system32\\",
			L"\\windows\\syswow64\\",
			L"\\windowsapps\\microsoft.windows.",
		};

		// Keyed by stable platform id wherever one exists - see
		// IndexExclusions.h on why a display name with a version in it
		// is a rule with an expiry date.
		list.indexExclusions.platformIds = {
			L"steam:431960",  // Wallpaper Engine
			L"steam:228980",  // Steamworks Common Redistributables
			L"steam:1070560", // Steam Linux Runtime 1.0
			L"steam:1391110", // Steam Linux Runtime 2.0
			L"steam:1628350", // Steam Linux Runtime 3.0
			L"steam:1826330", // Proton EasyAntiCheat Runtime
			L"steam:2180100", // Proton Hotfix
			L"steam:250820",  // SteamVR
			L"steam:323910",  // Steam Controller drivers
		};

		list.indexExclusions.displayNames = {
			L"wallpaper engine*",
			L"lively wallpaper*",

			L"opera*",
			L"google chrome*",
			L"mozilla firefox*",
			L"microsoft edge*",
			L"brave*",
			L"vivaldi*",
			L"chromium*",
			L"tor browser*",

			L"discord*",
			L"slack*",
			L"microsoft teams*",
			L"zoom*",
			L"telegram*",

			L"obs studio*",
			L"streamlabs*",
			L"elgato*",
			L"voicemeeter*",
			L"vtube studio*",

			L"spotify*",
			L"vlc media player*",
			L"itunes*",

			L"nvidia*",
			L"amd software*",
			L"radeon software*",
			L"msi afterburner*",
			L"rivatuner*",
			L"logitech*",
			L"razer*",
			L"corsair*",
			L"steelseries*",

			L"microsoft visual c++*",
			L"microsoft .net*",
			L"windows software development kit*",
			L"directx*",
			L"*redistributable*",
			L"steam linux runtime*",
			L"proton *",
			L"steamvr*",
		};

		list.indexExclusions.installRootFragments = {
			L"\\windows\\system32\\",
			L"\\windows\\syswow64\\",
		};

		return list;
	}

	// Parses helpers.json's schema:
	//   { "pathFragments": [...], "helperBasenames": [...], "launcherBasenames": [...] }
	// Any missing/wrong-typed field is left empty rather than failing the
	// whole parse - a data file that only overrides one array is valid.
	// Sets *outOk to false (if non-null) when the top-level document
	// itself isn't a valid JSON object, so LoadFromFile() knows to fall
	// back to BuiltInFallback() entirely rather than returning a
	// suspiciously-empty list.
	static HelperDenylist ParseJson(std::string_view jsonText, bool *outOk = nullptr)
	{
		HelperDenylist list;

		const QByteArray bytes(jsonText.data(), static_cast<int>(jsonText.size()));
		QJsonParseError parseError{};
		const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);

		if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
			if (outOk != nullptr) {
				*outOk = false;
			}
			return list;
		}

		const QJsonObject root = doc.object();
		list.pathFragments = ReadStringArray(root, QStringLiteral("pathFragments"));
		list.helperBasenames = ReadStringArray(root, QStringLiteral("helperBasenames"));
		list.launcherBasenames = ReadStringArray(root, QStringLiteral("launcherBasenames"));

		// "exclusions" is optional and each of its own fields is
		// optional too, same as every array above: a data file that
		// only overrides one thing stays valid. A file written before
		// this section existed simply parses with no exclusions, which
		// is the pre-exclusions behavior exactly - it does not become
		// an error.
		const QJsonValue exclusionsValue = root.value(QStringLiteral("exclusions"));
		if (exclusionsValue.isObject()) {
			const QJsonObject exclusions = exclusionsValue.toObject();
			list.excludedBasenames = ReadStringArray(exclusions, QStringLiteral("basenames"));
			list.excludedPathFragments = ReadStringArray(exclusions, QStringLiteral("pathFragments"));
			list.indexExclusions.platformIds = ReadStringArray(exclusions, QStringLiteral("platformIds"));
			list.indexExclusions.displayNames = ReadStringArray(exclusions, QStringLiteral("displayNames"));
			list.indexExclusions.installRootFragments =
				ReadStringArray(exclusions, QStringLiteral("installRootFragments"));
		}

		if (outOk != nullptr) {
			*outOk = true;
		}
		return list;
	}

	// Reads jsonPath (a full filesystem path, UTF-16) and parses it with
	// ParseJson(). Falls back to BuiltInFallback() if the file can't be
	// opened, isn't valid UTF-8/JSON, or isn't a JSON object - "missing
	// or malformed" per the project brief's requirement. Never returns
	// an empty list as a hard failure mode.
	static HelperDenylist LoadFromFile(const std::wstring &jsonPath)
	{
		QFile file(QString::fromStdWString(jsonPath));
		if (!file.open(QIODevice::ReadOnly)) {
			return BuiltInFallback();
		}

		const QByteArray bytes = file.readAll();
		file.close();

		bool ok = false;
		HelperDenylist list = ParseJson(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())), &ok);
		if (!ok) {
			return BuiltInFallback();
		}
		return list;
	}

	// True if the given process should be excluded from candidacy
	// (DESIGN.md 1.4 step 3): its full image path contains a denylisted
	// fragment, or its basename matches a helper/launcher pattern.
	// fullImagePath may be empty (basename-only check still applies).
	// Case-insensitive throughout, matching Windows' own filesystem
	// semantics.
	bool isHelperOrLauncher(const std::wstring &fullImagePath, const std::wstring &exeBasename) const
	{
		const std::wstring lowerPath = ToLower(fullImagePath);
		const std::wstring lowerBase = ToLower(exeBasename);

		for (const auto &fragment : pathFragments) {
			if (!lowerPath.empty() && lowerPath.find(ToLower(fragment)) != std::wstring::npos) {
				return true;
			}
		}

		for (const auto &pattern : helperBasenames) {
			if (WildcardMatch(ToLower(pattern), lowerBase)) {
				return true;
			}
		}

		for (const auto &pattern : launcherBasenames) {
			if (WildcardMatch(ToLower(pattern), lowerBase)) {
				return true;
			}
		}

		return false;
	}

	// True if this process should be skipped entirely - the union of the
	// helper/launcher filter and the excluded-app filter. This is what
	// the per-poll scan calls; the two predicates below stay separate
	// because they answer different questions and their existing tests
	// assert on them individually.
	//
	// fullImagePath may be empty, which is exactly how scanAndRank()
	// uses it on the first pass: basename-only, before deciding whether
	// this process is even worth an OpenProcess call.
	bool shouldIgnoreProcess(const std::wstring &fullImagePath, const std::wstring &exeBasename) const
	{
		if (isHelperOrLauncher(fullImagePath, exeBasename)) {
			return true;
		}
		return isExcludedApp(fullImagePath, exeBasename);
	}

	// True if the process is a known non-game app (browser, wallpaper
	// animator, overlay, voice chat, vendor tooling) - see
	// IndexExclusions.h for why these are excluded rather than aliased.
	bool isExcludedApp(const std::wstring &fullImagePath, const std::wstring &exeBasename) const
	{
		const std::wstring lowerBase = text::ToLower(exeBasename);
		for (const auto &pattern : excludedBasenames) {
			if (text::WildcardMatch(text::ToLower(pattern), lowerBase)) {
				return true;
			}
		}

		if (!fullImagePath.empty()) {
			const std::wstring lowerPath = text::ToLower(fullImagePath);
			for (const auto &fragment : excludedPathFragments) {
				if (lowerPath.find(text::ToLower(fragment)) != std::wstring::npos) {
					return true;
				}
			}
		}

		return false;
	}

	// Total rule count across every list this denylist carries (path
	// fragments, helper/launcher basenames, both excluded* lists, and
	// every IndexExclusions list). Diagnostics only, same spirit as
	// InstallIndex::RebuildStats - it exists so a Rescan that just
	// re-read helpers.json from disk can say how many rules it picked up
	// rather than merely that it ran. See DetectionEngine.h's
	// IndexRebuild::denylistRuleCount for where this is surfaced.
	std::size_t ruleCount() const
	{
		return pathFragments.size() + helperBasenames.size() + launcherBasenames.size() +
		       excludedBasenames.size() + excludedPathFragments.size() +
		       indexExclusions.platformIds.size() + indexExclusions.displayNames.size() +
		       indexExclusions.installRootFragments.size();
	}

	// True if exeBasename matches a launcher pattern specifically (used
	// where callers need to tell "helper/crash-handler" apart from
	// "the store launcher itself" - e.g. surfacing different log text).
	// Belt-and-braces per DESIGN.md 1.4 step 3: launchers additionally
	// never match a game's installRoot, so this is a second, independent
	// line of defense, not the only one.
	bool isLauncher(const std::wstring &exeBasename) const
	{
		const std::wstring lowerBase = ToLower(exeBasename);
		for (const auto &pattern : launcherBasenames) {
			if (WildcardMatch(ToLower(pattern), lowerBase)) {
				return true;
			}
		}
		return false;
	}

private:
	static std::vector<std::wstring> ReadStringArray(const QJsonObject &root, const QString &key)
	{
		std::vector<std::wstring> out;
		const QJsonValue value = root.value(key);
		if (!value.isArray()) {
			return out;
		}
		for (const QJsonValue &entry : value.toArray()) {
			if (entry.isString()) {
				out.push_back(entry.toString().toStdWString());
			}
		}
		return out;
	}

	// Both of these now live in TextMatch.h so IndexExclusions can share
	// exactly one definition - see that header. Kept as thin private
	// aliases so this class's existing call sites and tests read the
	// same as they did before.
	static std::wstring ToLower(const std::wstring &s) { return text::ToLower(s); }

	static bool WildcardMatch(const std::wstring &pattern, const std::wstring &textValue)
	{
		return text::WildcardMatch(pattern, textValue);
	}
};

} // namespace signalbox::detection
