#pragma once
// Access to the file behind a cRZIniFile of the game.
#include "cIGZFile.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

namespace RZIniFile
{
	constexpr ptrdiff_t FileOffset = 0x34;  // offset of the cIGZFile*

	// Reads the whole file of a cRZIniFile, with a NUL byte added after the data. The file is a record of a DBPF
	// file, opened by the game before it creates the cRZIniFile. Returns null if the file cannot be read.
	inline std::unique_ptr<char[]> ReadAll(void* iniFile, uint32_t& size)
	{
		cIGZFile* const file = *reinterpret_cast<cIGZFile**>(static_cast<uint8_t*>(iniFile) + FileOffset);
		if (file == nullptr || !file->IsOpen()) {
			return nullptr;
		}
		size = file->Length();
		std::unique_ptr<char[]> data(new (std::nothrow) char[size_t{size} + 1]);
		if (data == nullptr) {
			return nullptr;
		}
		file->SeekToBegin();
		constexpr uint32_t blockSize = 4 * 1024 * 1024;
		for (uint32_t offset = 0; offset < size; offset += blockSize) {
			if (!file->Read(data.get() + offset, (std::min)(blockSize, size - offset))) {
				return nullptr;
			}
		}
		data[size] = '\0';
		return data;
	}
}
