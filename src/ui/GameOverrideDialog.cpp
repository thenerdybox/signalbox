/*
 * SignalBox - ui/GameOverrideDialog.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * See GameOverrideDialog.h for the safety/persistence/offline notes this
 * file implements.
 */

#include "GameOverrideDialog.h"

#include <utility>

#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

namespace signalbox::ui {

GameOverrideDialog::GameOverrideDialog(core::PluginConfig &config, core::ICategoryLookup *lookup,
					detection::InstalledGame game, QWidget *parent)
	: QDialog(parent),
	  config_(config),
	  lookup_(lookup),
	  game_(std::move(game)),
	  overrideKey_(core::CategoryResolver::overrideKeyFor(game_))
{
	setWindowTitle(QStringLiteral("Fix category detection - SignalBox"));
	buildUi();
}

GameOverrideDialog::~GameOverrideDialog() = default;

void GameOverrideDialog::buildUi()
{
	auto *root = new QVBoxLayout(this);

	// --- Identity - read-only; see class doc comment's "WHY THE KEY IS
	// NEVER TYPED" note. The display name is what the user actually
	// recognizes; the key is shown only as secondary confirmation that
	// this dialog has hold of the right install, never as something
	// meant to be edited or retyped.
	gameNameLabel_ = new QLabel(
		QStringLiteral("<b>%1</b>").arg(QString::fromStdWString(game_.displayName).toHtmlEscaped()), this);
	gameNameLabel_->setTextFormat(Qt::RichText);
	root->addWidget(gameNameLabel_);

	keyLabel_ = new QLabel(QStringLiteral("Identified as: %1").arg(QString::fromStdWString(overrideKey_)), this);
	keyLabel_->setWordWrap(true);
	keyLabel_->setStyleSheet(QStringLiteral("QLabel { color: #888888; }"));
	root->addWidget(keyLabel_);

	currentOverrideLabel_ = new QLabel(this);
	currentOverrideLabel_->setWordWrap(true);
	root->addWidget(currentOverrideLabel_);
	refreshCurrentOverrideLabel();

	// --- Set the category for this app ---
	auto *setGroup = new QGroupBox(QStringLiteral("Set the category for this app"), this);
	auto *setLayout = new QVBoxLayout(setGroup);

	auto *searchRow = new QHBoxLayout();
	searchQueryEdit_ = new QLineEdit(QString::fromStdWString(game_.displayName), setGroup);
	searchButton_ = new QPushButton(QStringLiteral("Search Twitch"), setGroup);
	searchRow->addWidget(searchQueryEdit_, /*stretch=*/1);
	searchRow->addWidget(searchButton_);
	setLayout->addLayout(searchRow);
	connect(searchQueryEdit_, &QLineEdit::returnPressed, this, &GameOverrideDialog::onSearchClicked);
	connect(searchButton_, &QPushButton::clicked, this, &GameOverrideDialog::onSearchClicked);

	searchStatusLabel_ = new QLabel(setGroup);
	searchStatusLabel_->setWordWrap(true);
	setLayout->addWidget(searchStatusLabel_);

	// Twitch's own GET /helix/search/categories fuzzy search (via
	// ICategoryLookup::search() -> TwitchCategoryLookup ->
	// TwitchClient::searchCategories()) - a REAL search endpoint, not the
	// exact-match tier CategoryResolver's own tier 3 falls back to first.
	// Every row here is a genuine (categoryId, categoryName) pair Twitch
	// returned; nothing the user types ever becomes the stored id - see
	// onUseSelectedClicked().
	resultsList_ = new QListWidget(setGroup);
	setLayout->addWidget(resultsList_);
	connect(resultsList_, &QListWidget::currentRowChanged, this, &GameOverrideDialog::onResultSelectionChanged);
	connect(resultsList_, &QListWidget::itemDoubleClicked, this, &GameOverrideDialog::onUseSelectedClicked);

	useSelectedButton_ = new QPushButton(QStringLiteral("Use selected category"), setGroup);
	useSelectedButton_->setEnabled(false); // Only enabled once a real search result is selected.
	connect(useSelectedButton_, &QPushButton::clicked, this, &GameOverrideDialog::onUseSelectedClicked);
	setLayout->addWidget(useSelectedButton_);

	if (!lookup_) {
		// Twitch not connected yet (CategoryDock::attachTwitchClient()
		// hasn't run - see class doc comment's OFFLINE-SAFE note).
		// Disable the whole search sub-section rather than leaving live
		// controls that would just no-op silently on click.
		searchQueryEdit_->setEnabled(false);
		searchButton_->setEnabled(false);
		searchStatusLabel_->setText(
			QStringLiteral("Twitch isn't connected, so category search isn't available right now. "
				       "Connect to Twitch from the dock and reopen this dialog - or use one of "
				       "the options below, which don't need Twitch at all."));
	}

	root->addWidget(setGroup);

	// --- Other actions (need no Twitch connection - see class doc
	// comment's OFFLINE-SAFE note) ---
	auto *otherGroup = new QGroupBox(QStringLiteral("Other actions"), this);
	auto *otherLayout = new QVBoxLayout(otherGroup);

	neverSwitchButton_ = new QPushButton(QStringLiteral("Never switch category for this app"), otherGroup);
	neverSwitchButton_->setToolTip(
		QStringLiteral("SignalBox will keep detecting this app but never change your Twitch category "
				"for it - like an unmapped app, but remembered so you're never asked again."));
	connect(neverSwitchButton_, &QPushButton::clicked, this, &GameOverrideDialog::onNeverSwitchClicked);
	otherLayout->addWidget(neverSwitchButton_);

	clearOverrideButton_ = new QPushButton(QStringLiteral("Clear override"), otherGroup);
	clearOverrideButton_->setEnabled(config_.findUserOverride(overrideKey_).has_value());
	clearOverrideButton_->setToolTip(
		QStringLiteral("Remove any saved override for this app and go back to normal detection."));
	connect(clearOverrideButton_, &QPushButton::clicked, this, &GameOverrideDialog::onClearOverrideClicked);
	otherLayout->addWidget(clearOverrideButton_);

	root->addWidget(otherGroup);

	// --- Close (Cancel only - every action button above commits and
	// closes itself; there is no separate "Apply" step to accept()) ---
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	root->addWidget(buttons);
}

void GameOverrideDialog::refreshCurrentOverrideLabel()
{
	const auto existing = config_.findUserOverride(overrideKey_);
	if (!existing) {
		currentOverrideLabel_->setText(
			QStringLiteral("No override set - SignalBox is using its normal detection rules for this app."));
		return;
	}
	if (existing->ignored) {
		currentOverrideLabel_->setText(QStringLiteral("Current override: never switch category for this app."));
		return;
	}
	currentOverrideLabel_->setText(QStringLiteral("Current override: always use \"%1\".")
						.arg(QString::fromStdWString(existing->categoryName)));
}

void GameOverrideDialog::onSearchClicked()
{
	if (!lookup_)
		return; // Search controls are disabled in this case - see buildUi(); defensive no-op.

	const QString query = searchQueryEdit_->text().trimmed();
	if (query.isEmpty())
		return;

	searchButton_->setEnabled(false);
	searchStatusLabel_->setText(QStringLiteral("Searching..."));
	resultsList_->clear();
	lastSearchResults_.clear();
	useSelectedButton_->setEnabled(false);

	// TwitchClient's search is async (an outstanding Helix HTTP request);
	// this dialog can be closed (Cancel, or one of the other actions
	// below committing and calling accept()) before it answers. Guard the
	// callback with a QPointer rather than capturing `this` directly, so
	// a late answer to a dead dialog is a quiet no-op instead of a
	// use-after-free.
	QPointer<GameOverrideDialog> guard(this);
	lookup_->search(query.toStdWString(), [guard](std::vector<core::ResolvedCategory> results) {
		if (!guard)
			return; // Dialog is gone - nothing left to update.
		guard->searchButton_->setEnabled(true);
		guard->lastSearchResults_ = std::move(results);
		guard->resultsList_->clear();
		if (guard->lastSearchResults_.empty()) {
			guard->searchStatusLabel_->setText(QStringLiteral("No matching Twitch categories found."));
			return;
		}
		guard->searchStatusLabel_->setText(
			QStringLiteral("%1 result(s) - pick one below.").arg(guard->lastSearchResults_.size()));
		for (const auto &result : guard->lastSearchResults_)
			guard->resultsList_->addItem(QString::fromStdWString(result.categoryName));
	});
}

void GameOverrideDialog::onResultSelectionChanged()
{
	useSelectedButton_->setEnabled(resultsList_->currentRow() >= 0);
}

void GameOverrideDialog::onUseSelectedClicked()
{
	const int row = resultsList_->currentRow();
	if (row < 0 || static_cast<std::size_t>(row) >= lastSearchResults_.size())
		return;
	const core::ResolvedCategory &selected = lastSearchResults_[static_cast<std::size_t>(row)];

	// SAFETY (see class doc comment, non-negotiable): never persist an
	// empty categoryId for a non-ignored override - Twitch's PATCH
	// treats that as "unset the category". selected came straight from a
	// real ICategoryLookup::search() result, never typed text, but this
	// guard stays in place in case Twitch itself ever hands back a
	// malformed entry - refuse to save rather than trust it.
	//
	// Refusing must SAY so. A button that validates, declines and returns
	// silently is indistinguishable from a dead button, and this project
	// has already shipped exactly that bug once (the Rescan button, which
	// logged that it was pressed and never what it found - see
	// DetectionEngine.h's IndexRebuild doc comment). Should be
	// unreachable; if it ever is reached, the user gets a reason instead
	// of a dialog that ignores them.
	if (selected.categoryId.empty()) {
		searchStatusLabel_->setText(
			QStringLiteral("Twitch returned that category without an id, so it can't be saved. "
					"Pick another result, or try searching again."));
		return;
	}

	core::UserOverride override;
	override.categoryId = selected.categoryId;
	override.categoryName = selected.categoryName;
	override.ignored = false;
	config_.setUserOverride(overrideKey_, override);
	config_.setUserOverrideDisplayName(overrideKey_, game_.displayName); // Cosmetic only - see PluginConfig.h.
	config_.save(); // Immediate - see class doc comment's PERSISTENCE note.
	accept();
}

void GameOverrideDialog::onNeverSwitchClicked()
{
	core::UserOverride override;
	override.ignored = true; // categoryId/categoryName stay empty - see UserOverrideStore.h.
	config_.setUserOverride(overrideKey_, override);
	config_.setUserOverrideDisplayName(overrideKey_, game_.displayName);
	config_.save();
	accept();
}

void GameOverrideDialog::onClearOverrideClicked()
{
	config_.removeUserOverride(overrideKey_);
	config_.save();
	accept();
}

} // namespace signalbox::ui
