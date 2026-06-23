#include "RHWNeighborConnections.h"
#include "Logger.h"
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

	constexpr uint16_t kVanillaDividedNetworkConnectionMask = 0x1108;
	constexpr uint16_t kDirtRoadNeighborConnectionMask = 0x0400;
	constexpr uint16_t kRHWDividedNetworkConnectionMask =
		kVanillaDividedNetworkConnectionMask | kDirtRoadNeighborConnectionMask;

	// Windows cSC4TrafficSimulator layout confirmed against 1.1.641.
	constexpr size_t kMaximumXOffset = 0x28;
	constexpr size_t kMaximumZOffset = 0x2c;
	constexpr size_t kPathCellDataOffset = 0x58;
	constexpr size_t kPathCellSize = 0x2e;
	constexpr size_t kVehiclePathTypeIndex = 1;

	constexpr uint32_t kMinimumSearchDistance = 1;
	constexpr uint32_t kMaximumSearchDistance = 64;
	constexpr uint32_t kMaximumGroupingGap = 16;
	constexpr size_t kScanCapacity = kMaximumSearchDistance * 2 + 1;
	constexpr uint32_t kMaximumDebugLogEntries = 256;

	uint32_t g_maxSearchDistance = kMinimumSearchDistance;
	uint32_t g_maxGroupingGap = 0;
	uint32_t g_remainingDebugLogEntries = 0;

	using FindHighwayReturnTile = void (__thiscall*)(
		void* pTrafficSimulator,
		int32_t* pX,
		int32_t* pZ,
		bool reverseSearch);
	using GetTrafficNeighborConnection = void* (__thiscall*)(
		void* pTrafficSimulator,
		int32_t x,
		int32_t z);

	FindHighwayReturnTile const pFindHighwayReturnTile =
		reinterpret_cast<FindHighwayReturnTile>(kFindHighwayReturnTileAddress);
	GetTrafficNeighborConnection const pGetTrafficNeighborConnection =
		reinterpret_cast<GetTrafficNeighborConnection>(kGetTrafficNeighborConnectionAddress);

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
		uint16_t pathMatrix = 0;
		PathDirection direction = PathDirection::None;
		bool valid = false;
		bool isRHW = false;
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
				bands[bandCount++] = Band{tileCount, 0};
			}

			tiles[tileCount++] = tile;
			++bands[bandCount - 1].tileCount;
		}
	};

	uint16_t GetNeighborConnectionMask(void* const pTrafficSimulator, const int32_t x, const int32_t z)
	{
		const void* const pConnection = pGetTrafficNeighborConnection(pTrafficSimulator, x, z);
		return pConnection ? *static_cast<const uint16_t*>(pConnection) : 0;
	}

	int32_t ReadSimulatorInt32(void* const pTrafficSimulator, const size_t offset)
	{
		const auto* const bytes = static_cast<const uint8_t*>(pTrafficSimulator);
		return *reinterpret_cast<const int32_t*>(bytes + offset);
	}

	bool TryGetBoundaryCellAndEdge(
		void* const pTrafficSimulator,
		const int32_t x,
		const int32_t z,
		int32_t& cellX,
		int32_t& cellZ,
		uint32_t& edge)
	{
		const int32_t maxX = ReadSimulatorInt32(pTrafficSimulator, kMaximumXOffset);
		const int32_t maxZ = ReadSimulatorInt32(pTrafficSimulator, kMaximumZOffset);
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

	PathDirection GetPathDirection(
		void* const pTrafficSimulator,
		const int32_t x,
		const int32_t z,
		uint16_t& pathMatrix)
	{
		int32_t cellX = 0;
		int32_t cellZ = 0;
		uint32_t boundaryEdge = 0;
		if (!TryGetBoundaryCellAndEdge(
			pTrafficSimulator,
			x,
			z,
			cellX,
			cellZ,
			boundaryEdge))
		{
			return PathDirection::None;
		}

		const auto* const simulatorBytes = static_cast<const uint8_t*>(pTrafficSimulator);
		const auto* const pathCellData =
			*reinterpret_cast<const uint8_t* const*>(simulatorBytes + kPathCellDataOffset);
		if (!pathCellData)
		{
			return PathDirection::None;
		}

		const uint32_t cellIndex =
			(static_cast<uint32_t>(cellX) << 8) | static_cast<uint32_t>(cellZ);
		const auto* const pathMatrices = reinterpret_cast<const uint16_t*>(
			pathCellData + cellIndex * kPathCellSize);
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

	PathDirection GetOppositeDirection(const PathDirection direction)
	{
		return direction == PathDirection::Inbound
			? PathDirection::Outbound
			: PathDirection::Inbound;
	}

	const char* GetDirectionName(const PathDirection direction)
	{
		switch (direction)
		{
		case PathDirection::Inbound:
			return "inbound";
		case PathDirection::Outbound:
			return "outbound";
		case PathDirection::Bidirectional:
			return "bidirectional";
		default:
			return "none";
		}
	}

	void WriteDebugLog(
		const EdgeTile& origin,
		const int32_t resultX,
		const int32_t resultZ,
		const size_t sourceBandCount,
		const size_t targetBandCount,
		const char* const outcome)
	{
		if (g_remainingDebugLogEntries == 0)
		{
			return;
		}

		--g_remainingDebugLogEntries;
		Logger::GetInstance().WriteLineFormatted(
			LogLevel::Error,
			"[RHW neighbor debug] origin=(%d,%d) matrix=0x%04X direction=%s result=(%d,%d) "
			"sourceBands=%u targetBands=%u outcome=%s",
			origin.x,
			origin.z,
			origin.pathMatrix,
			GetDirectionName(origin.direction),
			resultX,
			resultZ,
			static_cast<uint32_t>(sourceBandCount),
			static_cast<uint32_t>(targetBandCount),
			outcome);
	}

	EdgeTile ReadEdgeTile(
		void* const pTrafficSimulator,
		const int32_t originX,
		const int32_t originZ,
		const int32_t stepX,
		const int32_t stepZ,
		const int32_t offset)
	{
		EdgeTile tile;
		tile.x = originX + stepX * offset;
		tile.z = originZ + stepZ * offset;
		tile.offset = offset;

		int32_t cellX = 0;
		int32_t cellZ = 0;
		uint32_t edge = 0;
		tile.valid = TryGetBoundaryCellAndEdge(
			pTrafficSimulator,
			tile.x,
			tile.z,
			cellX,
			cellZ,
			edge);
		if (!tile.valid)
		{
			return tile;
		}

		tile.isRHW =
			(GetNeighborConnectionMask(pTrafficSimulator, tile.x, tile.z) &
			 kDirtRoadNeighborConnectionMask) != 0;
		if (tile.isRHW)
		{
			tile.direction = GetPathDirection(
				pTrafficSimulator,
				tile.x,
				tile.z,
				tile.pathMatrix);
		}
		return tile;
	}

	const EdgeTile& GetScannedTile(
		const std::array<EdgeTile, kScanCapacity>& scannedTiles,
		const int32_t offset)
	{
		return scannedTiles[static_cast<size_t>(
			offset + static_cast<int32_t>(kMaximumSearchDistance))];
	}

	int32_t FindSourceExtent(
		const std::array<EdgeTile, kScanCapacity>& scannedTiles,
		const PathDirection sourceDirection,
		const int32_t increment)
	{
		int32_t extent = 0;
		uint32_t gap = 0;
		for (int32_t distance = 1;
			distance <= static_cast<int32_t>(g_maxSearchDistance);
			++distance)
		{
			const int32_t offset = distance * increment;
			const EdgeTile& tile = GetScannedTile(scannedTiles, offset);
			if (!tile.valid)
			{
				break;
			}

			if (!tile.isRHW)
			{
				if (++gap > g_maxGroupingGap)
				{
					break;
				}
				continue;
			}

			if (tile.direction != sourceDirection || gap > g_maxGroupingGap)
			{
				break;
			}

			extent = offset;
			gap = 0;
		}
		return extent;
	}

	bool FindTargetStart(
		const std::array<EdgeTile, kScanCapacity>& scannedTiles,
		const PathDirection sourceDirection,
		const PathDirection targetDirection,
		int32_t& targetStart)
	{
		for (int32_t offset = 1;
			offset <= static_cast<int32_t>(g_maxSearchDistance);
			++offset)
		{
			const EdgeTile& tile = GetScannedTile(scannedTiles, offset);
			if (!tile.valid)
			{
				break;
			}
			if (!tile.isRHW || tile.direction == sourceDirection)
			{
				continue;
			}
			if (tile.direction == targetDirection)
			{
				targetStart = offset;
				return true;
			}

			// A bidirectional or unclassified DirtRoad connection is a corridor
			// boundary, not something that should be searched through.
			break;
		}
		return false;
	}

	BandCollection BuildSourceBands(
		const std::array<EdgeTile, kScanCapacity>& scannedTiles,
		const PathDirection sourceDirection,
		const int32_t minimumOffset,
		const int32_t maximumOffset)
	{
		BandCollection result;
		for (int32_t offset = minimumOffset; offset <= maximumOffset; ++offset)
		{
			const EdgeTile& tile = GetScannedTile(scannedTiles, offset);
			if (tile.valid && tile.isRHW && tile.direction == sourceDirection)
			{
				result.Add(tile);
			}
		}
		return result;
	}

	BandCollection BuildTargetBands(
		const std::array<EdgeTile, kScanCapacity>& scannedTiles,
		const PathDirection targetDirection,
		const int32_t targetStart)
	{
		BandCollection result;
		uint32_t gap = 0;
		for (int32_t offset = targetStart;
			offset <= static_cast<int32_t>(g_maxSearchDistance);
			++offset)
		{
			const EdgeTile& tile = GetScannedTile(scannedTiles, offset);
			if (!tile.valid)
			{
				break;
			}

			if (!tile.isRHW)
			{
				if (++gap > g_maxGroupingGap)
				{
					break;
				}
				continue;
			}

			if (tile.direction != targetDirection || gap > g_maxGroupingGap)
			{
				break;
			}

			result.Add(tile);
			gap = 0;
		}
		return result;
	}

	bool FindTileInBands(
		const BandCollection& collection,
		const int32_t tileOffset,
		size_t& bandIndex,
		size_t& tileIndexInBand)
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

	void __fastcall FindRHWReturnTile(
		void* const pTrafficSimulator,
		void*,
		int32_t* const pX,
		int32_t* const pZ,
		const bool reverseSearch)
	{
		const int32_t originX = *pX;
		const int32_t originZ = *pZ;
		const uint16_t originMask = GetNeighborConnectionMask(pTrafficSimulator, originX, originZ);

		pFindHighwayReturnTile(pTrafficSimulator, pX, pZ, reverseSearch);

		if ((originMask & kDirtRoadNeighborConnectionMask) == 0)
		{
			return;
		}

		const int32_t stepX = *pX - originX;
		const int32_t stepZ = *pZ - originZ;
		if ((stepX == 0 && stepZ == 0) || (stepX != 0 && stepZ != 0))
		{
			*pX = originX;
			*pZ = originZ;
			return;
		}

		std::array<EdgeTile, kScanCapacity> scannedTiles{};
		for (int32_t offset = -static_cast<int32_t>(g_maxSearchDistance);
			offset <= static_cast<int32_t>(g_maxSearchDistance);
			++offset)
		{
			scannedTiles[static_cast<size_t>(
				offset + static_cast<int32_t>(kMaximumSearchDistance))] =
				ReadEdgeTile(
					pTrafficSimulator,
					originX,
					originZ,
					stepX,
					stepZ,
					offset);
		}

		const EdgeTile& origin = GetScannedTile(scannedTiles, 0);
		if (origin.direction != PathDirection::Inbound &&
			origin.direction != PathDirection::Outbound)
		{
			*pX = originX;
			*pZ = originZ;
			WriteDebugLog(origin, *pX, *pZ, 0, 0, "origin-not-one-way");
			return;
		}

		const PathDirection targetDirection = GetOppositeDirection(origin.direction);
		int32_t targetStart = 0;
		if (!FindTargetStart(scannedTiles, origin.direction, targetDirection, targetStart))
		{
			*pX = originX;
			*pZ = originZ;
			WriteDebugLog(origin, *pX, *pZ, 0, 0, "no-opposite-direction");
			return;
		}

		const int32_t negativeSourceExtent =
			FindSourceExtent(scannedTiles, origin.direction, -1);
		const int32_t positiveSourceExtent =
			FindSourceExtent(scannedTiles, origin.direction, 1);
		const BandCollection sourceBands = BuildSourceBands(
			scannedTiles,
			origin.direction,
			negativeSourceExtent,
			std::min(positiveSourceExtent, targetStart - 1));
		const BandCollection targetBands =
			BuildTargetBands(scannedTiles, targetDirection, targetStart);

		if (sourceBands.bandCount == 0 || targetBands.bandCount == 0)
		{
			*pX = originX;
			*pZ = originZ;
			WriteDebugLog(
				origin,
				*pX,
				*pZ,
				sourceBands.bandCount,
				targetBands.bandCount,
				"empty-band-group");
			return;
		}

		size_t sourceBandIndex = 0;
		size_t sourceTileIndex = 0;
		if (!FindTileInBands(sourceBands, 0, sourceBandIndex, sourceTileIndex))
		{
			*pX = originX;
			*pZ = originZ;
			WriteDebugLog(
				origin,
				*pX,
				*pZ,
				sourceBands.bandCount,
				targetBands.bandCount,
				"origin-not-in-source-band");
			return;
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

		*pX = result.x;
		*pZ = result.z;
		WriteDebugLog(
			origin,
			*pX,
			*pZ,
			sourceBands.bandCount,
			targetBands.bandCount,
			"paired");
	}
}

void RHWNeighborConnections::Install(
	const uint32_t maxSearchDistance,
	const uint32_t maxGroupingGap,
	const bool enableDebugLogging)
{
	g_maxSearchDistance = std::clamp(
		maxSearchDistance,
		kMinimumSearchDistance,
		kMaximumSearchDistance);
	g_maxGroupingGap = std::min(maxGroupingGap, kMaximumGroupingGap);
	g_remainingDebugLogEntries = enableDebugLogging ? kMaximumDebugLogEntries : 0;

	Patching::RedirectCall(
		kForwardPathFindReturnTileCallAddress,
		kFindHighwayReturnTileAddress,
		reinterpret_cast<void (*)(void)>(FindRHWReturnTile));
	Patching::RedirectCall(
		kReturnPathFindReturnTileCallAddress,
		kFindHighwayReturnTileAddress,
		reinterpret_cast<void (*)(void)>(FindRHWReturnTile));

	Patching::PatchTestWordPtrEaxImmediate16(
		kForwardPathNetworkMaskTestAddress,
		kVanillaDividedNetworkConnectionMask,
		kRHWDividedNetworkConnectionMask);
	Patching::PatchTestWordPtrEaxImmediate16(
		kReturnPathNetworkMaskTestAddress,
		kVanillaDividedNetworkConnectionMask,
		kRHWDividedNetworkConnectionMask);
}
