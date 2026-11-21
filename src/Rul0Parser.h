#pragma once
// Serves the sections and entries of a RUL0 file that is held in memory, the way
// cRZIniFile serves them to the game's loader (0x641b60).
//
// The game opens the file by reading all of it line by line (40 bytes per read,
// one string per line) to find the sections. It then reads every section again in
// the same way, builds two more strings per entry and collects them in a vector
// before it hands them to the code that fills the intersection rules. Here the
// file is split once, without copying its text: the names, keys and values point
// into the buffer and are terminated there.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include "Rul2Parser.h"

namespace Rul0Parser
{
	struct Entry
	{
		std::string_view key;
		std::string_view value;
	};

	struct Section
	{
		std::string lowerName;  // what cRZIniFile orders and finds sections by
		std::string_view name;  // as written in the file
		size_t firstEntry = 0;
		size_t entryCount = 0;
	};

	class IniFile
	{
	public:
		// Splits `data` into sections and entries. The text is changed: a NUL is
		// written after every name, key and value, so `data[size]` has to be
		// writable as well. Returns false if the data has to go through the
		// game's own code instead (NUL bytes, non-ASCII section names).
		bool Parse(char* data, size_t size);

		// The sections in the order in which cRZIniFile enumerates them (0x91cfa5):
		// sorted by lower case name. Of sections with the same name only the first
		// one of the file exists.
		const std::vector<Section>& Sections() const { return sections; }

		// The section that cRZIniFile reads the entries of for this name (0x91d91c).
		const Section* Find(std::string_view name) const;

		const Entry* Entries(const Section& section) const { return entries.data() + section.firstEntry; }
		size_t EntryCount() const { return entries.size(); }

	private:
		std::vector<Section> sections;
		std::vector<Entry> entries;
	};

	namespace detail
	{
		constexpr char toLower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 0x20) : c; }
	}

	inline bool IniFile::Parse(char* data, size_t size)
	{
		using namespace Rul2Parser::detail;
		sections.clear();
		entries.clear();

		// The game appends each 40 byte block it reads as a C string.
		if (size != 0 && std::memchr(data, '\0', size) != nullptr) {
			return false;
		}
		const std::string_view text(data, size);
		const auto terminate = [data, &text](std::string_view part) { data[part.data() - text.data() + part.size()] = '\0'; };

		// cRZIniFile::Open (0x91db73) records where each "[name]" line is. The entries
		// of a section (0x91d91c) are the "key=value" lines after it, up to the next
		// line that starts with '[', whether that is a section or not.
		bool inSection = false;
		size_t pos = 0;
		std::string_view line;
		while (readLine(text, pos, line)) {
			line = trimLeft(line);
			if (line.empty() || line[0] == ';') {
				continue;
			}
			if (line[0] == '[') {
				inSection = false;
				const size_t close = line.find(']', 1);
				if (close == std::string_view::npos || close < 2) {
					continue;
				}
				Section section;
				section.name = line.substr(1, close - 1);
				section.firstEntry = entries.size();
				section.lowerName.reserve(section.name.size());
				for (const char c : section.name) {
					if (static_cast<unsigned char>(c) > 0x7e) {
						return false;  // the game lower-cases such names through a wide string conversion
					}
					section.lowerName.push_back(detail::toLower(c));
				}
				terminate(section.name);
				sections.push_back(std::move(section));
				inSection = true;
			} else if (inSection) {
				const size_t eq = line.find('=');
				if (eq == std::string_view::npos) {
					continue;
				}
				const std::string_view key = trimRight(line.substr(0, eq));
				const std::string_view value = trimRight(trimLeft(line.substr(eq + 1)));
				if (key.empty() || value.empty()) {
					continue;
				}
				terminate(key);
				terminate(value);
				entries.push_back({key, value});
				sections.back().entryCount++;
			}
		}

		// The game keeps the sections in a map: ordered by name, the first of a name wins.
		std::stable_sort(sections.begin(), sections.end(),
			[](const Section& a, const Section& b) { return a.lowerName < b.lowerName; });
		sections.erase(std::unique(sections.begin(), sections.end(),
			[](const Section& a, const Section& b) { return a.lowerName == b.lowerName; }), sections.end());
		return true;
	}

	inline const Section* IniFile::Find(std::string_view name) const
	{
		// compares like the game: lower case, byte by byte
		const auto less = [](char a, char b) { return static_cast<unsigned char>(detail::toLower(a)) < static_cast<unsigned char>(detail::toLower(b)); };
		const auto section = std::lower_bound(sections.begin(), sections.end(), name, [&less](const Section& s, std::string_view n) {
			return std::lexicographical_compare(s.lowerName.begin(), s.lowerName.end(), n.begin(), n.end(), less);
		});
		if (section == sections.end() || std::lexicographical_compare(name.begin(), name.end(), section->lowerName.begin(), section->lowerName.end(), less)) {
			return nullptr;
		}
		return &*section;
	}

	// A std::vector<uint8_t> of the game.
	struct ByteVector
	{
		uint8_t* begin;
		uint8_t* end;
		uint8_t* capacity;
	};

	// The entries Rotate and Transpose turn the two cell grids of a rule with the
	// functions at 0x609040 and 0x608f20. These walk the grid column by column:
	// `width` times a loop over `size / width` cells. A rule that has no cells yet
	// has an empty grid, and its width was never set, so that the game counts up
	// to whatever number is there without doing anything, which takes a third of
	// the time of loading the NAM's RUL0. The two functions below call the game's
	// function with the smallest width that also gives no rows, and make sure that
	// the outcome is the same.

	// Wraps 0x609040: void (ByteVector* grid, int32_t* width, int32_t point[2], uint8_t rotation, char swapDirections)
	template <typename RotateGrid>
	void RotateGridQuickly(RotateGrid&& rotateGrid, ByteVector* grid, int32_t* width, int32_t* point, uint8_t rotation, char swapDirections)
	{
		const int32_t fullWidth = *width;
		const uint32_t size = static_cast<uint32_t>(grid->end - grid->begin);
		if (fullWidth <= 0 || size >= static_cast<uint32_t>(fullWidth)) {
			rotateGrid(grid, width, point, rotation, swapDirections);
			return;
		}
		*width = static_cast<int32_t>(size + 1);
		if ((rotation & 2) != 0) {
			// the point is mirrored in the width: point[0] = width - point[0] - 1
			point[0] = static_cast<int32_t>(static_cast<uint32_t>(point[0]) - (static_cast<uint32_t>(fullWidth) - (size + 1)));
		}
		rotateGrid(grid, width, point, rotation, swapDirections);
		if ((rotation & 1) == 0) {
			*width = fullWidth;  // only a quarter turn sets the width, to the number of rows
		}
	}

	// Wraps 0x608f20: void (ByteVector* grid, uint32_t* width, int32_t point[2], char swapDirections)
	template <typename TransposeGrid>
	void TransposeGridQuickly(TransposeGrid&& transposeGrid, ByteVector* grid, uint32_t* width, int32_t* point, char swapDirections)
	{
		const uint32_t size = static_cast<uint32_t>(grid->end - grid->begin);
		if (static_cast<int32_t>(*width) > 0 && size < *width) {
			*width = size + 1;  // the width becomes the number of rows, which is 0 either way
		}
		transposeGrid(grid, width, point, swapDirections);
	}
}
