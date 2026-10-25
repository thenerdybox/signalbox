/*
 * SignalBox - tests/layout/main.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Dock layout check. Builds the same widget structure the dock uses (a
 * widget-resizable scroll area holding group boxes of ResponsiveButtons, a
 * FlowLayout footer and the activity list - all from ui/DockLayout.h) without
 * OBS, resizes it from very narrow to very wide and from very short to very
 * tall, and asserts that every button is real-sized, fully inside the
 * scrollable area, tall enough for its wrapped caption, and that the page never
 * needs a sideways scroll bar to reach one.
 *
 * Runs on Qt's "minimal" platform plugin (no window is ever shown) unless
 * QT_QPA_PLATFORM says otherwise.
 */

#include <cstdio>
#include <string>
#include <vector>

#include <QApplication>
#include <QGroupBox>
#include <QLabel>
#include <QListWidget>
#include <QScrollArea>
#include <QScrollBar>
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

struct FakeDock {
	QWidget dock;
	QScrollArea *scroll = nullptr;
	QWidget *content = nullptr;
	std::vector<ResponsiveButton *> buttons;

	FakeDock()
	{
		auto *outer = new QVBoxLayout(&dock);
		outer->setContentsMargins(0, 0, 0, 0);
		scroll = new QScrollArea(&dock);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
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
		auto *log = new QListWidget(logGroup);
		log->setMinimumHeight(90);
		ConfigureActivityLog(log);
		logLayout->addWidget(log);
		root->addWidget(logGroup, 1);

		// Footer flow.
		auto *footer = new FlowLayout();
		footer->addWidget(new QLabel("v0.2.11 abc1234", content));
		for (const char *t : {"Check for updates", "Rescan", "Settings..."}) {
			auto *b = new ResponsiveButton(t, content);
			buttons.push_back(b);
			footer->addWidget(b);
		}
		root->addLayout(footer);
		ShrinkLabelsToDock(content);
	}

	void add(QVBoxLayout *layout, const char *text, QWidget *parent)
	{
		auto *b = new ResponsiveButton(text, parent);
		buttons.push_back(b);
		layout->addWidget(b);
	}
};

void CheckDockAt(int width, int height)
{
	FakeDock d;
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

	// Short docks scroll instead of hiding rows: every pixel of content is reachable.
	QScrollBar *v = d.scroll->verticalScrollBar();
	Check(v->maximum() + d.scroll->viewport()->height() >= d.content->height(),
	      "scrolling reaches the bottom of the content" + at);
	if (d.content->height() > d.scroll->viewport()->height())
		Check(v->maximum() > 0, "a vertical scroll range exists when the dock is short" + at);
}

void CheckActivityLog()
{
	QListWidget log;
	ConfigureActivityLog(&log);
	log.resize(260, 160);
	for (int i = 0; i < 20; ++i)
		AddLogRow(&log,
			  QString("Entry %1: SignalBox set your category to Marvel Rivals because it detected the game")
				  .arg(i),
			  false);
	AddLogRow(&log, QString(300, 'x'), true);
	log.show();
	QApplication::processEvents();
	log.scrollToBottom();
	QApplication::processEvents();

	Check(log.horizontalScrollBar()->maximum() > 0, "activity log: a long line gives a horizontal scroll bar");
	Check(log.verticalScrollBar()->maximum() > 0, "activity log: the vertical scroll bar is kept");
	Check(!log.wordWrap(), "activity log: long lines do not wrap");
	Check(log.verticalScrollBar()->value() == log.verticalScrollBar()->maximum(),
	      "activity log: still scrolled to the newest entry");

	QListWidget shortLog;
	ConfigureActivityLog(&shortLog);
	shortLog.resize(600, 300);
	AddLogRow(&shortLog, "Console mode on", false);
	shortLog.show();
	QApplication::processEvents();
	Check(shortLog.horizontalScrollBar()->maximum() == 0, "activity log: no horizontal bar when everything fits");
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
	CheckActivityLog();

	std::printf("\n=== LAYOUT RESULT: %s (%d checks, %d failing) ===\n", g_failures == 0 ? "PASS" : "FAIL", g_checks,
		    g_failures);
	return g_failures == 0 ? 0 : 1;
}
