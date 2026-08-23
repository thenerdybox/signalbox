/*
 * SignalBox - core/CategorySwitchCoordinator.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Owns the sequence DetectionStateMachine::Listener::onSwitchIn()/
 * onApplyFallback() only ever decided on, never performed (see
 * ui/CategoryDock.h's "STILL NOT WIRED" note and CategoryResolver.h's
 * class doc comment): resolve a detected game to a Twitch category
 * (CategoryResolver, DESIGN.md 1.5), check nothing else changed the
 * channel since we last touched it (DESIGN.md 3.5), PATCH the category,
 * and - on success, while live - drop a stream marker naming the game
 * (DESIGN.md Section 5). Every step reports back through ActivityCallback
 * so the dock's activity log stays the one place a streamer looks to
 * understand what this plugin just did.
 *
 * DEPENDS ON IChannelClient, NOT twitch::TwitchClient DIRECTLY: this
 * mirrors CategoryResolver's own ICategoryLookup seam (DESIGN.md 1.5 tier
 * 3) for the same reason - it lets this class be driven end-to-end by
 * tests/harness/main.cpp without linking obs-module.h or Qt6::Network.
 * See twitch/TwitchChannelClient.h for the concrete adapter that wraps
 * the real TwitchClient in the actual plugin.
 *
 * EXTERNAL-WRITER GUARD (DESIGN.md 3.5, event-driven per the owner's "no
 * polling" directive - ~1 extra GET per switch instead of a 120s ambient
 * poll, see TimingConstants.h's "NOTE ON EXTERNAL-WRITER DETECTION"):
 * immediately before every PATCH this class issues, it calls
 * IChannelClient::getChannelInfo() and compares the result against the
 * category id it last SUCCESSFULLY set ITSELF (lastAppliedCategoryId_ -
 * not TwitchClient's own redundancy-guard state, and not whatever
 * DetectionStateMachine believes is live, which is a detection fact
 * independent of Twitch - see DetectionStateMachine.h's "COMPARING"
 * note). No comparison basis yet (nothing successfully set this session)
 * means nothing to compare against, so the very first switch of a
 * session always proceeds - that is also exactly the situation Trigger A
 * (go-live mismatch) already handles at the detection layer, so this
 * guard would be redundant there even if it could fire. A mismatch never
 * re-applies our value in a loop (DESIGN.md 3.5's revised Restream
 * section: "detect and yield, don't fight") - it reports the mismatch
 * via ExternalWriterCallback (wired by the real caller to
 * DetectionStateMachine::onExternalChangeDetected(), the existing hook
 * DESIGN.md 3.5 describes that nothing has called until now) and skips
 * the PATCH entirely for this switch. A failed GET (network hiccup) is
 * NOT treated as a detected conflict - proceeding with the PATCH is the
 * reliable default over gating the whole feature on a secondary check's
 * success. If the channel already shows the category we're about to set
 * (someone else already got it right, or a previous attempt succeeded
 * but this coordinator's own bookkeeping hadn't caught up), that is not
 * a conflict either - only a value that is neither what we last set NOR
 * what we are about to set counts as "someone else changed it."
 *
 * FAILURE HANDLING: a failed PATCH is logged with
 * IChannelClient::setChannelCategory()'s human-readable reason and
 * nothing else happens - no retry loop (the real IChannelClient
 * implementation already owns backoff/pacing; this class issues exactly
 * one attempt per switch-in event, matching the "event-driven, not
 * polling" directive), and lastAppliedCategoryId_ is left untouched so a
 * later external-writer check is never fooled into thinking a failed
 * PATCH succeeded. A stream marker is only ever attempted after a PATCH
 * SUCCEEDS, and its own outcome (logged either way) never unwinds that
 * success - see switchIn()'s doc comment.
 *
 * TIER-2 (ALIAS TABLE) RESOLUTIONS CARRY NO ID: CategoryResolver.h
 * documents that a tier-2 hit (data/aliases.json) returns
 * ResolvedCategory::categoryId empty - the table stores hand-curated
 * category NAMES only, never ids. switchIn()/applyFallback() both funnel
 * every resolved category through applyResolvedCategory(), which treats
 * an empty categoryId as "not actually resolved yet": it resolves the
 * NAME via categoryLookup_'s exact-match tier (the same lookup
 * applyFallback() already used for its free-text fallback name) before
 * anything is PATCHed, and resolves to nothing - logged, no PATCH - if
 * that lookup also misses. This class must NEVER hand
 * IChannelClient::setChannelCategory() an empty id: per Twitch's Modify
 * Channel Information docs, an empty game_id UNSETS the live category,
 * which is exactly the harm "unmapped means no action" exists to
 * prevent. (twitch::TwitchClient additionally refuses an empty game_id
 * at the network layer, as a structural backstop - see its class doc
 * comment - but this class must never rely on that as the only guard.)
 *
 * ONCE-A-PATCH-SUCCEEDS FEEDBACK (the coordinator's own missing edge,
 * closed here): DetectionStateMachine::Listener::onSwitchIn()/
 * onApplyFallback() are fire-and-forget from the state machine's
 * perspective - it has no way to learn whether this class actually
 * changed anything. categoryAppliedCallback_ closes that loop: it fires
 * exactly once per successful PATCH (never on a failure, an unmapped
 * resolution, or an external-writer standdown), with the category NAME
 * that was actually applied. The real caller (ui::CategoryDock) wires
 * this straight to DetectionStateMachine::onLiveCategoryKnown() - the
 * existing hook Trigger A already uses for its comparison basis - so a
 * failed/standed-down/unmapped switch no longer leaves the state
 * machine believing a change happened that never did.
 *
 * REAUTH: IChannelClient::reauthRequired() is checked before anything
 * else in every entry point - no call is even attempted while true
 * (DESIGN.md 3.3's "never retry a 401 in a loop" extends one level up:
 * this class does not even try until the caller's adapter reports the
 * freeze has cleared).
 *
 * THREADING: Qt main thread only, same as CategoryResolver/TwitchClient -
 * this class does no I/O itself, only sequences calls through the
 * interfaces it is handed.
 */

#pragma once

#include <functional>
#include <optional>
#include <string>

#include "../detection/DetectedGame.h"
#include "CategoryResolver.h"

namespace signalbox::core {

// What DESIGN.md 3.5's external-writer guard reads back from
// GET /helix/channels. Deliberately smaller than twitch::ChannelInfo
// (which also carries title/broadcasterId - not this class's concern) so
// this header, and everything that includes it, stays free of the Qt
// types twitch::ChannelInfo pulls in - see the class doc comment's
// "harness" note.
struct ChannelSnapshot {
	std::wstring gameId;
	std::wstring gameName;
};

// Coordinator-facing seam over TwitchClient's channel-writing surface -
// the same pattern as ICategoryLookup (CategoryResolver.h) and for the
// same reason: lets CategorySwitchCoordinator be exercised by
// tests/harness/main.cpp without obs-module.h or Qt6::Network. The real
// implementation, twitch::TwitchChannelClient, adapts twitch::TwitchClient's
// async Qt signals to these callback-style methods.
class IChannelClient {
public:
	virtual ~IChannelClient() = default;

	// True while the underlying client is frozen after a 401 with no way
	// to refresh itself (TwitchClient.h's 401 HANDLING). Checked before
	// every call this class makes - see class doc comment.
	virtual bool reauthRequired() const = 0;

	// GET /helix/channels. onResult is invoked exactly once (sync or
	// async) with the current live category, or std::nullopt if the
	// request failed for any reason (network, backoff, frozen). Callers
	// must treat nullopt as "could not verify," never as "confirmed no
	// conflict."
	virtual void getChannelInfo(std::function<void(std::optional<ChannelSnapshot>)> onResult) = 0;

	// PATCH /helix/channels. onResult(success, humanReadableReason) is
	// invoked exactly once; reason is only meaningful when !success.
	virtual void setChannelCategory(const std::wstring &categoryId, const std::wstring &categoryName,
					 std::function<void(bool success, std::wstring reason)> onResult) = 0;

	// POST /helix/streams/markers. onResult(success) is invoked exactly
	// once. Fire-and-forget from the caller's perspective (DESIGN.md
	// Section 5: markers never gate or unwind a category change) - the
	// callback exists so callers (and the harness) can log/assert on the
	// outcome without polling anything.
	virtual void createStreamMarker(const std::wstring &description, std::function<void(bool success)> onResult) = 0;
};

class CategorySwitchCoordinator {
public:
	// Mirrors DetectionStateMachine::Listener::onLogEntry()'s "one
	// human-readable line" shape so the real caller (CategoryDock) can
	// forward straight into its existing activity-log plumbing. Deliberately
	// carries no "previousCategory"/Undo target - Undo is a detection-
	// level concept DetectionStateMachine's own log entries already own
	// (see its class doc comment); these entries are the Twitch-facing
	// follow-up, not a second source of Undo targets.
	using ActivityCallback = std::function<void(const std::wstring &message)>;

	// Fired when the external-writer guard (DESIGN.md 3.5) finds the
	// live category no longer matches what this coordinator last set.
	// The real caller wires this to
	// DetectionStateMachine::onExternalChangeDetected() - the existing,
	// previously-unused hook for exactly this situation - so automation
	// pauses through the same path DESIGN.md 3.5 already specifies
	// rather than a second, parallel pause mechanism.
	using ExternalWriterCallback = std::function<void()>;

	// Fired exactly once per successful PATCH, with the category name
	// that was actually applied - see class doc comment's "ONCE-A-PATCH-
	// SUCCEEDS FEEDBACK". Never fired for a failed PATCH, an unmapped
	// resolution, or an external-writer standdown. Optional - defaults
	// to nullptr so existing callers (and the harness) that don't care
	// about this are unaffected.
	using CategoryAppliedCallback = std::function<void(const std::wstring &categoryName)>;

	// Per-call completion signal for callers that need to know success/
	// failure directly rather than only via ActivityCallback's log line -
	// currently only ui::CategoryDock's Undo button (it must not mark a
	// log entry "(undone)" until the revert PATCH actually succeeds).
	// nullptr (the default) means "caller doesn't care"; switchIn() never
	// passes one.
	using CompletionCallback = std::function<void(bool success)>;

	// resolver must outlive this object - not owned, same non-owning-
	// reference pattern used throughout this codebase.
	CategorySwitchCoordinator(CategoryResolver &resolver, ActivityCallback activityCallback,
				   ExternalWriterCallback externalWriterCallback,
				   CategoryAppliedCallback categoryAppliedCallback = nullptr);

	// channelClient may be nullptr (Twitch not connected yet, or the
	// dock hasn't finished the auth flow) - every entry point below
	// no-ops with a log line in that case. Not owned; must outlive this
	// object or be cleared with another setChannelClient(nullptr) call
	// first - same ownership pattern as CategoryResolver's lookup_.
	void setChannelClient(IChannelClient *channelClient);

	// categoryLookup is used ONLY by applyFallback() (see its doc
	// comment) - resolver's own tier 3 lookup is attached separately via
	// CategoryResolver::setLookup(), by design the same instance in the
	// real plugin. Not owned; nullptr-safe (applyFallback() degrades to
	// "unmapped, change nothing").
	void setCategoryLookup(ICategoryLookup *categoryLookup);

	// DetectionStateMachine::Listener::onSwitchIn()'s delegate target.
	// `live` should be the caller's DetectionStateMachine::isLive() at
	// the moment of the call - it gates ONLY the stream-marker step
	// (DESIGN.md Section 5: markers require the broadcaster live), never
	// the category PATCH itself (DetectionStateMachine.h documents
	// onSwitchIn as not itself gated on live-ness - its "ONLY WHEN LIVE"
	// note is scoped to the prompts, not ordinary confirmed switches).
	//
	// Sequence on a resolved category (a resolution below threshold or
	// otherwise unmapped issues NO PATCH - CategoryResolver's own
	// "unmapped means no action" contract, inherited unchanged here):
	//   1. reauthRequired()/no channel client -> log, stop.
	//   2. resolve game -> category (CategoryResolver tiers 1-3).
	//   3. unmapped -> log "couldn't map ... - category unchanged", stop.
	//   4. categoryId empty (tier-2 alias hit - see class doc comment's
	//      "TIER-2" note) -> resolve the NAME via categoryLookup_'s
	//      exact-match tier first; still no id -> log, stop. NEVER falls
	//      through to step 6 with an empty id.
	//   5. GET /helix/channels (external-writer guard) - see class doc
	//      comment. Mismatch -> log + ExternalWriterCallback, stop
	//      (never PATCH over another writer).
	//   6. PATCH /helix/channels. Failure -> log with reason, stop
	//      (lastAppliedCategoryId_ untouched).
	//   7. Success -> lastAppliedCategoryId_ updated, log success,
	//      categoryAppliedCallback_ fired with the applied name.
	//   8. If live: POST /helix/streams/markers. Its outcome only ever
	//      produces a log line - it can never undo step 7.
	void switchIn(const detection::InstalledGame &game, bool live);

	// DetectionStateMachine::Listener::onApplyFallback()'s delegate
	// target. fallbackCategoryName is PluginConfig::fallbackCategoryName()
	// (a free-text label, "Just Chatting" by default) - resolved via
	// categoryLookup's exact-match tier only (CategoryResolver's tiers
	// 1/2 key off a detected game's identity, which a plain fallback
	// name doesn't have); a miss is "unmapped," same contract as
	// switchIn(). Same guarded-PATCH sequence as switchIn() from step 5
	// onward once a category id is in hand. onComplete is the Undo use
	// case described above the CompletionCallback typedef - every other
	// caller (including the normal GameClosed-accept path) leaves it
	// nullptr.
	void applyFallback(const std::wstring &fallbackCategoryName, bool live, CompletionCallback onComplete = nullptr);

private:
	// Shared tail end of both switchIn() and applyFallback(): takes
	// whatever CategoryResolver/categoryLookup_ resolved and either
	// PATCHes it (categoryId already present) or resolves the tier-2
	// alias NAME to a real id first (categoryId empty) - see class doc
	// comment's "TIER-2" note. The one and only place that decides "is
	// this id usable" before anything reaches performGuardedPatch().
	void applyResolvedCategory(const ResolvedCategory &resolved, bool live, CompletionCallback onComplete);
	void performGuardedPatch(const std::wstring &categoryId, const std::wstring &categoryName, bool live,
				  CompletionCallback onComplete);
	void issuePatch(const std::wstring &categoryId, const std::wstring &categoryName, bool live,
			 CompletionCallback onComplete);
	void maybeCreateMarker(const std::wstring &categoryName);
	void log(const std::wstring &message) const;

	CategoryResolver &resolver_;
	ICategoryLookup *categoryLookup_ = nullptr; // Not owned; see setCategoryLookup().
	ActivityCallback activityCallback_;
	ExternalWriterCallback externalWriterCallback_;
	CategoryAppliedCallback categoryAppliedCallback_; // See class doc comment's "ONCE-A-PATCH-SUCCEEDS" note.

	IChannelClient *channelClient_ = nullptr; // Not owned.

	// What this coordinator last SUCCEEDED in setting - the external-
	// writer guard's comparison basis. Deliberately independent of
	// anything DetectionStateMachine tracks - see class doc comment.
	std::optional<std::wstring> lastAppliedCategoryId_;
};

} // namespace signalbox::core
