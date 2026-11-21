// Checks Rul0Parser against a port of the game's own code.
//
// Without arguments this runs the built-in cases and randomized comparisons.
// Arguments are RUL0 files (plain text, or DBPF packages that contain one) which
// are read with both implementations and compared entry by entry:
//
//   Rul0ParserTests "C:\...\Plugins\NetworkAddonMod_Controller.dat" ...
#include "Rul0Parser.h"
#include "IniFileReference.h"
#include <chrono>
#include <cstdio>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using Entries = std::vector<std::pair<std::string, std::string>>;
	using Sections = std::vector<std::pair<std::string, Entries>>;

	// A port of what the game's RUL0 loader (0x641b60) gets from cRZIniFile.
	namespace Reference
	{
		using namespace IniFileReference;

		// cRZString::MakeLower (0x90eac5). Returns false for the names it handles
		// through a wide string conversion, which is not ported.
		bool MakeLower(std::string& name)
		{
			for (char& c : name) {
				if (static_cast<unsigned char>(c) > 0x7e) {
					return false;
				}
				c = c >= 'A' && c <= 'Z' ? c + 0x20 : c;
			}
			return true;
		}

		// cRZIniFile::Open (0x91db73), then the sections (0x91cfa5) and for each
		// of them its entries (0x91d91c), as the loader asks for them.
		// Returns false where the port is not exact.
		bool Enumerate(std::string_view data, Sections& result)
		{
			File file = {data};

			std::map<std::string, uint32_t> positions;  // by lower case name
			std::map<std::string, std::string> names;
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
						const std::string name = line.substr(1).substr(0, close - 1);
						std::string lower = name;
						if (!MakeLower(lower)) {
							return false;
						}
						positions.insert({lower, lineStart});  // the first of a name wins
						names.insert({lower, name});
					}
				}
				lineStart = file.pos;
			}

			for (const auto& section : positions) {
				const auto original = names.find(section.first);
				const std::string name = original == names.end() ? section.first : original->second;
				result.emplace_back(name, Entries());

				std::string lower = name;
				MakeLower(lower);
				const auto position = positions.find(lower);
				if (position == positions.end() || file.SeekToPosition(position->second) >= file.Length() || !ReadLine(file, line)) {
					continue;
				}
				Entries& entries = result.back().second;
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
			}
			return true;
		}

		Rul0Parser::ByteVector NewGrid(uint32_t size)  // 0x608ec0
		{
			uint8_t* const cells = size == 0 ? nullptr : new uint8_t[size]();
			return {cells, cells + size, cells + size};
		}

		// 0x609040, with the wrapping arithmetic of the game
		void RotateGrid(Rul0Parser::ByteVector* grid, int32_t* width, int32_t* point, uint8_t rotation, char swapDirections)
		{
			const auto sub = [](int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b)); };
			const int32_t w = *width;
			if (w == 0) {
				return;
			}
			const int32_t size = static_cast<int32_t>(grid->end - grid->begin);
			const int32_t h = size / w;
			if ((rotation & 2) != 0) {
				point[0] = sub(sub(w, point[0]), 1);
				point[1] = sub(sub(h, point[1]), 1);
				for (int32_t i = 0, j = size - 1; i < j; i++, j--) {
					std::swap(grid->begin[i], grid->begin[j]);
				}
			}
			if ((rotation & 1) != 0) {
				const Rul0Parser::ByteVector rotated = NewGrid(static_cast<uint32_t>(size));
				for (int32_t x = 0; x < w; x++) {
					for (int32_t i = 0; i < h; i++) {
						uint8_t cell = grid->begin[(h - 1 - i) * w + x];
						if (swapDirections != 0 && cell >= 1 && cell <= 6) {
							cell = static_cast<uint8_t>(((cell - 1) ^ 1) + 1);  // 1<->2, 3<->4, 5<->6
						}
						rotated.begin[x * h + i] = cell;
					}
				}
				*width = h;
				std::swap(point[0], point[1]);
				point[0] = sub(sub(h, point[0]), 1);
				delete[] grid->begin;
				*grid = rotated;
			}
		}

		// 0x608f20
		void TransposeGrid(Rul0Parser::ByteVector* grid, uint32_t* width, int32_t* point, char swapDirections)
		{
			std::swap(point[0], point[1]);
			if (*width == 0) {
				return;
			}
			const uint32_t size = static_cast<uint32_t>(grid->end - grid->begin);
			const Rul0Parser::ByteVector transposed = NewGrid(size);
			const uint32_t w = *width;
			const uint32_t h = size / w;
			for (int32_t x = 0; x < static_cast<int32_t>(w); x++) {
				for (int32_t i = 0; i < static_cast<int32_t>(h); i++) {
					uint8_t cell = grid->begin[static_cast<uint32_t>(i) * w + x];
					if (swapDirections != 0 && (cell == 1 || cell == 2)) {
						cell = static_cast<uint8_t>(3 - cell);
					}
					transposed.begin[static_cast<uint32_t>(x) * h + i] = cell;
				}
			}
			*width = h;
			delete[] grid->begin;
			*grid = transposed;
		}
	}

	int failures = 0;

	void Fail(const std::string& what, std::string_view data = {})
	{
		failures++;
		std::string shown(data.substr(0, 300));
		for (char& c : shown) {
			c = c == '\r' ? '<' : c == '\n' ? '|' : c == '\0' ? '@' : c;
		}
		printf("FAILED: %s\n  input: %s\n", what.c_str(), shown.c_str());
	}

	void Expect(bool condition, const char* what, std::string_view data = {})
	{
		if (!condition) {
			Fail(what, data);
		}
	}

	struct FastResult
	{
		bool supported = false;
		Sections sections;
		bool terminated = true;  // a NUL follows every name, key and value
		bool found = true;       // every section is found by its name
	};

	FastResult ReadFast(std::string_view data)
	{
		FastResult result;
		std::vector<char> buffer(data.begin(), data.end());
		buffer.push_back('#');  // the parser may write here
		Rul0Parser::IniFile file;
		result.supported = file.Parse(buffer.data(), data.size());
		if (!result.supported) {
			return result;
		}
		const auto copy = [&result](std::string_view part) {
			result.terminated &= part.data()[part.size()] == '\0';
			return std::string(part);
		};
		for (const Rul0Parser::Section& section : file.Sections()) {
			result.sections.emplace_back(copy(section.name), Entries());
			const std::string name = result.sections.back().first;
			result.found &= file.Find(name) == &section && file.Find(section.lowerName) == &section;
			const Rul0Parser::Entry* const entries = file.Entries(section);
			for (size_t i = 0; i < section.entryCount; i++) {
				result.sections.back().second.emplace_back(copy(entries[i].key), copy(entries[i].value));
			}
		}
		return result;
	}

	// The fast parser may refuse data, but whatever it accepts must match the port.
	// Returns the number of entries, or -1 if the data was refused.
	int Compare(std::string_view data)
	{
		Sections expected;
		const bool exact = Reference::Enumerate(data, expected);
		const FastResult fast = ReadFast(data);
		if (!fast.supported) {
			if (exact && data.find('\0') == std::string_view::npos) {
				Fail("refused data without a reason", data);
			}
			return -1;
		}
		int entries = 0;
		if (!exact) {
			Fail("accepted data that the game reads differently", data);
		} else if (!fast.terminated || !fast.found) {
			Fail(fast.terminated ? "a section is not found by its name" : "a string is not terminated", data);
		} else if (fast.sections.size() != expected.size()) {
			Fail("section count " + std::to_string(fast.sections.size()) + ", expected " + std::to_string(expected.size()), data);
		} else {
			for (size_t i = 0; i < expected.size(); i++) {
				if (fast.sections[i] != expected[i]) {
					Fail("section " + std::to_string(i) + " [" + expected[i].first + "] differs", data);
					break;
				}
				entries += static_cast<int>(expected[i].second.size());
			}
		}
		return entries;
	}

	void TestCases()
	{
		const std::string rule = "[HighwayIntersectionInfo_0x00001000]\r\nPiece = 0.0, 0.0, 0, 0, 0x02000000\r\nCellLayout=..+..<\r\nRotate=1\r\n";
		const std::pair<std::string, int> cases[] = {
			{rule, 3},
			{"[Ordering]\r\nRotationRing = 1000, 11000, 21000\r\nAddTypes = 2000, 12000 ; note\r\n" + rule, 5},
			{"[A]\nk=v", 1},
			{"[A]\nk=v\n", 1},
			{"[A]\rk=v\r\r\n\nk2 = v2 \t", 2},
			{"\n\n[A]\nk=v\n", 1},
			{"k=v\n[A]\nk=v\n", 1},                                 // entries before the first section
			{"[B]\nb=1\n[A]\na=1\n[C]\nc=1\n", 3},                 // sorted by name
			{"[b]\nb=1\n[A]\na=1\n[C]\nc=1\n[_]\nu=1\n", 4},       // in lower case
			{"[A]\nk=first\n[a]\nk=second\n[A]\nk=third\n", 1},    // the first of a name wins
			{"[Sec]\nk=1\n[Other]\nk=2\n[SEC]\nk=3\n[sec]\n", 2},
			{"[A]\nk=1\n[\nk=2\n", 1},                              // any line with '[' ends a section
			{"[A]\nk=1\n[]\nk=2\n", 1},
			{"[A]\nk=1\n[unterminated\nk=2\n[B]\nk=3\n", 2},
			{"[A]\nk=1\n  \t[B] trailing text\nk=2\n", 2},
			{"[A]x]\nk=1\n[ A ]\nk=2\n[A B]\nk=3\n", 3},
			{"[A]\n;k=1\n ; k=2\nk;=3\nk=;4\n", 2},
			{"[A]\nnoequals\n=v\nk=\n = \nk = \t\n\t k \t=\t v \t\n", 1},
			{"[A]\nk=a=b\nk==\n=k=v\n", 2},
			{"[A]\n" + std::string(100, ' ') + "key" + std::string(50, '\t') + "=" + std::string(60, ' ') + "value" + std::string(70, ' ') + "\n", 1},
			{"[" + std::string(200, 'N') + "]\n" + std::string(500, 'k') + "=" + std::string(981, 'v') + "\n", 1},
			{"[A]\nk=\xe9t\xe9\n\xb2=1\nk\v=\f1\n", 3},              // other bytes pass through
			{"[A]\n[B]\n[C]\n", 0},
			{"[A]", 0},
			{"", 0},
			{"no section\n", 0},
		};
		for (const auto& [data, entries] : cases) {
			Expect(Compare(data) == entries, "wrong number of entries", data);
		}

		// data that the game's own code has to handle
		Expect(Compare(rule + std::string(1, '\0') + rule) == -1, "NUL bytes should be refused");
		Expect(Compare(rule + "[Highway\xcdntersectionInfo_0x00001001]\r\nRotate=1\r\n") == -1, "non-ASCII section names should be refused");
		Expect(Compare(rule + "[\x7f]\r\nk=v\r\n") == -1, "non-ASCII section names should be refused");
		Expect(Compare(rule + "[\x7e\x01]\r\nk=v\r\n") == 4, "other names are fine");

		const FastResult order = ReadFast("[b]\nk=1\n[C]\n[A]\nk=2\nk=3\n[B]\nk=4\n");
		Expect(order.supported && order.sections == Sections{{"A", {{"k", "2"}, {"k", "3"}}}, {"b", {{"k", "1"}}}, {"C", {}}}, "sections and entries");
	}

	// Sections, entries and noise glued together at random, then compared.
	void TestRandom()
	{
		std::mt19937 rng(20261121);
		const auto pick = [&rng](uint32_t n) { return static_cast<uint32_t>(rng() % n); };
		const auto any = [&pick](const auto& list) { return list[pick(static_cast<uint32_t>(std::size(list)))]; };
		const std::string_view noise = "[]]==;; \t\t\r\n\nabAB01,.+<^\v\f\x01\x7e\x7f\x80\xe9";
		const std::string_view names[] = {"Ordering", "ordering", "ORDERING", "HighwayIntersectionInfo_0x00001000",
			"HighwayIntersectionInfo_0x00001001", "highwayintersectioninfo_0x00001000", "HighwayIntersectionInfo_0x0000100A",
			"HighwayIntersectionInfo_0x0000100a", "HighwayIntersectionInfo_4096", "A", "a", "B", "Z", "_", "a b", " A", "A ", "0", "[", "x]y",
			"N\xe4me", "", "HighwayIntersectionInfo_", "Highwayintersectioninfo_0x00001000"};
		const std::string_view keys[] = {"Piece", "PreviewEffect", "CellLayout", "ConsLayout", "CheckType", "CopyFrom", "Rotate", "Transpose",
			"RotationRing", "AddTypes", "AutoTileBase", "Costs", "OneWayDir", "k", "K", "a b", "", ";k", "k;", "[k", "\xe9"};
		const std::string_view values[] = {"0.0, 0.0, 0, 0, 0x02000000", "preview_road_puzzle_piece001", "..+..<", "  ^", "a - road: 0x02000200",
			"0x1000", "1", "3", "1000, 11000, 21000", "0x55387000", "v", "", "a=b", "; note", "v ; note", "[v]", "\xe9"};
		const std::string_view lines[] = {"", " ", ";comment", "; a=b", "[", "[]", "[x", "]", "text", "=", "==", "k=", "=v", "\t", " ; [A]", "[\x80]"};
		const std::string_view newlines[] = {"\r\n", "\r\n", "\r\n", "\n", "\r", "\r\n\r\n", "\n\n\r", ""};
		const std::string_view blanks[] = {"", "", "", "", " ", "\t", "  ", " \t "};

		size_t accepted = 0, entries = 0;
		for (int round = 0; round < 60000; round++) {
			std::string data;
			for (uint32_t n = pick(24); n > 0; n--) {
				data += any(blanks);
				switch (pick(8)) {
					case 0:
						data += any(lines);
						break;
					case 1:
					case 2:
						data += "[";
						data += any(names);
						data += pick(12) == 0 ? "" : "]";
						data += pick(8) == 0 ? " x" : "";
						break;
					default:
						data += any(keys);
						data += any(blanks);
						data += pick(20) == 0 ? "" : "=";
						data += any(blanks);
						data += any(values);
						break;
				}
				data += any(blanks);
				data += any(newlines);
			}
			for (uint32_t n = pick(4) == 0 ? pick(4) : 0; n > 0 && !data.empty(); n--) {  // damage a few bytes
				const uint32_t at = pick(static_cast<uint32_t>(data.size()));
				switch (pick(3)) {
					case 0: data[at] = any(noise); break;
					case 1: data.insert(at, 1, any(noise)); break;
					default: data.erase(at, 1); break;
				}
			}
			if (pick(500) == 0) {
				data.insert(pick(static_cast<uint32_t>(data.size() + 1)), 1, '\0');
			}

			const int count = Compare(data);
			if (count >= 0) {
				accepted++;
				entries += count;
			}
		}
		printf("random files: %zu of 60000 accepted, %zu entries\n", accepted, entries);
		Expect(accepted > 40000 && accepted < 58000 && entries > 100000, "random files should exercise the parser and the refusals");
	}

	// The grid functions of the game must do the same with and without the shortcut.
	void TestGrids()
	{
		std::mt19937 rng(20261121);
		const auto pick = [&rng](uint32_t n) { return static_cast<uint32_t>(rng() % n); };
		const int32_t points[] = {0, 1, 2, 5, -1, -7, 1000, -1000, INT32_MAX, INT32_MIN};
		const auto fill = [&](std::vector<uint8_t>& cells, uint32_t& width, int32_t point[2]) {
			const uint32_t columns = pick(9), rows = pick(9);
			cells.resize(pick(6) == 0 ? pick(40) : columns * rows);
			for (uint8_t& cell : cells) {
				cell = static_cast<uint8_t>(pick(10));
			}
			switch (pick(6)) {
				case 0: width = pick(20000); break;                                 // usually more than there are cells
				case 1: width = static_cast<uint32_t>(-static_cast<int32_t>(pick(2000))); break;
				case 2: width = static_cast<uint32_t>(cells.size()) + pick(3); break;
				default: width = columns; break;
			}
			point[0] = points[pick(static_cast<uint32_t>(std::size(points)))];
			point[1] = points[pick(static_cast<uint32_t>(std::size(points)))];
		};
		struct Grid
		{
			Rul0Parser::ByteVector cells;
			uint32_t width;
			int32_t point[2];

			Grid(const std::vector<uint8_t>& from, uint32_t width, const int32_t* point) : cells(Reference::NewGrid(static_cast<uint32_t>(from.size()))), width(width), point{point[0], point[1]}
			{
				std::copy(from.begin(), from.end(), cells.begin);
			}
			~Grid() { delete[] cells.begin; }
			bool operator==(const Grid& other) const
			{
				return width == other.width && point[0] == other.point[0] && point[1] == other.point[1]
					&& std::equal(cells.begin, cells.end, other.cells.begin, other.cells.end);
			}
		};

		size_t shortened = 0;
		for (int round = 0; round < 200000; round++) {
			std::vector<uint8_t> cells;
			uint32_t width;
			int32_t point[2];
			fill(cells, width, point);
			const uint8_t rotation = static_cast<uint8_t>(pick(4) | (pick(8) == 0 ? 0xf4 : 0));
			const char swapDirections = static_cast<char>(pick(2));
			shortened += static_cast<int32_t>(width) > 0 && cells.size() + 1 < width;

			Grid expected(cells, width, point), actual(cells, width, point);
			Reference::RotateGrid(&expected.cells, reinterpret_cast<int32_t*>(&expected.width), expected.point, rotation, swapDirections);
			Rul0Parser::RotateGridQuickly(Reference::RotateGrid, &actual.cells, reinterpret_cast<int32_t*>(&actual.width), actual.point, rotation, swapDirections);
			if (!(expected == actual)) {
				Fail("rotating a grid of " + std::to_string(cells.size()) + " cells, width " + std::to_string(static_cast<int32_t>(width))
					+ ", by " + std::to_string(rotation & 3) + " gives another result with the shortcut");
			}

			Grid expected2(cells, width, point), actual2(cells, width, point);
			Reference::TransposeGrid(&expected2.cells, &expected2.width, expected2.point, swapDirections);
			Rul0Parser::TransposeGridQuickly(Reference::TransposeGrid, &actual2.cells, &actual2.width, actual2.point, swapDirections);
			if (!(expected2 == actual2)) {
				Fail("transposing a grid of " + std::to_string(cells.size()) + " cells, width " + std::to_string(static_cast<int32_t>(width))
					+ " gives another result with the shortcut");
			}
		}
		printf("random grids: 200000 rotated and transposed, %zu of them with a width beyond the cells\n", shortened);
		Expect(shortened > 20000, "random grids should exercise the shortcut");

		// the case of the game: no cells, and a width that was never set
		int calls = 0;
		const auto countColumns = [&calls](Rul0Parser::ByteVector* grid, int32_t* width, int32_t* point, uint8_t rotation, char swapDirections) {
			calls += *width;
			Reference::RotateGrid(grid, width, point, rotation, swapDirections);
		};
		Rul0Parser::ByteVector empty = {};
		int32_t width = 0x02345678, point[2] = {3, 4};
		Rul0Parser::RotateGridQuickly(countColumns, &empty, &width, point, 1, 0);
		Expect(calls == 1 && width == 0 && point[0] == -5 && point[1] == 3 && empty.begin == nullptr, "an empty grid is rotated in one step");
	}

	void TestFile(const char* path)
	{
		using Clock = std::chrono::steady_clock;
		const auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };

		std::string text;
		if (!IniFileReference::ReadRulFile(path, 0x10000000, text)) {
			failures++;
			printf("FAILED: no uncompressed RUL0 found in %s\n", path);
			return;
		}

		Sections expected;
		auto start = Clock::now();
		const bool exact = Reference::Enumerate(text, expected);
		const auto referenceTime = Clock::now() - start;

		std::vector<char> buffer(text.begin(), text.end());
		buffer.push_back('#');
		start = Clock::now();
		Rul0Parser::IniFile file;
		const bool supported = file.Parse(buffer.data(), text.size());
		const auto fastTime = Clock::now() - start;

		const FastResult fast = ReadFast(text);
		size_t entries = 0, differences = fast.sections.size() == expected.size() ? 0 : 1;
		for (size_t i = 0; i < std::min(fast.sections.size(), expected.size()); i++) {
			differences += fast.sections[i] != expected[i];
			entries += expected[i].second.size();
		}
		const bool ok = exact && supported && fast.supported && fast.terminated && fast.found && differences == 0 && file.EntryCount() >= entries;
		if (!ok) {
			failures++;
		}
		printf("%s: %s\n  %zu bytes, %zu sections (port: %zu), %zu entries, %zu sections differ\n  port %.0f ms, fast parser %.1f ms\n",
			ok ? "ok" : "FAILED", path, text.size(), fast.sections.size(), expected.size(), entries, differences,
			ms(referenceTime), ms(fastTime));
	}
}

int main(int argc, char** argv)
{
	TestCases();
	TestRandom();
	TestGrids();
	for (int i = 1; i < argc; i++) {
		TestFile(argv[i]);
	}
	printf(failures == 0 ? "all passed\n" : "%d FAILED\n", failures);
	return failures == 0 ? 0 : 1;
}
