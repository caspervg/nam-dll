#include "NeighborConnections.h"
#include "Logger.h"
#include "Patching.h"

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

	- OWR: changes OWR's neighbor connection type from "none" to the same
	  DirtRoad/RHW type. Because of that, enabling OWR necessarily also
	  enables the RHW part of this patch.

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
	constexpr uint32_t kFindHighwayReturnTileAddress = 0x0070dd30;
	constexpr uint32_t kGetTrafficNeighborConnectionAddress = 0x0070f970;
	constexpr uint32_t kOWRNetworkToNcAddress = 0x00aa84ec;

	constexpr uint16_t kVanillaDividedNetworkNcMask = 0x1108;
	constexpr uint16_t kRoadNcMask = 0x0004;
	constexpr uint16_t kDirtRoadNcMask = 0x0400;
	constexpr uint16_t kOWRMetadataNcMask = 0x2000;
	constexpr uint32_t kNoNcType = 0;
	constexpr uint32_t kDirtRoadNcType = 10;

	// Windows cSC4TrafficSimulator layout confirmed against 1.1.641.
	constexpr size_t kMaxXOffset = 0x28;
	constexpr size_t kMaxZOffset = 0x2c;
	constexpr size_t kPathCellDataOffset = 0x58;
	constexpr size_t kPathCellSize = 0x2e;
	constexpr size_t kVehiclePathTypeIndex = 1;

	constexpr uint32_t kMinSearchDistance = 1;
	constexpr uint32_t kMaxSearchDistance = 64;
	constexpr uint32_t kMaxGroupingGap = 16;
	constexpr size_t kScanCapacity = kMaxSearchDistance * 2 + 1;

	uint32_t gMaxSearchDistance = kMinSearchDistance;
	uint32_t gMaxGroupingGap = 0;
	bool gEnableNWM = false;
	uint32_t gDiagnosticLogCount = 0;

	using FindHighwayReturnTile = void (__thiscall*)(void* pTrafficSim, int32_t* pX, int32_t* pZ, bool reverseSearch);
	using GetTrafficNeighborConnection = void* (__thiscall*)(void* pTrafficSim, int32_t x, int32_t z);

	auto const pFindHighwayReturnTile = reinterpret_cast<FindHighwayReturnTile>(kFindHighwayReturnTileAddress);
	auto const pGetTrafficNeighborConnection = reinterpret_cast<GetTrafficNeighborConnection>(kGetTrafficNeighborConnectionAddress);

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

	const EdgeTile& GetScannedTile(const EdgeScan& scannedTiles, int32_t offset);

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

	struct PairingDiagnostics
	{
		const char* mode = "";
		uint16_t originMask = 0;
		bool reverseSearch = false;
		bool fallbackPass = false;
	};

	const char* PathDirName(const PathDir dir)
	{
		switch (dir)
		{
		case PathDir::None:
			return "None";
		case PathDir::Inbound:
			return "Inbound";
		case PathDir::Outbound:
			return "Outbound";
		case PathDir::Bidi:
			return "Bidi";
		default:
			return "?";
		}
	}

	bool ScanContainsBidirectionalPath(const EdgeScan& scannedTiles)
	{
		for (int32_t offset = -static_cast<int32_t>(gMaxSearchDistance);
			offset <= static_cast<int32_t>(gMaxSearchDistance);
			++offset)
		{
			if (GetScannedTile(scannedTiles, offset).direction == PathDir::Bidi)
			{
				return true;
			}
		}
		return false;
	}

	void LogPairingDiagnostics(
		const PairingDiagnostics& diagnostics,
		const EdgeScan& scannedTiles,
		const int32_t originX,
		const int32_t originZ,
		const int32_t stepX,
		const int32_t stepZ,
		const uint16_t eligibleNcMask,
		const bool allowBidiTarget,
		const bool paired,
		const char* const reason,
		const int32_t targetStart,
		const size_t sourceBandCount,
		const size_t targetBandCount,
		const int32_t resultX,
		const int32_t resultZ)
	{
		constexpr uint32_t kMaxDiagnosticLogs = 16;
		if (gDiagnosticLogCount >= kMaxDiagnosticLogs)
		{
			return;
		}

		const bool scanHasBidi = ScanContainsBidirectionalPath(scannedTiles);
		if (paired && !scanHasBidi)
		{
			return;
		}

		++gDiagnosticLogCount;

		Logger& logger = Logger::GetInstance();
		logger.WriteLineFormatted(
			LogLevel::Error,
			"NC diagnostic %u/%u: mode=%s reverse=%d fallbackPass=%d origin=(%d,%d) originMask=0x%04X step=(%d,%d) eligibleMask=0x%04X allowBidiTarget=%d paired=%d reason=%s targetStart=%d sourceBands=%u targetBands=%u result=(%d,%d)",
			gDiagnosticLogCount,
			kMaxDiagnosticLogs,
			diagnostics.mode,
			diagnostics.reverseSearch ? 1 : 0,
			diagnostics.fallbackPass ? 1 : 0,
			originX,
			originZ,
			diagnostics.originMask,
			stepX,
			stepZ,
			eligibleNcMask,
			allowBidiTarget ? 1 : 0,
			paired ? 1 : 0,
			reason,
			targetStart,
			static_cast<unsigned>(sourceBandCount),
			static_cast<unsigned>(targetBandCount),
			resultX,
			resultZ);

		logger.WriteLine(
			LogLevel::Error,
			"NC diagnostic scan: offset x z valid eligible ncMask pathMatrix direction");
		for (int32_t offset = -static_cast<int32_t>(gMaxSearchDistance);
			offset <= static_cast<int32_t>(gMaxSearchDistance);
			++offset)
		{
			const EdgeTile& tile = GetScannedTile(scannedTiles, offset);
			logger.WriteLineFormatted(
				LogLevel::Error,
				"NC diagnostic scan: %+d %d %d %d %d 0x%04X 0x%04X %s",
				offset,
				tile.x,
				tile.z,
				tile.valid ? 1 : 0,
				tile.isEligibleNc ? 1 : 0,
				tile.ncMask,
				tile.pathMatrix,
				PathDirName(tile.direction));
		}

		if (gDiagnosticLogCount == kMaxDiagnosticLogs)
		{
			logger.WriteLine(
				LogLevel::Error,
				"NC diagnostic log limit reached; suppressing further neighbor connection diagnostics this session.");
		}
	}

	bool LogPairingFailureWithoutScan(
		const PairingDiagnostics& diagnostics,
		const int32_t originX,
		const int32_t originZ,
		const int32_t stepX,
		const int32_t stepZ,
		const uint16_t eligibleNcMask,
		const bool allowBidiTarget,
		const char* const reason)
	{
		constexpr uint32_t kMaxDiagnosticLogs = 16;
		if (gDiagnosticLogCount >= kMaxDiagnosticLogs)
		{
			return false;
		}

		++gDiagnosticLogCount;

		Logger& logger = Logger::GetInstance();
		logger.WriteLineFormatted(
			LogLevel::Error,
			"NC diagnostic %u/%u: mode=%s reverse=%d fallbackPass=%d origin=(%d,%d) originMask=0x%04X step=(%d,%d) eligibleMask=0x%04X allowBidiTarget=%d paired=0 reason=%s",
			gDiagnosticLogCount,
			kMaxDiagnosticLogs,
			diagnostics.mode,
			diagnostics.reverseSearch ? 1 : 0,
			diagnostics.fallbackPass ? 1 : 0,
			originX,
			originZ,
			diagnostics.originMask,
			stepX,
			stepZ,
			eligibleNcMask,
			allowBidiTarget ? 1 : 0,
			reason);

		if (gDiagnosticLogCount == kMaxDiagnosticLogs)
		{
			logger.WriteLine(
				LogLevel::Error,
				"NC diagnostic log limit reached; suppressing further neighbor connection diagnostics this session.");
		}

		return false;
	}

	uint16_t GetNeighborConnectionMask(void* const pTrafficSim, const int32_t x, const int32_t z)
	{
		const void* const pConnection = pGetTrafficNeighborConnection(pTrafficSim, x, z);
		return pConnection ? *static_cast<const uint16_t*>(pConnection) : 0;
	}

	int32_t ReadSimulatorInt32(void* const pTrafficSim, const size_t offset)
	{
		const auto* const bytes = static_cast<const uint8_t*>(pTrafficSim);
		return *reinterpret_cast<const int32_t*>(bytes + offset);
	}

	// Converts the outside neighbor coordinate to the city-edge cell and edge index.
	bool TryGetBoundaryCellAndEdge(void* const pTrafficSim, const int32_t x, const int32_t z, int32_t& cellX, int32_t& cellZ, uint32_t& edge)
	{
		const int32_t maxX = ReadSimulatorInt32(pTrafficSim, kMaxXOffset);
		const int32_t maxZ = ReadSimulatorInt32(pTrafficSim, kMaxZOffset);
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

		return cellX >= 0 && cellX <= maxX && cellZ >= 0 && cellZ <= maxZ;
	}

	PathDir GetPathDirection(void* const pTrafficSim, const int32_t x, const int32_t z, uint16_t& pathMatrix)
	{
		int32_t cellX = 0;
		int32_t cellZ = 0;
		uint32_t boundaryEdge = 0;
		if (!TryGetBoundaryCellAndEdge(pTrafficSim, x, z, cellX, cellZ, boundaryEdge))
		{
			return PathDir::None;
		}

		const auto* const simulatorBytes = static_cast<const uint8_t*>(pTrafficSim);
		const auto* const pathCellData = *reinterpret_cast<const uint8_t* const*>(simulatorBytes + kPathCellDataOffset);
		if (!pathCellData)
		{
			return PathDir::None;
		}

		const uint32_t cellIndex = (static_cast<uint32_t>(cellX) << 8) | static_cast<uint32_t>(cellZ);
		const auto* const pathMatrices = reinterpret_cast<const uint16_t*>(pathCellData + cellIndex * kPathCellSize);
		pathMatrix = pathMatrices[kVehiclePathTypeIndex];

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

	EdgeTile ReadEdgeTile(void* const pTrafficSim, const int32_t originX, const int32_t originZ,
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

	const EdgeTile& GetScannedTile(const EdgeScan& scannedTiles, const int32_t offset)
	{
		return scannedTiles[static_cast<size_t>(offset + static_cast<int32_t>(kMaxSearchDistance))];
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

			if (!IsSourceDirection(tile.direction, sourceDir, allowBidiSource, true, gap) ||
				gap > gMaxGroupingGap)
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
			if (tile.direction == sourceDir ||
				IsSourceDirection(tile.direction, sourceDir, allowBidiSource, true, gap))
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

			if (!IsTargetDirection(tile.direction, targetDir, allowBidiTarget) ||
				gap > gMaxGroupingGap)
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
	bool TryGetEdgeScanStep(void* const pTrafficSim, const int32_t x, const int32_t z, int32_t& stepX, int32_t& stepZ)
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

	bool TryFindPairedReturnTile(void* const pTrafficSim, const int32_t originX, const int32_t originZ,
		const int32_t stepX, const int32_t stepZ, const uint16_t eligibleNcMask,
		const bool allowBidiSource, const bool allowBidiTarget,
		const PairingDiagnostics& diagnostics, int32_t& resultX, int32_t& resultZ)
	{
		if ((stepX == 0 && stepZ == 0) || (stepX != 0 && stepZ != 0))
		{
			return LogPairingFailureWithoutScan(
				diagnostics,
				originX,
				originZ,
				stepX,
				stepZ,
				eligibleNcMask,
				allowBidiTarget,
				"invalid edge scan step");
		}

		EdgeScan scannedTiles{};
		for (int32_t offset = -static_cast<int32_t>(gMaxSearchDistance);
			offset <= static_cast<int32_t>(gMaxSearchDistance);
			++offset)
		{
			scannedTiles[static_cast<size_t>(
				offset + static_cast<int32_t>(kMaxSearchDistance))] =
				ReadEdgeTile(
					pTrafficSim,
					originX,
					originZ,
					stepX,
					stepZ,
					eligibleNcMask,
					offset);
		}

		const EdgeTile& origin = GetScannedTile(scannedTiles, 0);
		auto fail = [&](const char* const reason, const int32_t targetStart = 0, const size_t sourceBandCount = 0, const size_t targetBandCount = 0)
		{
			LogPairingDiagnostics(
				diagnostics,
				scannedTiles,
				originX,
				originZ,
				stepX,
				stepZ,
				eligibleNcMask,
				allowBidiTarget,
				false,
				reason,
				targetStart,
				sourceBandCount,
				targetBandCount,
				originX,
				originZ);
			return false;
		};

		PathDir sourceDirection = PathDir::None;
		if (!TryFindSourceDirectionNearOrigin(
			scannedTiles,
			origin.direction,
			allowBidiSource,
			sourceDirection))
		{
			return fail("source direction unresolved");
		}

		const PathDir targetDirection = GetOppositeDirection(sourceDirection);
		int32_t targetStart = 0;
		if (!FindTargetStart(
			scannedTiles,
			sourceDirection,
			targetDirection,
			allowBidiSource,
			allowBidiTarget,
			targetStart))
		{
			return fail("no target start");
		}

		const auto negSourceExtent = FindSourceExtent(scannedTiles, sourceDirection, allowBidiSource, -1);
		const auto posSourceExtent = FindSourceExtent(scannedTiles, sourceDirection, allowBidiSource, 1);
		const auto maxOffset = std::min(posSourceExtent, targetStart - 1);
		const auto sourceBands = BuildSourceBands(
			scannedTiles,
			sourceDirection,
			allowBidiSource,
			negSourceExtent,
			maxOffset);
		const auto targetBands = BuildTargetBands(scannedTiles, targetDirection, allowBidiTarget, targetStart);

		if (sourceBands.bandCount == 0 || targetBands.bandCount == 0)
		{
			return fail("empty source or target bands", targetStart, sourceBands.bandCount, targetBands.bandCount);
		}

		size_t sourceBandIndex = 0;
		size_t sourceTileIndex = 0;
		if (!FindTileInBands(sourceBands, 0, sourceBandIndex, sourceTileIndex))
		{
			return fail("origin missing from source bands", targetStart, sourceBands.bandCount, targetBands.bandCount);
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
		LogPairingDiagnostics(
			diagnostics,
			scannedTiles,
			originX,
			originZ,
			stepX,
			stepZ,
			eligibleNcMask,
			allowBidiTarget,
			true,
			"paired",
			targetStart,
			sourceBands.bandCount,
			targetBands.bandCount,
			resultX,
			resultZ);
		return true;
	}

	// Road NCs are broad. Only one-way Road edge tiles should enter the NWM search.
	bool IsOneWayRoadConnection(void* const pTrafficSim, const int32_t originX, const int32_t originZ, const uint16_t originMask)
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

	uint16_t BuildPatchedNeighborConnectionMask(const bool enableRHW, const bool enableNWM)
	{
		uint16_t mask = kVanillaDividedNetworkNcMask;
		if (enableRHW)
		{
			mask |= kDirtRoadNcMask;
		}
		if (enableNWM)
		{
			mask |= kRoadNcMask;
		}
		return mask;
	}

	void __fastcall FindReturnTile(void* const pTrafficSim, void*, int32_t* const pX, int32_t* const pZ, const bool reverseSearch)
	{
		const int32_t originX = *pX;
		const int32_t originZ = *pZ;
		const uint16_t originMask = GetNeighborConnectionMask(pTrafficSim, originX, originZ);
		const bool useRHWPairing = (originMask & kDirtRoadNcMask) != 0;
		const bool useNWMPairing = !useRHWPairing && IsOneWayRoadConnection(pTrafficSim, originX, originZ, originMask);

		if (!useRHWPairing && !useNWMPairing)
		{
			// Road was added to the mask for NWM, but ordinary Road NCs must stay unchanged.
			if ((originMask & kRoadNcMask) == 0)
			{
				pFindHighwayReturnTile(pTrafficSim, pX, pZ, reverseSearch);
			}
			return;
		}

		int32_t stepX = 0;
		int32_t stepZ = 0;
		if (useRHWPairing)
		{
			pFindHighwayReturnTile(pTrafficSim, pX, pZ, reverseSearch);
			stepX = *pX - originX;
			stepZ = *pZ - originZ;
		}
		else if (!TryGetEdgeScanStep(pTrafficSim, originX, originZ, stepX, stepZ))
		{
			*pX = originX;
			*pZ = originZ;
			return;
		}

		const uint16_t eligibleNeighborConnectionMask = useRHWPairing ? kDirtRoadNcMask : kRoadNcMask;
		const bool useOWRPairing = useRHWPairing && (originMask & kOWRMetadataNcMask) != 0;
		const bool allowBidirectionalSource = useOWRPairing;
		const bool allowBidirectionalTarget = useOWRPairing || useNWMPairing;
		PairingDiagnostics diagnostics;
		diagnostics.mode = useOWRPairing ? "OWR" : (useRHWPairing ? "DirtRoad/RHW" : "NWM/Road");
		diagnostics.originMask = originMask;
		diagnostics.reverseSearch = reverseSearch;
		diagnostics.fallbackPass = false;
		bool paired = TryFindPairedReturnTile(
			pTrafficSim,
			originX,
			originZ,
			stepX,
			stepZ,
			eligibleNeighborConnectionMask,
			allowBidirectionalSource,
			allowBidirectionalTarget,
			diagnostics,
			*pX,
			*pZ);

		if (!paired && useNWMPairing)
		{
			diagnostics.fallbackPass = true;
			paired = TryFindPairedReturnTile(
				pTrafficSim,
				originX,
				originZ,
				-stepX,
				-stepZ,
				eligibleNeighborConnectionMask,
				allowBidirectionalSource,
				allowBidirectionalTarget,
				diagnostics,
				*pX,
				*pZ);
		}

		if (!paired)
		{
			*pX = originX;
			*pZ = originZ;
		}
	}
}

void NeighborConnections::Install(const Options& options)
{
	const bool enableRHW = options.enableRHW || options.enableOWR; // For OWR to be enabled, RHW needs to be enabled
	gEnableNWM = options.enableNWM;
	gMaxSearchDistance = std::clamp(options.maxSearchDistance, kMinSearchDistance, kMaxSearchDistance);
	gMaxGroupingGap = std::min(options.maxGroupingGap, kMaxGroupingGap);
	gDiagnosticLogCount = 0;

	Patching::RedirectCall(kForwardPathFindReturnTileCallAddress, kFindHighwayReturnTileAddress, reinterpret_cast<void (*)()>(FindReturnTile));
	Patching::RedirectCall(kReturnPathFindReturnTileCallAddress, kFindHighwayReturnTileAddress, reinterpret_cast<void (*)()>(FindReturnTile));

	// Add the RHW/DirtRoad connection type to the special search algorithm that exists for Avenue/Highway in vanilla
	const uint16_t patchedMask = BuildPatchedNeighborConnectionMask(enableRHW, options.enableNWM);
	Patching::PatchTestWordPtrEaxImmediate16(kForwardPathNetworkMaskTestAddress, kVanillaDividedNetworkNcMask, patchedMask);
	Patching::PatchTestWordPtrEaxImmediate16(kReturnPathNetworkMaskTestAddress, kVanillaDividedNetworkNcMask, patchedMask);

	if (options.enableOWR)
	{
		// Set the OWR neighbor connection type to the RHW/DirtRoad connection type
		Patching::PatchImmediate32(kOWRNetworkToNcAddress, kNoNcType, kDirtRoadNcType);
	}
}
