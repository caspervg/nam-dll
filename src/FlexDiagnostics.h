#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace nSC4Networks { struct cIntRule; }

// Diagnostics for puzzle piece placement: when a puzzle piece is placed (mouse click, not the hover preview),
// writes the network edges of each of its cells before and after the game merges the piece's edges into the
// existing tiles, after the network solve (with the chosen tile IDs), and the occupants afterwards, to
// NAM_FlexPlacement.log next to the DLL. Read-only: it changes nothing about the placement itself.
namespace FlexDiagnostics
{
	// `hidRanges`: comma-separated low-16-bit HID ranges to log, e.g. "0x6100-0x61FF,0x6300-0x63FF" (empty = all)
	void Install(const std::filesystem::path& logFilePath, const std::string& hidRanges);

	// called by the FLEX puzzle piece patch with the rule of the piece about to be placed at handle (x, z)
	void OnRule(uint32_t x, uint32_t z, const nSC4Networks::cIntRule& rule);
}
