/*
 * SignalBox - ui/ConsoleModeDialog.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * See ConsoleModeDialog.h.
 */

#include "ConsoleModeDialog.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#ifdef _WIN32
#include <windows.h>
#endif

namespace signalbox::ui {

namespace {
constexpr int kIdRole = Qt::UserRole;
}

ConsoleModeDialog::ConsoleModeDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QStringLiteral("Console mode - SignalBox"));
	setWindowFlag(Qt::WindowStaysOnTopHint, true);
	setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	setMinimumWidth(360);
	clock_.start();

	auto *root = new QVBoxLayout(this);

	auto *title = new QLabel(QStringLiteral("<b>Set category</b> - type the game you're playing"), this);
	title->setTextFormat(Qt::RichText);
	root->addWidget(title);

	currentLabel_ = new QLabel(this);
	currentLabel_->setVisible(false);
	root->addWidget(currentLabel_);

	edit_ = new QLineEdit(this);
	edit_->setPlaceholderText(QStringLiteral("Set category..."));
	edit_->setClearButtonEnabled(true);
	edit_->installEventFilter(this); // Up/Down/Enter go to the list.
	root->addWidget(edit_);

	list_ = new QListWidget(this);
	list_->setFocusPolicy(Qt::NoFocus); // The text field keeps the keyboard.
	list_->setSelectionMode(QAbstractItemView::SingleSelection);
	list_->setMinimumHeight(190);
	root->addWidget(list_);

	status_ = new QLabel(this);
	status_->setWordWrap(true);
	status_->setStyleSheet(QStringLiteral("QLabel { color: #888888; }"));
	root->addWidget(status_);

	auto *buttons = new QHBoxLayout();
	backButton_ = new QPushButton(QStringLiteral("Back to auto"), this);
	backButton_->setToolTip(QStringLiteral("Turn Console mode off and let SignalBox detect games again. "
					       "Your category is left as it is."));
	backButton_->setAutoDefault(false);
	auto *hideButton = new QPushButton(QStringLiteral("Hide"), this);
	hideButton->setToolTip(QStringLiteral("Close this box but stay in Console mode."));
	hideButton->setAutoDefault(false);
	buttons->addWidget(backButton_);
	buttons->addStretch(1);
	buttons->addWidget(hideButton);
	root->addLayout(buttons);

	debounceTimer_ = new QTimer(this);
	debounceTimer_->setSingleShot(true);
	debounceTimer_->setTimerType(Qt::PreciseTimer);

	connect(edit_, &QLineEdit::textChanged, this, &ConsoleModeDialog::onTextChanged);
	connect(debounceTimer_, &QTimer::timeout, this, &ConsoleModeDialog::onDebounceTimeout);
	connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem *) { pickCurrentRow(); });
	connect(backButton_, &QPushButton::clicked, this, &ConsoleModeDialog::backToAutoRequested);
	connect(hideButton, &QPushButton::clicked, this, &QDialog::hide);
}

ConsoleModeDialog::~ConsoleModeDialog() = default;

qint64 ConsoleModeDialog::nowMs() const
{
	return clock_.elapsed();
}

void ConsoleModeDialog::presentAndFocus()
{
	enter_.reset();
	answeredQuery_.clear();
	edit_->clear();
	showRecents();

	show();
	raise();
	activateWindow();
#ifdef _WIN32
	// Windows refuses to hand the foreground to a window whose process did not
	// just receive input, which is exactly the case when the hotkey is pressed
	// over a game. A synthetic Alt tap is the long-standing way to be let in.
	const HWND hwnd = reinterpret_cast<HWND>(winId());
	if (GetForegroundWindow() != hwnd) {
		keybd_event(VK_MENU, 0, 0, 0);
		keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
		SetForegroundWindow(hwnd);
	}
#endif
	edit_->setFocus(Qt::ActiveWindowFocusReason);
}

void ConsoleModeDialog::setRecents(const QStringList &names)
{
	recents_ = names;
	if (edit_->text().trimmed().isEmpty())
		showRecents();
}

void ConsoleModeDialog::setCurrentGame(const QString &name)
{
	currentLabel_->setVisible(!name.isEmpty());
	currentLabel_->setText(QStringLiteral("Last set: %1").arg(name));
}

void ConsoleModeDialog::setStatus(const QString &text)
{
	status_->setText(text);
}

void ConsoleModeDialog::showRecents()
{
	list_->clear();
	for (const QString &name : recents_) {
		auto *item = new QListWidgetItem(name, list_);
		item->setData(kIdRole, QString());
	}
	if (list_->count() > 0) {
		list_->setCurrentRow(0);
		setStatus(QStringLiteral("Recent games. Start typing to search Twitch."));
	} else {
		setStatus(QStringLiteral("Start typing - Twitch suggests as you go."));
	}
}

void ConsoleModeDialog::onTextChanged()
{
	enter_.reset();
	const QString text = edit_->text();
	debounce_.onTextChanged(text.toStdWString(), nowMs());
	if (debounce_.armed()) {
		debounceTimer_->start(static_cast<int>(core::SearchDebounce::kDelayMs));
		setStatus(QStringLiteral("Searching..."));
	} else {
		debounceTimer_->stop();
		answeredQuery_.clear();
		if (text.trimmed().isEmpty()) {
			showRecents();
		} else {
			list_->clear();
			setStatus(QStringLiteral("Keep typing - two letters or more."));
		}
	}
}

void ConsoleModeDialog::onDebounceTimeout()
{
	if (const auto query = debounce_.due(nowMs())) {
		emit queryRequested(QString::fromStdWString(*query));
	} else if (debounce_.armed()) {
		debounceTimer_->start(1); // Fired a hair early; wait out the remainder.
	}
}

void ConsoleModeDialog::setResults(const QString &query, const QVector<twitch::CategoryMatch> &matches)
{
	if (query != edit_->text().trimmed())
		return; // The box has moved on; this answers an older question.

	answeredQuery_ = query;
	list_->clear();
	for (const twitch::CategoryMatch &match : matches) {
		if (match.gameName.isEmpty() || match.gameId.isEmpty())
			continue;
		auto *item = new QListWidgetItem(match.gameName, list_);
		item->setData(kIdRole, match.gameId);
	}
	if (list_->count() > 0) {
		list_->setCurrentRow(0);
		setStatus(QStringLiteral("Up/Down to choose, Enter to set."));
		if (enter_.onResults(query.toStdWString(), true))
			pickCurrentRow();
	} else {
		enter_.reset();
		setStatus(QStringLiteral("No categories match \"%1\".").arg(query));
	}
}

void ConsoleModeDialog::setSearchFailed(const QString &query, const QString &reason)
{
	if (query != edit_->text().trimmed())
		return;
	enter_.reset();
	list_->clear();
	setStatus(reason.isEmpty() ? QStringLiteral("Couldn't search Twitch just now.") : reason);
}

void ConsoleModeDialog::pickCurrentRow()
{
	QListWidgetItem *item = list_->currentItem();
	if (!item)
		return;
	emit categoryPicked(item->data(kIdRole).toString(), item->text());
}

bool ConsoleModeDialog::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == edit_ && event->type() == QEvent::KeyPress) {
		auto *key = static_cast<QKeyEvent *>(event);
		switch (key->key()) {
		case Qt::Key_Down:
			if (list_->count() > 0)
				list_->setCurrentRow(qMin(list_->currentRow() + 1, list_->count() - 1));
			return true;
		case Qt::Key_Up:
			if (list_->count() > 0)
				list_->setCurrentRow(qMax(list_->currentRow() - 1, 0));
			return true;
		case Qt::Key_Return:
		case Qt::Key_Enter: {
			const QString typed = edit_->text().trimmed();
			if (typed.isEmpty()) {
				pickCurrentRow(); // A quick pick.
			} else if (typed == answeredQuery_) {
				pickCurrentRow();
			} else {
				// Typed and pressed Enter before the answer is back: send the search now if
				// it has not gone out yet, and pick the top match when it lands.
				const auto flushed = debounce_.flush();
				if (flushed)
					debounceTimer_->stop();
				if (enter_.onEnter(typed.toStdWString(), flushed.has_value())) {
					setStatus(QStringLiteral("Searching..."));
					emit queryRequested(QString::fromStdWString(*flushed));
				}
			}
			return true;
		}
		default:
			break;
		}
	}
	return QDialog::eventFilter(watched, event);
}

} // namespace signalbox::ui
