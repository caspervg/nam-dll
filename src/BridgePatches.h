#pragma once

namespace BridgePatches
{
	// Allows the far abutment of a bridge to be the last tile of the drag.
	void InstallLastTileAbutmentPatch();

	// Skips the network-clearance check around the far abutment of a bridge,
	// so that existing networks may lie directly behind it (as on the near side).
	void InstallFarAbutmentClearancePatch();

	// Experimental: removes the lateral one-tile network clearance of bridges,
	// so that bridges can be built directly next to existing bridges and networks.
	void InstallParallelBridgesPatch();

	// Logs how the bridges of each drag are placed, and why a bridge is rejected.
	void InstallDebugLog();
}
