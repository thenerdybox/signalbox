/*
 * SignalBox - core/CategoryResolver.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * See CategoryResolver.h for the tier order, the "unmapped means no
 * action" safety property, and why this stays OBS/Qt-dependency-light.
 */

#include "CategoryResolver.h"

#include "../detection/TextMatch.h"

#include <algorithm>
#include <cwctype>
#include <set>
#include <sstream>

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace signalbox::core {

namespace {

// Delegates rather than repeating the switch. A user override is keyed
// "steam:431960" and so is an entry in data/helpers.json's exclusions
// list - two spellings of the same platform token would mean a rule
// written against one never matches the other, and nothing would fail
// loudly enough to catch it. One definition, in DetectedGame.h.
const wchar_t *PlatformKeyPrefix(detection::Platform platform)
{
	return detection::PlatformToString(platform);
}

// Splits a normalized (already-lowercased, collapsed-whitespace) string
// into a token set for tokenSetOverlap(). Deliberately a std::set (not a
// multiset/vector) - DESIGN.md 1.5 calls this "token-set overlap", and a
// repeated word (e.g. "the the") should not let a title dominate the
// score just by repetition.
std::set<std::wstring> Tokenize(const std::wstring &normalized)
{
	std::set<std::wstring> tokens;
	std::wistringstream stream(normalized);
	std::wstring token;
	while (stream >> token) {
		tokens.insert(token);
	}
	return tokens;
}

} // namespace

CategoryResolver::CategoryResolver(IUserOverrideStore &overrideStore, ICategoryLookup *lookup)
	: overrideStore_(overrideStore),
	  lookup_(lookup)
{
}

void CategoryResolver::setLookup(ICategoryLookup *lookup)
{
	lookup_ = lookup;
}

void CategoryResolver::setDiagnosticCallback(DiagnosticCallback callback)
{
	diagnosticCallback_ = std::move(callback);
}

std::wstring CategoryResolver::overrideKeyFor(const detection::InstalledGame &game)
{
	if (!game.platformId.empty()) {
		return std::wstring(PlatformKeyPrefix(game.platform)) + L":" + game.platformId;
	}
	// No stable platform id (e.g. a Generic/Uninstall-registry entry
	// whose enumerate() left platformId as the raw registry subkey but
	// the caller only has installRoot/launchExe to hand) - fall back to
	// the most specific path we have, per DESIGN.md 1.5 "or absolute
	// exe path".
	return !game.launchExe.empty() ? game.launchExe : game.installRoot;
}

std::wstring CategoryResolver::normalize(const std::wstring &name)
{
	std::wstring stripped;
	stripped.reserve(name.size());
	for (wchar_t c : name) {
		// Strip (tm)/(r)/(c) glyphs (DESIGN.md 1.5). Deliberately does
		// NOT strip edition suffixes ("Definitive Edition" etc.) -
		// DESIGN.md 1.5 calls that too risky, since the suffix is often
		// the *correct* Twitch listing.
		if (c == L'™' || c == L'®' || c == L'©') {
			continue;
		}
		stripped.push_back(c);
	}

	// Collapse whitespace runs to a single space, trim ends, casefold.
	// Light punctuation (colon, hyphen/dash, comma, apostrophe, quotes)
	// is treated as whitespace too, not just stripped-to-nothing - a
	// title separator like "Elden Ring: Shadow of the Erdtree" must
	// tokenize identically to "Elden Ring Shadow of the Erdtree" for
	// tokenSetOverlap() to score them as the near-exact match they are;
	// collapsing straight to nothing instead would glue "Ring:" and
	// "Shadow" into one bogus token instead of splitting them.
	std::wstring collapsed;
	collapsed.reserve(stripped.size());
	bool lastWasSpace = false;
	for (wchar_t c : stripped) {
		const bool isSpace = std::iswspace(static_cast<wint_t>(c)) != 0 || c == L':' || c == L'-' ||
				      c == L'–' || c == L'—' || c == L',' || c == L'\'' || c == L'"';
		if (isSpace) {
			if (!lastWasSpace && !collapsed.empty()) {
				collapsed.push_back(L' ');
			}
			lastWasSpace = true;
		} else {
			collapsed.push_back(static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c))));
			lastWasSpace = false;
		}
	}
	while (!collapsed.empty() && collapsed.back() == L' ') {
		collapsed.pop_back();
	}

	return collapsed;
}

double CategoryResolver::tokenSetOverlap(const std::wstring &normalizedA, const std::wstring &normalizedB)
{
	const std::set<std::wstring> tokensA = Tokenize(normalizedA);
	const std::set<std::wstring> tokensB = Tokenize(normalizedB);
	if (tokensA.empty() || tokensB.empty()) {
		return 0.0;
	}

	std::size_t intersectionSize = 0;
	for (const auto &token : tokensA) {
		if (tokensB.count(token) > 0) {
			++intersectionSize;
		}
	}

	std::set<std::wstring> unionSet = tokensA;
	unionSet.insert(tokensB.begin(), tokensB.end());

	// Jaccard index: |A n B| / |A u B|. Symmetric, bounded [0,1], and a
	// good match for "token-set overlap" as DESIGN.md 1.5 phrases it -
	// exact reuse of every distinct word from both titles scores 1.0,
	// disjoint titles score 0.0.
	return static_cast<double>(intersectionSize) / static_cast<double>(unionSet.size());
}

void CategoryResolver::loadAliasTable(const std::wstring &jsonPath)
{
	// Parses into local containers and only commits them at the very end,
	// once the whole file has read and parsed cleanly. This used to clear
	// aliases_/promptOnlyAliases_/patternAliases_ up front, unconditionally
	// - harmless while this only ever ran once, at plugin load, but wrong
	// the moment a Rescan can call this a second time: a missing file, a
	// transient read failure, or a typo'd trailing comma in a hand-edited
	// data/aliases.json would wipe out a perfectly good table that was
	// already loaded, silently dropping every alias mid-session instead of
	// just failing to pick up the edit. The previously-loaded table is a
	// better fallback than nothing, same principle HelperDenylist::
	// LoadFromFile already applies to its own reload (falls back to
	// BuiltInFallback() rather than an empty list) - this class has no
	// compiled-in fallback to fall back to, so "leave the live table alone"
	// is the equivalent move.
	if (jsonPath.empty()) {
		return; // No file configured - nothing to load, nothing to replace.
	}

	QFile file(QString::fromStdWString(jsonPath));
	if (!file.open(QIODevice::ReadOnly)) {
		return; // Missing/unreadable file - keep whatever table is already loaded.
	}

	const QByteArray bytes = file.readAll();
	file.close();

	QJsonParseError parseError{};
	const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
	if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
		return; // Malformed - same as above, the live table is untouched.
	}

	// Two accepted value shapes per entry, so the file stays readable for
	// the common case and still carries policy for the uncommon one:
	//
	//   "Krita": "Art"
	//   "Visual Studio Code": { "category": "Software and Game Development", "prompt": true }
	//
	// "prompt": true marks an app that must never switch the category on
	// its own - see isPromptOnly() and DetectionStateMachine's
	// PromptKind::CreativeApp. Having VS Code open is not evidence that
	// someone is streaming VS Code; it is frequently just open. The
	// string form means "switch normally" and is unchanged, so every
	// alias file written before this existed keeps working exactly as it
	// did.
	std::map<std::wstring, std::wstring> newAliases;
	std::set<std::wstring> newPromptOnlyAliases;
	std::vector<std::pair<std::wstring, AliasEntry>> newPatternAliases;

	const QJsonObject aliasesObj = doc.object().value(QStringLiteral("aliases")).toObject();
	for (auto it = aliasesObj.constBegin(); it != aliasesObj.constEnd(); ++it) {
		const std::wstring key = normalize(it.key().toStdWString());
		if (key.empty()) {
			continue;
		}

		std::wstring categoryName;
		bool promptOnly = false;

		if (it.value().isString()) {
			categoryName = it.value().toString().toStdWString();
		} else if (it.value().isObject()) {
			const QJsonObject entry = it.value().toObject();
			categoryName = entry.value(QStringLiteral("category")).toString().toStdWString();
			promptOnly = entry.value(QStringLiteral("prompt")).toBool(false);
		} else {
			continue;
		}

		if (categoryName.empty()) {
			continue;
		}

		if (key.find(L'*') != std::wstring::npos) {
			newPatternAliases.emplace_back(key, AliasEntry{categoryName, promptOnly});
		} else {
			newAliases[key] = categoryName;
			if (promptOnly) {
				newPromptOnlyAliases.insert(key);
			}
		}
	}

	// The file parsed cleanly - commit the new table in one shot. This
	// class is main-thread-only (see the header's THREADING note), so
	// there is no concurrent resolve() call that could observe a
	// half-old/half-new table mid-swap; the single-shot commit is about
	// never leaving the table half-built if this function were ever
	// extended to fail partway through in the future, not about a race
	// that exists today.
	aliases_ = std::move(newAliases);
	promptOnlyAliases_ = std::move(newPromptOnlyAliases);
	patternAliases_ = std::move(newPatternAliases);
}

std::size_t CategoryResolver::aliasCount() const
{
	return aliases_.size() + patternAliases_.size();
}

const CategoryResolver::AliasEntry *CategoryResolver::findAlias(const std::wstring &normalizedName) const
{
	const auto exact = aliases_.find(normalizedName);
	if (exact != aliases_.end()) {
		exactLookupScratch_.categoryName = exact->second;
		exactLookupScratch_.promptOnly = promptOnlyAliases_.count(normalizedName) != 0;
		return &exactLookupScratch_;
	}

	// Pattern fallback. normalize() deliberately does NOT strip edition
	// or version suffixes (DESIGN.md 1.5 - the suffix is often the
	// correct Twitch listing), which means an exact table can never keep
	// up with "Adobe After Effects 2020", "Adobe Photoshop 2026",
	// "Clip Studio Paint EX", "Ableton Live 12 Suite" and every future
	// year of each. Enumerating those was the original approach and it
	// failed exactly as you would expect: After Effects was missing, so
	// it was not prompt-only, so it silently switched someone's category
	// to it. One pattern per product replaces one entry per product per
	// year.
	//
	// First match wins, so order in the file is meaningful - put the
	// more specific pattern first.
	for (const auto &entry : patternAliases_) {
		if (detection::text::WildcardMatch(entry.first, normalizedName)) {
			return &entry.second;
		}
	}
	return nullptr;
}

std::wstring CategoryResolver::aliasCategoryFor(const detection::InstalledGame &game) const
{
	if (game.displayName.empty()) {
		return {};
	}
	const AliasEntry *entry = findAlias(normalize(game.displayName));
	return entry == nullptr ? std::wstring{} : entry->categoryName;
}

bool CategoryResolver::isPromptOnly(const detection::InstalledGame &game) const
{
	if (game.displayName.empty()) {
		return false;
	}

	// A USER OVERRIDE IS THE ANSWER TO THE QUESTION - NEVER ASK IT AGAIN.
	//
	// Tier 1 (resolve()'s first check) wins over every other tier, and it
	// has to win here too, or the override is unreachable in practice for
	// exactly the apps it exists to fix. The Generic default below asks
	// about anything that arrived without store metadata; "Fix This!"
	// exists so that question can be answered permanently. Without this
	// check the two contradict each other: the answer gets persisted, and
	// the next detection asks anyway, forever. That is what happened on
	// the first live test - an override was saved for VS Code and the log
	// still said "not switching on its own, it needs your say-so".
	//
	// Both override kinds suppress the prompt, for the same reason:
	//   - a category override means the user already said what this is,
	//     so switch to it the way a curated alias would.
	//   - ignored=true means they said "never switch for this", and
	//     resolve() returns nothing for it - prompting about an app that
	//     can only ever resolve to "change nothing" is a question whose
	//     every answer is the same.
	if (overrideStore_.findUserOverride(overrideKeyFor(game))) {
		return false;
	}

	// An explicit alias entry is a human saying what this is, so it wins
	// either way: "prompt": true asks, and the plain string form still
	// switches automatically even for a Generic entry. That is the
	// curation path for a game that only exists in the Uninstall
	// registry.
	if (const AliasEntry *entry = findAlias(normalize(game.displayName))) {
		return entry->promptOnly;
	}

	// UNCURATED UNINSTALL-REGISTRY ENTRIES ASK. THEY DO NOT SWITCH.
	//
	// Platform::Generic means no store told us anything - we found a
	// folder in Windows' Uninstall registry and nothing more. Every other
	// platform here arrived with real metadata from a games store, which
	// is evidence the thing is a game. "It has an uninstaller" is not.
	//
	// This was a denylist until a real session made it obvious that a
	// denylist cannot win. Three separate false positives in one morning
	// - Opera GX, Wallpaper Engine, then Microsoft 365 - and a look at
	// what remained indexed found 47 more of the same kind waiting their
	// turn: AMD chipset drivers, Realtek audio, Npcap, Wireshark, 7-Zip,
	// GPU-Z, WinRAR, TeamViewer. Each one would have changed a live
	// streamer's category the moment its process ran. Enumerating them
	// is an unwinnable, permanent maintenance burden, and every entry
	// missed is someone's stream mislabelled.
	//
	// So the default inverts: with no evidence it is a game, ask. That
	// costs one click, once, and the answer can be made permanent. The
	// old default cost a wrong category on a live stream, silently.
	// Steam, Epic, GOG and Ubisoft entries are untouched and still switch
	// automatically - they came with the evidence.
	//
	// The exclusions in helpers.json still matter, but their job changed:
	// they now suppress the QUESTION for things nobody would ever stream,
	// rather than being the only thing standing between a driver package
	// and someone's category.
	return game.platform == detection::Platform::Generic;
}

void CategoryResolver::setPromptOnlyForTesting(const std::set<std::wstring> &normalizedNames)
{
	promptOnlyAliases_ = normalizedNames;
}

void CategoryResolver::setAliasTableForTesting(const std::map<std::wstring, std::wstring> &normalizedNameToCategoryName)
{
	aliases_ = normalizedNameToCategoryName;
}

void CategoryResolver::resolve(const detection::InstalledGame &game, const ResolveCallback &callback)
{
	// --- Tier 1: persistent user overrides - highest priority. ---
	const std::wstring key = overrideKeyFor(game);
	if (const auto override = overrideStore_.findUserOverride(key)) {
		if (override->ignored || override->categoryId.empty()) {
			// "Ignore this exe entirely" (DESIGN.md 1.5) resolves to
			// nothing, indistinguishably from an unmapped game - both
			// mean "change nothing".
			callback(std::nullopt);
		} else {
			callback(ResolvedCategory{override->categoryId, override->categoryName});
		}
		return;
	}

	// --- Tier 2: bundled alias table. ---
	const std::wstring normalizedName = normalize(game.displayName);
	if (const AliasEntry *alias = findAlias(normalizedName)) {
		// findAlias() covers both the exact table and the wildcard
		// patterns, so accepting a creative-app prompt for something
		// matched by pattern actually resolves to a category. Reading
		// the exact map here instead would resolve nothing for exactly
		// the entries patterns exist to cover.
		callback(ResolvedCategory{std::wstring(), alias->categoryName});
		return;
	}

	// --- Tier 3: Twitch Helix lookup (async). ---
	if (!lookup_) {
		// No lookup configured - resolve to nothing rather than guess
		// (DESIGN.md 1.5's core safety property). This is the harness's
		// default path when exercising tiers 1/2 in isolation.
		callback(std::nullopt);
		return;
	}

	// QUERY TWITCH WITHOUT THE TRADEMARK GLYPHS.
	//
	// Store display names carry (tm)/(r)/(c) and Twitch category names
	// never do. normalize() already knows this - it strips them before any
	// comparison - but the string sent OUT was the raw display name, so
	// the scoring below only ever ran on whatever a query full of glyphs
	// happened to return. Seen in a real session: Steam's "STAR WARS(tm):
	// The Old Republic(tm)" resolved to nothing, while Twitch lists it as
	// "Star Wars: The Old Republic".
	//
	// Only the glyphs are removed, not normalize()'s casefolding and
	// punctuation collapsing - this is a search query for a third-party
	// service, and stripping characters it might legitimately match on
	// would trade one miss for another. Case is irrelevant to Twitch's
	// search; the glyphs are not.
	std::wstring exactQuery;
	exactQuery.reserve(game.displayName.size());
	for (wchar_t c : game.displayName) {
		if (c != L'™' && c != L'®' && c != L'©') {
			exactQuery.push_back(c);
		}
	}
	lookup_->findExact(exactQuery, [this, exactQuery, normalizedName, callback](std::optional<ResolvedCategory> exact) {
		if (exact) {
			callback(exact);
			return;
		}

		lookup_->search(exactQuery, [this, exactQuery, normalizedName, callback](std::vector<ResolvedCategory> results) {
			// Score: case-insensitive exact > exact-after-normalizing-
			// both > token-set overlap. Below kFuzzyMatchThreshold ->
			// unmapped (DESIGN.md 1.5) - never guess.
			const ResolvedCategory *best = nullptr;
			double bestScore = 0.0;
			for (const auto &candidate : results) {
				const std::wstring normalizedCandidate = normalize(candidate.categoryName);
				double score;
				if (normalizedCandidate == normalizedName) {
					score = 1.0; // Exact-after-normalizing-both.
				} else {
					score = tokenSetOverlap(normalizedCandidate, normalizedName);
				}
				if (score > bestScore) {
					bestScore = score;
					best = &candidate;
				}
			}

			if (best && bestScore >= kFuzzyMatchThreshold) {
				callback(*best);
				return;
			}

			// Say which step actually failed. The three misses below
			// look identical from the outside - the category simply
			// does not change - but they need completely different
			// responses: nothing found means the query is wrong, a
			// near miss means the threshold or the name is wrong, and
			// either can be settled permanently with "Fix This!".
			if (diagnosticCallback_) {
				if (results.empty()) {
					diagnosticCallback_(L"Twitch returned no categories for \"" + exactQuery +
							    L"\" - use \"Fix This!\" to set one for this app");
				} else if (best) {
					diagnosticCallback_(L"Closest Twitch category to \"" + exactQuery + L"\" was \"" +
							    best->categoryName + L"\", not close enough to use - use \"Fix This!\" to set one");
				} else {
					diagnosticCallback_(L"Twitch returned categories for \"" + exactQuery +
							    L"\" but none scored at all - use \"Fix This!\" to set one");
				}
			}
			callback(std::nullopt);
		});
	});
}

} // namespace signalbox::core
