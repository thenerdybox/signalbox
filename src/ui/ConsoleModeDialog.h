/*
 * SignalBox - ui/ConsoleModeDialog.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * The small always-on-top box that opens when Console mode turns on: one text
 * field with keyboard focus, and the categories Twitch suggests for what has
 * been typed. Arrow keys move through the list, Enter picks.
 *
 * It knows nothing about Twitch. It asks for a search through queryRequested()
 * (debounced, two letters or more) and is handed the answer through
 * setResults(); the dock does the Helix call and the category change. A
 * result that answers an older query than the one now in the box is dropped.
 *
 * THREADING: Qt main thread only.
 */

#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QString>
#include <QStringList>
#include <QVector>

#include "../core/ConsoleMode.h"
#include "../twitch/TwitchClient.h" // CategoryMatch

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTimer;
QT_END_NAMESPACE

namespace signalbox::ui {

class ConsoleModeDialog : public QDialog {
	Q_OBJECT

public:
	explicit ConsoleModeDialog(QWidget *parent = nullptr);
	~ConsoleModeDialog() override;

	// Shows the box in front of everything, with the text field focused and
	// empty. Focus is taken on purpose: the streamer just pressed the key.
	void presentAndFocus();

	// Quick picks shown while the box is empty (names only, no ids).
	void setRecents(const QStringList &names);
	// The answer to a queryRequested(); ignored if the box has moved on.
	void setResults(const QString &query, const QVector<twitch::CategoryMatch> &matches);
	// The search could not be run (not connected, rate limited, ...).
	void setSearchFailed(const QString &query, const QString &reason);
	void setCurrentGame(const QString &name); // Shown under the title; empty = none yet.

signals:
	void queryRequested(const QString &query);
	// id is empty for a quick pick; the dock then sets it by name.
	void categoryPicked(const QString &id, const QString &name);
	void backToAutoRequested();

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	void onTextChanged();
	void onDebounceTimeout();
	void showRecents();
	void pickCurrentRow();
	void setStatus(const QString &text);
	qint64 nowMs() const;

	QLabel *currentLabel_ = nullptr;
	QLineEdit *edit_ = nullptr;
	QListWidget *list_ = nullptr;
	QLabel *status_ = nullptr;
	QPushButton *backButton_ = nullptr;
	QTimer *debounceTimer_ = nullptr;

	core::SearchDebounce debounce_;
	QElapsedTimer clock_;
	QStringList recents_;
	QString answeredQuery_;     // The query the list currently shows results for.
	core::EnterBeforeResults enter_; // Enter pressed while results were stale: pick when they land.
};

} // namespace signalbox::ui
