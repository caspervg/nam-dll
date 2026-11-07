#include "NeighborConnections.h"
#include "Patching.h"
#include "NetworkStubs.h"
#include "Masking.h"
#include "cISC4TrafficSimulator.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

/*
	Neighbor connection patch
	=========================

	SimCity 4 has special return-tile handling for divided networks such as
	Avenue and Highway. When a path crosses a city edge, the game pairs the
	visible NC connection with a nearby tile carrying traffic in the
	opposite direction:

	    city edge
	    v
	    S S S . . T T T
	    ^ source  ^ target return carriageway

	This patch reuses that divided-network path for more networks:

	- RHW/DirtRoad: adds the DirtRoad neighbor connection bit to the game's
	  divided-network mask.

	- OWR: maps OWR to a new neighbor connection type 13, which is persisted so
	  neighboring cities create OWR stubs. Traffic code sees it as the DirtRoad/RHW
	  type 10 (via GetType). The per-tile mask carries both bits, and the OWR bit
	  selects OWR pairing, independent of the RHW setting. Without the DLL, a
	  saved type 13 connection carries no traffic but does not break the game.

	- NWM: adds Road to the divided-network mask, but only uses the custom
	  return search when the Road edge tile is clearly one-way (i.e. half of a AVE-6).
	  Normal Road neighbor connections are bidirectional, so they are left alone.

	Return-tile matching
	--------------------

	The hook receives the tile being checked at the city edge. For RHW/OWR it
	first asks the original game function for the adjacent tile direction. For NWM
	it scans along the city edge directly. NWM pieces are Road overrides, so the
	Road neighbor connection alone does not tell us which side of the source tile
	contains the matching carriageway. The search therefore tries both directions
	along the edge and accepts the first valid opposite-side match.

	The scan reads a short strip of edge tiles:

	    -8 ... -2 -1  0  1  2 ... 8
	              [ source ] [ target ]

	Tiles with the same traffic direction are grouped into bands. Empty tiles
	inside a group are allowed up to NeighborConnectionMaxGroupingGap, which
	lets separated carriageways and multi-tile NWM setups stay together.

	The source band and lane are then mirrored onto the target side:

	    source bands              target bands
	    far -> near               near -> far
	    [A][B][C]       maps to   [C][B][A]

	Within the selected band, the same mirrored position is used. This keeps a
	tile in a three-lane outgoing side paired with the corresponding tile in a
	three-lane incoming side, while still handling unequal band widths by
	scaling the index.
*/

namespace
{
	// cSC4TrafficSimulator::PerformPathSearch checks this mask before using the
	// adjacent tile as the return carriageway for a divided network connection.
	constexpr uint32_t kForwardPathNetworkMaskTestAddress = 0x0071c8c1;
	constexpr uint32_t kReturnPathNetworkMaskTestAddress = 0x0071c93d;
	constexpr uint32_t kForwardPathFindReturnTileCallAddress = 0x0071c8d6;
	constexpr uint32_t kReturnPathFindReturnTileCallAddress = 0x0071c952;
	constexpr uint32_t kOWRNetworkToNcAddress = 0x00aa84ec;

	// Neighbor connection types, as mapped from network types by the table at 0x00aa84c4.
	enum class NcType : uint32_t
	{
		None = 0,
		Road = 2,
		Highway = 3,
		Avenue = 8,
		DirtRoad = 10,
		GroundHighway = 12,
		OneWayRoad = 13,  // new, added by this patch
	};

	constexpr uint16_t kVanillaDividedNetworkNcMask = 0x1108;
	static_assert(kVanillaDividedNetworkNcMask == asMask(NcType::Highway, NcType::Avenue, NcType::GroundHighway));
	constexpr uint16_t kRoadNcMask = asMask(NcType::Road);
	constexpr uint16_t kDirtRoadNcMask = asMask(NcType::DirtRoad);
	constexpr uint16_t kOneWayRoadNcMask = asMask(NcType::OneWayRoad);

	constexpr uint32_t kMinSearchDistance = 1;
	constexpr uint32_t kMaxSearchDistance = 64;
	constexpr uint32_t kMaxGroupingGap = 16;
	constexpr size_t kScanCapacity = kMaxSearchDistance * 2 + 1;

	uint32_t gMaxSearchDistance = kMinSearchDistance;
	uint32_t gMaxGroupingGap = 0;
	bool gEnableNWM = false;

	// Traffic direction seen from the city edge for one path type.
	enum class PathDir : uint8_t
	{
		None,
		Inbound,
		Outbound,
		Bidi // short for Bidirectional
	};

	// One scanned tile on the city edge, including its NC mask and path direction.
	struct EdgeTile
	{
		int32_t x = 0;
		int32_t z = 0;
		int32_t offset = 0;
		uint16_t ncMask = 0;
		uint16_t pathMatrix = 0;
		PathDir direction = PathDir::None;
		bool valid = false;
		bool isEligibleNc = false;
	};

	using EdgeScan = std::array<EdgeTile, kScanCapacity>;

	const EdgeTile& GetScannedTile(const EdgeScan& scannedTiles, const int32_t offset)
	{
		return scannedTiles[static_cast<size_t>(offset + static_cast<int32_t>(kMaxSearchDistance))];
	}

	// A contiguous run of scanned tiles that belong to the same carriageway group.
	struct Band
	{
		size_t firstTile = 0;
		size_t tileCount = 0;
	};

	// The grouped source or target side used when mirroring a return tile.
	struct BandCollection
	{
		std::array<EdgeTile, kScanCapacity> tiles{};
		std::array<Band, kScanCapacity> bands{};
		size_t tileCount = 0;
		size_t bandCount = 0;

		void Add(const EdgeTile& tile)
		{
			if (tileCount >= tiles.size())
			{
				return;
			}

			const bool startsNewBand =
				tileCount == 0 || tiles[tileCount - 1].offset + 1 != tile.offset;
			if (startsNewBand)
			{
				if (bandCount >= bands.size())
				{
					return;
				}
				bands[bandCount++] = Band{tileCount, 0};
			}

			tiles[tileCount++] = tile;
			++bands[bandCount - 1].tileCount;
		}
	};

	uint16_t GetNeighborConnectionMask(cSC4TrafficSimulator* const pTrafficSim, const int32_t x, const int32_t z)
	{
		const void* const pConnection = cSC4TrafficSimulator::GetNeighborConnectionEntry(pTrafficSim, x, z);
		return pConnection ? *static_cast<const uint16_t*>(pConnection) : 0;
	}

	// Converts the outside neighbor coordinate to the city-edge cell and edge index.
	bool TryGetBoundaryCellAndEdge(cSC4TrafficSimulator* const pTrafficSim, const int32_t x, const int32_t z, int32_t& cellX, int32_t& cellZ, uint32_t& edge)
	{
		const int32_t maxX = pTrafficSim->maxCellX;
		const int32_t maxZ = pTrafficSim->maxCellZ;
		const bool xOutside = x < 0 || x > maxX;
		const bool zOutside = z < 0 || z > maxZ;
		if (xOutside == zOutside)
		{
			return false;
		}

		cellX = x;
		cellZ = z;
		if (x < 0)
		{
			cellX = 0;
			edge = 0;
		}
		else if (x > maxX)
		{
			cellX = maxX;
			edge = 2;
		}
		else if (z < 0)
		{
			cellZ = 0;
			edge = 1;
		}
		else
		{
			cellZ = maxZ;
			edge = 3;
		}
		return true;
	}

	PathDir GetPathDirection(cSC4TrafficSimulator* const pTrafficSim, const int32_t x, const int32_t z, uint16_t& pathMatrix)
	{
		int32_t cellX = 0;
		int32_t cellZ = 0;
		uint32_t boundaryEdge = 0;
		if (!TryGetBoundaryCellAndEdge(pTrafficSim, x, z, cellX, cellZ, boundaryEdge))
		{
			return PathDir::None;
		}

		const TrafficSimCellConnections* const cells = pTrafficSim->cellConnections;
		if (!cells)
		{
			return PathDir::None;
		}

		const uint32_t cellIndex = (static_cast<uint32_t>(cellX) << 8) | static_cast<uint32_t>(cellZ);
		pathMatrix = cells[cellIndex].entryExitConnectionsPerTravelType[static_cast<uint32_t>(cISC4TrafficSimulator::TravelType::Car)];

		bool inbound = false;
		bool outbound = false;
		for (uint32_t otherEdge = 0; otherEdge < 4; ++otherEdge)
		{
			// DoConnectionsChanged stores bit (exitEdge + entryEdge * 4).
			outbound |= (pathMatrix & (1u << (boundaryEdge + otherEdge * 4))) != 0;
			inbound |= (pathMatrix & (1u << (otherEdge + boundaryEdge * 4))) != 0;
		}

		if (inbound && outbound)
		{
			return PathDir::Bidi;
		}
		if (inbound)
		{
			return PathDir::Inbound;
		}
		if (outbound)
		{
			return PathDir::Outbound;
		}
		return PathDir::None;
	}

	PathDir GetOppositeDirection(const PathDir dir)
	{
		return dir == PathDir::Inbound ? PathDir::Outbound : PathDir::Inbound;
	}

	bool IsOneWayDirection(const PathDir dir)
	{
		return dir == PathDir::Inbound || dir == PathDir::Outbound;
	}

	bool IsTargetDirection(const PathDir dir, const PathDir targetDir, const bool allowBidiFallback)
	{
		return dir == targetDir || (allowBidiFallback && dir == PathDir::Bidi);
	}

	bool IsSourceDirection(
		const PathDir dir,
		const PathDir sourceDir,
		const bool allowBidiSource,
		const bool bidiMustBeContiguous,
		const uint32_t gap)
	{
		if (dir == sourceDir)
		{
			return true;
		}
		return allowBidiSource && dir == PathDir::Bidi && (!bidiMustBeContiguous || gap == 0);
	}

	bool TryFindSourceDirectionNearOrigin(
		const EdgeScan& scannedTiles,
		const PathDir originDir,
		const bool allowBidiSource,
		PathDir& sourceDir)
	{
		if (IsOneWayDirection(originDir))
		{
			sourceDir = originDir;
			return true;
		}
		if (!allowBidiSource || originDir != PathDir::Bidi)
		{
			return false;
		}

		uint32_t gap = 0;
		for (int32_t offset = -1; offset >= -static_cast<int32_t>(gMaxSearchDistance); --offset)
		{
			const EdgeTile& tile = GetScannedTile(scannedTiles, offset);
			if (!tile.valid)
			{
				break;
			}
			if (!tile.isEligibleNc)
			{
				if (++gap > gMaxGroupingGap)
				{
					break;
				}
				continue;
			}
			if (IsOneWayDirection(tile.direction))
			{
				sourceDir = tile.direction;
				return true;
			}
			if (tile.direction != PathDir::Bidi)
			{
				break;
			}
			gap = 0;
		}

		return false;
	}

	EdgeTile ReadEdgeTile(cSC4TrafficSimulator* const pTrafficSim, const int32_t originX, const int32_t originZ,
	                      const int32_t stepX, const int32_t stepZ, const uint16_t eligibleNcMask, const int32_t offset)
	{
		EdgeTile tile;
		tile.x = originX + stepX * offset;
		tile.z = originZ + stepZ * offset;
		tile.offset = offset;

		int32_t cellX = 0;
		int32_t cellZ = 0;
		uint32_t edge = 0;
		tile.valid = TryGetBoundaryCellAndEdge(pTrafficSim, tile.x, tile.z, cellX, cellZ, edge);
		if (!tile.valid)
		{
			return tile;
		}

		tile.ncMask = GetNeighborConnectionMask(pTrafficSim, tile.x, tile.z);
		tile.isEligibleNc = (tile.ncMask & eligibleNcMask) != 0;
		tile.direction = GetPathDirection(pTrafficSim, tile.x, tile.z, tile.pathMatrix);
		return tile;
	}

	// Finds how far the source carriageway continues from the origin, allowing small gaps.
	int32_t FindSourceExtent(
		const EdgeScan& scannedTiles,
		const PathDir sourceDir,
		const bool allowBidiSource,
		const int32_t inc)
	{
		int32_t extent = 0;
		uint32_t gap = 0;
		for (int32_t distance = 1;
			distance <= static_cast<int32_t>(gMaxSearchDistance);
			++distance)
		{
			const int32_t offset = distance * inc;
			const EdgeTile& tile = GetScannedTile(scannedTiles, offset);
			if (!tile.valid)
			{
				break;
			}

			if (!tile.isEligibleNc)
			{
				if (++gap > gMaxGroupingGap)
				{
					break;
				}
				continue;
			}

			if (!IsSourceDirection(tile.direction, sourceDir, allowBidiSource, true, gap))
			{
				break;
			}

			extent = offset;
			gap = 0;
		}
		return extent;
	}

	// Finds the first tile after the source side that can act as the return side.
	bool FindTargetStart(
		const EdgeScan& scannedTiles,
		const PathDir sourceDir,
		const PathDir targetDir,
		const bool allowBidiSource,
		const bool allowBidiTarget,
		int32_t& targetStart)
	{
		uint32_t gap = 0;
		for (int32_t offset = 1;
			offset <= static_cast<int32_t>(gMaxSearchDistance);
			++offset)
		{
			const EdgeTile& tile = GetScannedTile(scannedTiles, offset);
			if (!tile.valid)
			{
				break;
			}
			if (!tile.isEligibleNc)
			{
				++gap;
				continue;
			}
			if (IsSourceDirection(tile.direction, sourceDir, allowBidiSource, true, gap))
			{
				gap = 0;
				continue;
			}
			if (IsTargetDirection(tile.direction, targetDir, allowBidiTarget))
			{
				targetStart = offset;
				return true;
			}

			break;
		}
		return false;
	}

	BandCollection BuildSourceBands(
		const EdgeScan& scannedTiles,
		const PathDir sourceDir,
		const bool allowBidiSource,
		const int32_t minOffset,
		const int32_t maxOffset)
	{
		BandCollection result;
		for (int32_t offset = minOffset; offset <= maxOffset; ++offset)
		{
			const EdgeTile& tile = GetScannedTile(scannedTiles, offset);
			if (tile.valid &&
				tile.isEligibleNc &&
				IsSourceDirection(tile.direction, sourceDir, allowBidiSource, false, 0))
			{
				result.Add(tile);
			}
		}
		return result;
	}

	BandCollection BuildTargetBands(const EdgeScan& scannedTiles, const PathDir targetDir, const bool allowBidiTarget, const int32_t targetStart)
	{
		BandCollection result;
		uint32_t gap = 0;
		for (int32_t offset = targetStart;
			offset <= static_cast<int32_t>(gMaxSearchDistance);
			++offset)
		{
			const EdgeTile& tile = GetScannedTile(scannedTiles, offset);
			if (!tile.valid)
			{
				break;
			}

			if (!tile.isEligibleNc)
			{
				if (++gap > gMaxGroupingGap)
				{
					break;
				}
				continue;
			}

			if (!IsTargetDirection(tile.direction, targetDir, allowBidiTarget))
			{
				break;
			}

			result.Add(tile);
			gap = 0;
		}
		return result;
	}

	bool FindTileInBands(const BandCollection& collection, const int32_t tileOffset, size_t& bandIndex, size_t& tileIndexInBand)
	{
		for (size_t currentBand = 0; currentBand < collection.bandCount; ++currentBand)
		{
			const Band& band = collection.bands[currentBand];
			for (size_t currentTile = 0; currentTile < band.tileCount; ++currentTile)
			{
				if (collection.tiles[band.firstTile + currentTile].offset == tileOffset)
				{
					bandIndex = currentBand;
					tileIndexInBand = currentTile;
					return true;
				}
			}
		}
		return false;
	}

	// Returns the one-tile step along the current city edge.
	bool TryGetEdgeScanStep(cSC4TrafficSimulator* const pTrafficSim, const int32_t x, const int32_t z, int32_t& stepX, int32_t& stepZ)
	{
		int32_t cellX = 0;
		int32_t cellZ = 0;
		uint32_t edge = 0;
		if (!TryGetBoundaryCellAndEdge(pTrafficSim, x, z, cellX, cellZ, edge))
		{
			return false;
		}

		stepX = edge == 1 || edge == 3 ? 1 : 0;
		stepZ = edge == 0 || edge == 2 ? 1 : 0;
		return true;
	}

	bool TryFindPairedReturnTile(cSC4TrafficSimulator* const pTrafficSim, const int32_t originX, const int32_t originZ,
		const int32_t stepX, const int32_t stepZ, const uint16_t eligibleNcMask,
		const bool allowBidiSource, const bool allowBidiTarget,
		int32_t& resultX, int32_t& resultZ)
	{
		if ((stepX == 0 && stepZ == 0) || (stepX != 0 && stepZ != 0))
		{
			return false;
		}

		EdgeScan scannedTiles{};
		for (int32_t offset = -static_cast<int32_t>(gMaxSearchDistance);
			offset <= static_cast<int32_t>(gMaxSearchDistance);
			++offset)
		{
			scannedTiles[static_cast<size_t>(offset + static_cast<int32_t>(kMaxSearchDistance))] =
				ReadEdgeTile(pTrafficSim, originX, originZ, stepX, stepZ, eligibleNcMask, offset);
		}

		const EdgeTile& origin = GetScannedTile(scannedTiles, 0);
		PathDir sourceDirection = PathDir::None;
		if (!TryFindSourceDirectionNearOrigin(scannedTiles, origin.direction, allowBidiSource, sourceDirection))
		{
			return false;
		}

		const PathDir targetDirection = GetOppositeDirection(sourceDirection);
		int32_t targetStart = 0;
		if (!FindTargetStart(scannedTiles, sourceDirection, targetDirection, allowBidiSource, allowBidiTarget, targetStart))
		{
			return false;
		}

		const auto negSourceExtent = FindSourceExtent(scannedTiles, sourceDirection, allowBidiSource, -1);
		const auto posSourceExtent = FindSourceExtent(scannedTiles, sourceDirection, allowBidiSource, 1);
		const auto maxOffset = std::min(posSourceExtent, targetStart - 1);
		const auto sourceBands = BuildSourceBands(scannedTiles, sourceDirection, allowBidiSource, negSourceExtent, maxOffset);
		const auto targetBands = BuildTargetBands(scannedTiles, targetDirection, allowBidiTarget, targetStart);
		if (sourceBands.bandCount == 0 || targetBands.bandCount == 0)
		{
			return false;
		}

		size_t sourceBandIndex = 0;
		size_t sourceTileIndex = 0;
		if (!FindTileInBands(sourceBands, 0, sourceBandIndex, sourceTileIndex))
		{
			return false;
		}

		// Source bands and lanes are indexed from the target-facing side. Target
		// bands and lanes are already stored from the source-facing side.
		const size_t facingSourceBandIndex = sourceBands.bandCount - sourceBandIndex - 1;
		const size_t targetBandIndex = facingSourceBandIndex * targetBands.bandCount / sourceBands.bandCount;
		const Band& sourceBand = sourceBands.bands[sourceBandIndex];
		const Band& targetBand = targetBands.bands[targetBandIndex];
		const size_t facingSourceTileIndex = sourceBand.tileCount - sourceTileIndex - 1;
		const size_t targetTileIndex = facingSourceTileIndex * targetBand.tileCount / sourceBand.tileCount;
		const EdgeTile& result = targetBands.tiles[targetBand.firstTile + targetTileIndex];

		resultX = result.x;
		resultZ = result.z;
		return true;
	}

	// Road NCs are broad. Only one-way Road edge tiles should enter the NWM search.
	bool IsOneWayRoadConnection(cSC4TrafficSimulator* const pTrafficSim, const int32_t originX, const int32_t originZ, const uint16_t originMask)
	{
		if (!gEnableNWM ||
			(originMask & kRoadNcMask) == 0 ||
			(originMask & kVanillaDividedNetworkNcMask) != 0 ||
			(originMask & kDirtRoadNcMask) != 0)
		{
			return false;
		}

		uint16_t pathMatrix = 0;
		const auto pathDirection = GetPathDirection(pTrafficSim, originX, originZ, pathMatrix);
		return IsOneWayDirection(pathDirection);
	}

	uint16_t BuildPatchedNeighborConnectionMask(const bool enableRHW, const bool enableOWR, const bool enableNWM)
	{
		uint16_t mask = kVanillaDividedNetworkNcMask;
		if (enableRHW)
		{
			mask |= kDirtRoadNcMask;
		}
		if (enableOWR)
		{
			mask |= kOneWayRoadNcMask;
		}
		if (enableNWM)
		{
			mask |= kRoadNcMask;
		}
		return mask;
	}

	void __fastcall FindReturnTile(cSC4TrafficSimulator* const pTrafficSim, void*, int32_t* const pX, int32_t* const pZ, const bool reverseSearch)
	{
		const int32_t originX = *pX;
		const int32_t originZ = *pZ;
		const uint16_t originMask = GetNeighborConnectionMask(pTrafficSim, originX, originZ);
		// OWR tiles also carry the DirtRoad bit (traffic sees OWR as type 10), so test OWR first.
		const bool useOWRPairing = (originMask & kOneWayRoadNcMask) != 0;
		const bool useRHWPairing = !useOWRPairing && (originMask & kDirtRoadNcMask) != 0;
		const bool useNWMPairing = !useOWRPairing && !useRHWPairing && IsOneWayRoadConnection(pTrafficSim, originX, originZ, originMask);

		if (!useOWRPairing && !useRHWPairing && !useNWMPairing)
		{
			// Vanilla divided networks keep the vanilla step. Anything else here is an ordinary
			// Road NC that only passed the mask test because NWM added Road, so it stays unchanged.
			if ((originMask & kVanillaDividedNetworkNcMask) != 0)
			{
				cSC4TrafficSimulator::FindHighwayReturnTile(pTrafficSim, pX, pZ, reverseSearch);
			}
			return;
		}

		int32_t stepX = 0;
		int32_t stepZ = 0;
		if (useOWRPairing || useRHWPairing)
		{
			cSC4TrafficSimulator::FindHighwayReturnTile(pTrafficSim, pX, pZ, reverseSearch);
			stepX = *pX - originX;
			stepZ = *pZ - originZ;
		}
		else if (!TryGetEdgeScanStep(pTrafficSim, originX, originZ, stepX, stepZ))
		{
			*pX = originX;
			*pZ = originZ;
			return;
		}

		const uint16_t eligibleNcMask = useOWRPairing ? kOneWayRoadNcMask : (useRHWPairing ? kDirtRoadNcMask : kRoadNcMask);
		const bool allowBidiSource = useOWRPairing;
		const bool allowBidiTarget = useOWRPairing || useNWMPairing;
		bool paired = TryFindPairedReturnTile(
			pTrafficSim, originX, originZ, stepX, stepZ, eligibleNcMask, allowBidiSource, allowBidiTarget, *pX, *pZ);

		if (!paired && useNWMPairing)
		{
			paired = TryFindPairedReturnTile(
				pTrafficSim, originX, originZ, -stepX, -stepZ, eligibleNcMask, allowBidiSource, allowBidiTarget, *pX, *pZ);
		}

		if (!paired)
		{
			*pX = originX;
			*pZ = originZ;
		}
	}

	// OWR neighbor connections use type 13. The persisted cSC4NeighborConnection type (offset 0x24)
	// stays 13 so neighboring cities create OWR stubs, while GetType reports 10 so the traffic code
	// treats OWR like RHW. The sites that need the real type read offset 0x24 directly.
	constexpr uint32_t kOneWayRoadNcType = static_cast<uint32_t>(NcType::OneWayRoad);
	constexpr uint32_t kOneWayRoadTileMask = asMask(NcType::DirtRoad, NcType::OneWayRoad);

	constexpr uint32_t NeighborConnection_GetTypeSlot = 0xab2048;  // cSC4NeighborConnection vtable 0xab2034, slot 0x14
	constexpr uint32_t NeighborConnection_GetTypeOrig = 0x6bdd80;  // accessor shared with other classes, so patch the slot only
	constexpr uint32_t TreatAllIncomingConnectionsAsNew_GetType = 0x4a937a;
	constexpr uint32_t ChangeSymmetricConnection_GetType = 0x4a8e44;
	constexpr uint32_t AddNeighborConnection_TileMask = 0x71540a;
	constexpr uint32_t RemoveNeighborConnection_TileMask = 0x713aff;
	constexpr uint32_t NeighborsSim_FindConnection_GetType = 0x6cf6d8;
	constexpr uint32_t CreateSymmetricConnections_TypeBound = 0x4aaa73;
	constexpr uint32_t CreateSymmetricConnections_JumpTableEntry13 = 0x4ab7e8;  // padding right after the 12-entry table
	constexpr uint32_t CreateSymmetricConnections_CreateStub = 0x4aaa96;

	uint32_t __fastcall NeighborConnection_GetType(const uint8_t* const pConnection, void*)
	{
		const uint32_t type = *reinterpret_cast<const uint32_t*>(pConnection + 0x24);
		return type == kOneWayRoadNcType ? static_cast<uint32_t>(NcType::DirtRoad) : type;
	}

	// edx = 1 << type. An OWR connection (esi) sets both the DirtRoad and OWR bits, whichever type the caller passed.
	void NAKED_FUN Hook_AddNeighborConnection_TileMask(void)
	{
		__asm {
			mov edx, 1;
			shl edx, cl;
			cmp dword ptr [esi + 0x24], 13;  // raw type == OneWayRoad
			jne done;
			or edx, kOneWayRoadTileMask;
done:
			ret;
		}
	}

	// edx = ~(1 << type). Removal passes GetType (10 for OWR), so types 10 and 13 clear both bits.
	void NAKED_FUN Hook_RemoveNeighborConnection_TileMask(void)
	{
		__asm {
			mov edx, 1;
			shl edx, cl;
			cmp ecx, 10;
			je both;
			cmp ecx, 13;
			jne invert;
both:
			or edx, kOneWayRoadTileMask;
invert:
			not edx;
			ret;
		}
	}

	// eax = type of connection esi, compared with the requested type in ebp afterwards.
	// OWR connections match a request for either 13 or 10.
	void NAKED_FUN Hook_FindConnection_GetType(void)
	{
		__asm {
			mov eax, dword ptr [esi + 0x24];
			cmp eax, ebp;
			je done;
			cmp eax, 13;
			jne done;
			mov eax, 10;
done:
			ret;
		}
	}

	// Jump table case for type 13: create the stub with network type OneWayRoad.
	void NAKED_FUN Hook_CreateSymmetricConnections_OneWayRoad(void)
	{
		__asm {
			mov dword ptr [esp + 0x14], 10;  // network type OneWayRoad
			push CreateSymmetricConnections_CreateStub;
			ret;
		}
	}

	void InstallOneWayRoadConnections()
	{
		Patching::PatchImmediate32(NeighborConnection_GetTypeSlot, NeighborConnection_GetTypeOrig, reinterpret_cast<uint32_t>(&NeighborConnection_GetType));

		// call [edx+0x14] (GetType) -> mov eax, [esi+0x24], so incoming connection changes keep type 13
		Patching::PatchBytes(TreatAllIncomingConnectionsAsNew_GetType, {0xff, 0x52, 0x14}, {0x8b, 0x46, 0x24});
		Patching::PatchBytes(ChangeSymmetricConnection_GetType, {0xff, 0x52, 0x14}, {0x8b, 0x46, 0x24});

		Patching::ReplaceWithCall(AddNeighborConnection_TileMask, {0xba, 0x01, 0x00, 0x00, 0x00, 0xd3, 0xe2}, Hook_AddNeighborConnection_TileMask);
		Patching::ReplaceWithCall(RemoveNeighborConnection_TileMask, {0xba, 0x01, 0x00, 0x00, 0x00, 0xd3, 0xe2, 0xf7, 0xd2}, Hook_RemoveNeighborConnection_TileMask);
		Patching::ReplaceWithCall(NeighborsSim_FindConnection_GetType, {0x8b, 0x06, 0x8b, 0xce, 0xff, 0x50, 0x14}, Hook_FindConnection_GetType);

		// Add the table entry before raising the bound that makes it reachable.
		Patching::PatchImmediate32(CreateSymmetricConnections_JumpTableEntry13, 0xcccccccc, reinterpret_cast<uint32_t>(&Hook_CreateSymmetricConnections_OneWayRoad));
		Patching::PatchBytes(CreateSymmetricConnections_TypeBound, {0x83, 0xf8, 0x0b}, {0x83, 0xf8, 0x0c});

		// Last, so that if anything above fails, the game never creates type 13 connections.
		Patching::PatchImmediate32(kOWRNetworkToNcAddress, static_cast<uint32_t>(NcType::None), kOneWayRoadNcType);
	}
}

void NeighborConnections::Install(const Options& options)
{
	gEnableNWM = options.enableNWM;
	gMaxSearchDistance = std::clamp(options.maxSearchDistance, kMinSearchDistance, kMaxSearchDistance);
	gMaxGroupingGap = std::min(options.maxGroupingGap, kMaxGroupingGap);

	Patching::InstallCallHook(kForwardPathFindReturnTileCallAddress, reinterpret_cast<void (*)()>(FindReturnTile));
	Patching::InstallCallHook(kReturnPathFindReturnTileCallAddress, reinterpret_cast<void (*)()>(FindReturnTile));

	// Add the enabled connection types to the special search algorithm that exists for Avenue/Highway in vanilla
	const uint16_t patchedMask = BuildPatchedNeighborConnectionMask(options.enableRHW, options.enableOWR, options.enableNWM);
	Patching::PatchTestWordPtrEaxImmediate16(kForwardPathNetworkMaskTestAddress, kVanillaDividedNetworkNcMask, patchedMask);
	Patching::PatchTestWordPtrEaxImmediate16(kReturnPathNetworkMaskTestAddress, kVanillaDividedNetworkNcMask, patchedMask);

	if (options.enableOWR)
	{
		InstallOneWayRoadConnections();
	}
}
