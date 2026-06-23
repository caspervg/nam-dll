#include "NeighborConnections.h"
#include "Patching.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

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

	using FindHighwayReturnTile = void (__thiscall*)(void* pTrafficSim, int32_t* pX, int32_t* pZ, bool reverseSearch);
	using GetTrafficNeighborConnection = void* (__thiscall*)(void* pTrafficSim, int32_t x, int32_t z);

	auto const pFindHighwayReturnTile = reinterpret_cast<FindHighwayReturnTile>(kFindHighwayReturnTileAddress);
	auto const pGetTrafficNeighborConnection = reinterpret_cast<GetTrafficNeighborConnection>(kGetTrafficNeighborConnectionAddress);

	enum class PathDirection : uint8_t
	{
		None,
		Inbound,
		Outbound,
		Bidirectional
	};

	struct EdgeTile
	{
		int32_t x = 0;
		int32_t z = 0;
		int32_t offset = 0;
		uint16_t neighborConnectionMask = 0;
		uint16_t pathMatrix = 0;
		PathDirection direction = PathDirection::None;
		bool valid = false;
		bool isEligibleNeighborConnection = false;
	};

	struct Band
	{
		size_t firstTile = 0;
		size_t tileCount = 0;
	};

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

	PathDirection GetPathDirection(void* const pTrafficSim, const int32_t x, const int32_t z, uint16_t& pathMatrix)
	{
		int32_t cellX = 0;
		int32_t cellZ = 0;
		uint32_t boundaryEdge = 0;
		if (!TryGetBoundaryCellAndEdge(pTrafficSim, x, z, cellX, cellZ, boundaryEdge))
		{
			return PathDirection::None;
		}

		const auto* const simulatorBytes = static_cast<const uint8_t*>(pTrafficSim);
		const auto* const pathCellData = *reinterpret_cast<const uint8_t* const*>(simulatorBytes + kPathCellDataOffset);
		if (!pathCellData)
		{
			return PathDirection::None;
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
			return PathDirection::Bidirectional;
		}
		if (inbound)
		{
			return PathDirection::Inbound;
		}
		if (outbound)
		{
			return PathDirection::Outbound;
		}
		return PathDirection::None;
	}

	PathDirection GetOppositeDirection(const PathDirection dir)
	{
		return dir == PathDirection::Inbound ? PathDirection::Outbound : PathDirection::Inbound;
	}

	bool IsOneWayDirection(const PathDirection dir)
	{
		return dir == PathDirection::Inbound || dir == PathDirection::Outbound;
	}

	bool IsTargetDirection(const PathDirection dir, const PathDirection targetDir, const bool allowBidiFallback)
	{
		return dir == targetDir || (allowBidiFallback && dir == PathDirection::Bidirectional);
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

		tile.neighborConnectionMask = GetNeighborConnectionMask(pTrafficSim, tile.x, tile.z);
		tile.isEligibleNeighborConnection = (tile.neighborConnectionMask & eligibleNcMask) != 0;
		tile.direction = GetPathDirection(pTrafficSim, tile.x, tile.z, tile.pathMatrix);
		return tile;
	}

	const EdgeTile& GetScannedTile(const std::array<EdgeTile, kScanCapacity>& scannedTiles, const int32_t offset)
	{
		return scannedTiles[static_cast<size_t>(offset + static_cast<int32_t>(kMaxSearchDistance))];
	}

	int32_t FindSourceExtent(const std::array<EdgeTile, kScanCapacity>& scannedTiles,
		                     const PathDirection sourceDir, const int32_t inc)
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

			if (!tile.isEligibleNeighborConnection)
			{
				if (++gap > gMaxGroupingGap)
				{
					break;
				}
				continue;
			}

			if (tile.direction != sourceDir || gap > gMaxGroupingGap)
			{
				break;
			}

			extent = offset;
			gap = 0;
		}
		return extent;
	}

	bool FindTargetStart(const std::array<EdgeTile, kScanCapacity>& scannedTiles,
		const PathDirection sourceDir,
		const PathDirection targetDir,
		const bool allowBidiTarget,
		int32_t& targetStart)
	{
		for (int32_t offset = 1;
			offset <= static_cast<int32_t>(gMaxSearchDistance);
			++offset)
		{
			const EdgeTile& tile = GetScannedTile(scannedTiles, offset);
			if (!tile.valid)
			{
				break;
			}
			if (!tile.isEligibleNeighborConnection || tile.direction == sourceDir)
			{
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
		const std::array<EdgeTile, kScanCapacity>& scannedTiles,
		const PathDirection sourceDir,
		const int32_t minOffset,
		const int32_t maxOffset)
	{
		BandCollection result;
		for (int32_t offset = minOffset; offset <= maxOffset; ++offset)
		{
			const EdgeTile& tile = GetScannedTile(scannedTiles, offset);
			if (tile.valid && tile.isEligibleNeighborConnection && tile.direction == sourceDir)
			{
				result.Add(tile);
			}
		}
		return result;
	}

	BandCollection BuildTargetBands(
		const std::array<EdgeTile, kScanCapacity>& scannedTiles,
		const PathDirection targetDir,
		const bool allowBidiTarget,
		const int32_t targetStart)
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

			if (!tile.isEligibleNeighborConnection)
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

	bool TryFindPairedReturnTile(
		void* const pTrafficSim,
		const int32_t originX,
		const int32_t originZ,
		const int32_t stepX,
		const int32_t stepZ,
		const uint16_t eligibleNcMask,
		const bool allowBidiTarget,
		int32_t& resultX,
		int32_t& resultZ)
	{
		if ((stepX == 0 && stepZ == 0) || (stepX != 0 && stepZ != 0))
		{
			return false;
		}

		std::array<EdgeTile, kScanCapacity> scannedTiles{};
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
		if (!IsOneWayDirection(origin.direction))
		{
			return false;
		}

		const PathDirection targetDirection = GetOppositeDirection(origin.direction);
		int32_t targetStart = 0;
		if (!FindTargetStart(scannedTiles,origin.direction, targetDirection, allowBidiTarget, targetStart))
		{
			return false;
		}

		const auto negSourceExtent = FindSourceExtent(scannedTiles, origin.direction, -1);
		const auto posSourceExtent = FindSourceExtent(scannedTiles, origin.direction, 1);
		const auto maxOffset = std::min(posSourceExtent, targetStart - 1);
		const auto sourceBands = BuildSourceBands(scannedTiles, origin.direction, negSourceExtent, maxOffset);
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
		const size_t facingSourceBandIndex =
			sourceBands.bandCount - sourceBandIndex - 1;
		const size_t targetBandIndex =
			facingSourceBandIndex * targetBands.bandCount / sourceBands.bandCount;
		const Band& sourceBand = sourceBands.bands[sourceBandIndex];
		const Band& targetBand = targetBands.bands[targetBandIndex];
		const size_t facingSourceTileIndex =
			sourceBand.tileCount - sourceTileIndex - 1;
		const size_t targetTileIndex =
			facingSourceTileIndex * targetBand.tileCount / sourceBand.tileCount;
		const EdgeTile& result =
			targetBands.tiles[targetBand.firstTile + targetTileIndex];

		resultX = result.x;
		resultZ = result.z;
		return true;
	}

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

		const uint16_t eligibleNeighborConnectionMask =
			useRHWPairing ? kDirtRoadNcMask : kRoadNcMask;
		const bool allowBidirectionalTarget = useNWMPairing;
		bool paired = TryFindPairedReturnTile(
			pTrafficSim,
			originX,
			originZ,
			stepX,
			stepZ,
			eligibleNeighborConnectionMask,
			allowBidirectionalTarget,
			*pX,
			*pZ);

		if (!paired && useNWMPairing)
		{
			paired = TryFindPairedReturnTile(
				pTrafficSim,
				originX,
				originZ,
				-stepX,
				-stepZ,
				eligibleNeighborConnectionMask,
				allowBidirectionalTarget,
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
