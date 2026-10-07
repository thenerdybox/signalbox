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
 *      implemented HERE: this widget stays a plain panel. The one
 *      floating surface is PromptToast (ui/PromptToast.h), owned by
 *      CategoryDock and used only for the no-game-while-live prompt,
 *      built with exactly the combination this note asks for
 *      (Qt::WindowStaysOnTopHint, Qt::WindowDoesNotAcceptFocus and
 *      Qt::WA_ShowWithoutActivating) and never calling
 *      activateWindow()/raise(). The no-game prompt is also drawn with a
 *      highlighted border so it is hard to miss in a crowded dock.
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
 *        GameClosed (no game while live) has three real answers and so
 *        does not use responded() at all - it emits noGameResponded():
 *                        "Stream ending soon" -> StreamEnding
 *                        "Switch to <fallback>" -> JustChatting
 *                        "Waiting for a game ..." -> Waiting
 *        (the older table below still describes the other kinds):
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

#include <QStringList>
#include <QWidget>

#include <vector>

#include "../core/DetectionStateMachine.h" // PromptKind

QT_BEGIN_NAMESPACE
class QBoxLayout;
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
	//
	// recentCategories is the dock's most-recent-first list. It is offered
	// ("Or set it to:") ONLY on the two no-game prompts, GameClosed and
	// NoGameIdle - the moments the honest answer is often "I'm playing
	// something else now" - and never on the others, where the question is
	// about one specific detected game. At most kMaxRecentButtons are shown,
	// minus any that equals currentCategory (case-insensitive): offering the
	// category you are already in is a button that does nothing.
	void showPrompt(core::PromptKind kind, const QString &gameName, const QString &currentCategory,
			const QString &targetCategory = QString(), const QStringList &recentCategories = QStringList());

	// Hides the panel without emitting a response - used when the
	// state machine's own timeout already resolved it, so the UI
	// doesn't also fire a duplicate response.
	void dismissWithoutResponse();

signals:
	// The three-way answer to the no-game-while-live prompt
	// (PromptKind::GameClosed) - see class doc comment #3.
	void noGameResponded(core::NoGameChoice choice);

	// acceptAction: true for the "do the thing" button (Set to
	// <game>/Switch to <fallback>), false for the "keep things as they
	// are" button (Keep <category>/Wait) or "Ignore"/"Don't ask this
	// stream". dontAskAgainThisStream is true only for "Don't ask this
	// stream" (GoLiveMismatch) and for "Ignore this change" when its
	// checkbox is checked (GameClosed) - see class doc comment #3.
	void responded(bool acceptAction, bool dontAskAgainThisStream);

	// One of the "Or set it to:" buttons was pressed. Unlike the other
	// answers this carries a category name, and the panel has already hidden
	// itself; the owner answers the outstanding prompt and applies it.
	// The kind is the prompt that was showing, because the two no-game
	// prompts are answered through different state-machine calls.
	void recentCategoryChosen(core::PromptKind kind, const QString &categoryName);

private:
	static constexpr int kMaxRecentButtons = 3;

	void onPrimaryClicked();
	void onSecondaryClicked();
	void onTertiaryClicked();

	QBoxLayout *buttonRow_ = nullptr;
	core::PromptKind kind_ = core::PromptKind::GoLiveMismatch;

	QLabel *messageLabel_ = nullptr;
	QPushButton *primaryButton_ = nullptr;
	QPushButton *secondaryButton_ = nullptr;
	QPushButton *tertiaryButton_ = nullptr;
	QCheckBox *dontAskAgainCheckbox_ = nullptr;

	// "Or set it to:" - see showPrompt(). One widget so it hides as a unit.
	QWidget *recentSection_ = nullptr;
	std::vector<QPushButton *> recentButtons_;
	QStringList recentNames_; // Full names, parallel to recentButtons_ (button text may be elided).
};

} // namespace signalbox::ui
