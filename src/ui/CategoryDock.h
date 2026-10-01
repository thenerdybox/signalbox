/*
 * SignalBox - ui/CategoryDock.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * The dock registered via obs_frontend_add_dock_by_id() (OBS 30+ API -
 * DESIGN.md 4.1). Owns the DetectionStateMachine and acts as its
 * Listener, embeds PromptWidget for the non-modal prompts, shows current
 * detection/automation status, the activity log with one-click Undo, the
 * manual-lock toggle, a compact Twitch-auth panel, and a footer with
 * version/patch-notes link, "Check for updates", Rescan and Settings.
 *
 * LAYOUT IS NOT FIXED - Status and Twitch swap, and only one of them is
 * ever on screen (refreshDockSections()). Unauthenticated shows Twitch;
 * authenticated shows Status; losing authentication swaps back. Activity
 * and the footer are always present. This dock competes for vertical
 * space with the streamer's own scene/source docks, so a group that
 * cannot act on anything right now does not get to occupy it - and a
 * "Connect to Twitch" box left visible under "Connected as <you>" was
 * worse than merely wasteful, since it invited a reconnection nobody
 * needed. Reconnecting stays permanently reachable in SettingsDialog,
 * which is what makes hiding the group safe rather than a dead end.
 *
 * "FIX THIS..." (next to the Status group's detected-game label) is what
 * makes CategoryResolver tier 1 (the persistent per-game override,
 * DESIGN.md 1.5) reachable at all: it opens GameOverrideDialog.h/.cpp on
 * currentGameForFix()'s InstalledGame, so a wrong detection can be
 * corrected - "always use category X" (via a real Twitch category
 * search) or "never switch for this app" - without ever hand-editing
 * config.json. See onFixDetectionClicked() and currentGameForFix()'s doc
 * comments below for how the right InstalledGame identity gets there.
 *

 * INTEGRATION SEAM - WIRED: plugin-main.cpp's DetectionEngine::
 * ResultCallback now posts each DetectionResult to this dock via
 * QMetaObject::invokeMethod(dock, ..., Qt::QueuedConnection) - the
 * required worker-thread -> Qt-main-thread hop happens there, not here
 * (this class still only ever touches its own state machine from the
 * main thread; see onDetectionResult()/onTrackedProcessExited() below).
 * The one thing plugin-main.cpp cannot reach on its own is a tracked
 * process's exit reason (crash vs. clean - DESIGN.md 2.2), because
 * DetectionStateMachine::onPollResult() deliberately does NOT treat a
 * bare "nothing detected" poll as an exit signal (see its own doc
 * comment) - onTrackedProcessExited() below is the minimal addition that
 * closes that gap; see the project report.
 *
 * NOW WIRED: DetectionStateMachine::Listener::onSwitchIn()/
 * onApplyFallback() delegate to coordinator_ (core::CategorySwitchCoordinator,
 * see its own class doc comment for the resolve -> external-writer-guard
 * -> PATCH -> marker sequence). coordinator_ is constructed with a
 * reference to resolver_ (owned by plugin-main.cpp, passed in - see below)
 * and starts with no IChannelClient/ICategoryLookup attached; both get
 * attached once Twitch auth actually succeeds (attachTwitchClient()) -
 * until then, coordinator_'s entry points no-op with a log line, exactly
 * like every other "not connected yet" path in this class.
 *
 * ONE PluginConfig INSTANCE (fixes the previous "two separate copies"
 * gap): config_ is a reference to the SAME PluginConfig plugin-main.cpp
 * constructs and passes to CategoryResolver - this class no longer owns
 * (or loads) its own copy, so a Settings > Save actually persists what
 * DetectionStateMachine is running with, and vice versa on the next OBS
 * launch. See SettingsDialog.cpp's onSaveClicked() for the three toggles
 * (autoSwitchEnabled/promptsEnabled/fallbackSwitchingEnabled) that used
 * to be applied to the live DetectionStateMachine only and silently
 * dropped on restart - they now round-trip through config_ too.
 *
 * TIMERS (lightweight-by-design requirement - see .cpp for the full
 * accounting): exactly two QTimer instances exist.
 *   1. tickTimer_ - interval TimingConstants::pollIntervalS (default 5s),
 *      runs for the plugin's lifetime. This is DetectionStateMachine's
 *      own required heartbeat (its onTick() doc comment: "call at least
 *      as often as pollIntervalS") - grace/prompt timeouts do not work
 *      without it. Once onDetectionResult() above is wired up, that
 *      heartbeat could carry onTick() instead and this timer could be
 *      removed; kept for now so the state machine functions standalone.
 *      Each tick is O(1) - see DetectionStateMachine's own performance
 *      note - so this is cheap even though it never stops.
 *   2. countdownTimer_ - 1Hz, started ONLY while
 *      stateMachine_.secondsUntilNextDeadline() has a value (a grace
 *      window or an outstanding prompt) AND the dock is visible; stopped
 *      the instant either condition stops holding. This is the ONLY
 *      thing that repaints on a tick - everything else in this class
 *      updates from Listener callbacks (i.e. on actual state changes).
 *
 * VISIBILITY: presentation work (formatting label text, rebuilding the
 * log list) is skipped while !isVisible() and caught up once in
 * showEvent() - see presentationDirty_. The underlying data (log
 * entries, current status) keeps updating regardless; only the Qt
 * widget work is gated.
 *
 * THREADING: this is a QWidget - Qt main thread only, always.
 */

#pragma once

#include <deque>
#include <memory>

#include <QWidget>

#include <obs-frontend-api.h>

#include "../core/CategoryResolver.h"
#include "../core/CategorySwitchCoordinator.h"
#include "../core/DetectionStateMachine.h"
#include "../core/PluginConfig.h"
#include "../detection/DetectedGame.h"
#include "../detection/ProcessScanner.h" // ExitReason
#include "../twitch/HttpTransport.h" // twitch::HttpTransport - update check + all Twitch traffic.
#include "../twitch/TokenStore.h"
#include "../twitch/TwitchAuth.h" // TokenSet - held as a value member (currentTokens_).

QT_BEGIN_NAMESPACE
class QGroupBox;
class QLabel;
class QListWidget;
class QPushButton;
class QTimer;

QT_END_NAMESPACE

namespace signalbox::twitch {
class TwitchClient;
class TwitchCategoryLookup;
class TwitchChannelClient;
}

namespace signalbox::ui {

class PromptWidget;
class PromptToast;

class CategoryDock : public QWidget, private core::DetectionStateMachine::Listener {
	Q_OBJECT

public:
	// config and resolver are owned by plugin-main.cpp and must outlive
	// this dock (OBS never destroys the dock before obs_module_unload()
	// - see plugin-main.cpp's own ownership note on g_dock). Passing
	// both in rather than this class constructing its own copies is what
	// keeps exactly one PluginConfig/CategoryResolver alive for the
	// whole plugin - see class doc comment's "ONE PluginConfig INSTANCE"
	// note.
	explicit CategoryDock(core::PluginConfig &config, core::CategoryResolver &resolver, QWidget *parent = nullptr);
	~CategoryDock() override;

signals:
	// Dock "Rescan" button (project brief item 6: DetectionEngine::
	// requestRescan() had zero callers). This class does not own or
	// reach into DetectionEngine (that stays plugin-main.cpp's job per
	// the module ownership split - DESIGN.md 4.2) - plugin-main.cpp
	// connects this signal to g_engine->requestRescan() once both exist.
	void rescanRequested();

public:
	// Feed one poll's worth of detection from the coordinator (see the
	// class doc comment's INTEGRATION SEAM note - nothing calls this
	// yet in a real build). Safe to call only from the Qt main thread;
	// the caller is responsible for the worker-thread -> main-thread hop.
	void onDetectionResult(std::optional<detection::DetectedGame> detected, std::size_t indexedGameCount);

	// Forwards a tracked process exit (DESIGN.md 2.2's crash-vs-clean
	// classification, sized by DetectionEngine's ProcessScanner on the
	// worker thread - see DetectionEngine.h) to the state machine's
	// onTrackedProcessExited(). MINIMAL, NECESSARY ADDITION (see the
	// project report): without this, ACTIVE never transitions to GRACE
	// at all, because DetectionStateMachine::onPollResult() documents
	// itself as NOT treating a bare "nothing detected" poll as an exit
	// signal - only this call does. exitedGame carries the identity of
	// whichever process actually exited (DetectionEngine's own tracked
	// pid, which is not necessarily DetectionStateMachine's activeGame_ -
	// see DetectionStateMachine::onTrackedProcessExited()'s doc comment);
	// the state machine, not this class, is responsible for deciding
	// whether that identity matches its own incumbent. Same threading
	// contract as onDetectionResult(): Qt main thread only, caller's job
	// to marshal.
	void onTrackedProcessExited(detection::ExitReason reason, const detection::InstalledGame &exitedGame);

	// Reports what an index rebuild actually did. Called on every
	// rebuild, but only a user-requested one (the Rescan button) logs
	// unconditionally - the periodic timer's rebuilds stay quiet unless
	// the numbers actually moved, since a line every ten minutes saying
	// nothing changed is just noise in the one place the user goes to
	// find out what happened.
	// denylistRuleCount is how many helpers.json rules were in force for
	// THIS rebuild, and is only meaningful (and only reported) when
	// userRequested is true - a user rescan re-reads helpers.json from
	// disk before rebuilding, the periodic timer deliberately does not.
	// See DetectionEngine.cpp's rescanRequested branch for why only the
	// button pays for that re-read.
	void onIndexRebuilt(std::size_t gameCount, std::size_t excludedCount, std::size_t duplicateCount,
			     double durationMs, bool userRequested, std::size_t denylistRuleCount = 0);

	// Reports the OTHER half of a user rescan: the alias table
	// (data/aliases.json) being re-read on the main thread, which
	// happens in plugin-main.cpp's rescanRequested handler rather than
	// on DetectionEngine's worker thread, because CategoryResolver is
	// main-thread-only. Called immediately on the button press, so this
	// line lands BEFORE the "Rescan complete" line the worker produces a
	// moment later - the two together are what tell a streamer that a
	// hand-edited data file was actually picked up, without going and
	// reading the OBS log. Editing either file used to require a full
	// OBS restart; the whole point of saying so here is that a silent
	// reload is indistinguishable from one that did not happen.
	void onAliasTableReloaded(std::size_t aliasCount);

	// Supplies the registered Twitch public client id, lazily
	// constructing the internal TwitchAuth. Until this is called, the
	// Twitch panel stays in its disabled "not configured" state. See
	// the class doc comment's INTEGRATION SEAM - plugin-main.cpp would
	// call this once a client id exists (out of scope here).
	void setTwitchClientId(const QString &clientId);

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private:
	// --- DetectionStateMachine::Listener ---
	void onSwitchIn(const detection::InstalledGame &game) override;
	void onApplyFallback() override;
	void onReapplyCategory(const detection::InstalledGame &game) override;
	void onPrompt(core::PromptKind kind, const detection::InstalledGame &relevantGame) override;
	void onAutomationPaused(const std::wstring &reasonForDock) override;
	void onLogEntry(const std::wstring &message, const std::wstring &previousCategory) override;

	// --- OBS frontend event trampoline (STREAMING_STARTED/_STOPPED) ---
	// NOTE ON PLACEMENT: DESIGN.md 4.2 and plugin-main.cpp's own doc
	// comment describe plugin-main.cpp as "the one file allowed to know
	// about both the OBS module API and the plugin's own core types."
	// Registering this here instead is a deliberate exception forced by
	// this task's file-ownership boundary (plugin-main.cpp is
	// off-limits) - see the class doc comment's INTEGRATION SEAM note
	// and the project report. Architecturally, a future pass may prefer
	// to move this registration to plugin-main.cpp (which already
	// includes obs-frontend-api.h) and have it call setLive() on this
	// dock's state machine instead.
	static void frontendEventTrampoline(enum obs_frontend_event event, void *privateData);
	void handleFrontendEvent(enum obs_frontend_event event);

	// One GET /helix/channels at go-live feeding
	// DetectionStateMachine::onLiveCategoryKnown(),
	// which is what gives Trigger A (the ADDENDUM's go-live mismatch
	// prompt - DetectionStateMachine.h calls it the product's "founding
	// use case") something to compare the first confirmed detection
	// against. No-ops if Twitch isn't connected yet - Trigger A simply
	// can't fire this stream (goLiveMismatchApplies() already treats an
	// unknown live category as "nothing to compare," per its own doc
	// comment). Also called from attachTwitchClient() for the edge case
	// of connecting Twitch after already going live.
	void syncLiveCategoryOnGoLive();

	// One GET /helix/channels, on demand, updating BOTH the state
	// machine's comparison basis and this dock's own "Live category"
	// label. Called on connect (attachTwitchClient()) and at go-live.
	// Never on a timer (the scheduled re-checks while live go through
	// maybeVerifyLiveCategory()). A failed GET leaves the last known
	// value alone rather than replacing it with "(unknown)".
	// logAsGoLive: write the result to the OBS log as the go-live read.
	void syncLiveCategoryFromChannel(bool logAsGoLive = false);

	// Scheduled live-category verification: called from the heartbeat
	// tick. When the state machine says a slot is due (quick checks after
	// go-live, then every liveCategoryCheckIntervalS), reads the channel
	// and hands the result to DetectionStateMachine::onLiveCategoryVerified(),
	// which decides whether to re-apply the running game's category. At
	// most one read is ever in flight; a failed or backed-off read is
	// simply "not verifiable" and waits for the next slot.
	void maybeVerifyLiveCategory();

	// Common path of onSwitchIn()/onReapplyCategory(): the only-while-live
	// and "never switch for this app" policy, then the coordinator.
	void dispatchSwitch(const detection::InstalledGame &game, bool reassert);

	// Brings the dock (and with it the prompt) forward after the
	// notification card was clicked. User-initiated, so activating is fine.
	void revealPrompt();

	// Decides which SECTIONS of the dock exist right now, and the Twitch
	// group's internal widget visibility, from current state. The single
	// place that decides either - see its definition.
	void refreshDockSections();

	void buildUi();
	void wirePromptWidget();

	// Presentation (gated on isVisible(); see class doc comment).
	void markPresentationDirty();
	void refreshPresentationIfVisible();
	void refreshStatusLabels();
	void refreshLogView();
	void updateCountdownTimerState();

	void onManualLockToggled(bool locked);
	void onSettingsClicked();
	void onCheckForUpdatesClicked();
	void onConnectTwitchClicked();
	void onCopyDeviceCodeClicked();
	void onOpenAuthPageClicked();
	void onResumeClicked();
	void onUndoRequested(std::size_t logIndex);
	void onRescanClicked();
	void onFixDetectionClicked();

	// "Stream Ending" - DetectionStateMachine::beginStreamEndingHold() /
	// clearStreamEndingHold(). Checkable, and the check state is driven
	// FROM the state machine in refreshStatusLabels() rather than being
	// the source of truth, because the hold clears itself (stream stops,
	// a new game starts) and a button left looking pressed after that
	// would be a lie about what the plugin is doing.
	void onStreamEndingToggled(bool holding);

	// "Just Chatting" - applies PluginConfig::fallbackCategoryName()
	// right now, through the coordinator rather than by hand, so the
	// external-writer guard learns about it (DESIGN.md 3.5). Setting the
	// same category directly on Twitch instead would look like an
	// external writer to the very next switch and freeze automation -
	// which is the exact failure this button is most likely to be
	// pressed near.
	void onJustChattingClicked();

	// Recomputes whether Trigger D may fire, from the three things that
	// decide it: the Settings toggle, whether Twitch is connected at all,
	// and whether "only while live" would make the answer unusable
	// offline anyway. Called from every place any of those can change -
	// see its definition.
	void refreshIdlePromptEnabled();

	// The InstalledGame currently showing in the Status group, if any -
	// what "Fix This!" acts on. stateMachine_ already retains the full
	// identity (not just a display name) for every state this can return
	// - see DetectionStateMachine.h's activeGame()/pendingCandidate()/
	// graceGame()/promptRelevantGame() accessors - so this class needs no
	// separate storage of its own to satisfy that; it only needs to ask
	// in the right order. promptRelevantGame() comes first when a prompt
	// is outstanding: for a GameClosed prompt, that is the one point
	// after grace expires but before the prompt resolves where
	// graceGame_ is still authoritative but about to be cleared (see
	// DetectionStateMachine::onTick()'s Grace/promptOutstanding_
	// handling) - promptRelevantGame_ is the copy that survives that
	// transition. Falls through active -> pending -> grace to match
	// refreshStatusLabels()'s own display precedence exactly, so "Fix
	// this..." always corrects whatever the label just told the user.
	std::optional<detection::InstalledGame> currentGameForFix() const;

	// Twitch's authentication docs require validating the access token on
	// startup and hourly thereafter - see TimingConstants.h's
	// ShouldValidateTwitchToken() for the (harness-testable) pure timing
	// decision this wraps. Called once from setTwitchClientId()'s warm-
	// start path (the "on startup" half) and once per tickTimer_ tick
	// (the "hourly" half, piggybacked on the heartbeat already running
	// for DetectionStateMachine::onTick() - no new timer). A no-op
	// whenever there's no Twitch auth/token to validate yet.
	void maybeValidateTwitchToken();

	// PromptWidget::dismissWithoutResponse() has a real job: the state
	// machine can resolve/clear its own outstanding prompt without ever
	// routing through PromptWidget's click handlers (onTick()'s timeout,
	// a grace reappearance making a GameClosed prompt moot, setLive(false)
	// dropping it outright) - called from every path that can change
	// stateMachine_'s prompt state so the widget never lingers, visible,
	// with dead buttons.
	void syncPromptWidgetVisibility();

	// Appends one line to the activity log and refreshes the visible
	// list (if visible) - the single place both Listener::onLogEntry()/
	// onAutomationPaused() and coordinator_'s ActivityCallback funnel
	// through, so there is exactly one bounded-ring-buffer push
	// implementation, not two copies of it.
	void appendActivityLogEntry(const std::wstring &message, const std::wstring &previousCategory);

	// Builds (or rebuilds, on a fresh device-code connect / token
	// refresh) twitchClient_/categoryLookup_/channelClient_ and attaches
	// the latter two to resolver_/coordinator_. accessToken/broadcasterId
	// may be empty (broadcasterId empty until resolveCurrentUser()
	// resolves it - see TwitchClient.h's resolveCurrentUser() doc
	// comment); callers are responsible for calling resolveCurrentUser()
	// themselves afterward when broadcasterId is empty.
	void attachTwitchClient(const QString &accessToken, const QString &broadcasterId);

	// Persists currentTokens_ to tokenStore_ (DPAPI-encrypted tokens.bin -
	// see twitch/TokenStore.h). Called after every successful auth,
	// refresh, or user-id resolution, per TokenStore.h's own contract.
	void saveCurrentTokens();

	// --- Model (always kept up to date, independent of visibility) ---
	struct LogEntry {
		std::wstring message;
		std::wstring previousCategory; // Empty if nothing to undo.
		bool undone = false;
	};
	static constexpr std::size_t kMaxLogEntries = 200; // Bounded ring buffer - see class doc comment.
	std::deque<LogEntry> logEntries_;

	core::PluginConfig &pluginConfig_;   // Owned by plugin-main.cpp - see constructor doc comment.
	core::CategoryResolver &resolver_;   // Owned by plugin-main.cpp - see constructor doc comment.
	core::DetectionStateMachine stateMachine_;
	core::CategorySwitchCoordinator coordinator_;

	std::unique_ptr<twitch::TwitchAuth> twitchAuth_;
	twitch::TokenStore tokenStore_;
	twitch::TokenSet currentTokens_; // In-memory mirror of whatever tokenStore_ last saved/loaded.
	QString twitchClientId_;

	std::unique_ptr<twitch::TwitchClient> twitchClient_;
	std::unique_ptr<twitch::TwitchCategoryLookup> categoryLookup_;
	std::unique_ptr<twitch::TwitchChannelClient> channelClient_;

	twitch::HttpTransport *updateCheckTransport_ = nullptr; // Only used on "Check for updates" clicks.
	// Set only when a check found a NEWER release than this build. While it
	// holds a URL, the button is an "open the release page" action rather
	// than a "check again" one - see onCheckForUpdatesClicked().
	QString availableUpdateUrl_;

	bool presentationDirty_ = true;
	QString pendingDeviceCode_;
	QString pendingVerificationUri_;

	// True between deviceCodeReady and whichever of tokensAcquired /
	// authFailed ends the flow. Drives the device-code row's visibility -
	// see refreshTwitchPanel().
	bool deviceFlowActive_ = false;

	// --- Widgets ---
	QLabel *detectedGameLabel_ = nullptr;
	QLabel *liveCategoryLabel_ = nullptr;
	QLabel *connectionStateLabel_ = nullptr;
	QLabel *countdownLabel_ = nullptr;
	QLabel *pausedBanner_ = nullptr;
	QPushButton *resumeButton_ = nullptr;
	QString pausedReason_; // Last onAutomationPaused() text, shown in the banner.
	QString liveCategory_; // Last category the coordinator CONFIRMED is live.
	std::optional<detection::DetectedGame> lastLoggedDetection_; // So the OBS log gets changes, not polls.
	std::size_t lastIndexedCount_ = 0; // Compared against, so quiet rebuilds stay quiet.
	bool haveIndexedCount_ = false;
	QPushButton *manualLockButton_ = nullptr;
	QPushButton *fixDetectionButton_ = nullptr; // "Fix This!" - opens GameOverrideDialog on currentGameForFix().
	QPushButton *streamEndingButton_ = nullptr; // Checkable; mirrors stateMachine_.streamEndingHold().
	QPushButton *justChattingButton_ = nullptr; // One-click apply of the fallback category.
	QLabel *streamEndingBanner_ = nullptr;      // Visible only while the hold is on - see refreshStatusLabels().
	PromptWidget *promptWidget_ = nullptr;
	std::unique_ptr<PromptToast> toast_; // Parentless always-on-top card - see PromptToast.h.
	bool liveCheckInFlight_ = false;     // One scheduled verification read at a time.
	QListWidget *activityLogView_ = nullptr;

	QLabel *twitchStatusLabel_ = nullptr;
	QLabel *deviceCodeLabel_ = nullptr;
	// The two mutually exclusive sections - see refreshDockSections().
	// Exactly one is ever visible: Twitch while unauthenticated, Status
	// once connected. Activity and the footer are always present.
	QGroupBox *statusGroup_ = nullptr;
	QGroupBox *twitchGroup_ = nullptr;

	QPushButton *connectTwitchButton_ = nullptr;
	QPushButton *copyDeviceCodeButton_ = nullptr;
	QPushButton *openAuthPageButton_ = nullptr;

	QLabel *versionLabel_ = nullptr;
	QPushButton *settingsButton_ = nullptr;
	QPushButton *checkUpdatesButton_ = nullptr;
	QPushButton *rescanButton_ = nullptr;

	QTimer *tickTimer_ = nullptr;     // Always running - see class doc comment.
	QTimer *countdownTimer_ = nullptr; // Only while a deadline is pending AND visible.

	// Hourly Twitch token validation (project brief item 5) - see
	// maybeValidateTwitchToken()'s doc comment and TimingConstants.h's
	// ShouldValidateTwitchToken(). 0 means "never validated this run."
	qint64 lastValidatedAtUnixS_ = 0;
};

} // namespace signalbox::ui
