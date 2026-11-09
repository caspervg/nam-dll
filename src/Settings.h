#pragma once
#include <filesystem>
#include <cstdint>

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
	bool enableVerticalPathSeparationPatch;
	bool enableDualNetworkTransitPatch;
	bool enableTransportMenuPatch;
	bool enableKeyboardShortcuts;
	uint32_t directDragTile;  // orthogonal straight tile of the override network to drag directly, 0 = off
};
