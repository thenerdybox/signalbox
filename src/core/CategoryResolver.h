/*
 * SignalBox - core/CategoryResolver.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * DESIGN.md 1.5: turns a DetectedGame's platform-supplied display name
 * into a Twitch category, via three tiers, first hit wins:
 *   1. Persistent user overrides (IUserOverrideStore - PluginConfig in the
 *      real plugin) - "this game is always X", or "ignore this exe
 *      entirely".
 *   2. Bundled alias table (data/aliases.json - same data-not-code
 *      principle as data/helpers.json, so community fixes are data PRs).
 *      Ships with the non-game mappings DESIGN.md calls out by name
 *      (Photoshop -> Art, Ableton -> Music, VS Code -> Software and Game
 *      Development, ...).
 *   3. Twitch Helix lookup (ICategoryLookup - GET /helix/games exact match,
 *      then GET /helix/search/categories fuzzy) - async, only reached if
 *      neither of the above hit.
 *
 * UNMAPPED MEANS NO ACTION - this is a core safety property, not a UX
 * inconvenience to route around. resolve() calls its callback with
 * std::nullopt whenever nothing resolves the game with adequate
 * confidence: an "ignore this exe" override, an alias/exact/fuzzy miss, or
 * a fuzzy match scoring below kFuzzyMatchThreshold. The caller's only
 * correct response to std::nullopt is "change nothing, let the UI ask" -
 * never guess a category (DESIGN.md 1.5).
 *
 * TIER 3 SUCCESSFUL RESOLUTIONS ARE NOT RE-CACHED HERE. TwitchClient
 * already persists a permanent category-lookup cache
 * (category-cache.json - see TwitchClient.h's class doc comment) keyed by
 * the exact name/query CategoryResolver passes it, so a real
 * ICategoryLookup implementation wrapping TwitchClient gets "hit Helix
 * once per game, ever" for free without this class needing a second
 * cache. See the project report for TwitchCategoryLookup, the concrete
 * adapter that will eventually wrap twitch::TwitchClient - not built yet,
 * since wiring it in requires the CategoryDock<->TwitchClient coordinator
 * that is out of scope for this pass (see report).
 *
 * DELIBERATELY OBS/QT-DEPENDENCY-LIGHT, MIRRORING DetectionStateMachine:
 * this class depends only on IUserOverrideStore and ICategoryLookup -
 * both pure interfaces - plus QJsonDocument for parsing the bundled alias
 * table (Qt6::Core only: no event loop, no network, no GUI - the same
 * choice HelperDenylist.h and InstallIndex.cpp already made, and that
 * header explicitly sanctions for "a worker thread or a standalone test
 * program"). This is what lets CategoryResolver be driven end-to-end by a
 * standalone harness alongside DetectionStateMachine, without an OBS
 * process or a live Twitch connection - see tests/harness/main.cpp.
 *
 * THREADING: intended for the Qt main thread (it is driven by
 * DetectionStateMachine::Listener::onSwitchIn, which only ever fires
 * there - see DetectionStateMachine.h). Not used from DetectionEngine's
 * worker thread. resolve() may invoke its callback synchronously (tier 1
 * or 2 hit, or no lookup_ configured) or asynchronously via lookup_ (tier
 * 3) - callers must not assume either.
 */

#pragma once

#include <functional>
#include <map>
#include <set>
#include <utility>
#include <optional>
#include <string>
#include <vector>

#include "../detection/DetectedGame.h"
#include "UserOverrideStore.h"

namespace signalbox::core {

// A resolved Twitch category. categoryId may be empty for a tier-2 (alias
// table) resolution that only names the category - data/aliases.json
// deliberately stores names, not ids (ids can change per-environment/
// region; names are what's hand-curated). A caller that needs an id
// before it can PATCH resolves the name via
// TwitchClient::findCategoryByExactName() first - that call is itself
// free after the first time, per TwitchClient's own permanent cache.
struct ResolvedCategory {
	std::wstring categoryId;
	std::wstring categoryName;
};

// Tier-3 (Twitch Helix) lookup abstraction. The real implementation
// adapts twitch::TwitchClient's async signals to these callback-style
// methods; that adapter is not built in this pass (see this header's
// class doc comment) - resolve() with lookup_ == nullptr simply skips
// tier 3 and falls through to "unmapped", which is always a safe outcome.
class ICategoryLookup {
public:
	virtual ~ICategoryLookup() = default;

	// GET /helix/games?name=<exact>. onResult is invoked exactly once
	// (sync or async) with the match, or std::nullopt on a miss.
	virtual void findExact(const std::wstring &name, std::function<void(std::optional<ResolvedCategory>)> onResult) = 0;

	// GET /helix/search/categories?query=<query>. onResult is invoked
	// exactly once (sync or async) with every result Twitch returned -
	// CategoryResolver applies DESIGN.md 1.5's scoring/threshold itself
	// so every ICategoryLookup implementation (including test fakes)
	// shares one scoring policy.
	virtual void search(const std::wstring &query, std::function<void(std::vector<ResolvedCategory>)> onResult) = 0;
};

class CategoryResolver {
public:
	// Fuzzy search results scoring below this are "no match" (DESIGN.md
	// 1.5's own worked threshold: "token-set overlap >= 0.8"). Never
	// lowered to paper over a bad match - see class doc comment.
	static constexpr double kFuzzyMatchThreshold = 0.8;

	// overrideStore must outlive this object (PluginConfig, owned by
	// whichever coordinator constructs both). lookup may be nullptr (see
	// class doc comment) and must outlive this object if non-null;
	// ownership stays with the caller in both cases - same pattern as
	// DetectionStateMachine::Listener.
	explicit CategoryResolver(IUserOverrideStore &overrideStore, ICategoryLookup *lookup = nullptr);

	// Attaches (or clears, with nullptr) tier 3's lookup after
	// construction. Exists because the real ICategoryLookup
	// implementation (twitch::TwitchCategoryLookup, wrapping
	// twitch::TwitchClient) cannot exist until Twitch auth has completed
	// - CategoryResolver itself is constructed much earlier (plugin
	// load), with lookup_ == nullptr, so tier 3 simply contributes
	// nothing (the same safe "unmapped" behavior as always) until the
	// coordinator calls this once a client exists. Not owned; must
	// outlive this object or be cleared with another setLookup(nullptr)
	// call first - same ownership pattern as the constructor argument.
	void setLookup(ICategoryLookup *lookup);

	// Optional narration of WHY a tier-3 lookup resolved to nothing.
	//
	// "Couldn't map that game to a Twitch category" is a dead end: it says
	// a mapping failed and nothing about which step failed, so diagnosing
	// one meant reading this file. A real case made that concrete - a game
	// whose store name carried trademark glyphs was searched for verbatim,
	// Twitch returned nothing, and the only visible symptom was a category
	// that would not change, which reads like a stuck state machine rather
	// than a missed lookup. It was reported as one.
	//
	// So a miss now says what it asked for and what came back. Optional
	// and nullptr by default: this is a UI/logging concern, and wiring it
	// in is the OBS-aware caller's job - CategoryResolver stays free of
	// obs_log so tests/harness/main.cpp can keep driving it with no OBS
	// process (same reason ICategoryLookup is a seam at all).
	using DiagnosticCallback = std::function<void(const std::wstring &detail)>;
	void setDiagnosticCallback(DiagnosticCallback callback);

	// Parses jsonPath (data/aliases.json's schema - see that file's
	// _comment) and replaces the in-memory alias table IN ONE SHOT, only
	// once the whole file has parsed successfully - see the .cpp for why.
	// Path resolution (obs_module_file) is the OBS-aware caller's job,
	// same OWNERSHIP pattern as HelperDenylist::LoadFromFile - this class
	// never calls obs_* itself.
	//
	// Safe to call more than once - a mid-session Rescan re-reads
	// data/aliases.json exactly this way, from the Qt main thread (see
	// plugin-main.cpp's rescanRequested handling; CategoryResolver is
	// main-thread-only per this class's own THREADING note, so that
	// reload never touches DetectionEngine's worker thread). An empty
	// path, an unreadable file, or malformed JSON leaves whatever table
	// is already loaded untouched rather than clearing it - on the very
	// first call (plugin load) that is indistinguishable from "tier 2
	// contributes nothing," same as before; on a later reload it means a
	// bad edit to the data file does not silently erase every alias a
	// working file had already loaded.
	void loadAliasTable(const std::wstring &jsonPath);

	// Total alias entries currently loaded (exact-table entries plus
	// wildcard patterns) - diagnostics only, for reporting how many
	// patterns a reload actually picked up. See DetectionEngine.h's
	// IndexRebuild::denylistRuleCount for the equivalent on the denylist
	// side.
	std::size_t aliasCount() const;

	// True if this entry is marked "prompt": true in the alias table -
	// a creative/dev app that must be OFFERED as a category rather than
	// applied automatically. Someone with VS Code or Photoshop open is
	// usually not streaming it; they left it open. Auto-switching there
	// is the single most annoying thing this plugin could do, and it is
	// also the case where asking costs nothing, because the app is not
	// going anywhere.
	//
	// Deliberately a query on the resolver rather than a field on
	// InstalledGame: which apps are prompt-only is alias-table policy,
	// editable as data, not a property of what a store reported as
	// installed. DetectionStateMachine reaches it through the predicate
	// CategoryDock installs - see setPromptOnlyPredicate().
	bool isPromptOnly(const detection::InstalledGame &game) const;

	// The alias table's category name for this entry, or empty if it has
	// none. Purely local - reads the table already in memory and makes
	// no Twitch call, which is what lets the creative-app prompt name
	// the category it is offering the instant it appears, rather than
	// popping up and then filling itself in a second later.
	std::wstring aliasCategoryFor(const detection::InstalledGame &game) const;

	// Test seam, matching setAliasTableForTesting(). Keys must already
	// be normalize()d.
	void setPromptOnlyForTesting(const std::set<std::wstring> &normalizedNames);

	// Test/harness seeding path - bypasses file I/O entirely. Real
	// callers should prefer loadAliasTable().
	void setAliasTableForTesting(const std::map<std::wstring, std::wstring> &normalizedNameToCategoryName);

	// Resolves game -> Twitch category per the three tiers above.
	// callback is invoked EXACTLY ONCE. std::nullopt means "unmapped -
	// change nothing" (see class doc comment); callers must never
	// substitute a guess.
	using ResolveCallback = std::function<void(std::optional<ResolvedCategory>)>;
	void resolve(const detection::InstalledGame &game, const ResolveCallback &callback);

	// Tier-1 override key for a given game: "platform:platformId"
	// (preferred - stable across path/casing noise) or, when
	// platformId is empty, the absolute exe path / install root as a
	// best-effort fallback. Exposed so callers building the "this game
	// is always X" dock action use the exact same key resolve() will
	// look up later.
	static std::wstring overrideKeyFor(const detection::InstalledGame &game);

	// Exposed for the harness/tests: DESIGN.md 1.5's normalization
	// ("strip (tm)/(r)/(c), collapse whitespace") and the fuzzy scoring
	// function, so their behavior can be asserted directly rather than
	// only indirectly through resolve().
	static std::wstring normalize(const std::wstring &name);
	static double tokenSetOverlap(const std::wstring &normalizedA, const std::wstring &normalizedB);

private:
	IUserOverrideStore &overrideStore_;
	ICategoryLookup *lookup_; // Not owned; may be nullptr.
	DiagnosticCallback diagnosticCallback_; // nullptr unless a caller wants miss detail - see setDiagnosticCallback().

	std::map<std::wstring, std::wstring> aliases_; // normalize(displayName) -> category name.
	// One resolved alias, from either the exact table or a wildcard
	// pattern. Patterns exist because normalize() deliberately keeps
	// version and edition suffixes, so an exact table needs one entry per
	// product PER YEAR - which is how "Adobe After Effects 2020" ended up
	// unlisted, therefore not prompt-only, therefore switching someone's
	// category to it automatically.
	struct AliasEntry {
		std::wstring categoryName;
		bool promptOnly = false;
	};

	// Exact table first, then patterns in file order (first match wins).
	// Returns nullptr when nothing matches.
	const AliasEntry *findAlias(const std::wstring &normalizedName) const;

	std::vector<std::pair<std::wstring, AliasEntry>> patternAliases_;
	mutable AliasEntry exactLookupScratch_; // Storage for findAlias()'s exact-table return.
	std::set<std::wstring> promptOnlyAliases_; // normalize()d keys of alias entries carrying "prompt": true.
};

} // namespace signalbox::core
