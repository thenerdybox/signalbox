/*
 * SignalBox - ui/PromptWidget.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * See PromptWidget.h for the non-modal/non-focus-stealing constraints
 * this widget must preserve, and the exact button -> responded() mapping
 * (designed together with DetectionStateMachine::respondToPrompt()).
 */

#include "PromptWidget.h"

#include <QBoxLayout>
#include <QCheckBox>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace signalbox::ui {

PromptWidget::PromptWidget(QWidget *parent) : QWidget(parent)
{
	// Passive panel, not a dialog - see the class doc comment's
	// constraint #1. This widget and its buttons never call
	// activateWindow()/raise()/setFocus() themselves; NoFocus policy
	// means clicking a button doesn't even move keyboard focus into
	// this panel, let alone into a new top-level window (there isn't
	// one - this is a plain child widget of the dock).
	setFocusPolicy(Qt::NoFocus);
	setAttribute(Qt::WA_ShowWithoutActivating, true);
	setAttribute(Qt::WA_StyledBackground, true); // So the highlighted border below actually paints.
	setObjectName(QStringLiteral("signalboxPrompt"));

	auto *layout = new QVBoxLayout(this);

	messageLabel_ = new QLabel(this);
	messageLabel_->setWordWrap(true);
	layout->addWidget(messageLabel_);

	auto *buttonRow = new QBoxLayout(QBoxLayout::LeftToRight);
	buttonRow_ = buttonRow;
	primaryButton_ = new QPushButton(this);
	secondaryButton_ = new QPushButton(this);
	tertiaryButton_ = new QPushButton(this);
	for (auto *button : {primaryButton_, secondaryButton_, tertiaryButton_})
		button->setFocusPolicy(Qt::NoFocus); // Clicking must not pull keyboard focus into the dock.
	buttonRow->addWidget(primaryButton_);
	buttonRow->addWidget(secondaryButton_);
	buttonRow->addWidget(tertiaryButton_);
	layout->addLayout(buttonRow);

	dontAskAgainCheckbox_ = new QCheckBox(QStringLiteral("Don't ask again this stream"), this);
	dontAskAgainCheckbox_->setFocusPolicy(Qt::NoFocus);
	layout->addWidget(dontAskAgainCheckbox_);

	setLayout(layout);

	connect(primaryButton_, &QPushButton::clicked, this, &PromptWidget::onPrimaryClicked);
	connect(secondaryButton_, &QPushButton::clicked, this, &PromptWidget::onSecondaryClicked);
	connect(tertiaryButton_, &QPushButton::clicked, this, &PromptWidget::onTertiaryClicked);

	hide(); // Only visible while a prompt is outstanding.
}

PromptWidget::~PromptWidget() = default;

void PromptWidget::showPrompt(core::PromptKind kind, const QString &gameName, const QString &currentCategory,
			       const QString &targetCategory)
{
	kind_ = kind;
	dontAskAgainCheckbox_->setChecked(false);

	// The no-game prompt has three long answers - stack them so none is
	// elided in a narrow dock - and gets a highlighted frame, because it is
	// the one question that should not sit unseen behind another tab.
	const bool prominent = (kind == core::PromptKind::GameClosed);
	buttonRow_->setDirection(prominent ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
	setStyleSheet(prominent ? QStringLiteral("#signalboxPrompt { border: 2px solid #e0a030; border-radius: 4px; }")
				: QString());
	QFont messageFont = messageLabel_->font();
	messageFont.setBold(prominent);
	messageLabel_->setFont(messageFont);

	switch (kind_) {
	case core::PromptKind::GoLiveMismatch:
		messageLabel_->setText(QStringLiteral("You're live in \"%1\" but %2 is running.")
						.arg(currentCategory, gameName));
		primaryButton_->setText(QStringLiteral("Set to %1").arg(gameName));
		secondaryButton_->setText(QStringLiteral("Keep %1").arg(currentCategory));
		tertiaryButton_->setText(QStringLiteral("Don't ask this stream"));
		dontAskAgainCheckbox_->setVisible(false); // "Don't ask" is its own button here, not a checkbox.
		break;
	case core::PromptKind::GameClosed:
		messageLabel_->setText(QStringLiteral("No game detected. You're live in \"%1\" - what's going on?")
						.arg(currentCategory));
		primaryButton_->setText(QStringLiteral("Stream ending soon"));
		secondaryButton_->setText(QStringLiteral("Switch to %1")
						  .arg(targetCategory.isEmpty() ? QStringLiteral("Just Chatting") : targetCategory));
		tertiaryButton_->setText(QStringLiteral("Waiting for a game (updating / loading / installing)"));
		dontAskAgainCheckbox_->setVisible(false); // The three answers cover it; no scope checkbox needed.
		break;
	case core::PromptKind::CreativeApp: {
		// Deliberately phrased as a question about what the user is
		// DOING, not a statement about what is running. Having
		// Photoshop open is not evidence of streaming Photoshop, so the
		// prompt has to ask rather than announce - and it has to make
		// "no" the easy answer, because "no" is the common one.
		const QString offered = targetCategory.isEmpty() ? QStringLiteral("a creative category") : targetCategory;
		messageLabel_->setText(QStringLiteral("%1 is open. Streaming it, or is it just open?").arg(gameName));
		primaryButton_->setText(QStringLiteral("Switch to %1").arg(offered));
		secondaryButton_->setText(QStringLiteral("Keep watching for a game"));
		tertiaryButton_->setText(QStringLiteral("Don't ask this stream"));
		dontAskAgainCheckbox_->setVisible(false); // Same shape as GoLiveMismatch: its own button, not a checkbox.
		break;
	}
	case core::PromptKind::NoGameIdle: {
		// The only prompt that can appear while offline, so it must not
		// assume there is a stream to talk about - no "you're live in",
		// no game name (there isn't one), and no wording that reads as
		// an alarm. It is a question about intent, asked once, at the
		// point where the answer is genuinely useful.
		const QString offered = targetCategory.isEmpty() ? QStringLiteral("Just Chatting") : targetCategory;
		messageLabel_->setText(QStringLiteral("No game running. What are you up to?"));
		primaryButton_->setText(QStringLiteral("Set to %1").arg(offered));
		secondaryButton_->setText(QStringLiteral("Wait for a game"));
		// "Just recording" is the answer that has to be one click and
		// then silent: someone capturing footage offline does not want
		// their live category touched at all, and does not want to be
		// asked again about it.
		tertiaryButton_->setText(QStringLiteral("Just recording"));
		dontAskAgainCheckbox_->setVisible(false);
		break;
	}
	}

	// A plain show() on a child widget with WA_ShowWithoutActivating -
	// no top-level window is created or raised, so there is nothing
	// here that can tab a fullscreen game out (constraint #1).
	show();
}

void PromptWidget::dismissWithoutResponse()
{
	hide();
}

void PromptWidget::onPrimaryClicked()
{
	if (kind_ == core::PromptKind::GameClosed) {
		hide();
		emit noGameResponded(core::NoGameChoice::StreamEnding);
		return;
	}
	// GoLiveMismatch: "Set to <game>" (accept). GameClosed: "Wait for a
	// new game" (keep waiting, not an accept). CreativeApp: "Switch to
	// <category>" (accept - and the only path that ever applies a
	// prompt-only app's category). NoGameIdle: "Set to <fallback>"
	// (accept).
	const bool acceptAction =
		(kind_ == core::PromptKind::GoLiveMismatch || kind_ == core::PromptKind::CreativeApp ||
		 kind_ == core::PromptKind::NoGameIdle);
	hide();
	emit responded(acceptAction, false);
}

void PromptWidget::onSecondaryClicked()
{
	if (kind_ == core::PromptKind::GameClosed) {
		hide();
		emit noGameResponded(core::NoGameChoice::JustChatting);
		return;
	}
	// GoLiveMismatch: "Keep <category>" (not an accept). GameClosed:
	// "Switch to <fallback>" (accept).
	const bool acceptAction = (kind_ == core::PromptKind::GameClosed);
	hide();
	emit responded(acceptAction, false);
}

void PromptWidget::onTertiaryClicked()
{
	if (kind_ == core::PromptKind::GameClosed) {
		hide();
		emit noGameResponded(core::NoGameChoice::Waiting);
		return;
	}
	// GoLiveMismatch: "Don't ask this stream" - keep + suppress.
	// CreativeApp: same, and this is the button that matters most for
	// that trigger - someone who leaves an editor open all session needs
	// one click to never hear about it again today.
	// NoGameIdle: "Just recording" - hold + suppress, same shape.
	// GameClosed: "Ignore this change" - hold + suppress iff the
	// checkbox is checked (constraint #3 - Ignore needs a scope).
	const bool dontAskAgain = (kind_ == core::PromptKind::GoLiveMismatch ||
				    kind_ == core::PromptKind::CreativeApp ||
				    kind_ == core::PromptKind::NoGameIdle)
					   ? true
					   : dontAskAgainCheckbox_->isChecked();
	hide();
	emit responded(false, dontAskAgain);
}

} // namespace signalbox::ui
