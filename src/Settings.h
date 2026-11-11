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
	int ghostPreviewColor[3];
	float ghostPreviewAlphaScale;
	float ghostPreviewDepthTolerance;
	float ghostPreviewGradingOpacity;
	int ghostPreviewCutColor[3];
	int ghostPreviewFillColor[3];
	bool enableUndergroundPlaceholderPatch;
	bool enableBridgeLastTileAbutmentPatch;
	bool enableBridgeFarAbutmentClearancePatch;
	bool enableParallelBridgesPatch;
	bool enableKeyboardShortcuts;
};
