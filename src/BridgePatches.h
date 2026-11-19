#pragma once

namespace BridgePatches
{
	// Allows a bridge drag to end on the first tile of the far bank, as it may start on the last tile
	// of the near bank: the far abutment moves onto the shore, so the network continues on that tile.
	void InstallShoreAbutmentPatch();

	// Skips the network-clearance check around the far abutment of a bridge,
	// so that existing networks may lie directly behind it (as on the near side).
	void InstallFarAbutmentClearancePatch();

	// Experimental: removes the lateral one-tile network clearance of bridges,
	// so that bridges can be built directly next to existing bridges and networks.
	void InstallParallelBridgesPatch();

	// Logs how the bridges of each drag are placed, and why a bridge is rejected.
	void InstallDebugLog();
}
