#pragma once
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
	bool enableRUL2FastLoading;
	bool verifyRUL2FastLoading;
	bool enableRUL0FastLoading;
	bool verifyRUL0FastLoading;
	bool enableNetworkSlopePatch;
	bool enableFlexPuzzlePiecePatch;
	bool enableCommuteLoopPatch;
	bool enableDirtRoadAccessPatch;
	bool enableVerticalPathSeparationPatch;
	bool enableDualNetworkTransitPatch;
	bool enableTransportMenuPatch;
	bool enableKeyboardShortcuts;
};
