#include "DiagonalTunnels.h"

#include "GameAddresses.h"
#include "Logger.h"
#include "NetworkStubs.h"
#include "PathStitcher.h"
#include "PieceInserter.h"
#include "PortalGeometry.h"
#include "RouteEdgeFixes.h"
#include "TerrainPinning.h"
#include "TrafficSimTunnels.h"
#include "Patching.h"
#include "cGZPersistResourceKey.h"
#include "cIGZPersistResourceManager.h"
#include "cIGZUnknown.h"
#include "cIGZVariant.h"
#include "cISCProperty.h"
#include "cISCPropertyHolder.h"
#include "cISCResExemplar.h"
#include "cISC4NetworkOccupant.h"
#include "cISC4Occupant.h"
#include "cISTETerrain.h"
#include "cRZAutoRefCount.h"
#include "GZServPtrs.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace TunnelPortal::DiagonalTunnels
{
	namespace
	{
		namespace Game = TunnelPortal::Game;
		namespace Site = TunnelPortal::Game::Site;
		namespace Geometry = TunnelPortal::Geometry;

		using tTunnel = cSC4NetworkTool::tTunnel;
		using tDraggedStep = cSC4NetworkTool::tDraggedStep;
		using DraggedCell = SC4Point<uint32_t>;

		// Exemplar group of Road network pieces, the one GetOccupantExemplar
		// (Mac 0x0012441c) looks portal exemplars up in for Road.
		constexpr uint32_t kExemplarType = 0x6534284A;
		constexpr uint32_t kRoadPieceExemplarGroup = 0x2821ED93;

		// networkTypeFlags bit the drag sets on tunnel endpoint cells.
		constexpr uint32_t kTunnelCellNetworkFlag = 0x00080000;

		constexpr uint32_t kPropertyResourceKeyType0 = 0x27812820;

		bool IsDiagonalPortalExemplar(uint32_t id)
		{
			return id == kRoadPortalLeftExemplar || id == kRoadPortalRightExemplar;
		}

		// Which diagonal piece an occupant was built from, or 0.
		//
		// cISC4NetworkOccupant::PieceId returns the field at +0xF8 (Mac
		// cSC4NetworkOccupant::PieceId); whether InsertTunnelPiece stores the
		// exemplar ID there is not confirmed. The prebuilt-model occupant does
		// parent its property holder to the exemplar (Mac
		// cSC4NetworkOccupantWithPreBuiltModel::AddModel), so the exemplar's
		// ResourceKeyType0 instance is the dependable fallback: our pieces use
		// the exemplar instance as model instance.
		uint32_t GetDiagonalPortalExemplar(cISC4NetworkOccupant* occupant)
		{
			const uint32_t pieceId = occupant->PieceId();
			if (IsDiagonalPortalExemplar(pieceId))
			{
				return pieceId;
			}

			cISC4Occupant* const baseOccupant = occupant->AsOccupant();
			cISCPropertyHolder* const properties = baseOccupant ? baseOccupant->AsPropertyHolder() : nullptr;
			const cISCProperty* const property = properties ? properties->GetProperty(kPropertyResourceKeyType0) : nullptr;
			const cIGZVariant* const value = property ? property->GetPropertyValue() : nullptr;
			if (value
				&& value->GetType() == cIGZVariant::Type::Uint32Array
				&& value->GetCount() >= 3
				&& IsDiagonalPortalExemplar(value->RefUint32()[2]))
			{
				return value->RefUint32()[2];
			}

			return 0;
		}

		// One end of a drag tunnel, resolved from the drag geometry. Directions
		// are piece rotations (0=N 1=E 2=S 3=W).
		struct DragPortal
		{
			Endpoint endpoint;
			uint8_t mouthDirection = 0;    // toward the tunnel
			uint8_t surfaceDirection = 0;  // toward the surface road
			cSC4NetworkCellInfo* cellInfo = nullptr;
			cISC4NetworkOccupant* occupant = nullptr;
			cRZAutoRefCount<cIGZUnknown> tunnel;

			bool IsStraight() const { return surfaceDirection == ((mouthDirection + 2) & 3); }
			// Road on the mouth's counter-clockwise side, e.g. mouth N and road
			// from the W. The clockwise case is the mirrored Right piece.
			bool IsLeft() const { return surfaceDirection == ((mouthDirection + 3) & 3); }
		};

		// Piece rotation from `from` to an orthogonally adjacent `to`. North is
		// -z (kNextZ for path direction 1 is -1).
		bool TryGetAdjacentDirection(const DraggedCell& from, const DraggedCell& to, uint8_t& directionOut)
		{
			const int64_t dx = static_cast<int64_t>(to.x) - static_cast<int64_t>(from.x);
			const int64_t dz = static_cast<int64_t>(to.y) - static_cast<int64_t>(from.y);
			if (dx == 0 && dz == -1) { directionOut = 0; return true; }
			if (dx == 1 && dz == 0) { directionOut = 1; return true; }
			if (dx == 0 && dz == 1) { directionOut = 2; return true; }
			if (dx == -1 && dz == 0) { directionOut = 3; return true; }
			return false;
		}

		bool IsValidStep(const cSC4NetworkTool* tool, int32_t step)
		{
			return step >= 0
				&& static_cast<size_t>(step) < tool->draggedSteps.size()
				&& tool->draggedSteps[step].firstDraggedCellsIndex <= tool->draggedSteps[step].lastDraggedCellsIndex
				&& tool->draggedSteps[step].lastDraggedCellsIndex < tool->draggedCells.size();
		}

		// Native InsertTunnelPieces' test: the first cells of the two portal
		// steps share a row or a column.
		bool IsOrthogonal(const cSC4NetworkTool* tool, const tTunnel& tunnel)
		{
			if (!IsValidStep(tool, tunnel.startStep) || !IsValidStep(tool, tunnel.endStep))
			{
				return true;  // leave anything unexpected to the native code
			}

			const DraggedCell start = tool->draggedCells[tool->draggedSteps[tunnel.startStep].firstDraggedCellsIndex];
			const DraggedCell end = tool->draggedCells[tool->draggedSteps[tunnel.endStep].firstDraggedCellsIndex];
			return start.x == end.x || start.y == end.y;
		}

		// Finds the portal cell of a portal step and its two sides.
		//
		// The portal cell is the one cell of the step that touches the surface
		// road in the neighbouring outside step. On a 45 degree zigzag each step
		// holds two cells and exactly one of them borders the previous column.
		// The mouth points at the other cell of the step, where the zigzag
		// continues underground; a one-cell step points at the inside step.
		bool ResolveDragPortal(
			const cSC4NetworkTool* tool,
			int32_t portalStep,
			int32_t outsideStep,
			int32_t insideStep,
			cISC4NetworkOccupant::eNetworkType networkType,
			DragPortal& portalOut)
		{
			Logger& logger = Logger::GetInstance();
			if (!IsValidStep(tool, portalStep) || !IsValidStep(tool, outsideStep) || !IsValidStep(tool, insideStep))
			{
				logger.WriteLineFormatted(
					LogLevel::Error,
					"DiagonalTunnels: portal step %d has no valid neighbours (outside %d, inside %d, %u steps).",
					portalStep, outsideStep, insideStep, static_cast<uint32_t>(tool->draggedSteps.size()));
				return false;
			}

			const tDraggedStep portal = tool->draggedSteps[portalStep];
			const tDraggedStep outside = tool->draggedSteps[outsideStep];

			uint32_t matches = 0;
			uint32_t portalCellIndex = 0;
			uint8_t surfaceDirection = 0;
			for (uint32_t c = portal.firstDraggedCellsIndex; c <= portal.lastDraggedCellsIndex; ++c)
			{
				for (uint32_t o = outside.firstDraggedCellsIndex; o <= outside.lastDraggedCellsIndex; ++o)
				{
					uint8_t direction = 0;
					if (TryGetAdjacentDirection(tool->draggedCells[c], tool->draggedCells[o], direction))
					{
						++matches;
						portalCellIndex = c;
						surfaceDirection = direction;
					}
				}
			}

			if (matches != 1)
			{
				logger.WriteLineFormatted(
					LogLevel::Error,
					"DiagonalTunnels: portal step %d (cells %u..%u) touches the surface step %d %u times; exactly one is required.",
					portalStep, portal.firstDraggedCellsIndex, portal.lastDraggedCellsIndex, outsideStep, matches);
				return false;
			}

			const DraggedCell portalCell = tool->draggedCells[portalCellIndex];
			uint32_t mouthMatches = 0;
			uint8_t mouthDirection = 0;
			for (uint32_t c = portal.firstDraggedCellsIndex; c <= portal.lastDraggedCellsIndex; ++c)
			{
				uint8_t direction = 0;
				if (c != portalCellIndex && TryGetAdjacentDirection(portalCell, tool->draggedCells[c], direction))
				{
					++mouthMatches;
					mouthDirection = direction;
				}
			}

			if (mouthMatches == 0)
			{
				const tDraggedStep inside = tool->draggedSteps[insideStep];
				for (uint32_t c = inside.firstDraggedCellsIndex; c <= inside.lastDraggedCellsIndex; ++c)
				{
					uint8_t direction = 0;
					if (TryGetAdjacentDirection(portalCell, tool->draggedCells[c], direction))
					{
						++mouthMatches;
						mouthDirection = direction;
					}
				}
			}

			if (mouthMatches != 1 || mouthDirection == surfaceDirection)
			{
				logger.WriteLineFormatted(
					LogLevel::Error,
					"DiagonalTunnels: portal cell (%u,%u) of step %d has %u tunnel-side neighbours (mouth %u, surface %u); exactly one is required.",
					portalCell.x, portalCell.y, portalStep, mouthMatches,
					static_cast<uint32_t>(mouthDirection), static_cast<uint32_t>(surfaceDirection));
				return false;
			}

			portalOut.endpoint.x = portalCell.x;
			portalOut.endpoint.z = portalCell.y;
			portalOut.endpoint.networkType = networkType;
			portalOut.mouthDirection = mouthDirection;
			portalOut.surfaceDirection = surfaceDirection;
			return true;
		}

		bool IsExemplarInstalled(uint32_t instance)
		{
			cIGZPersistResourceManagerPtr resourceManager;
			if (!resourceManager)
			{
				return false;
			}

			const cGZPersistResourceKey key(kExemplarType, kRoadPieceExemplarGroup, instance);
			cRZAutoRefCount<cISCResExemplar> exemplar;
			return resourceManager->GetResource(key, GZIID_cISCResExemplar, exemplar.AsPPVoid(), 0, nullptr)
				&& exemplar;
		}

		// The diagonal pieces only exist when their DAT is installed. Checked
		// once; a missing DAT degrades to native straight portal models, which
		// still link and route but face the wrong way visually.
		bool AreDiagonalPortalsInstalled()
		{
			static const bool installed = [] {
				const bool left = IsExemplarInstalled(kRoadPortalLeftExemplar);
				const bool right = IsExemplarInstalled(kRoadPortalRightExemplar);
				if (!left || !right)
				{
					Logger::GetInstance().WriteLineFormatted(
						LogLevel::Error,
						"DiagonalTunnels: diagonal portal exemplars missing (left=%d right=%d); is NAM_DiagonalTunnelPortals.dat installed? Falling back to native portal models.",
						left ? 1 : 0, right ? 1 : 0);
				}
				return left && right;
			}();
			return installed;
		}

		// Native InsertTunnelPieces only marks the cell before inserting; the
		// drag already flagged it as a tunnel cell.
		bool PrepareDragPortal(cSC4NetworkTool* tool, DragPortal& portal)
		{
			Logger& logger = Logger::GetInstance();
			portal.cellInfo = tool->GetCellInfo((portal.endpoint.z << 16) | portal.endpoint.x);
			if (!portal.cellInfo)
			{
				logger.WriteLineFormatted(
					LogLevel::Error,
					"DiagonalTunnels: no cell info for portal (%u,%u).",
					portal.endpoint.x, portal.endpoint.z);
				return false;
			}

			logger.WriteLineFormatted(
				LogLevel::Debug,
				"DiagonalTunnels: portal (%u,%u) mouth=%u surface=%u kind=%s cellFlags=0x%08X edges=0x%08X",
				portal.endpoint.x,
				portal.endpoint.z,
				static_cast<uint32_t>(portal.mouthDirection),
				static_cast<uint32_t>(portal.surfaceDirection),
				portal.IsStraight() ? "straight" : (portal.IsLeft() ? "left" : "right"),
				portal.cellInfo->networkTypeFlags,
				portal.cellInfo->edgesPerNetwork[portal.endpoint.networkType]);

			portal.cellInfo->networkTypeFlags |= kTunnelCellNetworkFlag;
			portal.cellInfo->isTunnelEndpoint = true;
			return true;
		}

		// Inserts one portal piece in the drag's own placement context.
		//
		// For a diagonal end the diagonal exemplar is swapped in for the call,
		// and the native terrain lift is suppressed: InsertTunnelPiece raises
		// both mouth-side corners by tunnelModelHeights, and on a diagonal
		// portal one of those corners sits right beside the approach road and
		// would bury it. The portal cell then stays at the drag's tunnel start
		// height and the model carries the mouth structure itself.
		bool InsertDragPortal(cSC4NetworkTool* tool, DragPortal& portal)
		{
			if (portal.IsStraight() || !AreDiagonalPortalsInstalled())
			{
				portal.occupant = tool->InsertTunnelPiece(portal.mouthDirection, 0, portal.cellInfo);
				return portal.occupant != nullptr;
			}

			if (tool->tunnelExemplarIds.empty() || tool->tunnelRotations.empty() || tool->tunnelModelHeights.empty())
			{
				Logger::GetInstance().WriteLine(LogLevel::Error, "DiagonalTunnels: network tool has no tunnel pieces.");
				return false;
			}

			uint32_t& exemplarId = tool->tunnelExemplarIds.begin()[0];
			float& modelHeight = tool->tunnelModelHeights.begin()[0];
			const uint32_t nativeExemplarId = exemplarId;
			const float nativeModelHeight = modelHeight;

			// The diagonal models are authored for rotation == mouth direction,
			// so cancel the network's own rotation adjustment (0 for Road).
			const uint8_t direction = static_cast<uint8_t>((portal.mouthDirection - tool->tunnelRotations[0]) & 3);
			exemplarId = portal.IsLeft() ? kRoadPortalLeftExemplar : kRoadPortalRightExemplar;
			modelHeight = 0.0f;
			portal.occupant = tool->InsertTunnelPiece(direction, 0, portal.cellInfo);
			modelHeight = nativeModelHeight;
			exemplarId = nativeExemplarId;

			if (portal.occupant)
			{
				Logger::GetInstance().WriteLineFormatted(
					LogLevel::Debug,
					"DiagonalTunnels: inserted portal (%u,%u) pieceId=0x%08X diagonalPiece=0x%08X rotation=%u.",
					portal.endpoint.x,
					portal.endpoint.z,
					portal.occupant->PieceId(),
					GetDiagonalPortalExemplar(portal.occupant),
					static_cast<uint32_t>(portal.occupant->GetRotation()));
			}
			return portal.occupant != nullptr;
		}

		// PathStitcher resolver: every native MakeTunnelPaths call for a pair
		// that involves a diagonal portal gets the same overrides
		// LinkDragPortals uses, so re-stitching by the game (city load,
		// InitializeTunnelPaths on network changes) keeps both lanes.
		bool ResolveDiagonalPathStitch(
			cIGZUnknown* otherEnd,
			cIGZUnknown*& selfOut,
			uint8_t& selfLookupPathDirection,
			uint16_t& peerPathKeyLowWord,
			uint8_t& peerMouthPathDirection)
		{
			cRZAutoRefCount<cISC4NetworkOccupant> other;
			if (!otherEnd->QueryInterface(GZIID_cISC4NetworkOccupant, other.AsPPVoid()) || !other)
			{
				return false;
			}

			cRZAutoRefCount<cIGZUnknown> otherTunnel;
			if (!PieceInserter::QueryTunnelOccupant(other, otherTunnel))
			{
				return false;
			}

			cISC4NetworkOccupant* const self = Game::GetOtherEndOccupant(otherTunnel);
			if (!self)
			{
				return false;
			}

			uint8_t selfMouth = 0;
			uint8_t selfSurface = 0;
			uint8_t otherMouth = 0;
			uint8_t otherSurface = 0;
			const bool selfIsDiagonal = TryGetPortalFacing(self, selfMouth, selfSurface);
			const bool otherIsDiagonal = TryGetPortalFacing(other, otherMouth, otherSurface);
			if (!selfIsDiagonal && !otherIsDiagonal)
			{
				return false;
			}

			// A drag tunnel can pair a diagonal end with a straight Road end;
			// with Road's zero rotation adjustment its rotation is its mouth.
			if (!selfIsDiagonal)
			{
				selfMouth = static_cast<uint8_t>(self->GetRotation() & 3);
				selfSurface = static_cast<uint8_t>((selfMouth + 2) & 3);
			}
			if (!otherIsDiagonal)
			{
				otherMouth = static_cast<uint8_t>(other->GetRotation() & 3);
				otherSurface = static_cast<uint8_t>((otherMouth + 2) & 3);
			}

			selfOut = self;
			selfLookupPathDirection = Geometry::TunnelPieceDirectionToPathDirection(selfMouth);
			peerMouthPathDirection = Geometry::TunnelPieceDirectionToPathDirection(otherMouth);
			peerPathKeyLowWord = Geometry::PortalExitPathKeyLowWord(
				peerMouthPathDirection,
				Geometry::TunnelPieceDirectionToPathDirection(otherSurface));
			return true;
		}

		// Links and stitches a placed pair. Mirrors PortalPlacer for one-tile
		// networks, but always passes both sides of each portal so the stitch
		// asks the peer for the path that leaves its mouth toward its surface
		// road, wherever that road enters.
		void LinkDragPortals(DragPortal& first, DragPortal& second)
		{
			Logger& logger = Logger::GetInstance();
			if (!PieceInserter::QueryTunnelOccupant(first.occupant, first.tunnel)
				|| !PieceInserter::QueryTunnelOccupant(second.occupant, second.tunnel))
			{
				logger.WriteLine(LogLevel::Error, "DiagonalTunnels: placed portals are not tunnel occupants.");
				return;
			}

			Game::SetOtherEndOccupant(first.tunnel, second.occupant);
			Game::SetOtherEndOccupant(second.tunnel, first.occupant);

			const uint8_t firstMouth = Geometry::TunnelPieceDirectionToPathDirection(first.mouthDirection);
			const uint8_t firstSurface = Geometry::TunnelPieceDirectionToPathDirection(first.surfaceDirection);
			const uint8_t secondMouth = Geometry::TunnelPieceDirectionToPathDirection(second.mouthDirection);
			const uint8_t secondSurface = Geometry::TunnelPieceDirectionToPathDirection(second.surfaceDirection);

			const bool nativeCompatible =
				first.IsStraight() && second.IsStraight()
				&& first.mouthDirection == Geometry::InferTunnelPieceDirection(first.endpoint, second.endpoint)
				&& second.mouthDirection == Geometry::InferTunnelPieceDirection(second.endpoint, first.endpoint);

			bool stitched;
			if (nativeCompatible)
			{
				stitched = PathStitcher::RefreshTunnelPathInfo(first.tunnel, second.tunnel);
				stitched = PathStitcher::RefreshTunnelPathInfo(second.tunnel, first.tunnel) && stitched;
			}
			else
			{
				RouteEdgeFixes::Register(first.endpoint, second.endpoint, firstMouth, secondMouth);
				stitched = PathStitcher::RefreshTunnelPathInfo(
					first.tunnel,
					second.tunnel,
					firstMouth,
					Geometry::PortalExitPathKeyLowWord(secondMouth, secondSurface),
					secondMouth);
				stitched = PathStitcher::RefreshTunnelPathInfo(
					second.tunnel,
					first.tunnel,
					secondMouth,
					Geometry::PortalExitPathKeyLowWord(firstMouth, firstSurface),
					firstMouth) && stitched;
			}

			if (!stitched)
			{
				logger.WriteLineFormatted(
					LogLevel::Error,
					"DiagonalTunnels: tunnel (%u,%u)-(%u,%u) failed to stitch a path map; it will not route.",
					first.endpoint.x, first.endpoint.z, second.endpoint.x, second.endpoint.z);
			}

			PieceInserter::MarkOccupantUsable(first.occupant, "first");
			PieceInserter::MarkOccupantUsable(second.occupant, "second");
			TrafficSimTunnels::NotifyLinkedTunnels(first.endpoint, second.endpoint, first.tunnel, second.tunnel);
		}

		void PlaceDiagonalTunnel(
			cSC4NetworkTool* tool,
			const tTunnel& tunnel,
			cISC4NetworkOccupant::eNetworkType networkType)
		{
			Logger& logger = Logger::GetInstance();

			DragPortal first;
			DragPortal second;
			if (!ResolveDragPortal(tool, tunnel.startStep, tunnel.startStep - 1, tunnel.startStep + 1, networkType, first)
				|| !ResolveDragPortal(tool, tunnel.endStep, tunnel.endStep + 1, tunnel.endStep - 1, networkType, second))
			{
				return;
			}

			// Prepare both cells before inserting either, so a missing cell
			// cannot leave one unpaired portal behind.
			if (!PrepareDragPortal(tool, first) || !PrepareDragPortal(tool, second))
			{
				return;
			}

			if (!InsertDragPortal(tool, first) || !InsertDragPortal(tool, second))
			{
				logger.WriteLineFormatted(
					LogLevel::Error,
					"DiagonalTunnels: failed to insert both portals of tunnel (%u,%u)-(%u,%u).",
					first.endpoint.x, first.endpoint.z, second.endpoint.x, second.endpoint.z);
				return;
			}

			LinkDragPortals(first, second);

			logger.WriteLineFormatted(
				LogLevel::Info,
				"DiagonalTunnels: placed %s tunnel (%u,%u) mouth %u -> (%u,%u) mouth %u, steps %d..%d.",
				Geometry::NetworkTypeName(networkType),
				first.endpoint.x, first.endpoint.z, static_cast<uint32_t>(first.mouthDirection),
				second.endpoint.x, second.endpoint.z, static_cast<uint32_t>(second.mouthDirection),
				tunnel.startStep, tunnel.endStep);
		}

		// --- Tunnel detection over the whole drag --------------------------------
		//
		// Port of cSC4NetworkTool::IdentifyTunnelCells (Win 0x00639490, Mac
		// 0x004cc4ee) and FindTunnelEnd (Win 0x00638770, Mac 0x004cba96) with two
		// changes: steps past numStraightSteps are considered, using the
		// diagonal part's cross-section, and slope tests use each step's real
		// horizontal length (cellSize, or cellSize * sqrt(2) on the diagonal)
		// instead of cellSize. For a straight drag the result is identical to
		// the native one.

		constexpr float kSqrt2 = 1.41421356f;
		constexpr float kDegreesPerRadian = 57.2957795f;

		bool IsDiagonalStep(const cSC4NetworkTool* tool, int32_t step)
		{
			return step >= tool->numStraightSteps;
		}

		int32_t StepDirection(const cSC4NetworkTool* tool, int32_t step)
		{
			return (IsDiagonalStep(tool, step) ? tool->diagonalDragDirection : tool->straightDragDirection) & 7;
		}

		float StepLength(const cSC4NetworkTool* tool, int32_t step)
		{
			return IsDiagonalStep(tool, step) ? tool->cellSize * kSqrt2 : tool->cellSize;
		}

		// Average terrain altitude of the cross-section vertices of a step's
		// first and last cell, as both native functions compute it.
		float StepHeight(const cSC4NetworkTool* tool, int32_t step, int32_t direction)
		{
			const tDraggedStep draggedStep = tool->draggedSteps[step];
			const DraggedCell first = tool->draggedCells[draggedStep.firstDraggedCellsIndex];
			const DraggedCell last = tool->draggedCells[draggedStep.lastDraggedCellsIndex];
			const int32_t firstVertex = static_cast<int32_t>(first.y * tool->numVerticesX + first.x);
			const int32_t lastVertex = static_cast<int32_t>(last.y * tool->numVerticesX + last.x);
			const int32_t offset1 = tool->crossSectionVertexOffset1[direction];
			const int32_t offset2 = tool->crossSectionVertexOffset2[direction];
			cISTETerrain* const terrain = tool->terrain;
			return 0.25f * (
				terrain->GetAltitude(firstVertex + offset1)
				+ terrain->GetAltitude(firstVertex + offset2)
				+ terrain->GetAltitude(lastVertex + offset1)
				+ terrain->GetAltitude(lastVertex + offset2));
		}

		bool StepHasWater(const cSC4NetworkTool* tool, int32_t step)
		{
			const tDraggedStep draggedStep = tool->draggedSteps[step];
			for (uint32_t c = draggedStep.firstDraggedCellsIndex; c <= draggedStep.lastDraggedCellsIndex; ++c)
			{
				const DraggedCell cell = tool->draggedCells[c];
				if (tool->terrain->CellHasWater(cell.x, cell.y))
				{
					return true;
				}
			}
			return false;
		}

		// Walks forward from tunnel.startStep while the terrain stays more than
		// one portal model height above the start, and picks as end the step on
		// the way down closest in height to the start. Accepts it when the height
		// difference fits MaxTunnelSlope over the tunnel's length.
		bool FindTunnelEnd(const cSC4NetworkTool* tool, tTunnel& tunnel)
		{
			const float modelHeight = tool->tunnelModelHeights.empty() ? 10.0f : tool->tunnelModelHeights[0];
			tunnel.endStep = -1;
			float bestDifference = 3.4028235e+38f;
			float previousHeight = tunnel.startHeight;
			float length = 0.0f;
			float endLength = 0.0f;

			for (int32_t step = tunnel.startStep + 1; step < tool->numPlaceableSteps - 1; ++step)
			{
				if (tool->draggedSteps[step].stepType != 0 || StepHasWater(tool, step))
				{
					break;
				}

				// Native samples the opposite cross-section here.
				const float height = StepHeight(tool, step, (StepDirection(tool, step) + 4) & 7);
				length += StepLength(tool, step);
				const float difference = std::abs(height - tunnel.startHeight);
				if (height < previousHeight && difference < bestDifference)
				{
					tunnel.endStep = step;
					tunnel.endHeight = height;
					bestDifference = difference;
					endLength = length;
				}

				if (height <= tunnel.startHeight + modelHeight)
				{
					break;
				}
				previousHeight = height;
			}

			if (tunnel.endStep < 0)
			{
				return false;
			}

			return bestDifference <= endLength * std::tan(tool->maxTunnelSlope)
				&& tunnel.endStep - tunnel.startStep > 2;
		}

		void IdentifyTunnelCellsOverWholeDrag(cSC4NetworkTool* tool, int32_t numSteps)
		{
			Logger& logger = Logger::GetInstance();
			const int32_t stepCount = std::min<int32_t>(numSteps, static_cast<int32_t>(tool->draggedSteps.size()));
			if (stepCount <= 3 || !tool->terrain)
			{
				return;
			}

			int32_t firstStep = 0;
			while (firstStep < stepCount && tool->draggedSteps[firstStep].stepType != 0)
			{
				++firstStep;
			}
			if (firstStep >= stepCount)
			{
				return;
			}

			const bool trace = tool->isPlacing;
			if (trace)
			{
				logger.WriteLineFormatted(
					LogLevel::Debug,
					"DiagonalTunnels: detecting tunnels over %d steps (straight %d dir %d, diagonal dir %d), start slope %.1f deg, max tunnel slope %.1f deg, model height %.1f.",
					stepCount,
					tool->numStraightSteps,
					tool->straightDragDirection,
					tool->diagonalDragDirection,
					tool->minTerrainSlopeForTunnelStart * kDegreesPerRadian,
					tool->maxTunnelSlope * kDegreesPerRadian,
					tool->tunnelModelHeights.empty() ? 0.0f : tool->tunnelModelHeights[0]);
			}

			float previousHeight = StepHeight(tool, firstStep, StepDirection(tool, firstStep));
			for (int32_t step = firstStep + 1; step < stepCount; ++step)
			{
				if (tool->draggedSteps[step].stepType != 0
					|| tool->draggedSteps[step - 1].stepType != 0
					|| (step >= 2 && tool->draggedSteps[step - 2].stepType != 0))
				{
					continue;
				}

				const float height = StepHeight(tool, step, StepDirection(tool, step));
				const float rise = height - previousHeight;
				const float minRise = StepLength(tool, step) * std::tan(tool->minTerrainSlopeForTunnelStart);
				if (rise > minRise && step > 1)
				{
					tTunnel tunnel{ previousHeight, 0.0f, step - 1, -1 };
					const bool found = FindTunnelEnd(tool, tunnel);
					const bool longEnough = found && tunnel.endStep - tunnel.startStep > 3;
					if (trace)
					{
						logger.WriteLineFormatted(
							LogLevel::Debug,
							"DiagonalTunnels:   step %d rises %.1f (> %.1f) from %.1f: end %d at %.1f -> %s.",
							step, rise, minRise, previousHeight, tunnel.endStep, tunnel.endHeight,
							longEnough ? "tunnel" : (found ? "too short" : "no valid end"));
					}

					if (longEnough)
					{
						tool->tunnels.push_back(tunnel);
						tDraggedStep* const steps = tool->draggedSteps.begin();
						steps[tunnel.startStep].stepType = 2;
						steps[tunnel.endStep].stepType = 2;
						for (int32_t hidden = tunnel.startStep + 1; hidden < tunnel.endStep; ++hidden)
						{
							steps[hidden].stepType = 3;
						}
					}
				}
				previousHeight = height;
			}
		}

		// Replaces PlaceNetwork's call to IdentifyTunnelCells (thiscall, one
		// stack argument).
		void __fastcall Hook_IdentifyTunnelCells(cSC4NetworkTool* tool, void*, int32_t& numSteps)
		{
			if (tool->networkType == cISC4NetworkOccupant::Road
				&& tool->numStraightSteps < tool->numDraggedSteps)
			{
				IdentifyTunnelCellsOverWholeDrag(tool, numSteps);
			}
			else
			{
				tool->IdentifyTunnelCells(numSteps);
			}
		}

		// Replaces PlaceNetwork's call to InsertTunnelPieces (thiscall, one
		// stack argument).
		void __fastcall Hook_InsertTunnelPieces(
			cSC4NetworkTool* tool,
			void*,
			cISC4NetworkOccupant::eNetworkType networkType)
		{
			// Previews highlight tunnel cells before the native axis test, so
			// they already work for diagonals. Only Road is handled for now.
			if (!tool->isPlacing
				|| networkType != cISC4NetworkOccupant::Road
				|| std::all_of(
					tool->tunnels.begin(),
					tool->tunnels.end(),
					[tool](const tTunnel& tunnel) { return IsOrthogonal(tool, tunnel); }))
			{
				tool->InsertTunnelPieces(networkType);
				return;
			}

			// Native InsertTunnelPieces returns at the first diagonal tunnel. Move
			// the orthogonal tunnels to the front so it handles all of them, place
			// the diagonal ones here, then restore the drag's own order. Tunnels
			// are independent, so the processing order does not matter.
			const std::vector<tTunnel> dragOrder(tool->tunnels.begin(), tool->tunnels.end());
			tTunnel* const firstDiagonal = std::stable_partition(
				tool->tunnels.begin(),
				tool->tunnels.end(),
				[tool](const tTunnel& tunnel) { return IsOrthogonal(tool, tunnel); });

			tool->InsertTunnelPieces(networkType);

			// Like the native loop, skip tunnels past the steps that survived
			// the earlier placement stages.
			for (tTunnel* tunnel = firstDiagonal; tunnel != tool->tunnels.end(); ++tunnel)
			{
				if (tunnel->endStep < tool->numPlaceableSteps)
				{
					PlaceDiagonalTunnel(tool, *tunnel, networkType);
				}
			}

			std::copy(dragOrder.begin(), dragOrder.end(), tool->tunnels.begin());

			// The new portals are registered with the traffic simulator by now.
			TerrainPinning::MarkCommittedPortalsImmovable();
		}
	}

	void InstallHooks()
	{
		PathStitcher::SetAutomaticOverrideResolver(&ResolveDiagonalPathStitch);

		const uint32_t identifyTunnelCellsHookRel =
			reinterpret_cast<uint32_t>(&Hook_IdentifyTunnelCells)
				- (Site::kPlaceNetworkIdentifyTunnelCellsCall + 5);
		Patching::PatchImmediate32(
			Site::kPlaceNetworkIdentifyTunnelCellsCall + 1,
			Site::kPlaceNetworkIdentifyTunnelCellsCallRel,
			identifyTunnelCellsHookRel);

		const uint32_t insertTunnelPiecesHookRel =
			reinterpret_cast<uint32_t>(&Hook_InsertTunnelPieces)
				- (Site::kPlaceNetworkInsertTunnelPiecesCall + 5);
		Patching::PatchImmediate32(
			Site::kPlaceNetworkInsertTunnelPiecesCall + 1,
			Site::kPlaceNetworkInsertTunnelPiecesCallRel,
			insertTunnelPiecesHookRel);
	}

	bool TryGetPortalFacing(
		cISC4NetworkOccupant* occupant,
		uint8_t& mouthDirectionOut,
		uint8_t& surfaceDirectionOut)
	{
		if (!occupant)
		{
			return false;
		}

		const uint32_t exemplarId = GetDiagonalPortalExemplar(occupant);
		if (exemplarId == 0)
		{
			return false;
		}

		// InsertDragPortal cancels the rotation adjustment, so the stored
		// rotation is the mouth direction.
		const uint8_t mouth = static_cast<uint8_t>(occupant->GetRotation() & 3);
		mouthDirectionOut = mouth;
		surfaceDirectionOut = static_cast<uint8_t>(
			(mouth + (exemplarId == kRoadPortalLeftExemplar ? 3 : 1)) & 3);
		return true;
	}
}
