/*
 * SignalBox - tests/layout/main.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Dock layout check. Builds the same widget structure the dock uses (a
 * widget-resizable scroll area holding group boxes of ResponsiveButtons and the
 * activity log, with the footer pinned underneath it - all from
 * ui/DockLayout.h) without OBS, resizes it from very narrow to very wide and
 * from very short to very tall, and asserts that:
 *   - every button is real-sized, fully inside the scrollable area, tall enough
 *     for its wrapped caption, and the page never needs a sideways scroll bar;
 *   - the footer buttons sit OUTSIDE the scroll area and are visible at every size;
 *   - activity log rows never overlap, are tall enough for their wrapped text,
 *     keep every Undo button inside the visible width, and stay scrolled to the
 *     newest entry through resizes.
 *
 * Runs on Qt's "minimal" platform plugin (no window is ever shown) unless
 * QT_QPA_PLATFORM says otherwise.
 */

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <QApplication>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStringList>
#include <QVBoxLayout>

#include "ui/DockLayout.h"

using namespace signalbox::ui;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const std::string &what)
{
	++g_checks;
	if (!ok) {
		++g_failures;
		std::printf("  FAIL: %s\n", what.c_str());
	}
}

// Long, multi-line, awkward entries: spaces, a long unbroken path, entries with an Undo button.
const QStringList &LongEntries()
{
	static const QStringList entries = {
		"Console mode on - not looking for games on this PC",
		"Switched to \"Marvel Rivals\" because SignalBox detected the game running and confirmed it for three polls in a row (category changed)",
		"Switched to \"The Legend of Zelda: Tears of the Kingdom\" - category changed from \"Just Chatting\"",
		"Rescan complete - 214 apps indexed (37 excluded, 4 duplicates), 812ms - helpers.json reloaded, 330 rules",
		"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SomeVeryLongGameFolderName\\Binaries\\Win64\\Game-Win64-Shipping.exe",
		"Couldn't check for updates - GitHub returned 403 and the rate limit resets in about 41 minutes, try again later",
		"Switched to \"Dead by Daylight\" - category changed from \"Software and Game Development\"",
		"Ignoring \"Dell Display and Peripheral Manager\" - you asked SignalBox never to switch for it",
	};
	return entries;
}

bool HasUndo(const QString &entry)
{
	return entry.contains("category changed");
}

struct FakeDock {
	QWidget dock;
	QScrollArea *scroll = nullptr;
	QWidget *content = nullptr;
	QWidget *footerBar = nullptr;
	ActivityLogView *log = nullptr;
	std::vector<ResponsiveButton *> buttons;
	std::vector<ResponsiveButton *> footerButtons;

	FakeDock()
	{
		auto *outer = new QVBoxLayout(&dock);
		outer->setContentsMargins(0, 0, 0, 0);
		scroll = new QScrollArea(&dock);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored); // As the dock does.
		scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
		content = new QWidget(scroll);
		auto *root = new QVBoxLayout(content);
		scroll->setWidget(content);
		outer->addWidget(scroll);

		// Prompt-like stacked answers.
		auto *prompt = new QWidget(content);
		auto *promptLayout = new QVBoxLayout(prompt);
		auto *message = new QLabel("Nothing is running. What are you playing?", prompt);
		message->setWordWrap(true);
		promptLayout->addWidget(message);
		for (const char *t : {"Set to Just Chatting", "Keep the current category", "Wait for a game",
				      "Playing on another device"})
			add(promptLayout, t, prompt);
		root->addWidget(prompt);

		// Status group.
		auto *status = new QGroupBox("Status", content);
		auto *statusLayout = new QVBoxLayout(status);
		auto *detected = new QLabel("Detected: Dell Display and Peripheral Manager (confirming...)", status);
		detected->setWordWrap(true);
		statusLayout->addWidget(detected);
		for (const char *t : {"Fix This!", "Stream Ending/Stop Detection", "Just Chatting",
				      "Switching to a PC game", "Resume automatic switching",
				      "Automatic switching: OFF (locked)", "Console mode", "Set category..."})
			add(statusLayout, t, status);
		root->addWidget(status);

		// Twitch group with a flow of two buttons.
		auto *twitch = new QGroupBox("Twitch", content);
		auto *twitchLayout = new QVBoxLayout(twitch);
		auto *flowA = new FlowLayout();
		for (const char *t : {"Open page", "Copy code"}) {
			auto *b = new ResponsiveButton(t, twitch);
			buttons.push_back(b);
			flowA->addWidget(b);
		}
		twitchLayout->addLayout(flowA);
		add(twitchLayout, "Connect to Twitch", twitch);
		root->addWidget(twitch);

		// Activity.
		auto *logGroup = new QGroupBox("Activity", content);
		auto *logLayout = new QVBoxLayout(logGroup);
		log = new ActivityLogView(logGroup);
		log->setMinimumHeight(90);
		for (const QString &entry : LongEntries())
			log->addRow(entry, HasUndo(entry));
		logLayout->addWidget(log);
		root->addWidget(logGroup, 1);

		// Footer flow, pinned under the scroll area exactly as the dock does.
		footerBar = new QWidget(&dock);
		auto *footer = new FlowLayout(footerBar);
		footer->setContentsMargins(6, 4, 6, 6);
		footer->addWidget(new QLabel("v0.2.11 abc1234", footerBar));
		for (const char *t : {"Check for updates", "Rescan", "Settings..."}) {
			auto *b = new ResponsiveButton(t, footerBar);
			footerButtons.push_back(b);
			footer->addWidget(b);
		}
		outer->addWidget(footerBar);
		ShrinkLabelsToDock(content);
		ShrinkLabelsToDock(footerBar);
	}

	void add(QVBoxLayout *layout, const char *text, QWidget *parent)
	{
		auto *b = new ResponsiveButton(text, parent);
		buttons.push_back(b);
		layout->addWidget(b);
	}
};

// Rows must not overlap, nothing may be clipped, and an Undo button must be
// inside the log's visible width.
void CheckLogRows(ActivityLogView &log, const std::string &at)
{
	const QWidget *content = log.contentWidget();
	Check(!log.horizontalScrollBar()->isVisible(), "activity log: no sideways scroll bar" + at);
	Check(content->width() <= log.viewport()->width(), "activity log: content is no wider than the log" + at);
	int prevBottom = -1;
	for (int i = 0; i < log.rowCount(); ++i) {
		QWidget *row = log.rowWidget(i);
		const std::string name = "activity log: row " + std::to_string(i) + at;
		const QRect r(row->mapTo(content, QPoint(0, 0)), row->size());
		Check(r.top() > prevBottom, name + " starts below the row above it (no overlap)");
		prevBottom = r.bottom();
		Check(r.left() >= 0 && r.right() < content->width(), name + " is horizontally inside the log");
		WrapText *text = log.textWidget(i);
		Check(text != nullptr, name + " has its text");
		if (!text)
			continue;
		Check(text->height() >= text->heightForWidth(text->width()),
		      name + " is tall enough for its text wrapped at its real width");
		Check(row->height() >= text->height(), name + " is at least as tall as its text");
		if (QPushButton *undo = log.undoButton(i)) {
			const QRect b(undo->mapTo(content, QPoint(0, 0)), undo->size());
			Check(b.width() > 0 && b.height() > 0 && undo->isVisible(), name + " Undo button is visible");
			Check(b.left() >= 0 && b.right() < log.viewport()->width(),
			      name + " Undo button is inside the visible width, no sideways scrolling needed");
			Check(!b.intersects(QRect(text->mapTo(content, QPoint(0, 0)), text->size())),
			      name + " Undo button does not sit on top of its text");
			Check(b.bottom() <= r.bottom(), name + " Undo button is inside its row");
		}
	}
}

void CheckDockAt(int width, int height)
{
	FakeDock d;
	// A dock cannot be shorter than its footer's wrapped height (the footer is never squeezed or
	// scrolled away), so "short" means the footer plus a sliver of the scrolling part.
	height = std::max(height, d.footerBar->layout()->totalHeightForWidth(width) + 24);
	d.dock.resize(width, height);
	d.dock.show();
	QApplication::processEvents();
	d.dock.resize(width, height); // A second pass, as OBS does when the dock settles.
	QApplication::processEvents();

	const std::string at = " at " + std::to_string(width) + "x" + std::to_string(height);
	const int viewportW = d.scroll->viewport()->width();

	Check(!d.scroll->horizontalScrollBar()->isVisible(), "no sideways scroll bar needed to reach a button" + at);
	Check(d.content->width() <= viewportW, "content is no wider than the dock" + at);

	int i = 0;
	for (ResponsiveButton *b : d.buttons) {
		const std::string name = "button #" + std::to_string(i++) + " \"" + b->text().toStdString() + "\"" + at;
		const QRect r(b->mapTo(d.content, QPoint(0, 0)), b->size());
		Check(b->width() > 0 && b->height() > 0, name + " has a real size");
		Check(r.left() >= 0 && r.right() < d.content->width(), name + " is horizontally inside the content");
		Check(r.top() >= 0 && r.bottom() < d.content->height(), name + " is inside the scrollable content");
		Check(b->height() >= b->heightForWidth(b->width()), name + " is tall enough for its whole caption");
	}

	// Footer buttons are pinned OUTSIDE the scroll area and visible at every size, scrolled or not.
	const QRect scrollRect(d.scroll->mapTo(&d.dock, QPoint(0, 0)), d.scroll->size());
	for (ResponsiveButton *b : d.footerButtons) {
		const std::string name = "footer button \"" + b->text().toStdString() + "\"" + at;
		Check(!d.scroll->isAncestorOf(b), name + " is outside the scroll area");
		Check(b->isVisible() && b->width() > 0 && b->height() > 0, name + " is visible");
		const QRect r(b->mapTo(&d.dock, QPoint(0, 0)), b->size());
		Check(QRect(0, 0, d.dock.width(), d.dock.height()).contains(r),
		      name + " is inside the dock without any scrolling");
		Check(!r.intersects(scrollRect), name + " does not overlap the scroll area");
		Check(b->height() >= b->heightForWidth(b->width()), name + " shows its whole caption");
	}
	d.scroll->verticalScrollBar()->setValue(d.scroll->verticalScrollBar()->maximum());
	QApplication::processEvents();
	for (ResponsiveButton *b : d.footerButtons) {
		const QRect r(b->mapTo(&d.dock, QPoint(0, 0)), b->size());
		Check(QRect(0, 0, d.dock.width(), d.dock.height()).contains(r),
		      "footer button \"" + b->text().toStdString() + "\" stays visible after scrolling" + at);
	}
	d.scroll->verticalScrollBar()->setValue(0);

	// Short docks scroll instead of hiding rows: every pixel of content is reachable.
	QScrollBar *v = d.scroll->verticalScrollBar();
	Check(v->maximum() + d.scroll->viewport()->height() >= d.content->height(),
	      "scrolling reaches the bottom of the content" + at);
	if (d.content->height() > d.scroll->viewport()->height())
		Check(v->maximum() > 0, "a vertical scroll range exists when the dock is short" + at);

	// The dock's own log, laid out inside the scroll content.
	CheckLogRows(*d.log, at);
}

// The log on its own, at five widths, with long multi-line entries; then resized
// through every width to prove rows are re-measured rather than left stale.
void CheckActivityLogAlone()
{
	for (int width : {220, 260, 340, 500, 900}) {
		ActivityLogView log;
		for (int rep = 0; rep < 3; ++rep)
			for (const QString &entry : LongEntries())
				log.addRow(entry, HasUndo(entry));
		log.addRow(QString(300, 'x'), true); // One unbroken run, with Undo.
		log.resize(width, 300);
		log.show();
		QApplication::processEvents();
		const std::string at = " at width " + std::to_string(width);
		CheckLogRows(log, at);
		Check(log.verticalScrollBar()->maximum() > 0, "activity log: vertical scroll bar exists" + at);
		Check(log.verticalScrollBar()->value() == log.verticalScrollBar()->maximum(),
		      "activity log: scrolled to the newest entry" + at);

		for (int w2 : {900, 220, 500, 260, 340}) {
			log.resize(w2, 300);
			QApplication::processEvents();
			const std::string resized = " after resizing to " + std::to_string(w2);
			CheckLogRows(log, resized);
			Check(log.verticalScrollBar()->value() == log.verticalScrollBar()->maximum(),
			      "activity log: still on the newest entry" + resized);
		}

		log.addRow("Newest entry that just arrived", false);
		QApplication::processEvents();
		Check(log.verticalScrollBar()->value() == log.verticalScrollBar()->maximum(),
		      "activity log: a new entry scrolls into view" + at);

		// The dock clears and refills the log on every change.
		log.clear();
		log.addRow("Only entry", true);
		QApplication::processEvents();
		Check(log.rowCount() == 1, "activity log: clear() then add leaves exactly one row" + at);
		CheckLogRows(log, " after a rebuild" + at);
	}

	ActivityLogView shortLog;
	shortLog.resize(600, 300);
	shortLog.addRow("Console mode on", false);
	shortLog.show();
	QApplication::processEvents();
	Check(shortLog.verticalScrollBar()->maximum() == 0, "activity log: no vertical bar when everything fits");
	Check(!shortLog.horizontalScrollBar()->isVisible(), "activity log: no horizontal bar when everything fits");
}

} // namespace

int main(int argc, char **argv)
{
#ifdef QT_PLUGINS_DIR
	QCoreApplication::addLibraryPath(QStringLiteral(QT_PLUGINS_DIR));
#endif
	if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
		qputenv("QT_QPA_PLATFORM", "minimal");
	QApplication app(argc, argv);
	std::printf("SignalBox - dock layout check (platform: %s)\n", app.platformName().toUtf8().constData());

	for (int width : {220, 260, 340, 500, 900})
		for (int height : {120, 260, 520, 900})
			CheckDockAt(width, height);
	CheckActivityLogAlone();

	std::printf("\n=== LAYOUT RESULT: %s (%d checks, %d failing) ===\n", g_failures == 0 ? "PASS" : "FAIL", g_checks,
		    g_failures);
	return g_failures == 0 ? 0 : 1;
}
