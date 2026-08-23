/*
 * SignalBox - detection/providers/EpicProvider.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Real implementation. JSON parsing uses Qt6::Core's QJsonDocument -
 * already a required dependency of this plugin (CMakeLists.txt links
 * Qt6::Core unconditionally), so this introduces no new third-party
 * dependency (see THIRD-PARTY-NOTICES.md's now-superseded note about
 * vendoring nlohmann/json - Qt's JSON support covers the same need with
 * zero extra vendoring). QJsonDocument is a plain value-type parser, safe
 * to use off the Qt main thread (see HelperDenylist.h's identical note).
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "EpicProvider.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace signalbox::detection::providers {

namespace {

namespace fs = std::filesystem;

std::wstring ProgramDataManifestsDir()
{
	wchar_t buffer[MAX_PATH];
	const DWORD len = GetEnvironmentVariableW(L"PROGRAMDATA", buffer, static_cast<DWORD>(std::size(buffer)));
	std::wstring programData = (len > 0 && len < std::size(buffer)) ? std::wstring(buffer, len) : L"C:\\ProgramData";

	if (!programData.empty() && (programData.back() == L'\\' || programData.back() == L'/')) {
		programData.pop_back();
	}

	return programData + L"\\Epic\\EpicGamesLauncher\\Data\\Manifests";
}

std::string ReadFileUtf8(const std::wstring &path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return {};
	}
	std::ostringstream buffer;
	buffer << file.rdbuf();
	return buffer.str();
}

} // namespace

std::optional<InstalledGame> ParseEpicManifest(std::string_view jsonText)
{
	const QByteArray bytes(jsonText.data(), static_cast<int>(jsonText.size()));
	QJsonParseError parseError{};
	const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);

	if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
		return std::nullopt;
	}

	const QJsonObject obj = doc.object();

	const QString displayName = obj.value(QStringLiteral("DisplayName")).toString();
	const QString installLocation = obj.value(QStringLiteral("InstallLocation")).toString();
	const QString launchExecutable = obj.value(QStringLiteral("LaunchExecutable")).toString();

	if (displayName.isEmpty() || installLocation.isEmpty()) {
		return std::nullopt;
	}

	// DESIGN.md 1.2: skip DLC entries. Only applied when both fields are
	// actually present - an absent AppName/MainGameAppName must not be
	// treated as "this is DLC" (that would silently drop the main game
	// on a manifest variant that omits them).
	if (obj.contains(QStringLiteral("AppName")) && obj.contains(QStringLiteral("MainGameAppName"))) {
		const QString appName = obj.value(QStringLiteral("AppName")).toString();
		const QString mainGameAppName = obj.value(QStringLiteral("MainGameAppName")).toString();
		if (!appName.isEmpty() && !mainGameAppName.isEmpty() && appName != mainGameAppName) {
			return std::nullopt;
		}
	}

	InstalledGame game;
	game.displayName = displayName.toStdWString();

	std::wstring installRoot = installLocation.toStdWString();
	if (!installRoot.empty() && installRoot.back() != L'\\' && installRoot.back() != L'/') {
		installRoot.push_back(L'\\');
	}
	game.installRoot = installRoot;

	// Gold case (DESIGN.md 1.2): LaunchExecutable is the exact game
	// binary, relative to InstallLocation - exact-path match in
	// InstallIndex::match(), confidence High.
	if (!launchExecutable.isEmpty()) {
		std::wstring exe = launchExecutable.toStdWString();
		// Observed on a real manifest on this machine: LaunchExecutable
		// itself can mix '/' into an otherwise Windows path (e.g.
		// "FortniteGame/Binaries/Win64/FortniteBootstrapper.exe").
		// InstallIndex::match() normalizes separators before comparing,
		// so this wouldn't break matching either way, but normalize
		// here too so the stored InstalledGame is clean Windows-style
		// data, not a mix of both.
		for (wchar_t &c : exe) {
			if (c == L'/') {
				c = L'\\';
			}
		}
		while (!exe.empty() && exe.front() == L'\\') {
			exe.erase(exe.begin());
		}
		game.launchExe = installRoot + exe;
	}

	game.platform = Platform::Epic;
	game.platformId = obj.value(QStringLiteral("AppName")).toString().toStdWString();

	return game;
}

std::vector<InstalledGame> EpicProvider::enumerate()
{
	std::vector<InstalledGame> games;

	const std::wstring manifestsDir = ProgramDataManifestsDir();

	std::error_code ec;
	if (!fs::exists(manifestsDir, ec) || ec) {
		return games; // Epic not installed - normal, not a fault (IGameProvider.h).
	}

	fs::directory_iterator it(manifestsDir, fs::directory_options::skip_permission_denied, ec);
	if (ec) {
		return games;
	}

	for (const auto &entry : it) {
		std::error_code entryEc;
		if (!entry.is_regular_file(entryEc) || entryEc) {
			continue;
		}
		if (entry.path().extension() != L".item") {
			continue;
		}

		const std::string jsonText = ReadFileUtf8(entry.path().wstring());
		if (jsonText.empty()) {
			continue;
		}

		if (auto game = ParseEpicManifest(jsonText)) {
			games.push_back(std::move(*game));
		}
	}

	return games;
}

std::wstring EpicProvider::providerId() const
{
	return L"epic";
}

std::optional<std::chrono::system_clock::time_point> EpicProvider::changeSignal() const
{
	const std::wstring manifestsDir = ProgramDataManifestsDir();

	std::error_code ec;
	const auto fsTime = fs::last_write_time(manifestsDir, ec);
	if (ec) {
		return std::nullopt;
	}

	const auto sysNow = std::chrono::system_clock::now();
	const auto fsNow = fs::file_time_type::clock::now();
	const auto delta = fsTime - fsNow;
	return sysNow + std::chrono::duration_cast<std::chrono::system_clock::duration>(delta);
}

} // namespace signalbox::detection::providers
