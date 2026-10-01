/*
 * SignalBox - ui/PromptToast.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * See PromptToast.h for the no-focus-steal constraints this must keep.
 */

#include "PromptToast.h"

#include <QGuiApplication>
#include <QLabel>
#include <QMouseEvent>
#include <QScreen>
#include <QVBoxLayout>

namespace signalbox::ui {

namespace {
constexpr int kToastWidth = 340;
constexpr int kToastMargin = 16;
} // namespace

PromptToast::PromptToast() : QWidget(nullptr)
{
	setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
	setAttribute(Qt::WA_ShowWithoutActivating, true);
	setAttribute(Qt::WA_StyledBackground, true);
	setFocusPolicy(Qt::NoFocus);
	setCursor(Qt::PointingHandCursor);
	setObjectName(QStringLiteral("signalboxToast"));
	setStyleSheet(QStringLiteral("#signalboxToast { background: #23262b; border: 2px solid #e0a030; border-radius: 6px; }"
				      "QLabel { color: #f1f1f1; background: transparent; }"));

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(14, 10, 14, 10);
	titleLabel_ = new QLabel(this);
	QFont titleFont = titleLabel_->font();
	titleFont.setBold(true);
	titleLabel_->setFont(titleFont);
	layout->addWidget(titleLabel_);
	bodyLabel_ = new QLabel(this);
	bodyLabel_->setWordWrap(true);
	layout->addWidget(bodyLabel_);

	setFixedWidth(kToastWidth);
}

PromptToast::~PromptToast() = default;

void PromptToast::showToast(const QString &title, const QString &body)
{
	titleLabel_->setText(title);
	bodyLabel_->setText(body);
	adjustSize();

	if (const QScreen *screen = QGuiApplication::primaryScreen()) {
		const QRect area = screen->availableGeometry();
		move(area.right() - width() - kToastMargin, area.bottom() - height() - kToastMargin);
	}

	// show(), never raise()/activateWindow(): with the flags above this
	// appears over other windows without taking input focus.
	show();
}

void PromptToast::dismiss()
{
	if (isVisible())
		hide();
}

void PromptToast::mousePressEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton)
		emit clicked();
	QWidget::mousePressEvent(event);
}

} // namespace signalbox::ui
