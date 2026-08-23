/*
 * SignalBox - ui/GameOverrideDialog.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Correcting a wrong detection from the dock. Before this dialog existed,
 * CategoryResolver tier 1 (DESIGN.md 1.5 - the persistent per-game
 * override) was fully built and correctly persisted end to end, but had
 * no path to actually SET one: SettingsDialog's old override section
 * collected a free-text game name, which is not the key
 * CategoryResolver::overrideKeyFor() looks up ("steam:431960", not
 * "Path of Exile 2") - so anything typed there could never match at
 * resolve() time. This dialog is that path. It is the ONLY place in the
 * UI that writes a tier-1 override:
 *   - CategoryDock's "Fix this..." button constructs one with the
 *     InstalledGame already showing in the Status group (active /
 *     pending-confirmation / grace / an outstanding prompt - see
 *     CategoryDock::currentGameForFix()).
 *   - SettingsDialog's override list only ever REMOVES an existing entry
 *     (see SettingsDialog.h's own note on why free-text add was dropped).
 *
 * WHY THE KEY IS NEVER TYPED: overrideKeyFor() returns
 * "platform:platformId" (or an exe-path/install-root fallback) - a
 * stable identity string nobody would type correctly, let alone
 * willingly. This dialog is handed the InstalledGame identity already
 * resolved by whatever surfaced the detection, derives the key itself,
 * and only ever shows it as read-only secondary detail beside the game's
 * actual display name.
 *
 * WHY THE CATEGORY ID IS NEVER TYPED EITHER (SAFETY, NON-NEGOTIABLE):
 * Twitch's PATCH /helix/channels treats an empty game_id as "unset the
 * live category" - this codebase's whole safety posture is "unmapped
 * means no action" (CategoryResolver.h's class doc comment). A
 * UserOverride this dialog persists therefore always carries a
 * categoryId that came back from a real ICategoryLookup::search() result
 * the user explicitly picked from a list - never raw typed text. The one
 * exception is "Never switch for this app", which deliberately stores
 * ignored=true with an empty id: that is UserOverrideStore.h's own
 * distinct, intentional outcome, not a missing id slipping through.
 *
 * OFFLINE-SAFE: lookup may be nullptr (Twitch not connected yet - see
 * CategoryDock::attachTwitchClient(), which is the only place
 * categoryLookup_ gets constructed). The search section disables itself
 * and says why; "Never switch for this app" and "Clear override" need no
 * lookup at all and stay fully usable offline, since they touch nothing
 * but PluginConfig.
 *
 * PERSISTENCE: every action button here writes to config_ AND calls
 * config_.save() immediately, unlike SettingsDialog's toggles (which
 * batch behind one Save button). A live correction during a stream is
 * exactly the single, deliberate action PluginConfig.h's "call after any
 * change" contract describes - making the owner also remember to open
 * Settings and click Save afterward would just reintroduce "reachable in
 * theory, missed in practice" one level up.
 *
 * A QDialog is appropriate here for the same reason SettingsDialog.h
 * gives: this only ever opens from an explicit click on a button the
 * user can see, never unprompted mid-stream - PromptWidget's non-modal
 * constraint does not apply to it.
 *
 * THREADING: Qt main thread only.
 */

#pragma once

#include <string>
#include <vector>

#include <QDialog>

#include "../core/CategoryResolver.h" // ICategoryLookup, ResolvedCategory, overrideKeyFor()
#include "../core/PluginConfig.h"
#include "../detection/DetectedGame.h" // InstalledGame

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
QT_END_NAMESPACE

namespace signalbox::ui {

class GameOverrideDialog : public QDialog {
	Q_OBJECT

public:
	// game is copied - a snapshot of whichever InstalledGame was showing
	// when "Fix this..." was clicked. This dialog does not track further
	// detections while it's open (there is nothing to track against: the
	// whole point is to correct ONE identity the user already has in
	// front of them). lookup may be nullptr; see class doc comment's
	// OFFLINE-SAFE note - not owned, must outlive this dialog if
	// non-null (same ownership pattern CategoryResolver's constructor
	// already uses for the same pointer). config must outlive this
	// dialog too (owned by plugin-main.cpp, same as everywhere else it's
	// passed around).
	explicit GameOverrideDialog(core::PluginConfig &config, core::ICategoryLookup *lookup,
				     detection::InstalledGame game, QWidget *parent = nullptr);
	~GameOverrideDialog() override;

private slots:
	void onSearchClicked();
	void onResultSelectionChanged();
	void onUseSelectedClicked();
	void onNeverSwitchClicked();
	void onClearOverrideClicked();

private:
	void buildUi();
	void refreshCurrentOverrideLabel();

	core::PluginConfig &config_;
	core::ICategoryLookup *lookup_; // Not owned; may be nullptr - see class doc comment.
	detection::InstalledGame game_;
	std::wstring overrideKey_; // CategoryResolver::overrideKeyFor(game_), computed once at construction.

	std::vector<core::ResolvedCategory> lastSearchResults_; // Parallel to resultsList_'s rows.

	QLabel *gameNameLabel_ = nullptr;
	QLabel *keyLabel_ = nullptr;
	QLabel *currentOverrideLabel_ = nullptr;

	QLineEdit *searchQueryEdit_ = nullptr;
	QPushButton *searchButton_ = nullptr;
	QLabel *searchStatusLabel_ = nullptr;
	QListWidget *resultsList_ = nullptr;
	QPushButton *useSelectedButton_ = nullptr;

	QPushButton *neverSwitchButton_ = nullptr;
	QPushButton *clearOverrideButton_ = nullptr;
};

} // namespace signalbox::ui
