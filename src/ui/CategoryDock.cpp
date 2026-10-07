/*
 * SignalBox - ui/CategoryDock.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * See CategoryDock.h for the integration seam, timer accounting, and
 * visibility-gating notes this file implements.
 */

#include "CategoryDock.h"

#include <algorithm>

#include <QClipboard>
#include <QDateTime>
#include <QDockWidget>
#include <QDesktopServices>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "../plugin-support.h"
#include "signalbox-build-id.h"
#include "../twitch/TwitchAuth.h"
#include "../twitch/TwitchCategoryLookup.h"
#include "../twitch/TwitchChannelClient.h"
#include "../twitch/TwitchClient.h"
#include "GameOverrideDialog.h"
#include "PromptToast.h"
#include "PromptWidget.h"
#include "SettingsDialog.h"

namespace signalbox::ui {

namespace {

QString toQString(const std::wstring &s)
{
	return QString::fromStdWString(s);
}

} // namespace

CategoryDock::CategoryDock(core::PluginConfig &config, core::CategoryResolver &resolver, QWidget *parent)
	: QWidget(parent),
	  pluginConfig_(config),
	  resolver_(resolver),
	  stateMachine_(*this),
	  coordinator_(
		  resolver_,
		  [this](const std::wstring &message) { appendActivityLogEntry(message, L""); },
		  [this]() { stateMachine_.onExternalChangeDetected(); },
		  // Feedback channel (project brief item 3 - see
		  // CategorySwitchCoordinator.h's "ONCE-A-PATCH-SUCCEEDS" note):
		  // fires only once a switch/fallback PATCH actually succeeds,
		  // with the category name that was actually applied. Feeding
		  // this into onLiveCategoryKnown() - the same hook Trigger A's
		  // comparison basis already uses - replaces the state
		  // machine's old optimistic self-update with the real outcome.
		  // Also the ONLY confirmed value of "what is live right now",
		  // so it is what finally populates liveCategoryLabel_. That
		  // label previously never changed from "(unknown)" - which is
		  // a poor thing to show beside a detected game, because the
		  // whole question the dock exists to answer is whether those
		  // two agree.
		  [this](const std::wstring &categoryName) {
			  // This fires ONLY on a PATCH that actually succeeded,
			  // with the name that was actually applied - so it is
			  // the one place entitled to record a switch as having
			  // happened, and therefore the only place Undo can
			  // honestly be offered.
			  //
			  // previousCategory is captured before liveCategory_
			  // moves, because that is precisely Undo's restore
			  // target. On the very first switch of a session there
			  // is no previous value, and an Undo button with
			  // nothing to restore is worse than no button - so the
			  // entry is logged without one.
			  const std::wstring previousCategory = liveCategory_.toStdWString();

			  stateMachine_.onCategoryApplied(categoryName);
			  liveCategory_ = toQString(categoryName);

			  if (previousCategory != categoryName) {
				  appendActivityLogEntry(L"Switched to \"" + categoryName + L"\"", previousCategory);
			  }
			  refreshPresentationIfVisible();
		  })
{
	// Effective timing + the three persisted behavior toggles - see
	// class doc comment's "ONE PluginConfig INSTANCE" note. These used
	// to be hardcoded/live-only here; now they come from the single
	// shared config plugin-main.cpp already loaded before constructing
	// this dock.
	stateMachine_.setTiming(pluginConfig_.timing());
	stateMachine_.setManualLock(!pluginConfig_.autoSwitchEnabled());
	stateMachine_.setPromptsEnabled(pluginConfig_.promptsEnabled());
	stateMachine_.setFallbackEnabled(pluginConfig_.fallbackSwitchingEnabled()); // Off by default (project brief).

	// Trigger D starts OFF and is switched on by refreshIdlePromptEnabled()
	// once there is a Twitch connection to act on - buildUi()'s
	// refreshDockSections() call at the end of construction is the first
	// thing to run it, and setTwitchClientId()'s warm start runs it again
	// the moment a saved token turns up.

	// Trigger C: creative/dev apps are offered, never applied. The
	// resolver owns which apps those are (it owns the alias table); the
	// state machine only needs a yes/no, which keeps it exercisable in
	// the harness with no resolver at all.
	stateMachine_.setPromptOnlyPredicate(
		[this](const detection::InstalledGame &game) { return resolver_.isPromptOnly(game); });

	// Live category verification compares the channel's category with the
	// running game's, and a game's Twitch name is not always its install
	// name: a per-game override or an alias renames it. Tell the state
	// machine what this game maps to so a correct category is never
	// "corrected" - see DetectionStateMachine::onLiveCategoryVerified().
	stateMachine_.setExpectedCategoryProvider([this](const detection::InstalledGame &game) -> std::wstring {
		if (const auto override = pluginConfig_.findUserOverride(core::CategoryResolver::overrideKeyFor(game)))
			return override->ignored ? std::wstring() : override->categoryName;
		return resolver_.aliasCategoryFor(game);
	});

	toast_ = std::make_unique<PromptToast>();
	connect(toast_.get(), &PromptToast::clicked, this, [this]() { revealPrompt(); });

	// Why a lookup missed, in the activity log rather than nowhere - see
	// CategoryResolver::setDiagnosticCallback(). The coordinator's own
	// "couldn't map that game" line says THAT it failed; this says which
	// step, which is the difference between a diagnosable report and a
	// dead end.
	resolver_.setDiagnosticCallback(
		[this](const std::wstring &detail) { appendActivityLogEntry(detail, L""); });

	buildUi();
	wirePromptWidget();

	// Twitch client ID: NOT read from TwitchClientId.h here on purpose.
	// That header (src/twitch/TwitchClientId.h) intentionally makes
	// calling twitch::twitchClientId() while its constant is still ""
	// a [[deprecated]] warning specifically so it's loud - and this
	// project builds with warnings-as-errors, so calling it
	// unconditionally here would break EVERY build until someone pastes
	// in a real Client ID, for every developer, not just at ship time.
	// Once a real ID is in place the warning stops firing on its own
	// (see that header's comment) and wiring
	// `setTwitchClientId(QString::fromUtf8(twitch::twitchClientId()...))`
	// in here or in plugin-main.cpp becomes safe - left as the explicit
	// setTwitchClientId() seam below until then.

	// STREAM STATE (requirement 7): OBS_FRONTEND_EVENT_STREAMING_STARTED
	// / _STOPPED drives DetectionStateMachine::setLive(), which gates
	// both prompts. See the header's placement note on why this lives
	// here instead of plugin-main.cpp.
	obs_frontend_add_event_callback(&CategoryDock::frontendEventTrampoline, this);

	// Timer 1/2: the state machine's required heartbeat. Always running
	// - see class doc comment for why this is justified rather than
	// wasteful. Interval matches the configured poll cadence (default
	// 5s), not a UI refresh rate.
	tickTimer_ = new QTimer(this);
	tickTimer_->setInterval(static_cast<int>(pluginConfig_.timing().pollIntervalS) * 1000);
	connect(tickTimer_, &QTimer::timeout, this, [this]() {
		stateMachine_.onTick();
		maybeVerifyLiveCategory();    // Scheduled live-category check - see its doc comment.
		syncPromptWidgetVisibility(); // onTick() can resolve a timeout without going through PromptWidget's own handlers.
		maybeValidateTwitchToken();   // Hourly Twitch token validation (project brief item 5) - see its own doc comment.
		updateCountdownTimerState();
		refreshPresentationIfVisible();
	});
	tickTimer_->start();

	// Timer 2/2: 1Hz countdown display. Created stopped - started only
	// by updateCountdownTimerState() when there's something to count
	// down AND the dock is visible.
	countdownTimer_ = new QTimer(this);
	countdownTimer_->setInterval(1000);
	connect(countdownTimer_, &QTimer::timeout, this, [this]() {
		if (!isVisible()) {
			countdownTimer_->stop();
			return;
		}
		refreshStatusLabels();
	});

	refreshPresentationIfVisible();
}

CategoryDock::~CategoryDock()
{
	if (toast_)
		toast_->dismiss();
	obs_frontend_remove_event_callback(&CategoryDock::frontendEventTrampoline, this);
}

void CategoryDock::buildUi()
{
	auto *root = new QVBoxLayout(this);

	// --- Status ---
	statusGroup_ = new QGroupBox(QStringLiteral("Status"), this);
	auto *statusGroup = statusGroup_;
	auto *statusLayout = new QVBoxLayout(statusGroup);
	detectedGameLabel_ = new QLabel(QStringLiteral("Detected: (nothing)"), statusGroup);

	// "Fix This!" (class doc comment) - the reachable path to
	// CategoryResolver tier 1. Sits directly beside the label it
	// corrects rather than down in the footer, since the whole point is
	// to act on whatever that label is showing right now. Starts
	// disabled; refreshStatusLabels() enables it exactly when
	// currentGameForFix() has something to act on, same as every other
	// piece of state in this group.
	fixDetectionButton_ = new QPushButton(QStringLiteral("Fix This!"), statusGroup);
	fixDetectionButton_->setEnabled(false);
	fixDetectionButton_->setToolTip(
		QStringLiteral("Correct a wrong detection: set the Twitch category SignalBox should always use "
				"for this app, or tell it to never switch for this app."));
	connect(fixDetectionButton_, &QPushButton::clicked, this, &CategoryDock::onFixDetectionClicked);

	auto *detectedRow = new QHBoxLayout();
	detectedRow->addWidget(detectedGameLabel_, /*stretch=*/1);
	detectedRow->addWidget(fixDetectionButton_);
	statusLayout->addLayout(detectedRow);

	// --- The two "I know what I'm doing" buttons -----------------------
	//
	// Stacked directly under "Fix This!", and that grouping is the point:
	// all three answer the same question - the plugin has the wrong idea
	// about what is going on and the user wants to say so - and the
	// difference between them is only WHICH wrong idea. Fix This!
	// corrects a misidentified app permanently; Stream Ending says stop
	// acting for now; Just Chatting says the right answer is "no game",
	// apply it.
	//
	// Both are deliberately reachable without waiting for a prompt.
	// Trigger D asks the same question Just Chatting answers, but it asks
	// on its own schedule (two minutes of nothing), and a streamer who
	// already knows they are done gaming should not have to wait to be
	// asked - a prompt is a convenience, not the only door.
	streamEndingButton_ = new QPushButton(QStringLiteral("Stream Ending/Stop Detection"), statusGroup);
	streamEndingButton_->setCheckable(true);
	streamEndingButton_->setToolTip(
		QStringLiteral("Stop changing my category while I wrap up. Clears itself when the stream stops "
				"or a new game starts - you don't have to remember to turn it back on."));
	connect(streamEndingButton_, &QPushButton::toggled, this, &CategoryDock::onStreamEndingToggled);

	justChattingButton_ = new QPushButton(QStringLiteral("Just Chatting"), statusGroup);
	justChattingButton_->setToolTip(
		QStringLiteral("Set your Twitch category to your fallback category (Settings > Fallback category) "
				"right now. Automatic switching carries on as normal afterwards."));
	connect(justChattingButton_, &QPushButton::clicked, this, &CategoryDock::onJustChattingClicked);

	// One per row, full width, rather than side by side in a row of their
	// own. A dock is narrow and these two labels are long - packed onto
	// one line the second one is the one that loses, and a button reading
	// "Just Chat..." is a button nobody presses. Full width also makes
	// them read as a short list of things you can tell the plugin, which
	// is what they are.
	statusLayout->addWidget(streamEndingButton_);
	statusLayout->addWidget(justChattingButton_);

	// A hold that cannot be seen is the 45-minute dead zone again in a
	// different costume: the dock would go on listing a detected game and
	// counting polls while quietly changing nothing. Same reasoning as
	// pausedBanner_ below, different cause - and deliberately a separate
	// banner, because this one has no "Resume" button to offer (the
	// button that set it is right there, and it clears itself anyway).
	streamEndingBanner_ = new QLabel(statusGroup);
	streamEndingBanner_->setWordWrap(true);
	streamEndingBanner_->setVisible(false);
	streamEndingBanner_->setStyleSheet(QStringLiteral("QLabel { background: #1e3a4a; color: #bfe4f5; "
							   "border: 1px solid #37718c; border-radius: 3px; padding: 6px; }"));
	statusLayout->addWidget(streamEndingBanner_);

	liveCategoryLabel_ = new QLabel(QStringLiteral("Live category: (unknown)"), statusGroup);
	connectionStateLabel_ = new QLabel(QStringLiteral("Twitch: not connected"), statusGroup);
	countdownLabel_ = new QLabel(QString(), statusGroup);
	countdownLabel_->setVisible(false);
	for (auto *label : {liveCategoryLabel_, connectionStateLabel_, countdownLabel_})
		statusLayout->addWidget(label);

	// --- Paused banner ---
	// Hidden unless automation is actually paused, and deliberately loud
	// when it is not. A paused SignalBox looks exactly like a working one
	// from the outside: the dock still lists a detected game, the poll
	// timer still runs, the Rescan button still rebuilds the index. The
	// only difference is that every poll result is discarded. Without a
	// banner the failure is indistinguishable from "my game just isn't
	// detected", which is precisely how it went unnoticed for 45 minutes
	// of a real session.
	pausedBanner_ = new QLabel(statusGroup);
	pausedBanner_->setWordWrap(true);
	pausedBanner_->setVisible(false);
	pausedBanner_->setStyleSheet(QStringLiteral("QLabel { background: #5a3a1a; color: #ffd9a0; "
						     "border: 1px solid #a8702a; border-radius: 3px; padding: 6px; }"));
	statusLayout->addWidget(pausedBanner_);

	resumeButton_ = new QPushButton(QStringLiteral("Resume automatic switching"), statusGroup);
	resumeButton_->setVisible(false);
	resumeButton_->setToolTip(
		QStringLiteral("Start detecting again from scratch, as if the plugin had just loaded."));
	connect(resumeButton_, &QPushButton::clicked, this, &CategoryDock::onResumeClicked);
	statusLayout->addWidget(resumeButton_);

	manualLockButton_ = new QPushButton(QStringLiteral("Automatic switching: ON"), statusGroup);
	manualLockButton_->setCheckable(true);
	manualLockButton_->setChecked(false); // Unchecked = automatic switching enabled (not locked).
	connect(manualLockButton_, &QPushButton::toggled, this, &CategoryDock::onManualLockToggled);
	statusLayout->addWidget(manualLockButton_);

	root->addWidget(statusGroup);

	// --- Prompt (embedded, non-modal - see PromptWidget.h) ---
	promptWidget_ = new PromptWidget(this);
	root->addWidget(promptWidget_);

	// --- Twitch ---
	twitchGroup_ = new QGroupBox(QStringLiteral("Twitch"), this);
	auto *twitchGroup = twitchGroup_;
	auto *twitchLayout = new QVBoxLayout(twitchGroup);
	twitchStatusLabel_ = new QLabel(QStringLiteral("Not connected"), twitchGroup);
	twitchStatusLabel_->setWordWrap(true);
	twitchLayout->addWidget(twitchStatusLabel_);

	auto *deviceRow = new QHBoxLayout();
	deviceCodeLabel_ = new QLabel(twitchGroup);
	deviceCodeLabel_->setVisible(false);
	// Shown as a live link rather than an address to retype. Twitch embeds the
	// user code in the URL it hands back, so following the link needs no typing
	// at all - which is the entire point of the device-code flow.
	deviceCodeLabel_->setTextFormat(Qt::RichText);
	deviceCodeLabel_->setOpenExternalLinks(true);
	deviceCodeLabel_->setTextInteractionFlags(Qt::TextBrowserInteraction);
	deviceCodeLabel_->setWordWrap(true);
	openAuthPageButton_ = new QPushButton(QStringLiteral("Open page"), twitchGroup);
	openAuthPageButton_->setVisible(false);
	openAuthPageButton_->setToolTip(QStringLiteral("Open the Twitch authorization page in your browser again."));
	connect(openAuthPageButton_, &QPushButton::clicked, this, &CategoryDock::onOpenAuthPageClicked);
	copyDeviceCodeButton_ = new QPushButton(QStringLiteral("Copy code"), twitchGroup);
	copyDeviceCodeButton_->setVisible(false);
	connect(copyDeviceCodeButton_, &QPushButton::clicked, this, &CategoryDock::onCopyDeviceCodeClicked);
	deviceRow->addWidget(deviceCodeLabel_, /*stretch=*/1);
	deviceRow->addWidget(openAuthPageButton_);
	deviceRow->addWidget(copyDeviceCodeButton_);
	twitchLayout->addLayout(deviceRow);

	connectTwitchButton_ = new QPushButton(QStringLiteral("Connect to Twitch"), twitchGroup);
	connectTwitchButton_->setEnabled(false); // Enabled once setTwitchClientId() is called.
	connectTwitchButton_->setToolTip(QStringLiteral("Twitch client ID not configured yet."));
	connect(connectTwitchButton_, &QPushButton::clicked, this, &CategoryDock::onConnectTwitchClicked);
	twitchLayout->addWidget(connectTwitchButton_);

	root->addWidget(twitchGroup);

	// --- Activity log ---
	auto *logGroup = new QGroupBox(QStringLiteral("Activity"), this);
	auto *logLayout = new QVBoxLayout(logGroup);
	activityLogView_ = new QListWidget(logGroup);
	logLayout->addWidget(activityLogView_);
	root->addWidget(logGroup, /*stretch=*/1);

	// --- Footer ---
	auto *footer = new QHBoxLayout();
	// Version links to the patch notes; the build id sits beside it as
	// plain text (see CMakeLists.txt's BUILD IDENTITY block). Two compiles
	// of the same version are otherwise indistinguishable in the UI, which
	// makes "did my reinstall actually take?" unanswerable without hashing
	// the DLL by hand. A trailing "+" means it was built from uncommitted
	// changes.
	//
	// PATCH NOTES URL - the one place to change it. Points at the public
	// page rather than the repo's CHANGELOG.md: the page is written for
	// someone deciding whether to update, the changelog is written for
	// someone reading the project. Same content, different audience, and
	// this label is clicked by the first one.
	const QString patchNotesUrl = QStringLiteral("https://thenerdybox.com/patch-notes/signalbox");

	versionLabel_ = new QLabel(
		QStringLiteral("<a href=\"%3\">v%1</a>"
				" <span style=\"color:palette(mid);\">%2</span>")
			.arg(QString::fromUtf8(PLUGIN_VERSION), QString::fromUtf8(SIGNALBOX_BUILD_ID), patchNotesUrl),
		this);
	versionLabel_->setToolTip(QStringLiteral("Build %1 - click the version for patch notes.")
					  .arg(QString::fromUtf8(SIGNALBOX_BUILD_ID)));
	versionLabel_->setOpenExternalLinks(true); // User-clicked only - never auto-navigated.
	footer->addWidget(versionLabel_);
	footer->addStretch();
	checkUpdatesButton_ = new QPushButton(QStringLiteral("Check for updates"), this);
	connect(checkUpdatesButton_, &QPushButton::clicked, this, &CategoryDock::onCheckForUpdatesClicked);
	footer->addWidget(checkUpdatesButton_);
	rescanButton_ = new QPushButton(QStringLiteral("Rescan"), this);
	rescanButton_->setToolTip(
		QStringLiteral("Immediately re-scan installed games (this normally happens automatically every "
				"few minutes, or on the next detection)."));
	connect(rescanButton_, &QPushButton::clicked, this, &CategoryDock::onRescanClicked);
	footer->addWidget(rescanButton_);
	settingsButton_ = new QPushButton(QStringLiteral("Settings..."), this);
	connect(settingsButton_, &QPushButton::clicked, this, &CategoryDock::onSettingsClicked);
	footer->addWidget(settingsButton_);
	root->addLayout(footer);

	setLayout(root);

	// Nothing is connected yet at construction, so this opens on the
	// Twitch section with Status hidden. setTwitchClientId()'s warm start
	// calls it again a moment later if a saved token turns up, which is
	// what makes the common case (already authorised) show Status without
	// the Twitch box ever flashing into view.
	refreshDockSections();
}

void CategoryDock::wirePromptWidget()
{
	connect(promptWidget_, &PromptWidget::responded, this, [this](bool acceptAction, bool dontAskAgain) {
		stateMachine_.respondToPrompt(acceptAction, dontAskAgain);
		syncPromptWidgetVisibility();
		refreshPresentationIfVisible();
		updateCountdownTimerState();
	});

	// The no-game-while-live prompt has three answers of its own.
	connect(promptWidget_, &PromptWidget::noGameResponded, this, [this](core::NoGameChoice choice) {
		stateMachine_.respondNoGame(choice);
		syncPromptWidgetVisibility();
		refreshPresentationIfVisible();
		updateCountdownTimerState();
	});
}

namespace {

// Compares by the same identity the state machine uses, so this logs when
// the DETECTED GAME changes and not when a pid is recycled or a poll
// happens to rank two equal candidates differently.
bool SameDetected(const std::optional<detection::DetectedGame> &a, const std::optional<detection::DetectedGame> &b)
{
	if (a.has_value() != b.has_value())
		return false;
	if (!a.has_value())
		return true;
	return a->game.platformId == b->game.platformId && a->game.displayName == b->game.displayName;
}

} // namespace

void CategoryDock::onDetectionResult(std::optional<detection::DetectedGame> detected, std::size_t /*indexedGameCount*/)
{
	// One line per CHANGE in what is running, never one per poll. This is
	// the observability that was missing entirely: detection ran every
	// few seconds and wrote nothing anywhere, so a session where nothing
	// was detected and a session where the plugin was frozen produced
	// byte-identical logs.
	if (!SameDetected(detected, lastLoggedDetection_)) {
		if (detected) {
			obs_log(LOG_INFO, "detected: %s (%s, pid %u)",
				QString::fromStdWString(detected->game.displayName).toUtf8().constData(),
				QString::fromStdWString(detected->game.platformId).toUtf8().constData(), detected->pid);
		} else {
			obs_log(LOG_INFO, "detected: nothing");
		}
		lastLoggedDetection_ = detected;
	}

	stateMachine_.onPollResult(std::move(detected));
	syncPromptWidgetVisibility(); // A grace reappearance can make an outstanding GameClosed prompt moot mid-poll.
	updateCountdownTimerState();
	refreshPresentationIfVisible();
}

void CategoryDock::onTrackedProcessExited(detection::ExitReason reason, const detection::InstalledGame &exitedGame)
{
	stateMachine_.onTrackedProcessExited(reason, exitedGame);
	syncPromptWidgetVisibility(); // Entering Grace drops any outstanding prompt.
	updateCountdownTimerState();
	refreshPresentationIfVisible();
}

void CategoryDock::setTwitchClientId(const QString &clientId)
{
	if (clientId.isEmpty())
		return;
	twitchClientId_ = clientId;
	twitchAuth_ = std::make_unique<twitch::TwitchAuth>(clientId, this);

	connect(twitchAuth_.get(), &twitch::TwitchAuth::deviceCodeReady, this, [this](twitch::DeviceCodeInfo info) {
		pendingDeviceCode_ = info.userCode;
		pendingVerificationUri_ = info.verificationUri;
		deviceCodeLabel_->setText(
			QStringLiteral("Code <b>%1</b> &mdash; <a href=\"%2\">open the authorization page</a>")
				.arg(info.userCode.toHtmlEscaped(), info.verificationUri.toHtmlEscaped()));
		deviceFlowActive_ = true;
		refreshDockSections();
		// Open it straight away. The user just clicked "Connect to Twitch", so a
		// browser appearing is the thing they asked for, not an interruption -
		// and expecting anyone to retype a URL with a code in it is exactly the
		// friction this flow exists to remove. The link and the button above
		// remain for the cases where the browser doesn't come forward.
		QDesktopServices::openUrl(QUrl(info.verificationUri));
		twitchStatusLabel_->setText(
			QStringLiteral("Authorize in your browser, then this will connect automatically."));
	});
	connect(twitchAuth_.get(), &twitch::TwitchAuth::tokensAcquired, this, [this](twitch::TokenSet tokens) {
		currentTokens_ = tokens; // userId/login are empty until resolveCurrentUser() below resolves them.
		twitchStatusLabel_->setText(QStringLiteral("Connected - resolving account..."));
		connectionStateLabel_->setText(QStringLiteral("Twitch: connected"));
		deviceFlowActive_ = false;
		refreshDockSections();

		attachTwitchClient(currentTokens_.accessToken, QString());
		twitchClient_->resolveCurrentUser();
	});
	connect(twitchAuth_.get(), &twitch::TwitchAuth::tokensRefreshed, this, [this](twitch::TokenSet tokens) {
		// TwitchAuth::handleRefreshReply() already carries userId/login
		// forward from the token set that was passed to
		// refreshTokens() - see TwitchAuth.cpp - so this is always a
		// complete TokenSet, never a partial one.
		currentTokens_ = tokens;
		saveCurrentTokens();
		if (twitchClient_)
			twitchClient_->updateAccessToken(tokens.accessToken);
		// Unlatch the adapter's own reauthRequired_ flag on the SAME
		// event that unfreezes TwitchClient above - see
		// TwitchChannelClient.h's "REAUTH LATCH LIFECYCLE" note. Without
		// this, CategorySwitchCoordinator keeps refusing every switch
		// with "Twitch needs to be reconnected" for the rest of the
		// session after the first 401, even though TwitchClient itself
		// (and the dock's "Connected as X" header) has recovered.
		if (channelClient_)
			channelClient_->clearReauthRequired();
	});
	connect(twitchAuth_.get(), &twitch::TwitchAuth::authFailed, this, [this](QString reason) {
		twitchStatusLabel_->setText(QStringLiteral("Connection failed: %1").arg(reason));
		connectionStateLabel_->setText(QStringLiteral("Twitch: reconnect needed"));
		// The flow is over and it did not succeed, so the device code on
		// screen is dead - drop it and put "Connect to Twitch" back.
		deviceFlowActive_ = false;
		refreshDockSections();
	});

	connectTwitchButton_->setEnabled(true);
	connectTwitchButton_->setToolTip(QString());

	// Warm start: reuse a previously saved token set (DPAPI-encrypted
	// tokens.bin, twitch/TokenStore.h) rather than forcing a fresh
	// device-code flow on every OBS launch. A stale/expired access token
	// simply fails its first real Helix call, which freezes TwitchClient
	// and fires reauthRequired() - handled below in attachTwitchClient()
	// by kicking off exactly the same refresh path a mid-stream 401
	// would (DESIGN.md 3.3), so this deliberately skips a separate
	// proactive validate() call on startup rather than adding a second
	// code path for the same outcome.
	if (const auto stored = tokenStore_.load()) {
		currentTokens_.accessToken = QString::fromStdWString(stored->accessToken);
		currentTokens_.refreshToken = QString::fromStdWString(stored->refreshToken);
		currentTokens_.userId = QString::fromStdWString(stored->userId);
		currentTokens_.login = QString::fromStdWString(stored->login);
		currentTokens_.obtainedAtUnixS = stored->obtainedAtUnixS;
		currentTokens_.expiresInS = stored->expiresInS;

		attachTwitchClient(currentTokens_.accessToken, currentTokens_.userId);
		if (currentTokens_.userId.isEmpty()) {
			twitchClient_->resolveCurrentUser();
		} else {
			twitchStatusLabel_->setText(QStringLiteral("Connected as %1").arg(currentTokens_.login));
			connectionStateLabel_->setText(QStringLiteral("Twitch: connected as %1").arg(currentTokens_.login));
		}
	}

	// Whether or not a warm start found anything, the dock's sections
	// follow from what it found - a saved token means Status, no token
	// means Twitch. Called once here rather than inside both branches
	// above so neither can be the one that forgets.
	refreshDockSections();
}

void CategoryDock::attachTwitchClient(const QString &accessToken, const QString &broadcasterId)
{
	// A fresh instance each time (rather than reusing/updating in place)
	// keeps this simple and correct across every path that calls this:
	// first-ever connect, warm start, and a future manual "disconnect
	// then reconnect" all just build a clean client. Destroying the old
	// unique_ptrs (if any) disconnects their Qt signal connections
	// automatically.
	twitchClient_ = std::make_unique<twitch::TwitchClient>(twitchClientId_, accessToken, broadcasterId,
								 pluginConfig_.timing().minPatchSpacingS, this);
	categoryLookup_ = std::make_unique<twitch::TwitchCategoryLookup>(*twitchClient_, this);
	channelClient_ = std::make_unique<twitch::TwitchChannelClient>(*twitchClient_, this);
	liveCheckInFlight_ = false; // A read pending on the old client will never be answered.

	// CategoryResolver tier 3 and the coordinator's fallback-category
	// lookup share this one TwitchCategoryLookup instance - see
	// CategoryResolver::setLookup()'s doc comment.
	resolver_.setLookup(categoryLookup_.get());
	coordinator_.setCategoryLookup(categoryLookup_.get());
	coordinator_.setChannelClient(channelClient_.get());

	// Find out what the channel's category actually IS, right now, as
	// soon as there is a client that can ask.
	//
	// Until this existed, liveCategory_ was populated from exactly one
	// place - the coordinator's post-PATCH feedback callback - so the
	// dock read "Live category: (unknown)" for the entire session until
	// SignalBox itself changed something. On the first live test that
	// meant connecting Twitch, opening the dock, and being told the
	// plugin had no idea what category the channel was in, which is the
	// one question the dock exists to answer. syncLiveCategoryOnGoLive()
	// covers the go-live moment for Trigger A's benefit, but going live
	// is not the only time a streamer looks at this.
	//
	// One GET, once, on connect - not a poll (TimingConstants.h's "no
	// ambient polling" directive). Everything after this is kept current
	// by the PATCH feedback channel that was already doing the job.
	syncLiveCategoryFromChannel();

	connect(twitchClient_.get(), &twitch::TwitchClient::currentUserResolved, this,
		[this](QString userId, QString login) {
			currentTokens_.userId = userId;
			currentTokens_.login = login;
			saveCurrentTokens();
			twitchStatusLabel_->setText(QStringLiteral("Connected as %1").arg(login));
			connectionStateLabel_->setText(QStringLiteral("Twitch: connected as %1").arg(login));
			refreshDockSections();
		});

	connect(twitchClient_.get(), &twitch::TwitchClient::reauthRequired, this, [this]() {
		// DESIGN.md 3.3's 401 handling, one level up: TwitchClient only
		// ever holds an access token (never a refresh token - see its
		// class doc comment), so it cannot refresh itself. This is the
		// coordinator-of-the-coordinator responsibility TwitchClient.h
		// documents as belonging to "the owning coordinator" -
		// CategoryDock is that owner here.
		twitchStatusLabel_->setText(QStringLiteral("Twitch connection expired - reconnecting..."));
		connectionStateLabel_->setText(QStringLiteral("Twitch: reconnecting..."));
		if (!twitchAuth_ || currentTokens_.refreshToken.isEmpty()) {
			twitchStatusLabel_->setText(QStringLiteral("Twitch reconnect needed"));
			connectionStateLabel_->setText(QStringLiteral("Twitch: reconnect needed"));
			// Nothing left to refresh with, so this is a genuine dead
			// end that only a fresh authorisation clears. Bring the
			// button back - hiding it while connected is a tidiness
			// win, but leaving it hidden HERE would be a trap.
			currentTokens_.accessToken.clear();
			refreshDockSections();
			return;
		}
		twitchAuth_->refreshTokens(currentTokens_);
	});

	// Edge case: Twitch connects (fresh device-code flow, or a warm
	// start finishing) while OBS is already live - go-live already
	// happened without a channel client to sync from, so
	// handleFrontendEvent()'s own call never ran. Catch up here with the
	// same one-shot GET; a no-op if not live (syncLiveCategoryOnGoLive()
	// only needs channelClient_ to exist, which it now does).
	if (stateMachine_.isLive())
		syncLiveCategoryOnGoLive();

	// Startup validation (project brief item 5 - Twitch's docs require
	// this in addition to the hourly check tickTimer_ already piggybacks
	// on): every path that reaches here (fresh device-code connect, warm
	// start, or the live-reconnect edge case above) is "this session's
	// token becoming active" - see maybeValidateTwitchToken()'s doc
	// comment.
	maybeValidateTwitchToken();
}

void CategoryDock::saveCurrentTokens()
{
	twitch::StoredTokens stored;
	stored.accessToken = currentTokens_.accessToken.toStdWString();
	stored.refreshToken = currentTokens_.refreshToken.toStdWString();
	stored.userId = currentTokens_.userId.toStdWString();
	stored.login = currentTokens_.login.toStdWString();
	stored.obtainedAtUnixS = currentTokens_.obtainedAtUnixS;
	stored.expiresInS = currentTokens_.expiresInS;
	tokenStore_.save(stored);
}

void CategoryDock::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	refreshPresentationIfVisible(); // Catch up on anything skipped while hidden.
	updateCountdownTimerState();    // May need to (re)start the 1Hz countdown now that we're visible.
}

void CategoryDock::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);
	if (countdownTimer_)
		countdownTimer_->stop(); // No presentation work while hidden - see class doc comment.
}

// --- DetectionStateMachine::Listener ---------------------------------

void CategoryDock::onSwitchIn(const detection::InstalledGame &game)
{
	dispatchSwitch(game, /*reassert=*/false);
}

void CategoryDock::onReapplyCategory(const detection::InstalledGame &game)
{
	dispatchSwitch(game, /*reassert=*/true);
}

void CategoryDock::dispatchSwitch(const detection::InstalledGame &game, bool reassert)
{
	// "Only while live" policy layer (DetectionStateMachine.h: owned by
	// PluginConfig/the coordinator, layered ON TOP of the state machine -
	// onSwitchIn itself is not gated on live-ness by design, since an
	// ordinary confirmed switch is allowed offline unless the user opted
	// into this). Project brief item 6: this toggle was persisted,
	// surfaced in SettingsDialog, and enforced nowhere - this is that
	// enforcement.
	if (pluginConfig_.onlyWhileLive() && !stateMachine_.isLive()) {
		appendActivityLogEntry(L"Not live - skipping category change for \"" + game.displayName +
						L"\" (\"Only while live\" is on)",
					L"");
		return;
	}

	// "Never switch for this app" - stop here, and say the thing the user
	// actually asked for.
	//
	// An ignored override resolves to nothing (UserOverrideStore.h: it is
	// deliberately indistinguishable from unmapped inside the resolver),
	// so letting it reach the coordinator would log "Couldn't map that
	// game to a Twitch category" every time the app is detected. That
	// line is true and useless: it reads as a failure to recognise
	// something, when in fact the user explicitly told SignalBox to leave
	// this app alone and it is doing exactly that. The resolver cannot
	// tell those two apart by design; this is the layer that can.
	if (const auto override = pluginConfig_.findUserOverride(core::CategoryResolver::overrideKeyFor(game))) {
		if (override->ignored) {
			appendActivityLogEntry(L"Ignoring \"" + game.displayName +
							L"\" - you asked SignalBox never to switch for it",
						L"");
			return;
		}
	}
	// Delegate only - see core/CategorySwitchCoordinator.h for the
	// resolve -> external-writer-guard -> PATCH -> marker sequence.
	// `live` gates only the marker step there; coordinator_ itself
	// no-ops (with a log line) if Twitch isn't connected yet.
	coordinator_.switchIn(game, stateMachine_.isLive(), reassert);
}

void CategoryDock::onApplyFallback()
{
	// See onSwitchIn()'s note. onApplyFallback() is only ever reached via
	// an explicit GameClosed prompt accept, which the state machine only
	// ever raises while live (ADDENDUM hard constraint #4) - so this
	// branch should be unreachable in practice, but it keeps the policy
	// enforced by construction rather than by "onApplyFallback happens to
	// only be called while live today."
	if (pluginConfig_.onlyWhileLive() && !stateMachine_.isLive()) {
		appendActivityLogEntry(L"Not live - skipping fallback category change (\"Only while live\" is on)", L"");
		return;
	}
	// Same delegation, resolving PluginConfig's free-text fallback
	// category name via categoryLookup_'s exact-match tier - see
	// CategorySwitchCoordinator::applyFallback()'s doc comment.
	coordinator_.applyFallback(pluginConfig_.fallbackCategoryName().empty()
					   ? std::wstring(L"Just Chatting")
					   : pluginConfig_.fallbackCategoryName(),
				    stateMachine_.isLive());
}

void CategoryDock::onPrompt(core::PromptKind kind, const detection::InstalledGame &relevantGame)
{
	const QString gameName = toQString(relevantGame.displayName);
	const QString currentCategory = liveCategory_.isEmpty() ? QStringLiteral("your current category") : liveCategory_;
	// Sourced from the coordinator's feedback channel (see the
	// constructor), which reports the category a PATCH actually applied -
	// not what was requested. A prompt that names the wrong current
	// category is worse than one that names none, so the empty case
	// degrades to generic wording rather than guessing.
	// Trigger C names the category it is offering, and can do so without
	// waiting: a "prompt": true alias carries its category name in the
	// table already loaded in memory, so this is a map lookup, not a
	// Twitch call.
	//
	// Trigger D names the fallback category for the same reason, from
	// config rather than the alias table - it is offering "no game", not
	// a category derived from something that is running.
	QString targetCategory;
	if (kind == core::PromptKind::CreativeApp)
		targetCategory = toQString(resolver_.aliasCategoryFor(relevantGame));
	else if (kind == core::PromptKind::NoGameIdle || kind == core::PromptKind::GameClosed)
		targetCategory = toQString(pluginConfig_.fallbackCategoryName());
	promptWidget_->showPrompt(kind, gameName, currentCategory, targetCategory);

	// The one prompt worth an out-of-OBS nudge: the streamer is live, in a
	// game or on another screen, and may never look at the dock. The card
	// is non-activating and a click takes them to the prompt itself.
	if (kind == core::PromptKind::GameClosed) {
		toast_->showToast(QStringLiteral("SignalBox: no game detected"),
				   QStringLiteral("You're live in \"%1\". Stream ending, switching to Just Chatting, or "
						  "waiting for a game? Click to answer in OBS.")
					   .arg(currentCategory));
	}
	// A creative app offered while live is the same situation: nobody is
	// watching the dock mid-stream, so without the card it times out unseen.
	else if (kind == core::PromptKind::CreativeApp && stateMachine_.isLive()) {
		toast_->showToast(QStringLiteral("SignalBox: switch category?"),
				   QStringLiteral("\"%1\" is running. Switch to \"%2\"? Click to answer in OBS.")
					   .arg(gameName, targetCategory));
	}
	updateCountdownTimerState();
	refreshPresentationIfVisible();
}

void CategoryDock::onIndexRebuilt(std::size_t gameCount, std::size_t excludedCount, std::size_t duplicateCount,
				   double durationMs, bool userRequested, std::size_t denylistRuleCount)
{
	const bool changed = !haveIndexedCount_ || gameCount != lastIndexedCount_;
	lastIndexedCount_ = gameCount;
	haveIndexedCount_ = true;

	if (!userRequested && !changed) {
		return; // Periodic rebuild, nothing moved - say nothing.
	}

	std::wstring message = userRequested ? L"Rescan complete — " : L"Index refreshed — ";
	message += std::to_wstring(gameCount) + L" app";
	if (gameCount != 1)
		message += L"s";
	message += L" indexed";
	if (excludedCount > 0 || duplicateCount > 0) {
		message += L" (" + std::to_wstring(excludedCount) + L" excluded, " +
			   std::to_wstring(duplicateCount) + L" duplicate";
		if (duplicateCount != 1)
			message += L"s";
		message += L")";
	}
	message += L", " + std::to_wstring(static_cast<long long>(durationMs + 0.5)) + L"ms";

	// Only the button re-reads helpers.json, so only the button can
	// honestly claim it was reloaded - see this method's header comment.
	// Reported even when the count is unchanged: the useful signal is
	// "your edit was picked up", and a rule count that stayed the same
	// after an edit is exactly the case worth seeing.
	if (userRequested) {
		message += L" · helpers.json reloaded, " + std::to_wstring(denylistRuleCount) + L" rule";
		if (denylistRuleCount != 1)
			message += L"s";
	}
	appendActivityLogEntry(message, L"");
}

void CategoryDock::onAliasTableReloaded(std::size_t aliasCount)
{
	std::wstring message = L"Reloaded aliases.json — " + std::to_wstring(aliasCount) + L" alias";
	if (aliasCount != 1)
		message += L"es";
	appendActivityLogEntry(message, L"");
}

void CategoryDock::onAutomationPaused(const std::wstring &reasonForDock)
{
	pausedReason_ = toQString(reasonForDock);
	appendActivityLogEntry(reasonForDock, L"");
	refreshPresentationIfVisible();
}

void CategoryDock::onResumeClicked()
{
	// resume() deliberately re-detects from scratch rather than trusting
	// the previous ACTIVE game (DESIGN.md 3.5) - whatever caused the
	// pause may have invalidated that picture. The next poll repopulates
	// everything, so the dock briefly showing "(nothing)" here is
	// correct, not a glitch.
	pausedReason_.clear();
	stateMachine_.resume();
	appendActivityLogEntry(L"Resumed — detecting again from scratch", L"");
	refreshPresentationIfVisible();
}

void CategoryDock::onLogEntry(const std::wstring &message, const std::wstring &previousCategory)
{
	appendActivityLogEntry(message, previousCategory);
}

void CategoryDock::appendActivityLogEntry(const std::wstring &message, const std::wstring &previousCategory)
{
	// Mirrored into the OBS log, which is the only record that survives a
	// crash, a restart, or someone closing the dock - and the only thing
	// anyone can attach to a bug report. The in-dock list is a bounded
	// ring buffer that lives and dies with the widget.
	//
	// This is the whole of the plugin's OBS logging, and it is
	// deliberately driven by activity rather than by polling: an entry
	// exists only when something actually happened - a switch, a prompt,
	// a pause, a rebuild whose numbers moved. Detection runs every few
	// seconds and says nothing, because a log line per poll would be
	// worthless noise in the file people are asked to send us.
	obs_log(LOG_INFO, "%s", QString::fromStdWString(message).toUtf8().constData());

	logEntries_.push_back(LogEntry{message, previousCategory, false});
	while (logEntries_.size() > kMaxLogEntries) // Bounded ring buffer - see class doc comment.
		logEntries_.pop_front();
	refreshPresentationIfVisible();
}

// --- OBS frontend event -----------------------------------------------

void CategoryDock::frontendEventTrampoline(enum obs_frontend_event event, void *privateData)
{
	static_cast<CategoryDock *>(privateData)->handleFrontendEvent(event);
}

void CategoryDock::handleFrontendEvent(enum obs_frontend_event event)
{
	if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED) {
		stateMachine_.setLive(true);
		syncLiveCategoryOnGoLive();
	} else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED) {
		stateMachine_.setLive(false);
	} else {
		return; // Not a transition we care about.
	}
	// Live-ness is one of Trigger D's three gates whenever "only while
	// live" is on - see refreshIdlePromptEnabled().
	refreshIdlePromptEnabled();
	syncPromptWidgetVisibility(); // Going offline drops any outstanding prompt (ADDENDUM constraint #4).
	updateCountdownTimerState();
	refreshPresentationIfVisible();
}

void CategoryDock::syncPromptWidgetVisibility()
{
	// See CategoryDock.h's doc comment on this method. The state machine
	// can resolve its own outstanding prompt (timeout, a grace
	// reappearance making it moot, setLive(false)) without ever routing
	// through PromptWidget's click handlers - those are the only paths
	// that already hide() it themselves. This is the one place that
	// catches every other path; PromptWidget::dismissWithoutResponse()
	// exists for exactly this and previously had zero call sites.
	if (!stateMachine_.promptOutstanding() && promptWidget_->isVisible())
		promptWidget_->dismissWithoutResponse();

	// The notification card lives exactly as long as the no-game prompt.
	const bool noGamePromptUp =
		stateMachine_.promptOutstanding() &&
		(stateMachine_.outstandingPromptKind() == core::PromptKind::GameClosed ||
		 stateMachine_.outstandingPromptKind() == core::PromptKind::CreativeApp);
	if (!noGamePromptUp && toast_)
		toast_->dismiss();
}

void CategoryDock::revealPrompt()
{
	if (toast_)
		toast_->dismiss();
	for (QWidget *w = this; w; w = w->parentWidget()) {
		if (auto *dockWidget = qobject_cast<QDockWidget *>(w)) {
			dockWidget->setVisible(true);
			dockWidget->raise(); // Surfaces it if it is a background tab.
			break;
		}
	}
	// A click on the card is the user's own action, so bringing OBS forward
	// here is the intended result, not a focus steal.
	if (QWidget *top = window()) {
		top->show();
		top->raise();
		top->activateWindow();
	}
}

void CategoryDock::maybeValidateTwitchToken()
{
	// See CategoryDock.h's doc comment and TimingConstants.h's
	// ShouldValidateTwitchToken(). No-op until there's an actual token to
	// validate.
	if (!twitchAuth_ || currentTokens_.accessToken.isEmpty())
		return;

	const qint64 nowUnixS = QDateTime::currentSecsSinceEpoch();
	if (!core::ShouldValidateTwitchToken(lastValidatedAtUnixS_, nowUnixS, pluginConfig_.timing().tokenValidationIntervalS))
		return;

	// Record the attempt now, not on a successful response - this is a
	// scheduling floor ("don't ask more than once an hour"), not a
	// success tracker; a failed validate here already routes to
	// authFailed()'s reconnect UI (TwitchAuth.h), and retrying it every
	// single 5s tick while that's unresolved would be exactly the tight-
	// polling behavior the owner's directive rules out.
	lastValidatedAtUnixS_ = nowUnixS;
	twitchAuth_->validate(currentTokens_.accessToken);
}

void CategoryDock::syncLiveCategoryFromChannel(bool logAsGoLive)
{
	if (!channelClient_) {
		if (logAsGoLive)
			obs_log(LOG_INFO, "live category at go-live: not read (Twitch is not connected)");
		return; // Not connected yet - nothing to ask; see header doc comment.
	}

	channelClient_->getChannelInfo([this, logAsGoLive](std::optional<core::ChannelSnapshot> snapshot) {
		if (!snapshot) {
			if (logAsGoLive)
				obs_log(LOG_INFO, "live category at go-live: could not be read");
			return; // Couldn't verify - never overwrite a known value with a guess.
		}
		if (logAsGoLive)
			obs_log(LOG_INFO, "live category at go-live: \"%s\"", toQString(snapshot->gameName).toUtf8().constData());

		// BOTH, not just the state machine. This used to feed
		// onLiveCategoryKnown() alone, which gives Trigger A its
		// comparison basis but leaves the dock's own label reading
		// "(unknown)" - so the plugin knew the answer and still
		// displayed that it didn't.
		stateMachine_.onLiveCategoryKnown(snapshot->gameName);
		liveCategory_ = toQString(snapshot->gameName);
		refreshPresentationIfVisible();
	});
}

void CategoryDock::syncLiveCategoryOnGoLive()
{
	syncLiveCategoryFromChannel(/*logAsGoLive=*/true);
}

void CategoryDock::maybeVerifyLiveCategory()
{
	if (!stateMachine_.takeLiveCategoryCheckDue())
		return; // Not live, or no slot due yet.

	if (!channelClient_) {
		obs_log(LOG_DEBUG, "live category check: skipped (Twitch is not connected)");
		return;
	}
	if (liveCheckInFlight_) {
		obs_log(LOG_DEBUG, "live category check: skipped (previous read still in flight)");
		return;
	}

	liveCheckInFlight_ = true;
	channelClient_->getChannelInfo([this](std::optional<core::ChannelSnapshot> snapshot) {
		liveCheckInFlight_ = false;

		std::optional<std::wstring> liveName;
		if (snapshot) {
			liveName = snapshot->gameName;
			liveCategory_ = toQString(snapshot->gameName);
		}
		const core::VerifyResult result = stateMachine_.onLiveCategoryVerified(liveName);

		const QString live = toQString(result.liveCategory);
		const QString expected = toQString(result.expectedCategory);
		if (result.outcome == core::VerifyOutcome::Reapplied) {
			obs_log(LOG_INFO, "live category was \"%s\", re-applied \"%s\"%s", live.toUtf8().constData(),
				expected.toUtf8().constData(),
				result.withinGoLiveWindow ? " (possible multistream/external override)" : "");
		} else {
			const char *what = "unknown";
			switch (result.outcome) {
			case core::VerifyOutcome::NotLive: what = "not live"; break;
			case core::VerifyOutcome::NotVerifiable: what = "could not read the channel"; break;
			case core::VerifyOutcome::NoActiveGame: what = "no confirmed game"; break;
			case core::VerifyOutcome::Matches: what = "matches the running game"; break;
			case core::VerifyOutcome::SkippedLocked: what = "differs, left alone (manual lock)"; break;
			case core::VerifyOutcome::SkippedHold: what = "differs, left alone (stream-ending hold)"; break;
			case core::VerifyOutcome::SkippedPaused: what = "differs, left alone (automation paused)"; break;
			case core::VerifyOutcome::SkippedPrompt: what = "differs, left alone (waiting on your answer)"; break;
			case core::VerifyOutcome::SkippedKept: what = "differs, left alone (you chose it)"; break;
			case core::VerifyOutcome::SkippedRecentSwitch: what = "differs, left alone (just switched)"; break;
			case core::VerifyOutcome::Reapplied: break;
			}
			obs_log(LOG_DEBUG, "live category check: %s (channel: \"%s\", game: \"%s\")", what,
				live.toUtf8().constData(), expected.toUtf8().constData());
		}

		syncPromptWidgetVisibility();
		refreshPresentationIfVisible();
	});
}

void CategoryDock::refreshDockSections()
{
	// ONE place decides what this panel shows, from state, every time.
	//
	// Visibility used to be poked from each individual signal handler,
	// and the predictable thing happened: tokensAcquired hid the device
	// code label and the "Copy code" button but not "Open page", so a
	// successful reconnect left a button offering to reopen an
	// authorisation page for a code that had already been consumed. The
	// same scattering left "Connect to Twitch" visible underneath
	// "Connected as <you>". Neither is a hard failure, which is why both
	// survived - they just quietly say the plugin does not know its own
	// state. Deriving all of it here means a new state cannot forget a
	// widget.
	const bool connected = !currentTokens_.accessToken.isEmpty();

	// The device-code row belongs to an authorisation in flight and
	// nothing else.
	deviceCodeLabel_->setVisible(deviceFlowActive_);
	openAuthPageButton_->setVisible(deviceFlowActive_);
	copyDeviceCodeButton_->setVisible(deviceFlowActive_);

	// Offer connecting only when connecting is the thing to do. During a
	// flow it would start a second one; once connected it contradicts the
	// line above it.
	connectTwitchButton_->setVisible(!connected && !deviceFlowActive_);

	// STATUS AND TWITCH ARE MUTUALLY EXCLUSIVE - only ever one of them.
	//
	// A dock lives in the same screen space a streamer wants for their
	// scenes and sources, so every permanently-visible group has to earn
	// the height it occupies. Neither of these earns it all the time:
	// before authorising, Status can only report that nothing can happen
	// yet; after authorising, the Twitch group is a box confirming a
	// thing that already happened - and it was actively harmful, since it
	// kept offering to connect an account that was already connected.
	//
	// So the dock shows exactly what is actionable: Twitch while there is
	// connecting to do, Status once there is detection to watch. Activity
	// and the footer stay put, because the log is the record of what the
	// plugin did and the footer is how you rescan, reach settings, check
	// for updates and see which build you are on - none of which depends
	// on connection state.
	//
	// Losing authentication swaps them back, by the same rule and the
	// same single call. Reconnecting also stays permanently available in
	// Settings, which is what makes hiding this group safe rather than a
	// trap: see SettingsDialog's Twitch section.
	twitchGroup_->setVisible(!connected);
	statusGroup_->setVisible(connected);

	// Rides along here rather than getting its own set of call sites,
	// because it is derived from the SAME `connected` value this function
	// exists to react to, and every path that can change that value
	// already calls this one. Splitting them would recreate exactly the
	// scattered-visibility bug described above, in a place where the
	// symptom is subtler: a prompt that stops appearing, or appears when
	// it cannot be acted on.
	refreshIdlePromptEnabled();
}

// --- Presentation (gated on isVisible()) -------------------------------

void CategoryDock::markPresentationDirty()
{
	presentationDirty_ = true;
}

void CategoryDock::refreshPresentationIfVisible()
{
	if (!isVisible()) {
		markPresentationDirty();
		return;
	}
	refreshStatusLabels();
	refreshLogView();
	presentationDirty_ = false;
}

void CategoryDock::refreshStatusLabels()
{
	liveCategoryLabel_->setText(liveCategory_.isEmpty()
					    ? QStringLiteral("Live category: (unknown)")
					    : QStringLiteral("Live category: %1").arg(liveCategory_));

	if (auto active = stateMachine_.activeGame())
		detectedGameLabel_->setText(QStringLiteral("Detected: %1").arg(toQString(active->displayName)));
	else if (auto pending = stateMachine_.pendingCandidate())
		detectedGameLabel_->setText(QStringLiteral("Detected: %1 (confirming...)").arg(toQString(pending->displayName)));
	else if (auto grace = stateMachine_.graceGame())
		detectedGameLabel_->setText(QStringLiteral("%1 closed - waiting to see if it comes back").arg(toQString(grace->displayName)));
	else
		detectedGameLabel_->setText(QStringLiteral("Detected: (nothing)"));

	// "Fix This!" is only meaningful when there's an identity to
	// correct - see currentGameForFix()'s doc comment for the exact
	// precedence, which mirrors the label text built just above.
	fixDetectionButton_->setEnabled(currentGameForFix().has_value());

	manualLockButton_->setText(stateMachine_.isManuallyLocked() ? QStringLiteral("Automatic switching: OFF (locked)")
								      : QStringLiteral("Automatic switching: ON"));

	// The state machine is the source of truth for the hold, not this
	// button - it clears itself on a stream stop or a new game, neither
	// of which goes anywhere near the UI. QSignalBlocker rather than
	// relying on onStreamEndingToggled()'s own no-op guard: two
	// independent reasons a stray toggle can't do damage is the right
	// number for a control that suspends automation.
	const bool holding = stateMachine_.streamEndingHold();
	{
		const QSignalBlocker blocker(streamEndingButton_);
		streamEndingButton_->setChecked(holding);
	}
	// The resting label says what pressing it DOES; the held label says what
	// is happening. Appending ": ON" to the long name instead would just
	// elide in a narrow dock, which is the one place the state has to be
	// readable. The banner below carries the rest.
	streamEndingButton_->setText(holding ? QStringLiteral("Detection stopped — press to resume")
					       : QStringLiteral("Stream Ending/Stop Detection"));

	streamEndingBanner_->setVisible(holding);
	if (holding) {
		streamEndingBanner_->setText(QStringLiteral(
			"Stream ending — SignalBox is leaving your category alone.\n"
			"This clears itself when you stop streaming, or when a new game starts."));
	}

	// Nothing to apply a category to without a connection - and the
	// button's own text names a Twitch category, so leaving it live while
	// disconnected promises something it cannot do.
	justChattingButton_->setEnabled(!currentTokens_.accessToken.isEmpty());

	const bool paused = stateMachine_.isAutomationPaused();
	pausedBanner_->setVisible(paused);
	resumeButton_->setVisible(paused);
	if (paused) {
		pausedBanner_->setText(
			QStringLiteral("Automation is paused — SignalBox is not changing your category.\n%1")
				.arg(pausedReason_.isEmpty()
					     ? QStringLiteral("See the activity log for why.")
					     : pausedReason_));
	}

	if (auto remaining = stateMachine_.secondsUntilNextDeadline()) {
		countdownLabel_->setVisible(true);

		if (stateMachine_.promptOutstanding()) {
			countdownLabel_->setText(QStringLiteral("Waiting for your answer: %1s").arg(*remaining));
		} else {
			// PROMISE ONLY WHAT WILL ACTUALLY HAPPEN.
			//
			// This said "asking in Ns" during every grace window,
			// whatever the state machine was going to do at the end of
			// it. The GameClosed prompt (Trigger B) only fires while
			// live and with prompts enabled - see
			// DetectionStateMachine.h's ADDENDUM notes - so closing a
			// game offline counted down to a question that could never
			// be asked, then silently held the category instead.
			// Reported as a bug, and it was one: the countdown, not the
			// hold. Holding is the correct and documented behavior; the
			// label was describing a different plugin.
			const bool willAsk = stateMachine_.isLive() && stateMachine_.promptsEnabled();
			const auto grace = stateMachine_.graceGame();
			const QString held = grace ? toQString(grace->displayName) : QString();

			if (willAsk && !grace) {
				countdownLabel_->setText(
					QStringLiteral("No game detected - checking in with you in %1s").arg(*remaining));
			} else if (willAsk) {
				countdownLabel_->setText(
					QStringLiteral("No game detected - asking in %1s").arg(*remaining));
			} else if (!held.isEmpty()) {
				countdownLabel_->setText(QStringLiteral("No game detected - keeping \"%1\" in %2s")
								   .arg(held)
								   .arg(*remaining));
			} else {
				countdownLabel_->setText(
					QStringLiteral("No game detected - keeping your category in %1s")
						.arg(*remaining));
			}
		}
	} else {
		countdownLabel_->setVisible(false);
	}
}

void CategoryDock::refreshLogView()
{
	// Bounded rebuild (kMaxLogEntries caps this), and only ever called
	// from refreshPresentationIfVisible() - i.e. on an actual log
	// change while visible, or once when becoming visible again. Never
	// from a timer.
	activityLogView_->clear();
	for (std::size_t i = 0; i < logEntries_.size(); ++i) {
		const LogEntry &entry = logEntries_[i];
		auto *row = new QWidget(activityLogView_);
		auto *rowLayout = new QHBoxLayout(row);
		rowLayout->setContentsMargins(2, 2, 2, 2);

		QString text = toQString(entry.message);
		if (entry.undone)
			text += QStringLiteral(" (undone)");
		rowLayout->addWidget(new QLabel(text, row), /*stretch=*/1);

		// One-click Undo (DESIGN.md 2.2 / Section 5) - only offered for
		// entries that actually changed the category and haven't
		// already been undone.
		if (!entry.previousCategory.empty() && !entry.undone) {
			auto *undoButton = new QPushButton(QStringLiteral("Undo"), row);
			connect(undoButton, &QPushButton::clicked, this, [this, i]() { onUndoRequested(i); });
			rowLayout->addWidget(undoButton);
		}
		row->setLayout(rowLayout);

		auto *item = new QListWidgetItem(activityLogView_);
		item->setSizeHint(row->sizeHint());
		activityLogView_->addItem(item);
		activityLogView_->setItemWidget(item, row);
	}
	if (activityLogView_->count() > 0)
		activityLogView_->scrollToBottom();
}

void CategoryDock::updateCountdownTimerState()
{
	const bool shouldRun = isVisible() && stateMachine_.secondsUntilNextDeadline().has_value();
	if (shouldRun && !countdownTimer_->isActive())
		countdownTimer_->start();
	else if (!shouldRun && countdownTimer_->isActive())
		countdownTimer_->stop();
}

// --- User actions -------------------------------------------------------

void CategoryDock::onManualLockToggled(bool locked)
{
	stateMachine_.setManualLock(locked);
	refreshStatusLabels();
}

void CategoryDock::onStreamEndingToggled(bool holding)
{
	// refreshStatusLabels() writes this button's check state from the
	// state machine, and setChecked() emits toggled() - so without a
	// guard, every refresh that corrected the button would call back in
	// here and re-apply what it was correcting. Comparing against the
	// state machine first makes both directions idempotent: a press that
	// agrees with reality does nothing, which is exactly right.
	if (holding == stateMachine_.streamEndingHold())
		return;

	if (holding)
		stateMachine_.beginStreamEndingHold();
	else
		stateMachine_.clearStreamEndingHold();

	// The hold drops any outstanding prompt (see beginStreamEndingHold()),
	// so the widget has to be told, or it lingers with dead buttons.
	syncPromptWidgetVisibility();
	updateCountdownTimerState();
	refreshPresentationIfVisible();
}

void CategoryDock::onJustChattingClicked()
{
	const std::wstring category = pluginConfig_.fallbackCategoryName().empty()
					      ? std::wstring(L"Just Chatting")
					      : pluginConfig_.fallbackCategoryName();

	// Deliberately NOT gated on pluginConfig_.onlyWhileLive(), unlike
	// onApplyFallback(). That setting restrains AUTOMATION to the live
	// window; this is a button the user just pressed, and refusing a
	// direct instruction because of a setting about automatic behavior
	// would read as the button being broken. Setting the category before
	// going live is also the single most useful time to press it.
	//
	// Routed through the coordinator (not channelClient_ directly) so the
	// resolve -> external-writer-guard -> PATCH -> feedback sequence is
	// the same one every other category change takes: the guard's
	// lastAppliedCategoryId_ updates, and liveCategory_/the activity log
	// get the real applied name rather than the one we asked for.
	stateMachine_.noteUserCategoryChoice(); // Their choice: verification must not undo it a minute later.
	coordinator_.applyFallback(category, stateMachine_.isLive());
}

void CategoryDock::refreshIdlePromptEnabled()
{
	// Three gates, ANDed - see DetectionStateMachine.h's TRIGGER D note.
	//
	// The Twitch one is not defensive padding: the prompt's whole content
	// is an offer to set a category, and the state machine cannot know
	// whether that is possible. Asking "no game running, what are you up
	// to?" with no account connected produces a question whose only real
	// answer is "nothing you can do about it" - worse than silence.
	//
	// onlyWhileLive is here for the same reason, one step further out: it
	// makes onApplyFallback() refuse while offline, and Trigger D fires
	// (by design) while offline, so with that setting on the prompt's
	// primary button would land in a log line saying it was skipped.
	// Rather than special-case the button, don't ask the question.
	const bool connected = !currentTokens_.accessToken.isEmpty();
	const bool answerable = !pluginConfig_.onlyWhileLive() || stateMachine_.isLive();
	stateMachine_.setNoGameIdleEnabled(pluginConfig_.idlePromptEnabled() && connected && answerable);
}

void CategoryDock::onSettingsClicked()
{
	SettingsDialog dialog(pluginConfig_, stateMachine_, twitchAuth_.get(), currentTokens_.login, this);
	dialog.exec();
	// Reflect any changes (manual lock, timing) the dialog made.
	manualLockButton_->setChecked(stateMachine_.isManuallyLocked());
	// Two of Trigger D's three gates live in this dialog (the idle-prompt
	// toggle and "only while live"), so a Save has to be able to switch
	// it on or off without waiting for a connection event to notice.
	refreshIdlePromptEnabled();
	refreshPresentationIfVisible();
}

std::optional<detection::InstalledGame> CategoryDock::currentGameForFix() const
{
	// A prompt outstanding takes priority: for a GameClosed prompt in
	// particular, promptRelevantGame_ is the copy of the identity that
	// survives the moment graceGame_ itself gets reset (see
	// DetectionStateMachine::onTick()'s Grace-state handling) - using it
	// first means "Fix This!" keeps working for the exact game the
	// on-screen prompt is asking about, even mid-prompt. Otherwise this
	// mirrors refreshStatusLabels()'s own active -> pending -> grace
	// precedence exactly, so the button always corrects whatever the
	// label just told the user, never a different game.
	if (stateMachine_.promptOutstanding()) {
		if (auto relevant = stateMachine_.promptRelevantGame())
			return relevant;
	}
	if (auto active = stateMachine_.activeGame())
		return active;
	if (auto pending = stateMachine_.pendingCandidate())
		return pending;
	if (auto grace = stateMachine_.graceGame())
		return grace;
	return std::nullopt;
}

void CategoryDock::onFixDetectionClicked()
{
	const auto game = currentGameForFix();
	if (!game)
		return; // Button is disabled in this case (see refreshStatusLabels()) - defensive no-op.

	// categoryLookup_ may be nullptr (Twitch not connected yet - see
	// attachTwitchClient()) - GameOverrideDialog is built to handle that
	// itself (its "Never switch"/"Clear" actions need no lookup at all;
	// only its search section disables). Passed as the ICategoryLookup
	// seam, not the concrete TwitchCategoryLookup, so the dialog stays as
	// decoupled from Twitch specifics as CategoryResolver itself is.
	GameOverrideDialog dialog(pluginConfig_, categoryLookup_.get(), *game, this);
	if (dialog.exec() != QDialog::Accepted) {
		return; // Dismissed without saving - nothing changed, nothing to apply.
	}

	// APPLY IT NOW, DON'T WAIT FOR THE NEXT DETECTION.
	//
	// Saving an override only changes what the NEXT resolve() produces,
	// and the app being fixed is - by definition - already detected and
	// already resolved, so nothing re-resolves on its own. On the first
	// live test that read as the button doing nothing: the override was
	// written to disk correctly, and the dock went on showing the same
	// wrong state until an unrelated game launched minutes later. A
	// correction the user has just confirmed should take effect while
	// they are still looking at it.
	//
	// Re-checked rather than reusing `game`: exec() is modal but the
	// detection worker is not, so the current detection may have moved on
	// while the dialog was open. Applying the override of an app that is
	// no longer running would set a category for something the user
	// stopped doing.
	const auto stillCurrent = currentGameForFix();
	if (!stillCurrent || core::CategoryResolver::overrideKeyFor(*stillCurrent) !=
				     core::CategoryResolver::overrideKeyFor(*game)) {
		appendActivityLogEntry(L"Saved that fix - it applies the next time \"" + game->displayName +
					       L"\" is detected",
					L"");
		return;
	}

	// An "ignore this app" override resolves to nothing by design, so
	// there is deliberately no switch to make here - say so instead of
	// calling switchIn() and letting it log a confusing "couldn't map
	// that game" line for what is actually the requested outcome.
	if (const auto saved = pluginConfig_.findUserOverride(core::CategoryResolver::overrideKeyFor(*stillCurrent))) {
		if (saved->ignored) {
			appendActivityLogEntry(L"\"" + stillCurrent->displayName +
						       L"\" will no longer change your category",
						L"");
			return;
		}
	}

	coordinator_.switchIn(*stillCurrent, stateMachine_.isLive());
}

namespace {

// Compares two dotted numeric versions ("0.2.5") and returns <0, 0 or >0.
// Deliberately not a lexicographic string compare: "0.2.10" is NEWER than
// "0.2.9" and sorts BEFORE it as text, which is the classic way an update
// check quietly stops offering updates once a project passes .9. A missing
// component counts as 0, so "0.3" == "0.3.0". Anything non-numeric in a
// component makes that component 0 rather than throwing - a malformed tag
// should mean "no update offered", never a crash in a callback.
int CompareVersions(const QString &a, const QString &b)
{
	const QStringList lhs = a.split(QLatin1Char('.'));
	const QStringList rhs = b.split(QLatin1Char('.'));
	const int parts = std::max(lhs.size(), rhs.size());
	for (int i = 0; i < parts; ++i) {
		const int l = (i < lhs.size()) ? lhs.at(i).toInt() : 0;
		const int r = (i < rhs.size()) ? rhs.at(i).toInt() : 0;
		if (l != r)
			return (l < r) ? -1 : 1;
	}
	return 0;
}

} // namespace

void CategoryDock::onCheckForUpdatesClicked()
{
	// SECOND PRESS, WHEN AN UPDATE IS ALREADY KNOWN, OPENS IT. Re-querying
	// the API to be told the same thing again is not what the user wants at
	// that point, and a separate button for it would occupy dock height for
	// something that is usually irrelevant. QDesktopServices only ever runs
	// from this direct click - nothing in this plugin opens a browser on its
	// own initiative.
	if (!availableUpdateUrl_.isEmpty()) {
		QDesktopServices::openUrl(QUrl(availableUpdateUrl_));
		return;
	}

	// The ONLY network call this plugin makes without a direct, adjacent
	// user action being THIS click (DESIGN.md Section 8) - no automatic
	// check on launch, no timer.
	//
	// Goes through HttpTransport for the same reason everything else does:
	// OBS ships no TLS backend for Qt, so QNetworkAccessManager cannot
	// establish an HTTPS connection at all and fails with "TLS
	// initialization failed".
	if (!updateCheckTransport_)
		updateCheckTransport_ = new twitch::HttpTransport(this);

	checkUpdatesButton_->setEnabled(false);
	checkUpdatesButton_->setText(QStringLiteral("Checking..."));

	// /releases/latest deliberately EXCLUDES pre-releases, which is exactly
	// the behaviour wanted here: an untested build is published as a
	// pre-release on purpose, and nobody running a stable install should be
	// prompted to move onto one. A 404 therefore means "no stable release
	// yet", not "the repo is missing", and is reported as such below.
	twitch::HttpRequest request;
	request.method = twitch::HttpMethod::Get;
	request.url = QStringLiteral("https://api.github.com/repos/TheNerdyBox/signalbox/releases/latest");
	request.headers.append({QByteArrayLiteral("Accept"), QByteArrayLiteral("application/vnd.github+json")});
	// GitHub rejects requests with no User-Agent outright.
	request.headers.append({QByteArrayLiteral("User-Agent"), QByteArrayLiteral("SignalBox")});

	updateCheckTransport_->send(request, [this](twitch::HttpResponse response) {
		checkUpdatesButton_->setEnabled(true);
		checkUpdatesButton_->setText(QStringLiteral("Check for updates"));

		// EVERY OUTCOME GOES TO THE ACTIVITY LOG, not just a tooltip. A
		// tooltip requires knowing to hover over a button that looks
		// exactly as it did before the click, which is indistinguishable
		// from the button doing nothing - the same class of defect as the
		// silent pause this dock already learned about the hard way.
		if (response.transportError) {
			appendActivityLogEntry(L"Couldn't check for updates - " +
						response.transportErrorString.toStdWString(),
						L"");
			return;
		}
		if (response.httpStatus == 404) {
			appendActivityLogEntry(L"No stable release published yet - you're on a preview build", L"");
			return;
		}
		if (response.httpStatus != 200) {
			appendActivityLogEntry(L"Couldn't check for updates - GitHub returned " +
						std::to_wstring(response.httpStatus),
						L"");
			return;
		}

		const QJsonObject release = QJsonDocument::fromJson(response.body).object();
		const QString latestTag = release.value(QStringLiteral("tag_name")).toString();
		if (latestTag.isEmpty()) {
			appendActivityLogEntry(L"Couldn't check for updates - no version in GitHub's reply", L"");
			return;
		}

		// Tags are published as "v0.2.5"; PLUGIN_VERSION is "0.2.5".
		QString latest = latestTag;
		if (latest.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
			latest.remove(0, 1);
		const QString current = QString::fromUtf8(PLUGIN_VERSION);

		versionLabel_->setToolTip(QStringLiteral("Build %1 - latest release is v%2.")
						  .arg(QString::fromUtf8(SIGNALBOX_BUILD_ID), latest));

		if (CompareVersions(current, latest) >= 0) {
			// Covers "ahead of the latest release" too, which is the
			// normal state while running a preview build - saying
			// "up to date" there is true and is what the user needs.
			appendActivityLogEntry(L"You're up to date - v" + current.toStdWString() + L" is current", L"");
			return;
		}

		const QString releaseUrl = release.value(QStringLiteral("html_url")).toString();
		availableUpdateUrl_ = releaseUrl.isEmpty()
					      ? QStringLiteral("https://github.com/TheNerdyBox/signalbox/releases/latest")
					      : releaseUrl;
		checkUpdatesButton_->setText(QStringLiteral("Get v%1").arg(latest));
		checkUpdatesButton_->setToolTip(
			QStringLiteral("Opens the v%1 release page. Close OBS before running the installer.").arg(latest));
		appendActivityLogEntry(L"SignalBox v" + latest.toStdWString() + L" is available - you have v" +
					current.toStdWString() + L". Press \"Get v" + latest.toStdWString() +
					L"\" in the footer.",
				L"");
	});
}

void CategoryDock::onConnectTwitchClicked()
{
	if (!twitchAuth_)
		return;
	twitchStatusLabel_->setText(QStringLiteral("Requesting device code..."));
	twitchAuth_->beginDeviceCodeFlow();
}

void CategoryDock::onCopyDeviceCodeClicked()
{
	if (pendingDeviceCode_.isEmpty())
		return;
	QGuiApplication::clipboard()->setText(pendingDeviceCode_);
}

void CategoryDock::onOpenAuthPageClicked()
{
	// The page is opened automatically when the code arrives; this is for
	// afterwards - the browser opened behind a full-screen game, the tab was
	// closed, or the default browser was slow to appear.
	if (pendingVerificationUri_.isEmpty())
		return;
	QDesktopServices::openUrl(QUrl(pendingVerificationUri_));
}

void CategoryDock::onRescanClicked()
{
	emit rescanRequested(); // plugin-main.cpp connects this to DetectionEngine::requestRescan().
	appendActivityLogEntry(L"Requested an install-index rescan", L"");
}

void CategoryDock::onUndoRequested(std::size_t logIndex)
{
	if (logIndex >= logEntries_.size())
		return;
	const LogEntry &entry = logEntries_[logIndex];
	if (entry.previousCategory.empty() || entry.undone)
		return;

	// Undo is a real revert, not just a log annotation: it's a switch-in
	// request for the PREVIOUS category NAME (not a detected game, so it
	// can't go through onSwitchIn()'s InstalledGame path) - route it
	// through the same categoryLookup_ exact-match tier
	// CategorySwitchCoordinator::applyFallback() already uses for a
	// free-text category name. The entry is only marked undone once the
	// PATCH actually SUCCEEDS - never before - so "(undone)" in the log
	// is never a false record of something that's still live on Twitch.
	// A failure leaves the entry (and its Undo button) exactly as it
	// was; the coordinator's own ActivityCallback already logged why.
	const std::wstring previousCategory = entry.previousCategory;
	stateMachine_.noteUserCategoryChoice(); // An Undo is the user's own choice too.
	coordinator_.applyFallback(previousCategory, stateMachine_.isLive(), [this, logIndex, previousCategory](bool success) {
		if (!success)
			return;
		// Re-validate the index/identity - kMaxLogEntries' ring buffer
		// could have shifted entries while this PATCH was in flight.
		if (logIndex < logEntries_.size() && logEntries_[logIndex].previousCategory == previousCategory)
			logEntries_[logIndex].undone = true;
		refreshPresentationIfVisible();
	});
}

} // namespace signalbox::ui
