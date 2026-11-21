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
	bool enableRUL2FastLoading;
	bool verifyRUL2FastLoading;
	bool enableRUL0FastLoading;
	bool verifyRUL0FastLoading;
	bool enableNetworkSlopePatch;
	bool enableFlexPuzzlePiecePatch;
	bool enableCommuteLoopPatch;
	bool enableDirtRoadAccessPatch;
	bool enableTransitAccessPatch;
	bool enableNeighborConnectionPatch;
	bool enableRHWNeighborConnections;
	bool enableOWRNeighborConnections;
	bool enableNWMNeighborConnections;
	uint32_t neighborConnectionMaxSearchDistance;
	uint32_t neighborConnectionMaxGroupingGap;
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
	bool enableDirtRoadRegionalTransportMapPatch;
	bool enableUndergroundPlaceholderPatch;
	bool enableBridgeShoreAbutmentPatch;
	bool enableBridgeFarAbutmentClearancePatch;
	bool enableParallelBridgesPatch;
	bool enableBridgeDebugLog;
	bool enableKeyboardShortcuts;
};
