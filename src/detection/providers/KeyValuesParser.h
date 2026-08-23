/*
 * SignalBox - detection/providers/KeyValuesParser.h
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * Small tolerant parser for Valve's KeyValues text format (VDF), used for
 * both <SteamPath>\steamapps\libraryfolders.vdf and per-app
 * steamapps\appmanifest_<appid>.acf files - both are the same grammar
 * (DESIGN.md 1.2 "Steam provider"):
 *
 *   "key" "value"          -> leaf pair
 *   "key" { ... }          -> nested block
 *   // line comment        -> ignored to end of line
 *   \" and \\ escapes inside quoted strings
 *
 * Deliberately header-only: it is a handful of small functions used only
 * by SteamProvider.cpp, and keeping it header-only avoids adding a new
 * translation unit to the shared CMakeLists.txt target_sources list while
 * another agent may be concurrently editing that same file for src/twitch
 * and src/ui additions (see the project brief's file-ownership rule).
 *
 * Operates on plain std::string (the files are UTF-8 on disk; ASCII
 * delimiters are single-byte and don't collide with UTF-8 continuation
 * bytes, so byte-wise scanning is safe even though values may contain
 * multi-byte characters). Callers convert individual extracted values to
 * std::wstring only where a wide string is actually needed (paths,
 * display names) - see SteamProvider.cpp's Utf8ToWide.
 *
 * TESTABLE: every function here takes a string/string_view and returns a
 * value - no filesystem, registry, OBS, or Qt access. Exercisable from a
 * plain console program or unit test with a literal VDF string.
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace signalbox::detection::providers {

// One node in a parsed KeyValues document. A node is either a leaf
// (isBlock == false, value holds the pair's value) or a block
// (isBlock == true, children holds its nested key/value and key/block
// pairs, in file order).
struct VdfNode {
	std::string key;
	std::string value;
	std::vector<VdfNode> children;
	bool isBlock = false;

	// Case-insensitive lookup of a direct child by key. Steam's own VDF
	// files are inconsistent about key casing across Steam versions
	// (e.g. "LibraryFolders" vs "libraryfolders"), so every lookup in
	// this parser's callers goes through this rather than exact match.
	// Returns nullptr if no direct child has that key.
	const VdfNode *findChild(std::string_view childKey) const
	{
		for (const auto &child : children) {
			if (child.key.size() == childKey.size() &&
			    std::equal(child.key.begin(), child.key.end(), childKey.begin(), [](char a, char b) {
				    return std::tolower(static_cast<unsigned char>(a)) ==
					   std::tolower(static_cast<unsigned char>(b));
			    })) {
				return &child;
			}
		}
		return nullptr;
	}
};

namespace detail {

struct VdfToken {
	enum class Type { String, OpenBrace, CloseBrace } type;
	std::string text; // populated for Type::String only
};

// Tokenizes KeyValues text: quoted strings (with \" and \\ escapes),
// '{' / '}', and "// ..." line comments (skipped). Unquoted bare tokens
// (some third-party VDF variants allow them) are also accepted as a
// tolerance measure, terminated by whitespace or a brace.
inline std::vector<VdfToken> TokenizeKeyValues(std::string_view text)
{
	std::vector<VdfToken> tokens;
	std::size_t i = 0;
	const std::size_t n = text.size();

	while (i < n) {
		const char c = text[i];

		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			++i;
			continue;
		}

		if (c == '/' && i + 1 < n && text[i + 1] == '/') {
			while (i < n && text[i] != '\n') {
				++i;
			}
			continue;
		}

		if (c == '{') {
			tokens.push_back({VdfToken::Type::OpenBrace, {}});
			++i;
			continue;
		}

		if (c == '}') {
			tokens.push_back({VdfToken::Type::CloseBrace, {}});
			++i;
			continue;
		}

		if (c == '"') {
			++i;
			std::string value;
			while (i < n && text[i] != '"') {
				if (text[i] == '\\' && i + 1 < n && (text[i + 1] == '"' || text[i + 1] == '\\')) {
					value.push_back(text[i + 1]);
					i += 2;
				} else {
					value.push_back(text[i]);
					++i;
				}
			}
			if (i < n) {
				++i; // Skip closing quote.
			}
			tokens.push_back({VdfToken::Type::String, std::move(value)});
			continue;
		}

		// Tolerance: an unquoted bare token. Not standard Steam output
		// but harmless to accept - collect until whitespace/brace.
		std::string bare;
		while (i < n && text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n' &&
		       text[i] != '{' && text[i] != '}') {
			bare.push_back(text[i]);
			++i;
		}
		if (!bare.empty()) {
			tokens.push_back({VdfToken::Type::String, std::move(bare)});
		} else {
			++i; // Never spin on an unrecognized byte.
		}
	}

	return tokens;
}

// Recursive-descent parse of a block's contents starting at *pos, up to
// (and consuming) the matching CloseBrace or end-of-tokens. Tolerant of
// malformed input: an unexpected token is skipped rather than aborting
// the whole parse, so one bad file still yields whatever prefix parsed
// cleanly (DESIGN.md Section 7 risk 5 - providers/parsers fail softly).
inline void ParseBlockContents(const std::vector<VdfToken> &tokens, std::size_t &pos, VdfNode &out)
{
	while (pos < tokens.size()) {
		const VdfToken &keyTok = tokens[pos];

		if (keyTok.type == VdfToken::Type::CloseBrace) {
			++pos; // Consume; caller's block ends here.
			return;
		}

		if (keyTok.type != VdfToken::Type::String) {
			++pos; // Stray brace mismatch or similar - skip and continue.
			continue;
		}

		const std::string key = keyTok.text;
		++pos;

		if (pos >= tokens.size()) {
			// Truncated file: key with nothing after it. Drop it.
			return;
		}

		const VdfToken &valueTok = tokens[pos];

		if (valueTok.type == VdfToken::Type::OpenBrace) {
			++pos; // Consume '{'.
			VdfNode child;
			child.key = key;
			child.isBlock = true;
			ParseBlockContents(tokens, pos, child);
			out.children.push_back(std::move(child));
			continue;
		}

		if (valueTok.type == VdfToken::Type::String) {
			VdfNode child;
			child.key = key;
			child.isBlock = false;
			child.value = valueTok.text;
			out.children.push_back(std::move(child));
			++pos;
			continue;
		}

		// A CloseBrace right after a key with no value - malformed;
		// don't consume it so the enclosing block's own close is seen.
	}
}

} // namespace detail

// Parses a full KeyValues document and returns a synthetic root block
// node whose children are the top-level key/value and key/block pairs
// (a VDF file normally has exactly one top-level block, e.g.
// "LibraryFolders" { ... } or "AppState" { ... } - callers look that up
// with findChild()). Never throws; malformed input yields a partial or
// empty tree.
inline VdfNode ParseKeyValues(std::string_view text)
{
	VdfNode root;
	root.isBlock = true;

	const std::vector<detail::VdfToken> tokens = detail::TokenizeKeyValues(text);
	std::size_t pos = 0;
	detail::ParseBlockContents(tokens, pos, root);

	return root;
}

} // namespace signalbox::detection::providers
