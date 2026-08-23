/*
 * SignalBox - detection/TextMatch.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * The two string primitives every data-driven filter in src/detection/
 * needs: an ASCII lowercaser and a '*'-glob matcher. Both were originally
 * private statics inside HelperDenylist; IndexExclusions needs the exact
 * same semantics against the exact same kind of user-editable patterns,
 * and two copies of a matcher is how the two lists quietly stop agreeing
 * about what "Opera GX*" means.
 *
 * Deliberately free of Qt and OBS headers. InstallIndex.h includes this
 * (via IndexExclusions.h) and is itself included by DetectionEngine.h,
 * which must stay Qt-free - see DetectionEngine.h's threading-boundary
 * note. Parsing the JSON that produces these patterns happens in
 * HelperDenylist.h, which is allowed Qt's value-type JSON parser; nothing
 * here does I/O or parsing.
 */

#pragma once

#include <string>

namespace signalbox::detection::text {

// ASCII-only lowercase. Deliberately not locale-aware: every pattern this
// is used against is a Windows filename, an executable basename, or a
// store's display name, and Windows' own filesystem comparison is
// case-insensitive in the same ASCII-only way. A locale-aware fold would
// make matching depend on the user's system locale, which is exactly the
// kind of "works on my machine" behavior a shipped denylist cannot have.
inline std::wstring ToLower(const std::wstring &s)
{
	std::wstring out(s);
	for (wchar_t &c : out) {
		if (c >= L'A' && c <= L'Z') {
			c = static_cast<wchar_t>(c - L'A' + L'a');
		}
	}
	return out;
}

// Classic '*'-glob matcher (iterative, O(n*m) worst case, no backtracking
// recursion) - supports any number of '*' wildcards, which the shipped
// patterns need ("*setup*.exe" has two, "Opera GX*" has one). Both
// arguments are expected to already be lowercased by the caller.
inline bool WildcardMatch(const std::wstring &pattern, const std::wstring &textValue)
{
	std::size_t p = 0, t = 0;
	std::size_t starIdx = std::wstring::npos, matchIdx = 0;

	while (t < textValue.size()) {
		if (p < pattern.size() && (pattern[p] == textValue[t])) {
			++p;
			++t;
		} else if (p < pattern.size() && pattern[p] == L'*') {
			starIdx = p;
			matchIdx = t;
			++p;
		} else if (starIdx != std::wstring::npos) {
			p = starIdx + 1;
			++matchIdx;
			t = matchIdx;
		} else {
			return false;
		}
	}

	while (p < pattern.size() && pattern[p] == L'*') {
		++p;
	}
	return p == pattern.size();
}

} // namespace signalbox::detection::text
