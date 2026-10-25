/*
 * SignalBox - src/ui/DockLayout.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * The pieces that keep the dock's buttons visible at any dock size.
 *
 * An OBS dock can be dragged to nearly any width and height, and a plain
 * QPushButton refuses to be narrower than its text. In a dock that is
 * narrower than the longest button the content was wider than the viewport,
 * the horizontal scroll bar was off, and the right-hand end of every button
 * (and any button sharing a row with another) was simply cut off.
 *
 *   ResponsiveButton  a push button that wraps its caption onto more lines
 *                     instead of demanding the width of the whole sentence.
 *   FlowLayout        lays widgets out left to right and wraps onto extra
 *                     rows when they run out of width. Used where several
 *                     buttons used to share one fixed row.
 *   ConfigureActivityLog / MakeLogRow
 *                     the activity list: long lines stay on one line and the
 *                     list scrolls sideways instead of wrapping.
 *
 * Header-only and free of OBS, so the layout test
 * (tests/layout/main.cpp) can build the same widgets without OBS.
 */

#pragma once

#include <algorithm>
#include <vector>

#include <QAbstractItemView>
#include <QFontMetrics>
#include <QLabel>
#include <QLayout>
#include <QLayoutItem>
#include <QListView>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QRect>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QWidget>
#include <QHBoxLayout>
#include <QIcon>
#include <QStyleOptionFocusRect>

namespace signalbox::ui {

class ResponsiveButton : public QPushButton {
public:
	explicit ResponsiveButton(const QString &text, QWidget *parent = nullptr) : QPushButton(text, parent)
	{
		init();
	}
	explicit ResponsiveButton(QWidget *parent = nullptr) : QPushButton(parent) { init(); }

	bool hasHeightForWidth() const override { return true; }

	int heightForWidth(int width) const override
	{
		const QSize chrome = chromeSize();
		const int textWidth = std::max(1, width - chrome.width());
		const QFontMetrics fm(font());
		const QRect r = fm.boundingRect(0, 0, textWidth, 100000, textFlags(), text());
		return std::max(fm.height(), r.height()) + chrome.height();
	}

	// The whole caption on one line - what the button would like.
	QSize sizeHint() const override
	{
		const QSize chrome = chromeSize();
		const QFontMetrics fm(font());
		return QSize(fm.horizontalAdvance(text()) + chrome.width(), fm.height() + chrome.height());
	}

	// Narrow enough to fit any dock: a few characters of text. The layout
	// then asks heightForWidth() for the height that width needs.
	QSize minimumSizeHint() const override
	{
		const QSize chrome = chromeSize();
		const QFontMetrics fm(font());
		const int few = std::min(fm.horizontalAdvance(text()), fm.averageCharWidth() * 6);
		return QSize(few + chrome.width(), fm.height() + chrome.height());
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QStylePainter painter(this);
		QStyleOptionButton option;
		initStyleOption(&option);
		const QString caption = option.text;
		option.text.clear();
		option.icon = QIcon();
		painter.drawControl(QStyle::CE_PushButtonBevel, option);
		const QRect contents = style()->subElementRect(QStyle::SE_PushButtonContents, &option, this);
		style()->drawItemText(&painter, contents, textFlags(), option.palette, isEnabled(), caption,
				      QPalette::ButtonText);
		if (option.state & QStyle::State_HasFocus) {
			QStyleOptionFocusRect focus;
			focus.initFrom(this);
			focus.rect = contents;
			painter.drawPrimitive(QStyle::PE_FrameFocusRect, focus);
		}
	}

private:
	void init()
	{
		// Preferred (not Minimum) across: Minimum means "never narrower than sizeHint", which is
		// the whole caption on one line - the very thing that clipped the dock.
		QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Minimum);
		policy.setHeightForWidth(true);
		setSizePolicy(policy);
	}

	static int textFlags() { return Qt::AlignCenter | Qt::TextWordWrap | Qt::TextWrapAnywhere; }

	// Everything the style adds around the caption (padding, bevel, margins).
	QSize chromeSize() const
	{
		QStyleOptionButton option;
		initStyleOption(&option);
		const QFontMetrics fm(font());
		const QSize probe(200, fm.height());
		const QSize full = style()->sizeFromContents(QStyle::CT_PushButton, &option, probe, this);
		return QSize(std::max(0, full.width() - probe.width()), std::max(0, full.height() - probe.height()));
	}
};

// Left-to-right flow with wrapping. An item wider than the row is squeezed to
// the row's width (ResponsiveButton then wraps its caption), so nothing is ever
// wider than the space it is given.
class FlowLayout : public QLayout {
public:
	explicit FlowLayout(QWidget *parent = nullptr, int spacing = 6) : QLayout(parent), spacing_(spacing)
	{
		setContentsMargins(0, 0, 0, 0);
	}
	~FlowLayout() override
	{
		while (QLayoutItem *item = takeAt(0))
			delete item;
	}

	void addItem(QLayoutItem *item) override { items_.push_back(item); }
	int count() const override { return static_cast<int>(items_.size()); }
	QLayoutItem *itemAt(int index) const override
	{
		return index >= 0 && index < count() ? items_[static_cast<std::size_t>(index)] : nullptr;
	}
	QLayoutItem *takeAt(int index) override
	{
		if (index < 0 || index >= count())
			return nullptr;
		QLayoutItem *item = items_[static_cast<std::size_t>(index)];
		items_.erase(items_.begin() + index);
		return item;
	}
	Qt::Orientations expandingDirections() const override { return {}; }
	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int width) const override { return layoutRows(QRect(0, 0, width, 0), true); }
	void setGeometry(const QRect &rect) override
	{
		QLayout::setGeometry(rect);
		layoutRows(rect, false);
	}
	QSize sizeHint() const override { return minimumSize(); }
	QSize minimumSize() const override
	{
		QSize size;
		for (QLayoutItem *item : items_)
			size = size.expandedTo(item->minimumSize());
		const QMargins m = contentsMargins();
		return size + QSize(m.left() + m.right(), m.top() + m.bottom());
	}

private:
	int layoutRows(const QRect &rect, bool measureOnly) const
	{
		const QMargins m = contentsMargins();
		const QRect area = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
		const int rowWidth = std::max(1, area.width());
		int x = area.x();
		int y = area.y();
		int rowHeight = 0;
		for (QLayoutItem *item : items_) {
			if (item->isEmpty())
				continue; // Hidden widgets take no room.
			const int w = std::min(item->sizeHint().width(), rowWidth);
			const int h = item->hasHeightForWidth() ? item->heightForWidth(w) : item->sizeHint().height();
			if (x > area.x() && x + w > area.x() + rowWidth) {
				x = area.x();
				y += rowHeight + spacing_;
				rowHeight = 0;
			}
			if (!measureOnly)
				item->setGeometry(QRect(QPoint(x, y), QSize(w, h)));
			x += w + spacing_;
			rowHeight = std::max(rowHeight, h);
		}
		return y + rowHeight - rect.y() + m.top() + m.bottom();
	}

	std::vector<QLayoutItem *> items_;
	int spacing_;
};

// Text labels wrap and may be narrower than their longest word, instead of
// setting a minimum width the dock then has to scroll sideways to honour. A
// QLabel's own minimum is its longest unbreakable word, which for a game title
// or the Twitch category can be most of a narrow dock.
inline void ShrinkLabelsToDock(QWidget *root)
{
	for (QLabel *label : root->findChildren<QLabel *>()) {
		label->setWordWrap(true);
		label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	}
}

// The activity list. Long lines stay on one line and the list scrolls
// sideways; the vertical bar stays.
inline void ConfigureActivityLog(QListWidget *view)
{
	view->setWordWrap(false);
	view->setTextElideMode(Qt::ElideNone);
	view->setResizeMode(QListView::Adjust);
	view->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	view->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
	view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	view->setSelectionMode(QAbstractItemView::NoSelection);
}

// One activity row: the message on a single line, optionally followed by an
// Undo button. Returns the Undo button (or nullptr) so the caller can connect it.
inline QPushButton *AddLogRow(QListWidget *view, const QString &text, bool withUndo)
{
	auto *row = new QWidget(view);
	auto *rowLayout = new QHBoxLayout(row);
	rowLayout->setContentsMargins(2, 2, 2, 2);
	auto *label = new QLabel(text, row);
	label->setWordWrap(false);
	label->setTextInteractionFlags(Qt::TextSelectableByMouse);
	rowLayout->addWidget(label, /*stretch=*/1);
	QPushButton *undo = nullptr;
	if (withUndo) {
		undo = new QPushButton(QStringLiteral("Undo"), row);
		rowLayout->addWidget(undo);
	}
	auto *item = new QListWidgetItem(view);
	item->setSizeHint(row->sizeHint());
	view->addItem(item);
	view->setItemWidget(item, row);
	return undo;
}

} // namespace signalbox::ui
