#pragma once
// Reads the override rules of a RUL2 file that is held in memory.
//
// The game loads the file line by line through cRZIniFile (40 byte reads, two
// strings per line, all lines collected in a vector) and then runs sscanf twice
// per line. This does the same work on a buffer and yields the same rules in the
// same order. Lines that are not written in the plain form used by practically
// every rule are handed to `parseSlow`, which is expected to call the original
// sscanf, so that odd input is still interpreted by the game itself.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include "cSC4NetworkTileConflictRule.h"

namespace Rul2Parser
{
	namespace detail
	{
		constexpr bool isBlank(char c) { return c == ' ' || c == '\t'; }
		constexpr bool isNewline(char c) { return c == '\r' || c == '\n'; }

		constexpr int hexValue(char c)
		{
			return c >= '0' && c <= '9' ? c - '0'
				: c >= 'a' && c <= 'f' ? c - 'a' + 10
				: c >= 'A' && c <= 'F' ? c - 'A' + 10
				: -1;
		}

		// cRZIniFile::ReadLine (0x91ca15): a line ends at the first CR or LF, and all
		// CR/LF that follow are swallowed, so empty lines are never returned (except
		// when the read position is at one). Returns false at the end of the data.
		inline bool readLine(std::string_view data, size_t& pos, std::string_view& line)
		{
			if (pos >= data.size()) {
				return false;
			}
			size_t end = pos;
			while (end < data.size() && !isNewline(data[end])) {
				end++;
			}
			line = data.substr(pos, end - pos);
			while (end < data.size() && isNewline(data[end])) {
				end++;
			}
			pos = end;
			return true;
		}

		inline std::string_view trimLeft(std::string_view s)
		{
			size_t i = 0;
			while (i < s.size() && isBlank(s[i])) {
				i++;
			}
			return s.substr(i);
		}

		inline std::string_view trimRight(std::string_view s)
		{
			size_t n = s.size();
			while (n > 0 && isBlank(s[n - 1])) {
				n--;
			}
			return s.substr(0, n);
		}

		// Reads one number like sscanf's %i (or %d if `decimalOnly`), but only the
		// forms every scanf agrees on: "0x" with 1-8 hex digits, or 1-9 decimal
		// digits. Anything else (signs, octal, overflow, non-ASCII) is refused.
		inline bool parseNumber(const char*& p, const char* end, bool decimalOnly, uint32_t& value)
		{
			while (p != end && isBlank(*p)) {
				p++;
			}
			const char* q = p;
			uint32_t v = 0;
			if (!decimalOnly && end - q >= 2 && q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) {
				q += 2;
				const char* const digits = q;
				for (int d; q != end && (d = hexValue(*q)) >= 0; q++) {
					v = v << 4 | static_cast<uint32_t>(d);
				}
				if (q == digits || q - digits > 8) {
					return false;
				}
			} else {
				const char* const digits = q;
				for (; q != end && *q >= '0' && *q <= '9'; q++) {
					v = v * 10 + static_cast<uint32_t>(*q - '0');
				}
				const ptrdiff_t count = q - digits;
				if (count == 0 || count > 9 || (!decimalOnly && count > 1 && *digits == '0')) {  // %i reads a leading zero as octal
					return false;
				}
			}
			if (q != end && static_cast<unsigned char>(*q) >= 0x80) {
				return false;  // might count as a digit or as whitespace, depending on the locale
			}
			value = v;
			p = q;
			return true;
		}

		constexpr RotFlip toRotFlip(uint32_t rotation, uint32_t flip)
		{
			return static_cast<RotFlip>(static_cast<uint8_t>(rotation) | (flip != 0 ? 0x80 : 0));
		}
	}

	// Parses "id,rot,flip,id,rot,flip" like sscanf(" %i , %i , %d , %i , %i , %d")
	// with 6 matches. Returns false if the text needs the real sscanf to decide.
	inline bool ParseTilePair(std::string_view text, Tile& first, Tile& second)
	{
		const char* p = text.data();
		const char* const end = p + text.size();
		uint32_t v[6];
		for (int i = 0; i < 6; i++) {
			if (i != 0) {
				while (p != end && detail::isBlank(*p)) {
					p++;
				}
				if (p == end || *p != ',') {
					return false;
				}
				p++;
			}
			if (!detail::parseNumber(p, end, i % 3 == 2, v[i])) {
				return false;
			}
		}
		first = {v[0], detail::toRotFlip(v[1], v[2])};
		second = {v[3], detail::toRotFlip(v[4], v[5])};
		return true;
	}

	// What the game does with the two halves of a line (0x63d1e0): a rule if sscanf
	// finds 6 numbers in each. `scan` is sscanf, the DLL passes the game's own.
	template <typename Scanf>
	bool ScanRule(Scanf&& scan, std::string_view key, std::string_view value, cSC4NetworkTileConflictRule& rule)
	{
		constexpr const char* format = " %i , %i , %d , %i , %i , %d";
		const std::string keyText(key), valueText(value);
		int id1, rot1, flip1, id2, rot2, flip2;
		if (scan(keyText.c_str(), format, &id1, &rot1, &flip1, &id2, &rot2, &flip2) != 6) {
			return false;
		}
		rule._1 = {static_cast<uint32_t>(id1), detail::toRotFlip(rot1, flip1)};
		rule._2 = {static_cast<uint32_t>(id2), detail::toRotFlip(rot2, flip2)};
		if (scan(valueText.c_str(), format, &id1, &rot1, &flip1, &id2, &rot2, &flip2) != 6) {
			return false;
		}
		rule._3 = {static_cast<uint32_t>(id1), detail::toRotFlip(rot1, flip1)};
		rule._4 = {static_cast<uint32_t>(id2), detail::toRotFlip(rot2, flip2)};
		return true;
	}

	// Calls `addRule(rule)` for every override rule of the [RuleOverrides] section,
	// in file order. `parseSlow(key, value, rule)` handles the lines ParseTilePair
	// refuses and returns whether the line is a rule (see ScanRule).
	// Returns false, before any rule was added, if the data cannot be handled here
	// and has to go through the game's own loader instead.
	template <typename AddRule, typename ParseSlow>
	bool ForEachRuleOverride(std::string_view data, AddRule&& addRule, ParseSlow&& parseSlow)
	{
		constexpr std::string_view sectionName = "ruleoverrides";

		// The game appends each 40 byte block it reads as a C string.
		if (!data.empty() && std::memchr(data.data(), '\0', data.size()) != nullptr) {
			return false;
		}

		// cRZIniFile::Open (0x91db73) records the position of each "[name]" line, the
		// first of a name wins, names are compared in lower case.
		size_t pos = 0;
		std::string_view line;
		bool found = false;
		while (!found) {
			const size_t lineStart = pos;
			if (!detail::readLine(data, pos, line)) {
				return true;  // no such section: nothing to load
			}
			line = detail::trimLeft(line);
			const size_t close = line.find(']');
			if (line.empty() || line[0] != '[' || close == std::string_view::npos || close < 2) {
				continue;
			}
			const std::string_view name = line.substr(1, close - 1);
			found = name.size() == sectionName.size();
			for (size_t i = 0; i < name.size(); i++) {
				const char c = name[i];
				if (static_cast<unsigned char>(c) > 0x7e) {
					return false;  // the game lower-cases such names through a wide string conversion
				}
				if (found && (c >= 'A' && c <= 'Z' ? c + 0x20 : c) != sectionName[i]) {
					found = false;
				}
			}
			if (found) {
				pos = lineStart;
			}
		}

		// cRZIniFile::EnumEntries (0x91d91c) and the callback at 0x63d1e0.
		detail::readLine(data, pos, line);  // the section header
		while (detail::readLine(data, pos, line)) {
			line = detail::trimLeft(line);
			if (line.empty() || line[0] == ';') {
				continue;
			}
			if (line[0] == '[') {
				break;
			}
			const size_t eq = line.find('=');
			if (eq == std::string_view::npos) {
				continue;
			}
			const std::string_view key = detail::trimRight(line.substr(0, eq));
			const std::string_view value = detail::trimRight(detail::trimLeft(line.substr(eq + 1)));
			if (key.empty() || value.empty()) {
				continue;
			}
			cSC4NetworkTileConflictRule rule = {};
			if ((ParseTilePair(key, rule._1, rule._2) && ParseTilePair(value, rule._3, rule._4))
				|| parseSlow(key, value, rule)) {
				addRule(rule);
			}
		}
		return true;
	}
}
