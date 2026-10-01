/*
 * SignalBox - ui/SettingsDialog.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Settings surface for everything that isn't moment-to-moment dock
 * status: enable/disable automatic switching, enable/disable prompts,
 * the Just Chatting fallback toggle (off by default - hold-last-category
 * is the default, per the project brief), the "Advanced timing" group
 * (TimingConstants, persisted via PluginConfig), per-game user overrides,
 * and the "connect to Twitch" action.
 *
 * A QDialog is appropriate here (unlike PromptWidget) - this is an
 * explicit, user-initiated settings surface opened via a button in
 * CategoryDock, not something that appears unprompted mid-stream.
 *
 * OWNERSHIP NOTE: this dialog does not own the PluginConfig or
 * DetectionStateMachine it edits - both outlive it (they belong to
 * CategoryDock, which constructs this dialog on demand when its
 * "Settings" button is clicked and destroys it when closed). Nothing
 * here is persisted automatically - config_.save() is called exactly
 * once, from the Save button, per the lightweight-by-design requirement
 * ("persist on change, not on a timer": one explicit user action, one
 * write, never a periodic autosave).
 *
 * The three behavior toggles (auto-switching / prompts / fallback
 * switching) are applied to the live DetectionStateMachine AND persisted
 * via PluginConfig (config_.setAutoSwitchEnabled() etc. - see
 * onSaveClicked()), so they survive an OBS restart.
 *
 * PER-GAME OVERRIDES ARE VIEW-AND-REMOVE ONLY HERE, NOT ADD. This section
 * used to collect a free-text "game -> category" pair, which has no
 * stable key CategoryResolver's lookup scheme could ever use -
 * CategoryResolver::overrideKeyFor() needs a platform id or exe path,
 * neither of which free text supplies - so nothing typed there could
 * ever actually match at resolve() time; it was dead UI over dead
 * storage. Adding a real override needs a specific DETECTED install to
 * derive that key from, which is exactly what CategoryDock's
 * "Fix this..." button (see GameOverrideDialog.h) has and this settings
 * surface, opened independent of any particular detection, does not.
 * overridesList_ here is read-only display of config_.userOverrides()
 * (refreshOverridesList()) plus removal; a key selected for removal is
 * staged in pendingOverrideRemovals_ and only actually applied in
 * onSaveClicked(), same as every other control in this dialog - so
 * Cancel really cancels a pending removal too, not just the timing/
 * behavior fields.
 *
 * THREADING: Qt main thread only.
 */

#pragma once

#include <string>
#include <vector>

#include <QDialog>

#include "../core/DetectionStateMachine.h"
#include "../core/PluginConfig.h"

QT_BEGIN_NAMESPACE
class QCheckBox;
class QLineEdit;
class QListWidget;
class QSpinBox;
class QLabel;
class QPushButton;
QT_END_NAMESPACE

namespace signalbox::twitch {
class TwitchAuth;
}

namespace signalbox::ui {

class SettingsDialog : public QDialog {
	Q_OBJECT

public:
	// twitchAuth may be nullptr (no Twitch client id configured yet) -
	// the "Connect to Twitch" section is shown disabled in that case.
	//
	// connectedLogin is the Twitch account the DOCK is already connected
	// as, or empty for "not connected". It has to be passed in: TwitchAuth
	// exposes the auth flow's signals but not its resulting state, and
	// those signals only fire while a NEW flow is running - so a dialog
	// built after a successful connection heard nothing and used to open
	// reading "Not connected" with a "Connect to Twitch" button, asking a
	// streamer to authorise an account they had just authorised. The dock
	// is the one object that holds the answer (currentTokens_), so it
	// hands it over at construction.
	explicit SettingsDialog(core::PluginConfig &config, core::DetectionStateMachine &stateMachine,
				 twitch::TwitchAuth *twitchAuth, const QString &connectedLogin = QString(),
				 QWidget *parent = nullptr);
	~SettingsDialog() override;

private slots:
	void onSaveClicked();
	void onRemoveOverrideClicked();
	void onConnectTwitchClicked();

private:
	void buildUi();
	void refreshOverridesList(); // Populates overridesList_ from config_.userOverrides() minus pending removals.

	core::PluginConfig &config_;
	core::DetectionStateMachine &stateMachine_;
	twitch::TwitchAuth *twitchAuth_ = nullptr; // Not owned.
	QString connectedLogin_;                   // Empty = not connected; see the constructor's doc comment.

	QCheckBox *autoSwitchCheckbox_ = nullptr;
	QCheckBox *promptsEnabledCheckbox_ = nullptr;
	QCheckBox *fallbackEnabledCheckbox_ = nullptr;
	QLineEdit *fallbackCategoryEdit_ = nullptr;
	QCheckBox *idlePromptCheckbox_ = nullptr;
	QCheckBox *onlyWhileLiveCheckbox_ = nullptr;

	QSpinBox *confirmPollsSpin_ = nullptr;
	QSpinBox *crashGraceSpin_ = nullptr;
	QSpinBox *cleanExitGraceSpin_ = nullptr;
	QSpinBox *minPatchSpacingSpin_ = nullptr;
	QSpinBox *promptTimeoutSpin_ = nullptr;
	QSpinBox *liveCheckIntervalSpin_ = nullptr;
	QCheckBox *goLiveQuickChecksCheckbox_ = nullptr;
	QSpinBox *noGameSnoozeSpin_ = nullptr;

	QListWidget *overridesList_ = nullptr; // View-and-remove only - see class doc comment.
	std::vector<std::wstring> pendingOverrideRemovals_; // Keys staged for removal; applied in onSaveClicked().

	QLabel *twitchStatusLabel_ = nullptr;
	QPushButton *connectTwitchButton_ = nullptr;
};

} // namespace signalbox::ui
