/*
 * SignalBox - ui/PromptWidget.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * The three-button prompt from DESIGN.md's ADDENDUM. This is the
 * product's actual differentiator ("it doesn't take the wheel, it taps
 * you on the shoulder") - get the constraints right before anything else
 * about this widget:
 *
 *   1. NEVER MODAL. NEVER STEALS FOCUS. The user is very likely
 *      full-screen in a game, live, when this fires. This widget is a
 *      passive panel embedded in CategoryDock (a QWidget child of an OBS
 *      dock that's already part of the main window - showing/hiding it
 *      never creates or activates a new top-level window, so there is no
 *      window to "steal" focus with in the first place). It is NOT a
 *      QDialog::exec(), NOT QMessageBox::*(), and never calls
 *      raise()/activateWindow()/setFocus() on itself or anything that
 *      could pull input focus away from a fullscreen exclusive game. The
 *      class doc comment upstream (DESIGN.md ADDENDUM) allows "at most a
 *      passive, non-activating toast" as an addition - deliberately not
 *      implemented here; the embedded dock panel alone satisfies the
 *      hard constraint, and adding a second, floating surface would only
 *      add a second place that rule could be violated. If a future
 *      change adds a floating variant, it MUST be constructed with
 *      Qt::ToolTip | Qt::WindowStaysOnTopHint and the
 *      Qt::WA_ShowWithoutActivating widget attribute (Qt's own
 *      documented "notify without activating" combination), and must
 *      never call activateWindow()/raise() in a way that could raise it
 *      above a fullscreen exclusive game.
 *   2. TIMEOUT TO THE SAFE DEFAULT. No answer within
 *      TimingConstants::promptTimeoutS -> DetectionStateMachine treats it
 *      as "hold the last category", the least destructive outcome
 *      (DetectionStateMachine::onTick() enforces this unconditionally -
 *      see its class doc comment). dismissWithoutResponse() is how the
 *      owner (CategoryDock) hides this widget when that happens, without
 *      this widget emitting a second, redundant response.
 *   3. "IGNORE" NEEDS A SCOPE. Trigger B's third button ("Ignore this
 *      change") pairs with a "don't ask again this stream" checkbox -
 *      the update-restart scenario repeats several times per session and
 *      re-prompting every time is exactly the nagging this design exists
 *      to avoid. Concretely (see .cpp for the exact wiring):
 *        GoLiveMismatch: "Set to <game>" -> responded(true, false)
 *                        "Keep <category>" -> responded(false, false)
 *                        "Don't ask this stream" -> responded(false, true)
 *        GameClosed:     "Wait for a new game" -> responded(false, false)
 *                        "Switch to <fallback>" -> responded(true, false)
 *                        "Ignore this change" -> responded(false, checkbox)
 *        NoGameIdle:     "Set to <fallback>" -> responded(true, false)
 *                        "Wait for a game" -> responded(false, false)
 *                        "Just recording" -> responded(false, true)
 *      This matches DetectionStateMachine::respondToPrompt()'s doc
 *      comment exactly - the two classes were designed together.
 *   4. ONLY WHEN LIVE - WITH ONE NAMED EXCEPTION. This widget is only
 *      ever shown by CategoryDock in response to DetectionStateMachine::
 *      Listener::onPrompt(). Triggers A, B and C are raised only while
 *      live: there's nothing to get wrong when there's no audience.
 *      Trigger D (NoGameIdle) is deliberately exempt, because the useful
 *      moment to fix a stale category is before going live, not after -
 *      see DetectionStateMachine.h's "TRIGGER D" note for the guards
 *      that replace live-ness there. This widget still does not
 *      independently check live-ness; it trusts its caller, and that
 *      remains the rule - the exception is granted upstream, once, in a
 *      place that documents it, not decided here per prompt.
 *
 * THREADING: QWidget, Qt main thread only.
 */

#pragma once

#include <QWidget>

#include "../core/DetectionStateMachine.h" // PromptKind

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
class QCheckBox;
QT_END_NAMESPACE

namespace signalbox::ui {

class PromptWidget : public QWidget {
	Q_OBJECT

public:
	explicit PromptWidget(QWidget *parent = nullptr);
	~PromptWidget() override;

	// Populates and shows the panel for the given trigger. gameName is
	// the detected/last-active game's display name; currentCategory is
	// what's currently live. Both are used verbatim in the button/
	// message text - see the class doc comment's per-kind wording.
	void showPrompt(core::PromptKind kind, const QString &gameName, const QString &currentCategory,
			const QString &targetCategory = QString());

	// Hides the panel without emitting a response - used when the
	// state machine's own timeout already resolved it, so the UI
	// doesn't also fire a duplicate response.
	void dismissWithoutResponse();

signals:
	// acceptAction: true for the "do the thing" button (Set to
	// <game>/Switch to <fallback>), false for the "keep things as they
	// are" button (Keep <category>/Wait) or "Ignore"/"Don't ask this
	// stream". dontAskAgainThisStream is true only for "Don't ask this
	// stream" (GoLiveMismatch) and for "Ignore this change" when its
	// checkbox is checked (GameClosed) - see class doc comment #3.
	void responded(bool acceptAction, bool dontAskAgainThisStream);

private:
	void onPrimaryClicked();
	void onSecondaryClicked();
	void onTertiaryClicked();

	core::PromptKind kind_ = core::PromptKind::GoLiveMismatch;

	QLabel *messageLabel_ = nullptr;
	QPushButton *primaryButton_ = nullptr;
	QPushButton *secondaryButton_ = nullptr;
	QPushButton *tertiaryButton_ = nullptr;
	QCheckBox *dontAskAgainCheckbox_ = nullptr;
};

} // namespace signalbox::ui
