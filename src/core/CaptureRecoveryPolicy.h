/*
 * SignalBox - core/CaptureRecoveryPolicy.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * The decision half of "Capture card recovery": given once-a-second
 * observations of a capture source, say when it should be restarted. Pure
 * value logic with no OBS or Qt, so the standalone harness can exercise it.
 *
 * A source counts as "black" when it is showing, is switched on, and reports
 * no frames (width/height 0). It must stay black for blackAfterMs before a
 * restart is requested. After kMaxAttempts restarts in one black spell the
 * policy waits kBackoffMs before trying again. Any frame, or the source
 * being hidden or switched off, ends the spell and clears every counter.
 *
 * THREADING: Qt main thread only (the caller's timer).
 */

#pragma once

#include <cstdint>

namespace signalbox::core {

class CaptureRecoveryPolicy {
public:
	static constexpr int kMaxAttempts = 3;
	static constexpr std::int64_t kBackoffMs = 60000;

	enum class Action { None, Restart };

	explicit CaptureRecoveryPolicy(std::int64_t blackAfterMs = 5000) : blackAfterMs_(blackAfterMs) {}

	void setBlackAfterMs(std::int64_t ms) { blackAfterMs_ = ms; }

	// showing: source is visible and switched on. hasFrames: width/height > 0.
	Action tick(std::int64_t nowMs, bool showing, bool hasFrames)
	{
		if (!showing || hasFrames) {
			reset();
			return Action::None;
		}
		if (blackSinceMs_ < 0) {
			blackSinceMs_ = nowMs;
			return Action::None;
		}
		if (nowMs < backoffUntilMs_ || nowMs - blackSinceMs_ < blackAfterMs_) {
			return Action::None;
		}
		// Restart. Measure the next wait from now, so a restart gets its full
		// time to bring a picture back before being judged.
		blackSinceMs_ = nowMs;
		if (++attempts_ >= kMaxAttempts) {
			backoffUntilMs_ = nowMs + kBackoffMs;
			attempts_ = 0;
		}
		return Action::Restart;
	}

	void reset()
	{
		blackSinceMs_ = -1;
		attempts_ = 0;
		backoffUntilMs_ = 0;
	}

private:
	std::int64_t blackAfterMs_;
	std::int64_t blackSinceMs_ = -1;
	std::int64_t backoffUntilMs_ = 0;
	int attempts_ = 0;
};

} // namespace signalbox::core
