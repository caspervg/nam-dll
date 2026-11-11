#pragma once

#include <cstdint>

class cISC4NetworkOccupant;

// Draggable diagonal tunnels.
//
// The native drag pipeline already detects tunnels on diagonal drags:
// IdentifyTunnelCells has no axis test, marks the portal and hidden steps, and
// RemoveTunnelPiecesFromSetOfCellsToBeSolved takes them out of the regular
// solve. Only cSC4NetworkTool::InsertTunnelPieces refuses them, returning on
// the first tunnel whose two portal cells are neither in one row nor one
// column. The result in the stock game is a preview with a tunnel highlight and
// a committed road with a gap where the tunnel should be.
//
// We redirect PlaceNetwork's call to InsertTunnelPieces. Orthogonal tunnels
// still go to the native function. Diagonal tunnels are placed here: one
// portal per end, in the portal-step cell that touches the surface road,
// rotated so its mouth points at the other cell of the zigzag. Diagonal ends use a dedicated portal
// model whose surface road enters from the side perpendicular to the mouth.
// The pair is then linked and stitched with the same machinery as the portal
// tool, with explicit (mouth, surface) path keys.
//
// Spike scope: Road only.
namespace TunnelPortal::DiagonalTunnels
{
	// Exemplar/model/path instances of the diagonal portal pieces, in the Road
	// network piece group 0x2821ED93. Authored at rotation 0 with the mouth to
	// the north; Left has its surface road entering from the west, Right from
	// the east. Shipped in NAM_DiagonalTunnelPortals.dat.
	constexpr uint32_t kRoadPortalLeftExemplar = 0x4A7D1000;
	constexpr uint32_t kRoadPortalRightExemplar = 0x4A7D2000;

	// Redirects PlaceNetwork's InsertTunnelPieces call. Requires the
	// PathStitcher and RouteEdgeFixes hooks to be installed as well.
	void InstallHooks();

	// Recovers the facing of a placed diagonal portal from its piece ID and
	// rotation. Writes piece rotations (0=N 1=E 2=S 3=W) and returns true only
	// for occupants created from the diagonal portal exemplars.
	bool TryGetPortalFacing(
		cISC4NetworkOccupant* occupant,
		uint8_t& mouthDirectionOut,
		uint8_t& surfaceDirectionOut);
}
