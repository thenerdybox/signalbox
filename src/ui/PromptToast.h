/*
 * SignalBox - ui/PromptToast.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * A small always-on-top notification card for the one prompt that matters
 * when the streamer is not looking at OBS: "no game is running and you are
 * live". The dock's PromptWidget is where the answer is given; this only
 * makes sure the question is noticed, and a click takes the user to it.
 *
 * NEVER STEALS FOCUS, NEVER MODAL (PromptWidget.h, constraint #1). It is a
 * frameless tool window created with Qt::WindowDoesNotAcceptFocus (which
 * maps to WS_EX_NOACTIVATE on Windows) and Qt::WA_ShowWithoutActivating, so
 * showing it, and clicking it, never moves keyboard focus away from a game.
 * It never calls activateWindow()/raise(). It has no parent on purpose: a
 * window owned by OBS's main window would be hidden whenever OBS is
 * minimized, which is exactly when it is needed.
 *
 * WHY NOT QSystemTrayIcon::showMessage: that needs a second tray icon next
 * to OBS's own (OBS does not expose its icon), and Windows suppresses
 * those balloons under Focus Assist / while a game runs fullscreen, so the
 * notification would silently not appear in the very situation it is for.
 *
 * THREADING: QWidget, Qt main thread only.
 */

#pragma once

#include <QWidget>

QT_BEGIN_NAMESPACE
class QLabel;
QT_END_NAMESPACE

namespace signalbox::ui {

class PromptToast : public QWidget {
	Q_OBJECT

public:
	explicit PromptToast();
	~PromptToast() override;

	// Shows (or updates) the card in the bottom-right corner of the primary
	// screen's usable area, without activating it.
	void showToast(const QString &title, const QString &body);

	// Hides the card. Safe to call when it is not showing.
	void dismiss();

signals:
	// The user clicked the card; the owner brings the dock prompt forward.
	void clicked();

protected:
	void mousePressEvent(QMouseEvent *event) override;

private:
	QLabel *titleLabel_ = nullptr;
	QLabel *bodyLabel_ = nullptr;
};

} // namespace signalbox::ui
