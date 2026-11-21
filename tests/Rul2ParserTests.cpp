// Checks Rul2Parser against a port of the game's own RUL2 loading code.
//
// Without arguments this runs the built-in cases and a randomized comparison.
// Arguments are RUL2 files (plain text, or DBPF packages that contain one) which
// are loaded with both implementations and compared rule by rule:
//
//   Rul2ParserTests "C:\...\Plugins\NetworkAddonMod_Controller.dat" ...
#include "Rul2Parser.h"
#include "RuleEquivalence.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
	using Rules = std::vector<cSC4NetworkTileConflictRule>;

	// The callback the game passes to EnumEntries (0x63d1e0), with the sscanf of this build.
	bool ScanRule(std::string_view key, std::string_view value, cSC4NetworkTileConflictRule& rule)
	{
		return Rul2Parser::ScanRule(sscanf, key, value, rule);
	}

	// A port of the parts of cRZIniFile that the game uses to load RUL2, kept as
	// close to the disassembly as possible (and therefore slow).
	namespace Reference
	{
		struct File
		{
			std::string_view data;
			uint32_t pos = 0;

			uint32_t Length() const { return static_cast<uint32_t>(data.size()); }
			uint32_t SeekToPosition(uint32_t newPos) { return pos = std::min(newPos, Length()); }
			bool Read(char* out, uint32_t count)
			{
				if (count > Length() - pos) {
					return false;
				}
				std::memcpy(out, data.data() + pos, count);
				pos += count;
				return true;
			}
		};

		// cRZIniFile::ReadLine (0x91ca15)
		bool ReadLine(File& file, std::string& line)
		{
			std::string result;
			bool success = false;
			bool newlineFound = false;
			uint32_t blockStart = file.pos;
			uint32_t remaining = file.Length() - blockStart;
			char block[0x2a] = {};
			while (remaining != 0) {
				const uint32_t count = std::min<uint32_t>(remaining, 0x28);
				if (!file.Read(block, count)) {
					break;
				}
				remaining -= count;
				success = true;
				uint32_t i = 0;
				for (; i < count; i++) {
					if (block[i] == '\r' || block[i] == '\n') {
						newlineFound = true;
						file.SeekToPosition(blockStart + i);
						char c;
						bool read;
						while ((read = file.Read(&c, 1)) && (c == '\r' || c == '\n')) {
						}
						if (read) {
							file.SeekToPosition(file.pos - 1);
						}
						break;
					}
				}
				block[i] = '\0';
				result.append(block);  // as a C string
				if (newlineFound) {
					break;
				}
				blockStart = file.pos;
			}
			line = result;
			return success;
		}

		void TrimLeft(std::string& s) { s.erase(0, s.find_first_not_of(" \t")); }
		void TrimRight(std::string& s) { s.erase(s.find_last_not_of(" \t") + 1); }

		// cRZIniFile::Open (0x91db73), cRZIniFile::EnumEntries (0x91d91c) and the
		// loader at 0x63d3e0. Returns false where the port is not exact: section
		// names with bytes above 0x7e that could shadow the section.
		bool LoadRuleOverrides(std::string_view data, Rules& rules)
		{
			File file = {data};

			std::vector<std::pair<std::string, uint32_t>> sections;  // a map in the game: the first of a name wins
			std::string line;
			uint32_t lineStart = file.pos;
			while (ReadLine(file, line)) {
				TrimLeft(line);
				if (!line.empty() && line[0] == '[') {
					size_t close = 1;
					while (close < line.size() && line[close] != ']') {
						close++;
					}
					if (close < line.size() && close >= 2) {
						std::string name = line.substr(1).substr(0, close - 1);
						for (char& c : name) {
							if (static_cast<unsigned char>(c) > 0x7e) {
								name = "\x7f";  // lower-cased through a wide string conversion in the game
								break;
							}
							c = c >= 'A' && c <= 'Z' ? c + 0x20 : c;
						}
						sections.emplace_back(name, lineStart);
					}
				}
				lineStart = file.pos;
			}

			const auto section = std::find_if(sections.begin(), sections.end(),
				[](const auto& s) { return s.first == "ruleoverrides" || s.first == "\x7f"; });
			if (section == sections.end()) {
				return true;
			} else if (section->first == "\x7f") {
				return false;
			}

			if (file.SeekToPosition(section->second) >= file.Length() || !ReadLine(file, line)) {
				return true;
			}
			std::vector<std::pair<std::string, std::string>> entries;
			while (ReadLine(file, line)) {
				TrimLeft(line);
				if (!line.empty() && line[0] != ';') {
					if (line[0] == '[') {
						break;
					}
					std::string key;  // SplitToken("=")
					const size_t eq = line.find('=');
					if (eq != std::string::npos) {
						key = line.substr(0, eq);
						line.erase(0, eq + 1);
					}
					TrimRight(key);
					TrimLeft(line);
					TrimRight(line);
					if (!key.empty() && !line.empty()) {
						entries.emplace_back(key, line);
					}
				}
			}
			for (const auto& [key, value] : entries) {
				cSC4NetworkTileConflictRule rule = {};
				if (ScanRule(key, value, rule)) {
					rules.push_back(rule);
				}
			}
			return true;
		}
	}

	bool SameTile(const Tile& a, const Tile& b)
	{
		return a.id == b.id && a.rf == b.rf;
	}

	bool SameRule(const cSC4NetworkTileConflictRule& a, const cSC4NetworkTileConflictRule& b)
	{
		return SameTile(a._1, b._1) && SameTile(a._2, b._2) && SameTile(a._3, b._3) && SameTile(a._4, b._4);
	}

	struct FastResult
	{
		bool supported = false;
		Rules rules;
		size_t slowLines = 0;
	};

	FastResult LoadFast(std::string_view data)
	{
		FastResult result;
		result.supported = Rul2Parser::ForEachRuleOverride(data,
			[&](const cSC4NetworkTileConflictRule& rule) { result.rules.push_back(rule); },
			[&](std::string_view key, std::string_view value, cSC4NetworkTileConflictRule& rule) {
				result.slowLines++;
				return ScanRule(key, value, rule);
			});
		return result;
	}

	int failures = 0;

	void Fail(const std::string& what, std::string_view data)
	{
		failures++;
		std::string shown(data.substr(0, 300));
		for (char& c : shown) {
			c = c == '\r' ? '<' : c == '\n' ? '|' : c == '\0' ? '@' : c;
		}
		printf("FAILED: %s\n  input: %s\n", what.c_str(), shown.c_str());
	}

	// The fast parser may refuse data, but whatever it accepts must match the port.
	// Returns the number of lines that went through sscanf, or -1 if refused.
	int Compare(std::string_view data)
	{
		Rules expected;
		const bool exact = Reference::LoadRuleOverrides(data, expected);
		const FastResult fast = LoadFast(data);
		if (!fast.supported) {
			if (!fast.rules.empty()) {
				Fail("rules were added before the data was refused", data);
			}
			return -1;
		}
		if (!exact) {
			Fail("accepted data that the game reads differently", data);
		} else if (fast.rules.size() != expected.size()) {
			Fail("rule count " + std::to_string(fast.rules.size()) + ", expected " + std::to_string(expected.size()), data);
		} else {
			for (size_t i = 0; i < expected.size(); i++) {
				if (!SameRule(fast.rules[i], expected[i])) {
					Fail("rule " + std::to_string(i) + " differs", data);
					break;
				}
			}
		}
		return static_cast<int>(fast.slowLines);
	}

	void Expect(bool condition, const char* what, std::string_view data = {})
	{
		if (!condition) {
			Fail(what, data);
		}
	}

	void TestCases()
	{
		const std::string head = "[RuleOverrides]\r\n";
		const std::string rule = "0x03001c00,2,1,0x0324fd00,2,1=0x03aa6600,3,0,0x0324fd00,2,1";

		// plain rules take the fast path
		const std::string plain[] = {
			head + rule + "\r\n",
			head + rule,
			head + rule + "\n" + rule + "\r" + rule + "\n\n\r\n" + rule,
			head + "0X5FD45100,0,0,0x1,3,1=0,0,0,0,0,0\r\n",
			head + "\t\t0x5d566b80,3,0,0x05010100,3,0=0x5d566b80,3,0,0x5d566a80,1,0\t\r\n",
			head + " 0x1 , 2 , 1 , 0x2 , 0 , 0 = 0x3 , 1 , 0 , 0x4 , 3 , 1 ; comment\r\n",
			head + "0xFFFFFFFF,255,7,123456789,256,00=0xabcdef01,3,0,0x0,0,1;x=y\r\n",
			head + "1,2,0,3,0,1=4,1,0,5,3,0 trailing = text\r\n",
			"; comment\r\n\r\n  [ruleOVERRIDES] ignored\r\n; comment = 1\r\n" + rule + "\r\n",
			"\n\n" + head + rule,
			"[Other]\r\n" + rule + "\r\n" + head + rule + "\r\n[Next]\r\n" + rule + "\r\n",
			head + rule + "\r\n[RuleOverrides]\r\n0x1,0,0,0x2,0,0=0x3,0,0,0x4,0,0\r\n",
			head + rule + "\r\n  [unterminated\r\n" + rule + "\r\n",
			"[]\r\n[x\r\n" + head + rule + "\r\n",
			head + "=" + rule + "\r\n" + rule + "=\r\nno equals sign\r\n \t \r\n" + rule + "\r\n",
			head + std::string(100, ' ') + rule + std::string(100, '\t') + "\r\n",
			head,
			"",
			"[RuleOverrides]",
			rule + "\r\n",
			"[RuleOverrides\r\n" + rule + "\r\n",
			"[ RuleOverrides]\r\n" + rule + "\r\n",
		};
		for (const std::string& data : plain) {
			Expect(Compare(data) == 0, "plain rules should not need sscanf", data);
		}

		// unusual numbers are left to sscanf
		const std::string odd[] = {
			"010,0,0,0x2,0,0=0x3,0,0,0x4,0,0",                // octal
			"-1,0,0,0x2,0,0=0x3,0,0,0x4,0,0",                 // sign
			"+0x1,0,0,0x2,0,0=0x3,0,0,0x4,0,0",
			"0x123456789,0,0,0x2,0,0=0x3,0,0,0x4,0,0",        // overflow
			"4294967296,0,0,0x2,0,0=0x3,0,0,0x4,0,0",
			"1234567890,0,0,0x2,0,0=0x3,0,0,0x4,0,0",
			"0x,0,0,0x2,0,0=0x3,0,0,0x4,0,0",                 // no digits
			"0xg,0,0,0x2,0,0=0x3,0,0,0x4,0,0",
			"0x1,0,0x1,0x2,0,0=0x3,0,0,0x4,0,0",              // hex where %d is read
			"0x1,0,0,0x2,0,0=0x3,0,0,0x4,0",                  // too few numbers
			"0x1,0,0,0x2,0,0=0x3,0,0,0x4,0,",
			"0x1,0,0,0x2,0,0=0x3;0,0,0x4,0,0",                // wrong separator
			"0x1,0,0,0x2,0,0=0x3,0,0,,0x4,0,0",
			"0x1,0,0,0x2,0,0 0x3,0,0,0x4,0,0=1",
			"0x1,0,0,0x2,0,0=0x3,0,0,0x4,0,0\xb2",            // may be a digit in some locales
			"0x1\xa0,0,0,0x2,0,0=0x3,0,0,0x4,0,0",            // may be a space in some locales
			"0x1,0\v,0,0x2,0,0=0x3,\f0,0,0x4,0,0",            // whitespace that is not trimmed
			"abc=def",
			"0x1,0,0,0x2,0,0==0x3,0,0,0x4,0,0",
		};
		for (const std::string& line : odd) {
			Expect(Compare(head + line + "\r\n" + rule + "\r\n") == 1, "odd line should be left to sscanf", line);
		}

		// data that the game's own loader has to handle
		Expect(Compare(head + rule + std::string(1, '\0') + "\r\n" + rule) == -1, "NUL bytes should be refused");
		Expect(Compare("[Rule\xd6verrides]\r\n" + head + rule) == -1, "non-ASCII section names should be refused");
		Expect(Compare(head + rule + "\r\n[\xe9]\r\n") == 0, "later sections do not matter");

		Tile a, b;
		Expect(Rul2Parser::ParseTilePair("0x03001c00,2,1,0x0324fd00,3,0", a, b)
			&& a.id == 0x03001c00 && a.rf == R2F1 && b.id == 0x0324fd00 && b.rf == R3F0, "ParseTilePair values");
	}

	// Rules, section headers and noise glued together at random, then compared.
	void TestRandom()
	{
		std::mt19937 rng(20261121);
		const auto pick = [&rng](uint32_t n) { return static_cast<uint32_t>(rng() % n); };
		const std::string_view noise = "0123456789abcdefxX,,,==;[] \t\t+-\r\n\n\v\f\x01\x7f\x80\xa0\xb2gz";
		const std::string_view numbers[] = {"0", "1", "2", "3", "0x0", "0x1", "0x5f000100", "0X0A0b0C0d", "255", "256", "010", "08",
			"0x", "-1", "+2", "0xfffffffff", "99999999999", "123456789", "0x12345678", "00", "1e3", ""};
		const std::string_view lines[] = {"[RuleOverrides]", "[ruleoverrides]", "[RULEOVERRIDES] x", " \t[RuleOverrides]", "[Other]", "[",
			"[]", "[RuleOverrides", ";comment", "; a=b", "", " ", "key=value", "=", "0x1,0,0,0x2,0,0=", "[Rule\xd6verrides]"};
		const std::string_view newlines[] = {"\r\n", "\r\n", "\n", "\r", "\r\n\r\n", "\n\n\r", ""};
		const std::string_view blanks[] = {"", "", "", "", " ", "\t", "  ", " \t "};

		size_t accepted = 0, rules = 0, slow = 0;
		for (int round = 0; round < 60000; round++) {
			std::string data;
			if (pick(8) != 0) {
				data += "[RuleOverrides]\r\n";
			}
			for (uint32_t n = pick(12); n > 0; n--) {
				if (pick(6) == 0) {
					data += lines[pick(static_cast<uint32_t>(std::size(lines)))];
				} else {
					for (int i = 0; i < 12; i++) {
						data += blanks[pick(static_cast<uint32_t>(std::size(blanks)))];
						data += pick(40) == 0 ? numbers[pick(static_cast<uint32_t>(std::size(numbers)))]
							: i % 3 == 0 ? numbers[pick(8)]  // id
							: numbers[pick(i % 3 == 1 ? 4 : 2)];  // rotation, flip
						data += blanks[pick(static_cast<uint32_t>(std::size(blanks)))];
						data += i == 11 ? (pick(4) == 0 ? " ; note" : "") : i == 5 ? "=" : ",";
					}
				}
				data += newlines[pick(static_cast<uint32_t>(std::size(newlines)))];
			}
			for (uint32_t n = pick(4) == 0 ? pick(4) : 0; n > 0 && !data.empty(); n--) {  // damage a few bytes
				const uint32_t at = pick(static_cast<uint32_t>(data.size()));
				switch (pick(3)) {
					case 0: data[at] = noise[pick(static_cast<uint32_t>(noise.size()))]; break;
					case 1: data.insert(at, 1, noise[pick(static_cast<uint32_t>(noise.size()))]); break;
					default: data.erase(at, 1); break;
				}
			}
			if (pick(500) == 0) {
				data.insert(pick(static_cast<uint32_t>(data.size() + 1)), 1, '\0');
			}

			const int slowLines = Compare(data);
			if (slowLines >= 0) {
				accepted++;
				slow += slowLines;
				rules += LoadFast(data).rules.size();
			}
		}
		printf("random: %zu of 60000 inputs accepted, %zu rules, %zu lines left to sscanf\n", accepted, rules, slow);
		Expect(accepted > 50000 && rules > 100000 && slow > 10000, "random inputs should exercise both paths");
	}

	// Returns the RUL2 text of a file: the file itself, or the entry inside a DBPF package.
	bool ReadRul2(const char* path, std::string& text)
	{
		std::ifstream file(path, std::ios::binary);
		std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		if (!file || content.empty()) {
			return false;
		}
		if (content.compare(0, 4, "DBPF") != 0) {
			text = std::move(content);
			return true;
		}
		const auto u32 = [&content](size_t offset) {
			uint32_t value = 0;
			if (offset + 4 <= content.size()) {
				std::memcpy(&value, content.data() + offset, 4);
			}
			return value;
		};
		const uint32_t count = u32(36), index = u32(40);
		for (uint32_t i = 0; i < count; i++) {
			const size_t entry = index + i * size_t{20};
			if (u32(entry) == 0x0a5bcf4b && u32(entry + 4) == 0xaa5bcf57 && u32(entry + 8) == 0x10000002) {
				const uint32_t offset = u32(entry + 12), size = u32(entry + 16);
				if (size_t{offset} + size > content.size() || (size > 6 && u32(offset + 4) << 16 == 0xfb100000)) {
					return false;  // truncated or compressed
				}
				text = content.substr(offset, size);
				return true;
			}
		}
		return false;
	}

	void TestFile(const char* path)
	{
		using Clock = std::chrono::steady_clock;
		const auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };

		std::string text;
		if (!ReadRul2(path, text)) {
			failures++;
			printf("FAILED: no uncompressed RUL2 found in %s\n", path);
			return;
		}

		Rules expected;
		auto start = Clock::now();
		const bool exact = Reference::LoadRuleOverrides(text, expected);
		const auto referenceTime = Clock::now() - start;

		start = Clock::now();
		const FastResult fast = LoadFast(text);
		const auto fastTime = Clock::now() - start;

		size_t differences = fast.rules.size() == expected.size() ? 0 : 1;
		for (size_t i = 0; i < std::min(fast.rules.size(), expected.size()); i++) {
			differences += !SameRule(fast.rules[i], expected[i]);
		}
		if (!exact || !fast.supported || differences != 0) {
			failures++;
		}

		// what the DLL does with the rules, to see how long the whole load takes
		std::unordered_set<cSC4NetworkTileConflictRule, RuleEquivalenceHash, RuleEquivalence> set;
		start = Clock::now();
		Rul2Parser::ForEachRuleOverride(text,
			[&set](const cSC4NetworkTileConflictRule& rule) { set.insert(rule); },
			[](std::string_view key, std::string_view value, cSC4NetworkTileConflictRule& rule) {
				return ScanRule(key, value, rule);
			});
		const auto loadTime = Clock::now() - start;

		printf("%s: %s\n  %zu bytes, %zu rules (port: %zu), %zu lines left to sscanf, %zu differences\n"
			"  port %.0f ms, fast parser %.0f ms, fast parser into rule set %.0f ms\n",
			exact && fast.supported && differences == 0 ? "ok" : "FAILED", path,
			text.size(), fast.rules.size(), expected.size(), fast.slowLines, differences,
			ms(referenceTime), ms(fastTime), ms(loadTime));
	}
}

int main(int argc, char** argv)
{
	TestCases();
	TestRandom();
	for (int i = 1; i < argc; i++) {
		TestFile(argv[i]);
	}
	printf(failures == 0 ? "all passed\n" : "%d FAILED\n", failures);
	return failures == 0 ? 0 : 1;
}
