/*
 * SignalBox - twitch/TokenStore.cpp
 * Copyright (c) 2026 TheNerdyBox. SPDX-License-Identifier: MIT.
 *
 * tokens.bin format (all integers native-endian, this is a single-machine
 * file that never needs to be portable - DPAPI blobs don't travel between
 * machines/users anyway, see header):
 *   uint32_t  magic    ("CMTK")
 *   uint32_t  version  (1)
 *   then, for each of accessToken/refreshToken/userId/login in that order:
 *     uint32_t   length in UTF-16 code units
 *     wchar_t[]  raw UTF-16LE data (no NUL terminator stored)
 *   int64_t   obtainedAtUnixS
 *   int32_t   expiresInS
 * That whole buffer is what gets DPAPI-encrypted; the encrypted blob is
 * the entire contents of tokens.bin. No separate plaintext header.
 *
 * obs_module_config_path() only builds a path string (verified against
 * libobs obs-module.c - obs_module_get_config_path() is pure string
 * concatenation) - it does not create the directory. save() creates it
 * via os_mkdirs() before writing.
 */

#include "TokenStore.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>

#include <cstring>
#include <vector>

#pragma comment(lib, "crypt32.lib")

namespace signalbox::twitch {

namespace {

constexpr std::uint32_t kMagic = 0x4B544D43; // "CMTK" little-endian
constexpr std::uint32_t kVersion = 1;
// Sanity ceiling so a corrupted/truncated/hostile file can't make load()
// allocate something absurd - real token blobs are a few hundred bytes.
constexpr std::size_t kMaxFileBytes = 64 * 1024;

// DPAPI's optional "description" string - shown in some Windows
// credential UIs, otherwise inert. Doubles as a sanity label if anyone
// ever inspects the blob with a DPAPI tool.
const wchar_t *kDpapiDescription = L"SignalBox - Twitch OAuth tokens";

// obs_module_config_path()'s return is bmalloc'd UTF-8; converts to a
// bmalloc'd wide string via libobs' own conversion helper and copies it
// into a std::wstring, freeing both temporaries. Returns an empty wstring
// if the module's config path isn't available (obs_module_config_path()
// is documented to return NULL when that happens).
std::wstring obsConfigPathToWide(const char *file)
{
	char *utf8Path = obs_module_config_path(file);
	if (!utf8Path)
		return {};

	std::wstring result;
	wchar_t *widePath = nullptr;
	if (os_utf8_to_wcs_ptr(utf8Path, 0, &widePath) > 0 && widePath) {
		result = widePath;
	}
	if (widePath)
		bfree(widePath);
	bfree(utf8Path);
	return result;
}

// UTF-8 directory portion (trailing-slash form obs_module_config_path("")
// returns) for os_mkdirs(), which - like all libobs os_* path functions -
// takes UTF-8, not the wide path load()/save() use for the actual
// CreateFileW calls.
std::string obsConfigDirUtf8()
{
	char *utf8Dir = obs_module_config_path("");
	if (!utf8Dir)
		return {};
	std::string result(utf8Dir);
	bfree(utf8Dir);
	return result;
}

void appendU32(std::vector<std::uint8_t> &buf, std::uint32_t value)
{
	const auto *bytes = reinterpret_cast<const std::uint8_t *>(&value);
	buf.insert(buf.end(), bytes, bytes + sizeof(value));
}

void appendI64(std::vector<std::uint8_t> &buf, std::int64_t value)
{
	const auto *bytes = reinterpret_cast<const std::uint8_t *>(&value);
	buf.insert(buf.end(), bytes, bytes + sizeof(value));
}

void appendI32(std::vector<std::uint8_t> &buf, std::int32_t value)
{
	const auto *bytes = reinterpret_cast<const std::uint8_t *>(&value);
	buf.insert(buf.end(), bytes, bytes + sizeof(value));
}

void appendWString(std::vector<std::uint8_t> &buf, const std::wstring &s)
{
	appendU32(buf, static_cast<std::uint32_t>(s.size()));
	const auto *bytes = reinterpret_cast<const std::uint8_t *>(s.data());
	buf.insert(buf.end(), bytes, bytes + s.size() * sizeof(wchar_t));
}

// All read helpers below return false (leaving *out untouched, or
// zero-initialized) on any malformed/truncated input - load()'s contract
// is "std::nullopt on any failure", never a crash on a corrupt file.
bool readU32(const std::uint8_t *&cursor, const std::uint8_t *end, std::uint32_t &out)
{
	if (cursor + sizeof(out) > end)
		return false;
	std::memcpy(&out, cursor, sizeof(out));
	cursor += sizeof(out);
	return true;
}

bool readI64(const std::uint8_t *&cursor, const std::uint8_t *end, std::int64_t &out)
{
	if (cursor + sizeof(out) > end)
		return false;
	std::memcpy(&out, cursor, sizeof(out));
	cursor += sizeof(out);
	return true;
}

bool readI32(const std::uint8_t *&cursor, const std::uint8_t *end, std::int32_t &out)
{
	if (cursor + sizeof(out) > end)
		return false;
	std::memcpy(&out, cursor, sizeof(out));
	cursor += sizeof(out);
	return true;
}

bool readWString(const std::uint8_t *&cursor, const std::uint8_t *end, std::wstring &out)
{
	std::uint32_t len = 0;
	if (!readU32(cursor, end, len))
		return false;

	// len is code units, not bytes - guard the multiply before it's used
	// as a pointer offset below.
	const std::size_t byteLen = static_cast<std::size_t>(len) * sizeof(wchar_t);
	if (static_cast<std::size_t>(end - cursor) < byteLen)
		return false;

	out.assign(reinterpret_cast<const wchar_t *>(cursor), len);
	cursor += byteLen;
	return true;
}

} // namespace

TokenStore::TokenStore()
{
	path_ = obsConfigPathToWide("tokens.bin");
}

TokenStore::~TokenStore() = default;

std::optional<StoredTokens> TokenStore::load() const
{
	if (path_.empty())
		return std::nullopt;

	HANDLE file = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
				   FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return std::nullopt; // Most common case: never connected yet.

	LARGE_INTEGER size{};
	if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || static_cast<std::uint64_t>(size.QuadPart) > kMaxFileBytes) {
		CloseHandle(file);
		return std::nullopt;
	}

	std::vector<std::uint8_t> encrypted(static_cast<std::size_t>(size.QuadPart));
	DWORD bytesRead = 0;
	BOOL readOk = ReadFile(file, encrypted.data(), static_cast<DWORD>(encrypted.size()), &bytesRead, nullptr);
	CloseHandle(file);
	if (!readOk || bytesRead != encrypted.size())
		return std::nullopt;

	DATA_BLOB input{};
	input.pbData = encrypted.data();
	input.cbData = static_cast<DWORD>(encrypted.size());
	DATA_BLOB output{};

	// CRYPTPROTECT_UI_FORBIDDEN: never let DPAPI pop a credential UI -
	// this call must be silent, always (DESIGN.md 3.3). A failure here
	// (wrong user/machine, blob moved between machines, corruption) is
	// expected and NOT an error condition per the header's contract -
	// the caller's response is "show the reconnect button", not a log
	// spam or a dialog.
	if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
		return std::nullopt;

	std::vector<std::uint8_t> plain(output.pbData, output.pbData + output.cbData);
	LocalFree(output.pbData);

	const std::uint8_t *cursor = plain.data();
	const std::uint8_t *end = plain.data() + plain.size();

	std::uint32_t magic = 0, version = 0;
	if (!readU32(cursor, end, magic) || !readU32(cursor, end, version))
		return std::nullopt;
	if (magic != kMagic || version != kVersion)
		return std::nullopt; // Unknown format - never guess, never partially load.

	StoredTokens tokens;
	if (!readWString(cursor, end, tokens.accessToken))
		return std::nullopt;
	if (!readWString(cursor, end, tokens.refreshToken))
		return std::nullopt;
	if (!readWString(cursor, end, tokens.userId))
		return std::nullopt;
	if (!readWString(cursor, end, tokens.login))
		return std::nullopt;

	std::int64_t obtainedAt = 0;
	std::int32_t expiresIn = 0;
	if (!readI64(cursor, end, obtainedAt))
		return std::nullopt;
	if (!readI32(cursor, end, expiresIn))
		return std::nullopt;

	tokens.obtainedAtUnixS = obtainedAt;
	tokens.expiresInS = expiresIn;

	// A tokens.bin without a usable access+refresh pair is useless -
	// treat it the same as "no file" rather than handing back a token
	// set that will just 401 immediately.
	if (tokens.accessToken.empty() || tokens.refreshToken.empty())
		return std::nullopt;

	return tokens;
}

void TokenStore::save(const StoredTokens &tokens) const
{
	if (path_.empty())
		return; // Config dir unavailable (e.g. running outside OBS in a test harness).

	const std::string dirUtf8 = obsConfigDirUtf8();
	if (!dirUtf8.empty())
		os_mkdirs(dirUtf8.c_str()); // MKDIR_EXISTS is a normal, expected return here.

	std::vector<std::uint8_t> plain;
	plain.reserve(256);
	appendU32(plain, kMagic);
	appendU32(plain, kVersion);
	appendWString(plain, tokens.accessToken);
	appendWString(plain, tokens.refreshToken);
	appendWString(plain, tokens.userId);
	appendWString(plain, tokens.login);
	appendI64(plain, static_cast<std::int64_t>(tokens.obtainedAtUnixS));
	appendI32(plain, static_cast<std::int32_t>(tokens.expiresInS));

	DATA_BLOB input{};
	input.pbData = plain.data();
	input.cbData = static_cast<DWORD>(plain.size());
	DATA_BLOB output{};

	if (!CryptProtectData(&input, kDpapiDescription, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
			       &output)) {
		// Never fall back to writing plaintext - silently dropping
		// the save is the safe failure here; the caller still has
		// the tokens in memory for this session.
		return;
	}

	HANDLE file = CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
				   nullptr);
	if (file != INVALID_HANDLE_VALUE) {
		DWORD written = 0;
		WriteFile(file, output.pbData, output.cbData, &written, nullptr);
		CloseHandle(file);
	}
	LocalFree(output.pbData);
}

void TokenStore::clear() const
{
	if (path_.empty())
		return;
	DeleteFileW(path_.c_str()); // Fine if it doesn't exist - "disconnect" is idempotent.
}

} // namespace signalbox::twitch
