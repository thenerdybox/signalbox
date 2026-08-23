/*
 * SignalBox - twitch/TwitchCategoryLookup.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * The concrete core::ICategoryLookup adapter CategoryResolver.h's class
 * doc comment describes as "not built yet" - wraps twitch::TwitchClient's
 * async, signal-based findCategoryByExactName()/searchCategories() behind
 * ICategoryLookup's callback interface (DESIGN.md 1.5 tier 3), so
 * CategoryResolver never needs to know TwitchClient, Qt signals, or an
 * event loop exist.
 *
 * ORDERING ASSUMPTION: TwitchClient's categoryFound()/categorySearchResults()
 * signals do not carry the query they answer (see TwitchClient.h). This
 * adapter therefore queues one callback per outstanding call, per call
 * kind (exact vs. search), and answers them FIFO as each signal fires.
 * This is correct as long as calls of the same kind complete in the order
 * they were issued, which holds for every path that reaches this adapter
 * in practice: CategoryResolver::resolve() issues at most one tier-3
 * lookup (an exact match, then - only on a miss - one search) per
 * detected switch, and CategorySwitchCoordinator only has one switch in
 * flight at a time. A future caller that fires overlapping lookups of the
 * same kind concurrently would need a request-id scheme TwitchClient's
 * signals do not currently provide - documented here rather than silently
 * assumed.
 *
 * THREADING: Qt main thread only, same as TwitchClient.
 */

#pragma once

#include <deque>
#include <functional>
#include <optional>
#include <vector>

#include <QObject>

#include "../core/CategoryResolver.h"

namespace signalbox::twitch {

class TwitchClient;

class TwitchCategoryLookup : public QObject, public core::ICategoryLookup {
	Q_OBJECT

public:
	// client must outlive this object - not owned, same pattern as every
	// other non-owning reference in this codebase (CategoryResolver's
	// overrideStore_, DetectionStateMachine's listener_).
	explicit TwitchCategoryLookup(TwitchClient &client, QObject *parent = nullptr);
	~TwitchCategoryLookup() override;

	void findExact(const std::wstring &name,
		       std::function<void(std::optional<core::ResolvedCategory>)> onResult) override;
	void search(const std::wstring &query, std::function<void(std::vector<core::ResolvedCategory>)> onResult) override;

private:
	TwitchClient &client_;
	std::deque<std::function<void(std::optional<core::ResolvedCategory>)>> pendingExact_;
	std::deque<std::function<void(std::vector<core::ResolvedCategory>)>> pendingSearch_;
};

} // namespace signalbox::twitch
