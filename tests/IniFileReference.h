#pragma once
// A port of the parts of cRZIniFile that the game reads RUL files with, kept as
// close to the disassembly as possible (and therefore slow), plus a reader for
// the RUL files of DBPF packages. Shared by the tests of the fast loaders.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

namespace IniFileReference
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
	inline bool ReadLine(File& file, std::string& line)
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

	inline void TrimLeft(std::string& s) { s.erase(0, s.find_first_not_of(" \t")); }
	inline void TrimRight(std::string& s) { s.erase(s.find_last_not_of(" \t") + 1); }

	// Returns the text of a RUL file: the file itself, or the entry with this instance ID inside a DBPF package.
	inline bool ReadRulFile(const char* path, uint32_t instance, std::string& text)
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
			if (u32(entry) == 0x0a5bcf4b && u32(entry + 4) == 0xaa5bcf57 && u32(entry + 8) == instance) {
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
}
