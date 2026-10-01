/*
 * SignalBox - core/CategorySwitchCoordinator.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * See CategorySwitchCoordinator.h for switchIn()'s full step sequence
 * (including the tier-2 alias id-resolution step), the external-writer
 * guard's comparison basis, and why marker outcomes can never unwind a
 * successful PATCH.
 */

#include "CategorySwitchCoordinator.h"

namespace signalbox::core {

CategorySwitchCoordinator::CategorySwitchCoordinator(CategoryResolver &resolver, ActivityCallback activityCallback,
						       ExternalWriterCallback externalWriterCallback,
						       CategoryAppliedCallback categoryAppliedCallback)
	: resolver_(resolver),
	  activityCallback_(std::move(activityCallback)),
	  externalWriterCallback_(std::move(externalWriterCallback)),
	  categoryAppliedCallback_(std::move(categoryAppliedCallback))
{
}

void CategorySwitchCoordinator::setChannelClient(IChannelClient *channelClient)
{
	channelClient_ = channelClient;
}

void CategorySwitchCoordinator::setCategoryLookup(ICategoryLookup *categoryLookup)
{
	categoryLookup_ = categoryLookup;
}

void CategorySwitchCoordinator::log(const std::wstring &message) const
{
	if (activityCallback_) {
		activityCallback_(message);
	}
}

void CategorySwitchCoordinator::switchIn(const detection::InstalledGame &game, bool live, bool reassert)
{
	if (!channelClient_) {
		log(L"Twitch is not connected - couldn't switch to \"" + game.displayName + L"\"");
		return;
	}
	if (channelClient_->reauthRequired()) {
		log(L"Twitch needs to be reconnected - couldn't switch to \"" + game.displayName + L"\"");
		return;
	}

	resolver_.resolve(game, [this, live, reassert](std::optional<ResolvedCategory> resolved) {
		if (!resolved) {
			// CategoryResolver's own safety property, unchanged: an
			// unmapped/below-threshold/ignored result issues NO
			// PATCH - see CategoryResolver.h's class doc comment.
			log(L"Couldn't map that game to a Twitch category - category unchanged");
			return;
		}
		applyResolvedCategory(*resolved, live, nullptr, reassert);
	});
}

void CategorySwitchCoordinator::applyFallback(const std::wstring &fallbackCategoryName, bool live,
					       CompletionCallback onComplete)
{
	if (!channelClient_) {
		log(L"Twitch is not connected - couldn't switch to the fallback category");
		if (onComplete) {
			onComplete(false);
		}
		return;
	}
	if (channelClient_->reauthRequired()) {
		log(L"Twitch needs to be reconnected - couldn't switch to the fallback category");
		if (onComplete) {
			onComplete(false);
		}
		return;
	}
	if (!categoryLookup_) {
		log(L"No Twitch category lookup available yet - couldn't resolve fallback category \"" + fallbackCategoryName +
		    L"\"");
		if (onComplete) {
			onComplete(false);
		}
		return;
	}

	categoryLookup_->findExact(
		fallbackCategoryName, [this, fallbackCategoryName, live, onComplete](std::optional<ResolvedCategory> resolved) {
			if (!resolved) {
				log(L"Couldn't find a Twitch category named \"" + fallbackCategoryName +
				    L"\" - category unchanged");
				if (onComplete) {
					onComplete(false);
				}
				return;
			}
			applyResolvedCategory(*resolved, live, onComplete);
		});
}

void CategorySwitchCoordinator::applyResolvedCategory(const ResolvedCategory &resolved, bool live,
							CompletionCallback onComplete, bool reassert)
{
	if (!resolved.categoryId.empty()) {
		performGuardedPatch(resolved.categoryId, resolved.categoryName, live, onComplete, reassert);
		return;
	}

	// Tier-2 alias hit (CategoryResolver.h's ResolvedCategory doc
	// comment): the alias table stores category NAMES only, never ids -
	// they're hand-curated and can change per-region, while names are
	// stable to edit in a PR. The id must be resolved via the same
	// Helix exact-match lookup applyFallback() already uses for its
	// free-text fallback name before this can PATCH anything - an empty
	// id must NEVER reach performGuardedPatch()/IChannelClient, because
	// an empty game_id UNSETS the live category on Twitch (Modify
	// Channel Information docs), which is worse than any wrong mapping
	// "unmapped means no action" exists to prevent. See class doc
	// comment's "TIER-2" note.
	if (!categoryLookup_) {
		log(L"Couldn't resolve \"" + resolved.categoryName +
		    L"\" to a Twitch category id (no lookup available yet) - category unchanged");
		if (onComplete) {
			onComplete(false);
		}
		return;
	}

	const std::wstring categoryName = resolved.categoryName;
	categoryLookup_->findExact(categoryName, [this, categoryName, live, onComplete, reassert](std::optional<ResolvedCategory> exact) {
		if (!exact || exact->categoryId.empty()) {
			// Still no id - resolve to nothing and let the UI ask,
			// never PATCH. Same "unmapped" outcome as any other
			// resolver miss.
			log(L"Couldn't resolve \"" + categoryName + L"\" to a Twitch category id - category unchanged");
			if (onComplete) {
				onComplete(false);
			}
			return;
		}
		performGuardedPatch(exact->categoryId, exact->categoryName, live, onComplete, reassert);
	});
}

void CategorySwitchCoordinator::performGuardedPatch(const std::wstring &categoryId, const std::wstring &categoryName,
						      bool live, CompletionCallback onComplete, bool reassert)
{
	if (!channelClient_) {
		if (onComplete) {
			onComplete(false);
		}
		return; // Cleared while a resolve() was in flight - nothing left to do.
	}
	if (channelClient_->reauthRequired()) {
		log(L"Twitch needs to be reconnected - couldn't set category to \"" + categoryName + L"\"");
		if (onComplete) {
			onComplete(false);
		}
		return;
	}

	// A re-assert comes from the live-category verification, which has just
	// read the channel itself - skip the redundant GET and the stand-down
	// it would trigger (see switchIn()'s doc comment).
	if (reassert) {
		issuePatch(categoryId, categoryName, live, onComplete);
		return;
	}

	// DESIGN.md 3.5, event-driven: check right before every PATCH, not
	// on an ambient timer. See class doc comment for the comparison
	// basis and why a failed GET does not block the PATCH.
	channelClient_->getChannelInfo(
		[this, categoryId, categoryName, live, onComplete](std::optional<ChannelSnapshot> snapshot) {
			if (!channelClient_) {
				if (onComplete) {
					onComplete(false);
				}
				return;
			}

			if (snapshot && lastAppliedCategoryId_ && snapshot->gameId != *lastAppliedCategoryId_ &&
			    snapshot->gameId != categoryId) {
				log(L"Category was changed outside SignalBox (now \"" + snapshot->gameName +
				    L"\") - standing down for this switch");
				if (externalWriterCallback_) {
					externalWriterCallback_();
				}
				if (onComplete) {
					onComplete(false);
				}
				return;
			}

			issuePatch(categoryId, categoryName, live, onComplete);
		});
}

void CategorySwitchCoordinator::issuePatch(const std::wstring &categoryId, const std::wstring &categoryName, bool live,
					    CompletionCallback onComplete)
{
	if (!channelClient_) {
		if (onComplete) {
			onComplete(false);
		}
		return;
	}

	channelClient_->setChannelCategory(
		categoryId, categoryName,
		[this, categoryId, categoryName, live, onComplete](bool success, std::wstring reason) {
			if (!success) {
				log(L"Couldn't set category to \"" + categoryName + L"\": " + reason);
				if (onComplete) {
					onComplete(false);
				}
				return; // lastAppliedCategoryId_ untouched - never claim a switch that didn't happen.
			}

			lastAppliedCategoryId_ = categoryId;
			log(L"Set Twitch category to \"" + categoryName + L"\"");

			// Feedback channel (see class doc comment's "ONCE-A-
			// PATCH-SUCCEEDS" note) - fires only here, i.e. only on
			// an actual, successful PATCH.
			if (categoryAppliedCallback_) {
				categoryAppliedCallback_(categoryName);
			}

			if (live) {
				maybeCreateMarker(categoryName);
			}

			if (onComplete) {
				onComplete(true);
			}
		});
}

void CategorySwitchCoordinator::maybeCreateMarker(const std::wstring &categoryName)
{
	if (!channelClient_) {
		return;
	}

	channelClient_->createStreamMarker(L"Now playing: " + categoryName, [this, categoryName](bool success) {
		// DESIGN.md Section 5: never fails or unwinds the switch that
		// already succeeded above (see issuePatch()) - this callback
		// only ever produces a log line, nothing else.
		if (success) {
			log(L"Added a stream marker for \"" + categoryName + L"\"");
		} else {
			log(L"Couldn't add a stream marker for \"" + categoryName + L"\" (non-fatal)");
		}
	});
}

} // namespace signalbox::core
