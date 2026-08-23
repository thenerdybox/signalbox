/*
 * SignalBox - ui/SettingsDialog.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * See SettingsDialog.h for the ownership/persistence notes - in
 * particular that config_.save() is called exactly once, from the Save
 * button, never on a timer.
 */

#include "SettingsDialog.h"

#include <QCheckBox>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "../twitch/TwitchAuth.h"

namespace signalbox::ui {

SettingsDialog::SettingsDialog(core::PluginConfig &config, core::DetectionStateMachine &stateMachine,
				twitch::TwitchAuth *twitchAuth, const QString &connectedLogin, QWidget *parent)
	: QDialog(parent),
	  config_(config),
	  stateMachine_(stateMachine),
	  twitchAuth_(twitchAuth),
	  connectedLogin_(connectedLogin)
{
	setWindowTitle(QStringLiteral("SignalBox Category Monitor by TheNerdyBox"));
	buildUi();

	if (twitchAuth_) {
		connect(twitchAuth_, &twitch::TwitchAuth::deviceCodeReady, this, [this](twitch::DeviceCodeInfo info) {
			twitchStatusLabel_->setText(QStringLiteral("Go to %1 and enter code: %2")
							     .arg(info.verificationUri, info.userCode));
		});
		connect(twitchAuth_, &twitch::TwitchAuth::tokensAcquired, this, [this](twitch::TokenSet tokens) {
			twitchStatusLabel_->setText(QStringLiteral("Connected as %1").arg(tokens.login));
		});
		connect(twitchAuth_, &twitch::TwitchAuth::authFailed, this, [this](QString reason) {
			twitchStatusLabel_->setText(QStringLiteral("Connection failed: %1").arg(reason));
		});
	}
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::buildUi()
{
	auto *root = new QVBoxLayout(this);

	// --- Behavior ---
	auto *behaviorGroup = new QGroupBox(QStringLiteral("Behavior"), this);
	auto *behaviorLayout = new QVBoxLayout(behaviorGroup);

	autoSwitchCheckbox_ = new QCheckBox(QStringLiteral("Automatically switch category on confirmed detection"),
					     behaviorGroup);
	autoSwitchCheckbox_->setChecked(!stateMachine_.isManuallyLocked());
	behaviorLayout->addWidget(autoSwitchCheckbox_);

	promptsEnabledCheckbox_ =
		new QCheckBox(QStringLiteral("Show prompts (go-live mismatch / game closed)"), behaviorGroup);
	promptsEnabledCheckbox_->setChecked(stateMachine_.promptsEnabled());
	behaviorLayout->addWidget(promptsEnabledCheckbox_);

	fallbackEnabledCheckbox_ = new QCheckBox(
		QStringLiteral("Allow switching to Just Chatting when no game is detected (off = hold last category)"),
		behaviorGroup);
	fallbackEnabledCheckbox_->setChecked(stateMachine_.fallbackEnabled()); // Off by default.
	behaviorLayout->addWidget(fallbackEnabledCheckbox_);

	auto *fallbackRow = new QHBoxLayout();
	fallbackRow->addWidget(new QLabel(QStringLiteral("Fallback category:"), behaviorGroup));
	fallbackCategoryEdit_ = new QLineEdit(QString::fromStdWString(config_.fallbackCategoryName()), behaviorGroup);
	fallbackRow->addWidget(fallbackCategoryEdit_);
	behaviorLayout->addLayout(fallbackRow);

	// Trigger D's permanent off switch. Sits under the fallback category
	// it offers, because that is the category it would set - the two read
	// as one setting from the user's side, and separating them would make
	// "what does this prompt actually do?" a question you have to go and
	// answer somewhere else.
	idlePromptCheckbox_ = new QCheckBox(
		QStringLiteral("Ask what I'm doing when no game has been running for a couple of minutes"),
		behaviorGroup);
	idlePromptCheckbox_->setChecked(config_.idlePromptEnabled());
	idlePromptCheckbox_->setToolTip(
		QStringLiteral("Asked at most once per idle spell, and never while a game is running. "
				"Needs Twitch connected, since the offer is to set your category."));
	behaviorLayout->addWidget(idlePromptCheckbox_);

	onlyWhileLiveCheckbox_ =
		new QCheckBox(QStringLiteral("Only change category while live"), behaviorGroup);
	onlyWhileLiveCheckbox_->setChecked(config_.onlyWhileLive());
	behaviorLayout->addWidget(onlyWhileLiveCheckbox_);

	root->addWidget(behaviorGroup);

	// --- Advanced timing ---
	auto *timingGroup = new QGroupBox(QStringLiteral("Advanced timing"), this);
	auto *timingForm = new QFormLayout(timingGroup);
	const core::TimingConstants &timing = config_.timing();

	confirmPollsSpin_ = new QSpinBox(timingGroup);
	confirmPollsSpin_->setRange(1, 20);
	confirmPollsSpin_->setValue(static_cast<int>(timing.confirmPolls));
	confirmPollsSpin_->setSuffix(QStringLiteral(" polls"));
	timingForm->addRow(QStringLiteral("Confirm before switching:"), confirmPollsSpin_);

	crashGraceSpin_ = new QSpinBox(timingGroup);
	crashGraceSpin_->setRange(10, 900);
	crashGraceSpin_->setValue(static_cast<int>(timing.crashGraceS));
	crashGraceSpin_->setSuffix(QStringLiteral(" s"));
	timingForm->addRow(QStringLiteral("Grace after a crash:"), crashGraceSpin_);

	cleanExitGraceSpin_ = new QSpinBox(timingGroup);
	cleanExitGraceSpin_->setRange(5, 600);
	cleanExitGraceSpin_->setValue(static_cast<int>(timing.cleanExitGraceS));
	cleanExitGraceSpin_->setSuffix(QStringLiteral(" s"));
	timingForm->addRow(QStringLiteral("Grace after a clean exit:"), cleanExitGraceSpin_);

	minPatchSpacingSpin_ = new QSpinBox(timingGroup);
	minPatchSpacingSpin_->setRange(5, 300);
	minPatchSpacingSpin_->setValue(static_cast<int>(timing.minPatchSpacingS));
	minPatchSpacingSpin_->setSuffix(QStringLiteral(" s"));
	timingForm->addRow(QStringLiteral("Minimum interval between category changes:"), minPatchSpacingSpin_);

	promptTimeoutSpin_ = new QSpinBox(timingGroup);
	promptTimeoutSpin_->setRange(5, 120);
	promptTimeoutSpin_->setValue(static_cast<int>(timing.promptTimeoutS));
	promptTimeoutSpin_->setSuffix(QStringLiteral(" s"));
	timingForm->addRow(QStringLiteral("Prompt timeout (falls through to hold):"), promptTimeoutSpin_);

	root->addWidget(timingGroup);

	// --- Per-game overrides (view-and-remove only - see class doc comment) ---
	auto *overridesGroup = new QGroupBox(QStringLiteral("Per-game overrides"), this);
	auto *overridesLayout = new QVBoxLayout(overridesGroup);
	overridesLayout->addWidget(new QLabel(
		QStringLiteral("Set from the dock's \"Fix this...\" button when SignalBox gets a detection wrong. "
				"Remove an entry here to go back to normal detection for that app."),
		overridesGroup));
	overridesList_ = new QListWidget(overridesGroup);
	refreshOverridesList();
	overridesLayout->addWidget(overridesList_);

	auto *removeButton = new QPushButton(QStringLiteral("Remove selected"), overridesGroup);
	connect(removeButton, &QPushButton::clicked, this, &SettingsDialog::onRemoveOverrideClicked);
	overridesLayout->addWidget(removeButton);

	root->addWidget(overridesGroup);

	// --- Twitch connection ---
	auto *twitchGroup = new QGroupBox(QStringLiteral("Twitch"), this);
	auto *twitchLayout = new QVBoxLayout(twitchGroup);
	// Opens reflecting the connection the dock ALREADY has - see the
	// header's connectedLogin note. Connecting again is still offered
	// (a token can expire, or the wrong account can be attached), but it
	// reads as "Reconnect", not as a demand to authorise from scratch.
	const bool alreadyConnected = !connectedLogin_.isEmpty();
	twitchStatusLabel_ = new QLabel(alreadyConnected
						? QStringLiteral("Connected as %1").arg(connectedLogin_)
						: QStringLiteral("Not connected"),
					 twitchGroup);
	twitchStatusLabel_->setWordWrap(true);
	twitchLayout->addWidget(twitchStatusLabel_);
	connectTwitchButton_ = new QPushButton(alreadyConnected ? QStringLiteral("Reconnect to Twitch")
								: QStringLiteral("Connect to Twitch"),
						twitchGroup);
	connectTwitchButton_->setEnabled(twitchAuth_ != nullptr);
	if (!twitchAuth_)
		connectTwitchButton_->setToolTip(QStringLiteral("Twitch client ID not configured yet."));
	else if (alreadyConnected)
		connectTwitchButton_->setToolTip(
			QStringLiteral("You're already connected. Use this only to switch accounts or fix an "
					"expired connection."));
	twitchLayout->addWidget(connectTwitchButton_);
	connect(connectTwitchButton_, &QPushButton::clicked, this, &SettingsDialog::onConnectTwitchClicked);
	root->addWidget(twitchGroup);

	// --- Save/Cancel ---
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::onSaveClicked);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	root->addWidget(buttons);
}

void SettingsDialog::onSaveClicked()
{
	// Single write on explicit user action - see class doc comment
	// (lightweight requirement: persist on change, not on a timer).
	stateMachine_.setManualLock(!autoSwitchCheckbox_->isChecked());
	stateMachine_.setPromptsEnabled(promptsEnabledCheckbox_->isChecked());
	stateMachine_.setFallbackEnabled(fallbackEnabledCheckbox_->isChecked());

	// Persist the same three toggles, not just apply them to the live
	// state machine - previously these were live-only and silently reset
	// to their compiled-in defaults on every OBS restart (see
	// PluginConfig.h's "Persisted mirrors of SettingsDialog's behavior
	// toggles" note, which already had storage for these; nothing wrote
	// to it until now).
	config_.setAutoSwitchEnabled(autoSwitchCheckbox_->isChecked());
	config_.setPromptsEnabled(promptsEnabledCheckbox_->isChecked());
	config_.setFallbackSwitchingEnabled(fallbackEnabledCheckbox_->isChecked());

	config_.setFallbackCategoryName(fallbackCategoryEdit_->text().toStdWString());
	config_.setIdlePromptEnabled(idlePromptCheckbox_->isChecked());
	config_.setOnlyWhileLive(onlyWhileLiveCheckbox_->isChecked());

	// Per-game override removals staged by onRemoveOverrideClicked() -
	// applied here, on Save, so Cancel behaves exactly like every other
	// control in this dialog (see class doc comment). Adding an override
	// is not this dialog's job at all - see GameOverrideDialog.h.
	for (const auto &key : pendingOverrideRemovals_)
		config_.removeUserOverride(key);
	pendingOverrideRemovals_.clear();

	core::TimingConstants timing = config_.timing();
	timing.confirmPolls = static_cast<std::uint32_t>(confirmPollsSpin_->value());
	timing.crashGraceS = static_cast<std::uint32_t>(crashGraceSpin_->value());
	timing.cleanExitGraceS = static_cast<std::uint32_t>(cleanExitGraceSpin_->value());
	timing.minPatchSpacingS = static_cast<std::uint32_t>(minPatchSpacingSpin_->value());
	timing.promptTimeoutS = static_cast<std::uint32_t>(promptTimeoutSpin_->value());
	config_.setTiming(timing);
	stateMachine_.setTiming(timing); // Live re-tune - see DetectionStateMachine::setTiming()'s doc comment.

	config_.save();
	accept();
}

void SettingsDialog::refreshOverridesList()
{
	overridesList_->clear();
	for (const auto &[key, override] : config_.userOverrides()) {
		const QString friendlyName = QString::fromStdWString(config_.userOverrideDisplayName(key));
		const QString keyText = QString::fromStdWString(key);
		// Falls back to the bare key when no friendly name was ever
		// recorded (e.g. a hand-edited config.json entry) - see
		// PluginConfig::userOverrideDisplayName()'s doc comment.
		const QString identity = friendlyName.isEmpty() ? keyText : QStringLiteral("%1 (%2)").arg(friendlyName, keyText);
		const QString detail = override.ignored ? QStringLiteral("never switch")
							 : QStringLiteral("always \"%1\"").arg(
								   QString::fromStdWString(override.categoryName));
		auto *item = new QListWidgetItem(QStringLiteral("%1 — %2").arg(identity, detail), overridesList_);
		item->setData(Qt::UserRole, keyText); // What onRemoveOverrideClicked() actually stages for removal.
	}
}

void SettingsDialog::onRemoveOverrideClicked()
{
	const auto selected = overridesList_->selectedItems();
	for (auto *item : selected) {
		pendingOverrideRemovals_.push_back(item->data(Qt::UserRole).toString().toStdWString());
		delete overridesList_->takeItem(overridesList_->row(item));
	}
}

void SettingsDialog::onConnectTwitchClicked()
{
	if (!twitchAuth_)
		return;
	twitchStatusLabel_->setText(QStringLiteral("Requesting device code..."));
	twitchAuth_->beginDeviceCodeFlow();
}

} // namespace signalbox::ui
