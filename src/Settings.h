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
	bool enableVerticalPathSeparationPatch;
	bool enableDualNetworkTransitPatch;
	bool enableTransportMenuPatch;
	bool enableGhostPreviewPatch;
	bool ghostPreviewOverlay;
	bool ghostPreviewGrading;
	bool ghostPreviewWireframe;
	uint8_t ghostPreviewColor[4];  // RGBA
	uint8_t ghostPreviewCutColor[4];
	uint8_t ghostPreviewFillColor[4];
	bool enableKeyboardShortcuts;
};
