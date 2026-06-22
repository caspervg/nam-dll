#pragma once
#include <cstdint>
#include <filesystem>

class Settings final
{
public:
	Settings();
	void Load(std::filesystem::path settingsFilePath);

	bool enableDiagonalStreets;
	bool disableAutoconnect;
	bool enableTunnels;
	bool reduceFerryBridgeHeightPatch;
	bool enableRUL2EnginePatch;
	bool enableNetworkSlopePatch;
	bool enableFlexPuzzlePiecePatch;
	bool enableCommuteLoopPatch;
	bool enableDirtRoadAccessPatch;
	bool enableRHWNeighborConnectionPatch;
	uint32_t rhwNeighborConnectionMaxSearchDistance;
	bool enableKeyboardShortcuts;
};
