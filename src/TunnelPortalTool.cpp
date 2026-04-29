#include "TunnelPortalTool.h"

#include "Logger.h"
#include "NetworkStubs.h"
#include "cISC4App.h"
#include "cISC4City.h"
#include "cISC4NetworkManager.h"
#include "cISC4NetworkTool.h"
#include "cISC4Occupant.h"
#include "cISC4TrafficSimulator.h"
#include "cISC4View3DWin.h"
#include "cIGZUnknown.h"
#include "cRZAutoRefCount.h"
#include "cRZBaseString.h"
#include "cSC4BaseViewInputControl.h"
#include "GZCLSIDDefs.h"
#include "Patching.h"

#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace
{
	constexpr uint32_t kTunnelPortalViewInputControlID = 0x4A7B6E31;
	constexpr uint32_t kPrimaryCursorTextID = kTunnelPortalViewInputControlID + 1;
	constexpr uint32_t kSecondaryCursorTextID = kTunnelPortalViewInputControlID + 2;
	constexpr uint32_t kNetworkTunnelOccupantID = GZCLSID::kcSC4NetworkTunnelOccupant;
	constexpr size_t kNetworkToolPlacingModeOffset = 0x50;
	constexpr size_t kNetworkToolPlacementStageOffset = 0x208;
	constexpr size_t kNetworkToolFailureCodeOffset = 0x20c;
	constexpr size_t kNetworkToolTunnelExemplarsOffset = 0x2e4;
	constexpr size_t kNetworkToolTunnelRotationOffsetsOffset = 0x2f0;
	constexpr size_t kNetworkToolDefaultExemplarGroupOffset = 0x358;
	constexpr size_t kNetworkToolTunnelRecordsOffset = 0xe4;
	constexpr size_t kCellInfoInsertResultOffset = 0x54;
	constexpr uint32_t kTunnelCellNetworkFlag = 0x00080000;
	constexpr uint32_t kNorthSouthPortalEdges = 0x02000200;
	constexpr uint32_t kEastWestPortalEdges = 0x00020002;
	constexpr std::array<uint32_t, 4> kSurfaceApproachEdgeForTunnelDirection = {
		0x02000000, // tunnel faces north, surface approach is south
		0x00000002, // tunnel faces east, surface approach is west
		0x00000200, // tunnel faces south, surface approach is north
		0x00020000, // tunnel faces west, surface approach is east
	};

	constexpr uint32_t NetworkMask(cISC4NetworkOccupant::eNetworkType type)
	{
		return 1u << static_cast<uint32_t>(type);
	}

	const char* NetworkTypeName(cISC4NetworkOccupant::eNetworkType type)
	{
		switch (type)
		{
		case cISC4NetworkOccupant::Road: return "Road";
		case cISC4NetworkOccupant::Rail: return "Rail";
		case cISC4NetworkOccupant::Highway: return "Highway";
		case cISC4NetworkOccupant::Street: return "Street";
		case cISC4NetworkOccupant::Avenue: return "Avenue";
		case cISC4NetworkOccupant::LightRail: return "Light rail";
		case cISC4NetworkOccupant::Monorail: return "Monorail";
		case cISC4NetworkOccupant::OneWayRoad: return "One-way road";
		case cISC4NetworkOccupant::DirtRoad: return "Dirt road";
		case cISC4NetworkOccupant::GroundHighway: return "Ground highway";
		default: return "Network";
		}
	}

	constexpr std::array<cISC4NetworkOccupant::eNetworkType, 9> kCandidateNetworks = {
		cISC4NetworkOccupant::Road,
		cISC4NetworkOccupant::Street,
		cISC4NetworkOccupant::DirtRoad,
		cISC4NetworkOccupant::OneWayRoad,
		cISC4NetworkOccupant::Avenue,
		cISC4NetworkOccupant::GroundHighway,
		cISC4NetworkOccupant::Rail,
		cISC4NetworkOccupant::LightRail,
		cISC4NetworkOccupant::Monorail,
	};

	struct Endpoint
	{
		uint32_t x = 0;
		uint32_t z = 0;
		cISC4NetworkOccupant::eNetworkType networkType = cISC4NetworkOccupant::Road;
	};

	template <typename T>
	T& FieldAt(void* object, size_t offset)
	{
		return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(object) + offset);
	}

	template <typename T>
	const T& FieldAt(const void* object, size_t offset)
	{
		return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(object) + offset);
	}

	class NetworkToolPlacementStateScope
	{
	public:
		explicit NetworkToolPlacementStateScope(cSC4NetworkTool* tool)
			: tool(tool),
			  oldPlacingMode(tool ? FieldAt<uint8_t>(tool, kNetworkToolPlacingModeOffset) : 0),
			  oldPlacementStage(tool ? FieldAt<uint32_t>(tool, kNetworkToolPlacementStageOffset) : 0),
			  oldFailureCode(tool ? FieldAt<uint32_t>(tool, kNetworkToolFailureCodeOffset) : 0)
		{
			if (tool)
			{
				FieldAt<uint8_t>(tool, kNetworkToolPlacingModeOffset) = 1;
				FieldAt<uint32_t>(tool, kNetworkToolFailureCodeOffset) = 0;

				Logger::GetInstance().WriteLineFormatted(
					LogLevel::Trace,
					"TunnelPortalTool: entered network placement state, old placing=%u stage=%u failure=0x%08X; preserving stage.",
					static_cast<uint32_t>(oldPlacingMode),
					oldPlacementStage,
					oldFailureCode);
			}
		}

		~NetworkToolPlacementStateScope()
		{
			if (tool)
			{
				const uint32_t failureCode = FieldAt<uint32_t>(tool, kNetworkToolFailureCodeOffset);
				const uint32_t placementStage = FieldAt<uint32_t>(tool, kNetworkToolPlacementStageOffset);
				FieldAt<uint8_t>(tool, kNetworkToolPlacingModeOffset) = oldPlacingMode;
				FieldAt<uint32_t>(tool, kNetworkToolFailureCodeOffset) = oldFailureCode;

				Logger::GetInstance().WriteLineFormatted(
					LogLevel::Trace,
					"TunnelPortalTool: restored network placement flag, stage=%u placement failure was 0x%08X.",
					placementStage,
					failureCode);
			}
		}

	private:
		cSC4NetworkTool* tool;
		uint8_t oldPlacingMode;
		uint32_t oldPlacementStage;
		uint32_t oldFailureCode;
	};

	cISC4City* GetCity()
	{
		cISC4AppPtr app;
		return app ? app->GetCity() : nullptr;
	}

	void* GetTrafficNetworkMap()
	{
		cISC4City* const city = GetCity();
		return city ? reinterpret_cast<void*>(city->GetTrafficNetwork()) : nullptr;
	}

	cISC4TrafficSimulator* GetTrafficSimulator()
	{
		cISC4City* const city = GetCity();
		return city ? city->GetTrafficSimulator() : nullptr;
	}

	cSC4NetworkTool* GetNetworkTool(cISC4NetworkManager* networkManager, cISC4NetworkOccupant::eNetworkType networkType)
	{
		if (!networkManager)
		{
			return nullptr;
		}

		cISC4NetworkTool* tool = networkManager->GetNetworkTool(static_cast<int32_t>(networkType), true);
		if (!tool)
		{
			tool = networkManager->GetNetworkTool(static_cast<int32_t>(networkType), false);
		}

		if (tool)
		{
			tool->Init();
		}

		return reinterpret_cast<cSC4NetworkTool*>(tool);
	}

	// Queries the existing cell at (endpoint.x, endpoint.z) via GetCellInfo (0x633220),
	// which computes on demand from the live occupant manager without needing a drag scan.
	cSC4NetworkCellInfo* GetEndpointCell(
		cSC4NetworkTool* tool,
		const Endpoint& endpoint)
	{
		if (!tool)
		{
			return nullptr;
		}

		const uint32_t xz = (endpoint.z << 16) | endpoint.x;
		cSC4NetworkCellInfo* const cellInfo = tool->GetCellInfo(xz);
		if (!cellInfo)
		{
			return nullptr;
		}

		if ((cellInfo->networkTypeFlags & NetworkMask(endpoint.networkType)) == 0)
		{
			return nullptr;
		}

		return cellInfo;
	}

	bool TryFindNetworkAtTile(
		uint32_t x,
		uint32_t z,
		cISC4NetworkOccupant::eNetworkType& networkTypeOut)
	{
		Logger& logger = Logger::GetInstance();

		void* const trafficNetworkMap = GetTrafficNetworkMap();
		if (!trafficNetworkMap)
		{
			logger.WriteLineFormatted(LogLevel::Trace,
				"TunnelPortalTool: no traffic network map for cell (%u,%u).", x, z);
			return false;
		}

		using GetNetworkInfoFn = void* (__thiscall*)(void*, int32_t, int32_t, uint32_t, bool);
		auto** const vtable = *reinterpret_cast<void***>(trafficNetworkMap);
		if (!vtable || !vtable[8])
		{
			return false;
		}
		const auto getNetworkInfo = reinterpret_cast<GetNetworkInfoFn>(vtable[8]);

		for (const cISC4NetworkOccupant::eNetworkType networkType : kCandidateNetworks)
		{
			void* const entry = getNetworkInfo(
				trafficNetworkMap,
				static_cast<int32_t>(x),
				static_cast<int32_t>(z),
				NetworkMask(networkType),
				true);
			if (entry)
			{
				logger.WriteLineFormatted(LogLevel::Trace,
					"TunnelPortalTool: found %s at (%u,%u).", NetworkTypeName(networkType), x, z);
				networkTypeOut = networkType;
				return true;
			}
		}

		logger.WriteLineFormatted(LogLevel::Trace,
			"TunnelPortalTool: no candidate network at (%u,%u).", x, z);
		return false;
	}

	uint8_t InferTunnelPieceDirection(const Endpoint& from, const Endpoint& to)
	{
		const int32_t dx = static_cast<int32_t>(to.x) - static_cast<int32_t>(from.x);
		const int32_t dz = static_cast<int32_t>(to.z) - static_cast<int32_t>(from.z);

		// InsertTunnelPieces uses a tunnel-piece rotation code, not the
		// kNextX/kNextZ direction numbering used by path code.
		if (std::abs(dx) >= std::abs(dz))
		{
			return dx >= 0 ? 1 : 3;
		}

		return dz >= 0 ? 2 : 0;
	}

	uint32_t GetPortalAxisEdges(uint8_t tunnelPieceDirection)
	{
		return (tunnelPieceDirection & 1) != 0
			? kEastWestPortalEdges
			: kNorthSouthPortalEdges;
	}

	uint8_t TunnelPieceDirectionToPathDirection(uint8_t tunnelPieceDirection)
	{
		// Tunnel-piece rotations are 0=N, 1=E, 2=S, 3=W.
		// Path keys use kNextX/kNextZ numbering: 0=W, 1=N, 2=E, 3=S.
		static constexpr std::array<uint8_t, 4> kPathDirectionForTunnelPieceDirection = { 1, 2, 3, 0 };
		return kPathDirectionForTunnelPieceDirection[tunnelPieceDirection & 3];
	}

	uint16_t TunnelPathKeyLowWord(uint8_t pathDirection)
	{
		const uint8_t direction = pathDirection & 3;
		return static_cast<uint16_t>(((direction ^ 2) << 8) | direction);
	}

	bool TryInferTunnelPieceDirectionFromSurfaceApproach(
		const char* label,
		const cSC4NetworkCellInfo* cellInfo,
		cISC4NetworkOccupant::eNetworkType networkType,
		uint8_t& directionOut)
	{
		if (!cellInfo)
		{
			return false;
		}

		const uint32_t edgeFlags = cellInfo->edgesPerNetwork[static_cast<uint32_t>(networkType)];
		uint32_t matchedEdge = 0;
		uint8_t matchedDirection = 0;
		uint32_t matchCount = 0;

		for (uint8_t direction = 0; direction < kSurfaceApproachEdgeForTunnelDirection.size(); ++direction)
		{
			const uint32_t edge = kSurfaceApproachEdgeForTunnelDirection[direction];
			if ((edgeFlags & edge) != 0)
			{
				matchedDirection = direction;
				matchedEdge = edge;
				++matchCount;
			}
		}

		if (matchCount == 1)
		{
			directionOut = matchedDirection;
			Logger::GetInstance().WriteLineFormatted(
				LogLevel::Trace,
				"TunnelPortalTool: inferred %s tunnel direction=%u from single surface approach edge 0x%08X (networkEdge=0x%08X).",
				label,
				static_cast<uint32_t>(directionOut),
				matchedEdge,
				edgeFlags);
			return true;
		}

		Logger::GetInstance().WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: could not infer %s tunnel direction from surface approach, matches=%u networkEdge=0x%08X.",
			label,
			matchCount,
			edgeFlags);
		return false;
	}

	using InsertTunnelPieceFn = cISC4NetworkOccupant* (__thiscall*)(
		cSC4NetworkTool* tool,
		uint8_t direction,
		uint8_t sequenceIndex,
		cSC4NetworkCellInfo* cellInfo);

	using SetOtherEndOccupantFn = void (__thiscall*)(cIGZUnknown* self, cISC4NetworkOccupant* otherEnd);
	using GetPathInfoFn = void* (__thiscall*)(cIGZUnknown* self);
	using InitTunnelPathFn = void (__thiscall*)(void* pathInfo, cIGZUnknown* self, cIGZUnknown* otherEnd);
	using DoTunnelChangedFn = void (__thiscall*)(cISC4TrafficSimulator* trafficSimulator, cIGZUnknown* tunnelOccupant, bool added);
	using DoConnectionsChangedFn = void (__thiscall*)(cISC4TrafficSimulator* trafficSimulator, uint32_t startX, uint32_t startZ, uint32_t endX, uint32_t endZ);
	using AddTrafficConnectionFn = int (__thiscall*)(
		cISC4TrafficSimulator* trafficSimulator,
		int x,
		uint32_t z,
		int fromDirection,
		int toDirection,
		int travelMode);
	using AddTripNodeFn = void (__thiscall*)(
		void* pathFinder,
		uint32_t x,
		uint32_t z,
		uint8_t edge,
		uint8_t travelMode,
		float cost,
		int currentNode,
		float heuristic,
		char outOfBounds);
	using GetOccupantExemplarFn = cIGZUnknown* (__thiscall*)(cSC4NetworkTool* tool, uint32_t exemplarId, uint32_t groupId);

	// When non-0xFF, Hook_InitTunnelPath replaces MakeTunnelPaths' coordinate-derived
	// path direction with this path-key direction. This uses kNextX/kNextZ numbering:
	// 0=west, 1=north, 2=east, 3=south.
	uint8_t sCustomPortalFacingOverride = 0xFF;
	uint32_t sInitTunnelPathHookHitCount = 0;
	uint32_t sInitTunnelPathOverrideCount = 0;
	uint8_t sLastInitTunnelPathOriginalDirection = 0xFF;
	uint8_t sLastInitTunnelPathOverrideDirection = 0xFF;
	uint16_t sCustomPeerPathLookupKeyLowWord = 0xFFFF;
	uint32_t sPeerPathLookupHookHitCount = 0;
	uint32_t sPeerPathLookupOverrideCount = 0;
	uint32_t sLastPeerPathLookupOriginalKey = 0;
	uint32_t sLastPeerPathLookupOverrideKey = 0;
	bool sTraceTrafficEdgeInsertions = false;
	bool sSuppressConnectionsChangedEntryTrace = false;
	Endpoint sTraceFirstEndpoint;
	Endpoint sTraceSecondEndpoint;

	InsertTunnelPieceFn InsertTunnelPiece = reinterpret_cast<InsertTunnelPieceFn>(0x00628390);
	AddTripNodeFn AddTripNode = reinterpret_cast<AddTripNodeFn>(0x006d8fa0);
	SetOtherEndOccupantFn SetOtherEndOccupant = reinterpret_cast<SetOtherEndOccupantFn>(0x00647530);
	DoTunnelChangedFn DoTunnelChanged = reinterpret_cast<DoTunnelChangedFn>(0x007140e0);
	DoConnectionsChangedFn DoConnectionsChanged = reinterpret_cast<DoConnectionsChangedFn>(0x0071a860);
	GetOccupantExemplarFn GetOccupantExemplar = reinterpret_cast<GetOccupantExemplarFn>(0x006244b0);

	uint32_t sCustomTunnelInsertionDepth = 0;

	struct CustomTunnelRouteEdgeFix
	{
		Endpoint first;
		Endpoint second;
		uint8_t firstArrivalEdge = 0xFF;
		uint8_t secondArrivalEdge = 0xFF;
		bool mixedAxis = false;
		bool active = false;
	};

	std::array<CustomTunnelRouteEdgeFix, 16> sCustomTunnelRouteEdgeFixes{};
	uint32_t sNextCustomTunnelRouteEdgeFix = 0;

	class CustomTunnelInsertionScope
	{
	public:
		CustomTunnelInsertionScope()
		{
			++sCustomTunnelInsertionDepth;
		}

		~CustomTunnelInsertionScope()
		{
			--sCustomTunnelInsertionDepth;
		}
	};

	bool SameCell(const Endpoint& endpoint, uint32_t x, uint32_t z)
	{
		return endpoint.x == x && endpoint.z == z;
	}

	void RegisterCustomTunnelRouteEdgeFix(
		const Endpoint& first,
		const Endpoint& second,
		uint8_t firstArrivalEdge,
		uint8_t secondArrivalEdge,
		bool mixedAxis)
	{
		CustomTunnelRouteEdgeFix& fix =
			sCustomTunnelRouteEdgeFixes[sNextCustomTunnelRouteEdgeFix % sCustomTunnelRouteEdgeFixes.size()];
		fix.first = first;
		fix.second = second;
		fix.firstArrivalEdge = firstArrivalEdge & 3;
		fix.secondArrivalEdge = secondArrivalEdge & 3;
		fix.mixedAxis = mixedAxis;
		fix.active = true;
		++sNextCustomTunnelRouteEdgeFix;

		Logger::GetInstance().WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: registered pathfinder tunnel edge fix first=(%u,%u) arrivalEdge=%u second=(%u,%u) arrivalEdge=%u mixedAxis=%u.",
			first.x,
			first.z,
			static_cast<uint32_t>(fix.firstArrivalEdge),
			second.x,
			second.z,
			static_cast<uint32_t>(fix.secondArrivalEdge),
			fix.mixedAxis ? 1u : 0u);
	}

	bool TryGetCustomTunnelArrivalEdge(
		uint32_t currentX,
		uint32_t currentZ,
		uint32_t destinationX,
		uint32_t destinationZ,
		uint8_t& edgeOut)
	{
		for (const CustomTunnelRouteEdgeFix& fix : sCustomTunnelRouteEdgeFixes)
		{
			if (!fix.active)
			{
				continue;
			}
			if (!fix.mixedAxis)
			{
				continue;
			}

			if (SameCell(fix.first, currentX, currentZ) && SameCell(fix.second, destinationX, destinationZ))
			{
				edgeOut = fix.secondArrivalEdge;
				return true;
			}
			if (SameCell(fix.second, currentX, currentZ) && SameCell(fix.first, destinationX, destinationZ))
			{
				edgeOut = fix.firstArrivalEdge;
				return true;
			}
		}

		return false;
	}

	size_t VectorCountAt(const void* object, size_t offset, size_t itemSize)
	{
		const uint8_t* const base = reinterpret_cast<const uint8_t*>(object) + offset;
		const auto start = *reinterpret_cast<const uintptr_t*>(base);
		const auto end = *reinterpret_cast<const uintptr_t*>(base + sizeof(uintptr_t));

		if (start == 0 || end < start || itemSize == 0)
		{
			return 0;
		}

		return static_cast<size_t>((end - start) / itemSize);
	}

	constexpr size_t kPathInfoPathMapOffset = 0x1c;
	constexpr size_t kPathPointSize = 12;
	constexpr size_t kMaxPathInfoBucketsToTrace = 4096;
	constexpr size_t kMaxPathInfoNodesToTrace = 16384;

	struct RawPathVector
	{
		uint8_t* start;
		uint8_t* end;
		uint8_t* capacity;
	};

	struct RawPathMapNode
	{
		RawPathMapNode* next;
		uint32_t key;
		RawPathVector path;
	};

	struct RawPathMap
	{
		uint32_t reserved;
		RawPathMapNode** start;
		RawPathMapNode** end;
		RawPathMapNode** capacity;
	};

	static_assert(sizeof(RawPathVector) == 0x0c);
	static_assert(sizeof(RawPathMapNode) == 0x14);
	static_assert(sizeof(RawPathMap) == 0x10);

	constexpr size_t kTrafficSimulatorTunnelMapOffset = 0xc8;
	constexpr size_t kTrafficSimulatorTunnelListOffset = 0x104;
	constexpr size_t kMaxTunnelMapBucketsToTrace = 4096;
	constexpr size_t kMaxTunnelMapNodesToTrace = 16384;
	constexpr size_t kMaxTunnelListNodesToTrace = 1024;

	struct RawTunnelMapNode
	{
		RawTunnelMapNode* next;
		uint16_t key;
		uint16_t padding;
		uint32_t value[11];
	};

	struct RawTunnelMap
	{
		uint32_t reserved;
		RawTunnelMapNode** start;
		RawTunnelMapNode** end;
		RawTunnelMapNode** capacity;
		uint32_t size;
	};

	static_assert(sizeof(RawTunnelMapNode) == 0x34);
	static_assert(sizeof(RawTunnelMap) == 0x14);

	struct RawTunnelListNode
	{
		RawTunnelListNode* next;
		RawTunnelListNode* previous;
		uint32_t value[11];
	};

	static_assert(offsetof(RawTunnelListNode, value) == 0x08);

	const RawPathMap* GetPathMap(void* pathInfo)
	{
		return pathInfo
			? reinterpret_cast<const RawPathMap*>(reinterpret_cast<const uint8_t*>(pathInfo) + kPathInfoPathMapOffset)
			: nullptr;
	}

	bool IsTraceablePathMap(const RawPathMap* map)
	{
		if (!map || !map->start || !map->end || map->end < map->start)
		{
			return false;
		}

		const uintptr_t bucketCount = static_cast<uintptr_t>(map->end - map->start);
		return bucketCount > 0 && bucketCount <= kMaxPathInfoBucketsToTrace;
	}

	const RawTunnelMap* GetTunnelMap(cISC4TrafficSimulator* trafficSimulator)
	{
		return trafficSimulator
			? reinterpret_cast<const RawTunnelMap*>(reinterpret_cast<const uint8_t*>(trafficSimulator) + kTrafficSimulatorTunnelMapOffset)
			: nullptr;
	}

	RawTunnelListNode* GetTunnelListSentinel(cISC4TrafficSimulator* trafficSimulator)
	{
		return trafficSimulator
			? *reinterpret_cast<RawTunnelListNode**>(reinterpret_cast<uint8_t*>(trafficSimulator) + kTrafficSimulatorTunnelListOffset)
			: nullptr;
	}

	bool IsTraceableTunnelMap(const RawTunnelMap* map)
	{
		if (!map || !map->start || !map->end || map->end < map->start)
		{
			return false;
		}

		const uintptr_t bucketCount = static_cast<uintptr_t>(map->end - map->start);
		return bucketCount > 0 && bucketCount <= kMaxTunnelMapBucketsToTrace;
	}

	uint32_t CountRawPathPoints(const RawPathVector& path)
	{
		if (!path.start || !path.end || path.end < path.start)
		{
			return 0;
		}

		return static_cast<uint32_t>((path.end - path.start) / kPathPointSize);
	}

	struct RawPathPoint
	{
		float x;
		float y;
		float z;
	};
	static_assert(sizeof(RawPathPoint) == kPathPointSize);

	const RawPathPoint* FirstRawPathPoint(const RawPathVector& path)
	{
		return CountRawPathPoints(path) > 0 ? reinterpret_cast<const RawPathPoint*>(path.start) : nullptr;
	}

	const RawPathPoint* LastRawPathPoint(const RawPathVector& path)
	{
		const uint32_t count = CountRawPathPoints(path);
		return count > 0 ? reinterpret_cast<const RawPathPoint*>(path.start) + (count - 1) : nullptr;
	}

	const RawPathMapNode* FindRawPathKey(const RawPathMap* map, uint32_t key)
	{
		if (!IsTraceablePathMap(map) || map->start == map->end)
		{
			return nullptr;
		}

		const uintptr_t bucketCount = static_cast<uintptr_t>(map->end - map->start);
		for (RawPathMapNode* node = map->start[key % bucketCount], *next = nullptr; node; node = next)
		{
			next = node->next;
			if (node->key == key)
			{
				return node;
			}
		}

		return nullptr;
	}

	float FacingScore2D(const RawPathPoint& point, uint8_t tunnelPieceDirection)
	{
		switch (tunnelPieceDirection & 3)
		{
		case 0: return -point.z;
		case 1: return point.x;
		case 2: return point.z;
		case 3: return -point.x;
		default: return 0.0f;
		}
	}

	const RawPathMapNode* FindPathWithMouthPoint(
		void* pathInfo,
		uint8_t tunnelPieceDirection,
		bool useLastPoint)
	{
		const RawPathMap* const map = GetPathMap(pathInfo);
		if (!IsTraceablePathMap(map))
		{
			return nullptr;
		}

		const RawPathMapNode* bestNode = nullptr;
		float bestScore = -3.4e38f;
		uint32_t totalKeys = 0;
		for (RawPathMapNode** bucket = map->start; bucket != map->end && totalKeys < kMaxPathInfoNodesToTrace; ++bucket)
		{
			for (RawPathMapNode* node = *bucket, *next = nullptr; node && totalKeys < kMaxPathInfoNodesToTrace; node = next)
			{
				next = node->next;
				++totalKeys;
				const RawPathPoint* const point = useLastPoint
					? LastRawPathPoint(node->path)
					: FirstRawPathPoint(node->path);
				if (!point)
				{
					continue;
				}

				const float score = FacingScore2D(*point, tunnelPieceDirection);
				if (!bestNode || score > bestScore)
				{
					bestNode = node;
					bestScore = score;
				}
			}
		}

		return bestNode;
	}

	void TracePathKeyDetails(const char* label, void* pathInfo, uint32_t key)
	{
		const RawPathMapNode* const node = FindRawPathKey(GetPathMap(pathInfo), key);
		const RawPathPoint* const first = node ? FirstRawPathPoint(node->path) : nullptr;
		const RawPathPoint* const last = node ? LastRawPathPoint(node->path) : nullptr;
		Logger::GetInstance().WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: %s path key detail pathInfo=%p key=0x%08X found=%u points=%u first=(%.1f,%.1f,%.1f) last=(%.1f,%.1f,%.1f).",
			label,
			pathInfo,
			key,
			node ? 1u : 0u,
			node ? CountRawPathPoints(node->path) : 0u,
			first ? first->x : 0.0f,
			first ? first->y : 0.0f,
			first ? first->z : 0.0f,
			last ? last->x : 0.0f,
			last ? last->y : 0.0f,
			last ? last->z : 0.0f);
	}

	uint32_t ReplacePathKeyLowWord(uint32_t key, uint16_t lowWord)
	{
		return (key & 0xffff0000u) | lowWord;
	}

	void TracePathInfoKeyMap(const char* label, void* pathInfo, void* peerPathInfo, uint8_t pathDirection)
	{
		Logger& logger = Logger::GetInstance();
		const RawPathMap* const map = GetPathMap(pathInfo);
		const RawPathMap* const peerMap = GetPathMap(peerPathInfo);

		if (!IsTraceablePathMap(map))
		{
			const uintptr_t start = map ? reinterpret_cast<uintptr_t>(map->start) : 0;
			const uintptr_t end = map ? reinterpret_cast<uintptr_t>(map->end) : 0;
			logger.WriteLineFormatted(
				LogLevel::Trace,
				"TunnelPortalTool: %s path key scan skipped, pathInfo=%p map=%p reserved=0x%08X start=%p end=%p capacity=%p.",
				label,
				pathInfo,
				map,
				map ? map->reserved : 0,
				reinterpret_cast<void*>(start),
				reinterpret_cast<void*>(end),
				map ? map->capacity : nullptr);
			return;
		}

		const uintptr_t bucketCount = static_cast<uintptr_t>(map->end - map->start);
		uint32_t totalKeys = 0;
		uint32_t matchingKeys = 0;
		uint32_t matchingNonEmpty = 0;
		uint32_t peerExactMatches = 0;
		uint32_t peerExactNonEmpty = 0;
		uint32_t firstAllKeys[4] = {};
		uint32_t firstMatchKeys[4] = {};
		uint32_t firstMissingPeerKey = 0;
		const RawPathMapNode* firstMatchingNode = nullptr;

		for (RawPathMapNode** bucket = map->start; bucket != map->end && totalKeys < kMaxPathInfoNodesToTrace; ++bucket)
		{
			for (RawPathMapNode* node = *bucket, *next = nullptr; node && totalKeys < kMaxPathInfoNodesToTrace; node = next)
			{
				next = node->next;
				if (totalKeys < 4)
				{
					firstAllKeys[totalKeys] = node->key;
				}
				++totalKeys;

				if (static_cast<uint8_t>(node->key) != pathDirection)
				{
					continue;
				}

				if (matchingKeys < 4)
				{
					firstMatchKeys[matchingKeys] = node->key;
				}

				++matchingKeys;
				if (!firstMatchingNode)
				{
					firstMatchingNode = node;
				}
				if (CountRawPathPoints(node->path) > 0)
				{
					++matchingNonEmpty;
				}

				const RawPathMapNode* const peerNode = FindRawPathKey(peerMap, node->key);
				if (peerNode)
				{
					++peerExactMatches;
					if (CountRawPathPoints(peerNode->path) > 0)
					{
						++peerExactNonEmpty;
					}
				}
				else if (firstMissingPeerKey == 0)
				{
					firstMissingPeerKey = node->key;
				}
			}
		}

		const RawPathPoint* const selfFirst = firstMatchingNode ? FirstRawPathPoint(firstMatchingNode->path) : nullptr;
		const RawPathPoint* const selfLast = firstMatchingNode ? LastRawPathPoint(firstMatchingNode->path) : nullptr;
		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: %s path key scan pathInfo=%p peerPathInfo=%p direction=%u buckets=%u capacity=%p totalKeys=%u allKeys=%08X/%08X/%08X/%08X matchingKeys=%u matchingNonEmpty=%u peerExactMatches=%u peerExactNonEmpty=%u firstKeys=%08X/%08X/%08X/%08X firstMissingPeerKey=%08X selfPoints=%u selfFirst=(%.1f,%.1f,%.1f) selfLast=(%.1f,%.1f,%.1f).",
			label,
			pathInfo,
			peerPathInfo,
			static_cast<uint32_t>(pathDirection),
			static_cast<uint32_t>(bucketCount),
			map->capacity,
			totalKeys,
			firstAllKeys[0],
			firstAllKeys[1],
			firstAllKeys[2],
			firstAllKeys[3],
			matchingKeys,
			matchingNonEmpty,
			peerExactMatches,
			peerExactNonEmpty,
			firstMatchKeys[0],
			firstMatchKeys[1],
			firstMatchKeys[2],
			firstMatchKeys[3],
			firstMissingPeerKey,
			firstMatchingNode ? CountRawPathPoints(firstMatchingNode->path) : 0,
			selfFirst ? selfFirst->x : 0.0f,
			selfFirst ? selfFirst->y : 0.0f,
			selfFirst ? selfFirst->z : 0.0f,
			selfLast ? selfLast->x : 0.0f,
			selfLast ? selfLast->y : 0.0f,
			selfLast ? selfLast->z : 0.0f);
	}

	uint16_t PackedCellKey(uint32_t x, uint32_t z)
	{
		return static_cast<uint16_t>(((x & 0xff) << 8) | (z & 0xff));
	}

	uint16_t PackedCellKeyZX(uint32_t x, uint32_t z)
	{
		return static_cast<uint16_t>(((z & 0xff) << 8) | (x & 0xff));
	}

	void TraceTunnelRecordMap(
		const char* label,
		cISC4TrafficSimulator* trafficSimulator,
		const Endpoint& first,
		const Endpoint& second)
	{
		Logger& logger = Logger::GetInstance();
		const RawTunnelMap* const map = GetTunnelMap(trafficSimulator);
		if (!IsTraceableTunnelMap(map))
		{
			logger.WriteLineFormatted(
				LogLevel::Trace,
				"TunnelPortalTool: %s tunnel map scan skipped, trafficSimulator=%p map=%p.",
				label,
				trafficSimulator,
				map);
			return;
		}

		const uint16_t firstXZ = PackedCellKey(first.x, first.z);
		const uint16_t firstZX = PackedCellKeyZX(first.x, first.z);
		const uint16_t secondXZ = PackedCellKey(second.x, second.z);
		const uint16_t secondZX = PackedCellKeyZX(second.x, second.z);
		const uintptr_t bucketCount = static_cast<uintptr_t>(map->end - map->start);
		uint32_t totalKeys = 0;
		uint32_t firstXZCount = 0;
		uint32_t firstZXCount = 0;
		uint32_t secondXZCount = 0;
		uint32_t secondZXCount = 0;
		uint16_t sampleKeys[8] = {};

		for (RawTunnelMapNode** bucket = map->start; bucket != map->end && totalKeys < kMaxTunnelMapNodesToTrace; ++bucket)
		{
			for (RawTunnelMapNode* node = *bucket, *next = nullptr; node && totalKeys < kMaxTunnelMapNodesToTrace; node = next)
			{
				next = node->next;
				if (totalKeys < 8)
				{
					sampleKeys[totalKeys] = node->key;
				}
				++totalKeys;

				if (node->key == firstXZ) ++firstXZCount;
				if (node->key == firstZX) ++firstZXCount;
				if (node->key == secondXZ) ++secondXZCount;
				if (node->key == secondZX) ++secondZXCount;
			}
		}

		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: %s tunnel map scan map=%p buckets=%u size=%u totalKeys=%u firstXZ=0x%04X count=%u firstZX=0x%04X count=%u secondXZ=0x%04X count=%u secondZX=0x%04X count=%u sample=%04X/%04X/%04X/%04X/%04X/%04X/%04X/%04X.",
			label,
			map,
			static_cast<uint32_t>(bucketCount),
			map->size,
			totalKeys,
			firstXZ,
			firstXZCount,
			firstZX,
			firstZXCount,
			secondXZ,
			secondXZCount,
			secondZX,
			secondZXCount,
			sampleKeys[0],
			sampleKeys[1],
			sampleKeys[2],
			sampleKeys[3],
			sampleKeys[4],
			sampleKeys[5],
			sampleKeys[6],
			sampleKeys[7]);

		for (RawTunnelMapNode** bucket = map->start; bucket != map->end; ++bucket)
		{
			for (RawTunnelMapNode* node = *bucket; node; node = node->next)
			{
				if (node->key == firstXZ || node->key == secondXZ)
				{
					logger.WriteLineFormatted(
						LogLevel::Trace,
						"TunnelPortalTool: %s tunnel record key=0x%04X values=%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X.",
						label,
						node->key,
						node->value[0],
						node->value[1],
						node->value[2],
						node->value[3],
						node->value[4],
						node->value[5],
						node->value[6],
						node->value[7],
						node->value[8],
						node->value[9],
						node->value[10]);
				}
			}
		}
	}

	bool IsSameCellBytes(const uint8_t x, const uint8_t z, const Endpoint& endpoint)
	{
		return x == static_cast<uint8_t>(endpoint.x) && z == static_cast<uint8_t>(endpoint.z);
	}

	bool IsNearEndpointCell(uint32_t x, uint32_t z, const Endpoint& endpoint)
	{
		const uint32_t minX = endpoint.x == 0 ? 0 : endpoint.x - 1;
		const uint32_t minZ = endpoint.z == 0 ? 0 : endpoint.z - 1;
		return x >= minX && x <= endpoint.x + 1 && z >= minZ && z <= endpoint.z + 1;
	}

	bool IsTrafficEdgeTraceCell(uint32_t x, uint32_t z)
	{
		return IsNearEndpointCell(x, z, sTraceFirstEndpoint) || IsNearEndpointCell(x, z, sTraceSecondEndpoint);
	}

	void TraceTunnelConnectionList(
		const char* label,
		cISC4TrafficSimulator* trafficSimulator,
		const Endpoint& first,
		const Endpoint& second)
	{
		Logger& logger = Logger::GetInstance();
		RawTunnelListNode* const sentinel = GetTunnelListSentinel(trafficSimulator);
		if (!sentinel)
		{
			logger.WriteLineFormatted(
				LogLevel::Trace,
				"TunnelPortalTool: %s tunnel list scan skipped, trafficSimulator=%p sentinel=%p.",
				label,
				trafficSimulator,
				sentinel);
			return;
		}

		uint32_t totalRecords = 0;
		uint32_t matchingRecords = 0;
		for (RawTunnelListNode* node = sentinel->next; node && node != sentinel && totalRecords < kMaxTunnelListNodesToTrace; node = node->next)
		{
			++totalRecords;

			const uint8_t firstX = static_cast<uint8_t>(node->value[0]);
			const uint8_t firstZ = static_cast<uint8_t>(node->value[0] >> 8);
			const uint8_t secondX = static_cast<uint8_t>(node->value[0] >> 16);
			const uint8_t secondZ = static_cast<uint8_t>(node->value[0] >> 24);
			const bool matchesEndpoints =
				(IsSameCellBytes(firstX, firstZ, first) && IsSameCellBytes(secondX, secondZ, second)) ||
				(IsSameCellBytes(firstX, firstZ, second) && IsSameCellBytes(secondX, secondZ, first));

			if (matchesEndpoints || totalRecords <= 4)
			{
				if (matchesEndpoints)
				{
					++matchingRecords;
				}
				logger.WriteLineFormatted(
					LogLevel::Trace,
					"TunnelPortalTool: %s tunnel list record node=%p match=%u endpoints=(%u,%u)->(%u,%u) values=%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X.",
					label,
					node,
					static_cast<uint32_t>(matchesEndpoints),
					static_cast<uint32_t>(firstX),
					static_cast<uint32_t>(firstZ),
					static_cast<uint32_t>(secondX),
					static_cast<uint32_t>(secondZ),
					node->value[0],
					node->value[1],
					node->value[2],
					node->value[3],
					node->value[4],
					node->value[5],
					node->value[6],
					node->value[7],
					node->value[8],
					node->value[9],
					node->value[10]);
			}
		}

		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: %s tunnel list scan sentinel=%p totalRecords=%u matchingRecords=%u.",
			label,
			sentinel,
			totalRecords,
			matchingRecords);
	}

	void TraceAnyTunnelConnectionList(
		const char* label,
		cISC4TrafficSimulator* trafficSimulator)
	{
		if (sSuppressConnectionsChangedEntryTrace)
		{
			return;
		}

		Logger& logger = Logger::GetInstance();
		RawTunnelListNode* const sentinel = GetTunnelListSentinel(trafficSimulator);
		if (!sentinel)
		{
			return;
		}

		uint32_t totalRecords = 0;
		for (RawTunnelListNode* node = sentinel->next; node && node != sentinel && totalRecords < kMaxTunnelListNodesToTrace; node = node->next)
		{
			++totalRecords;
		}

		if (totalRecords == 0)
		{
			return;
		}

		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: %s native tunnel list sentinel=%p totalRecords=%u.",
			label,
			sentinel,
			totalRecords);

		uint32_t index = 0;
		for (RawTunnelListNode* node = sentinel->next; node && node != sentinel && index < 4; node = node->next, ++index)
		{
			const uint8_t firstX = static_cast<uint8_t>(node->value[0]);
			const uint8_t firstZ = static_cast<uint8_t>(node->value[0] >> 8);
			const uint8_t secondX = static_cast<uint8_t>(node->value[0] >> 16);
			const uint8_t secondZ = static_cast<uint8_t>(node->value[0] >> 24);
			logger.WriteLineFormatted(
				LogLevel::Trace,
				"TunnelPortalTool: %s native tunnel list[%u] node=%p endpoints=(%u,%u)->(%u,%u) values=%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X.",
				label,
				index,
				node,
				static_cast<uint32_t>(firstX),
				static_cast<uint32_t>(firstZ),
				static_cast<uint32_t>(secondX),
				static_cast<uint32_t>(secondZ),
				node->value[0],
				node->value[1],
				node->value[2],
				node->value[3],
				node->value[4],
				node->value[5],
				node->value[6],
				node->value[7],
				node->value[8],
				node->value[9],
				node->value[10]);
		}
	}

	const RawTunnelMapNode* FindTunnelRecord(
		cISC4TrafficSimulator* trafficSimulator,
		const Endpoint& endpoint)
	{
		const RawTunnelMap* const map = GetTunnelMap(trafficSimulator);
		if (!IsTraceableTunnelMap(map))
		{
			return nullptr;
		}

		const uint16_t key = PackedCellKey(endpoint.x, endpoint.z);
		const uintptr_t bucketCount = static_cast<uintptr_t>(map->end - map->start);
		for (RawTunnelMapNode* node = map->start[key % bucketCount]; node; node = node->next)
		{
			if (node->key == key)
			{
				return node;
			}
		}

		return nullptr;
	}

	uint16_t FindTunnelRecordMask(
		cISC4TrafficSimulator* trafficSimulator,
		const Endpoint& endpoint)
	{
		const RawTunnelMapNode* const node = FindTunnelRecord(trafficSimulator, endpoint);
		if (node)
		{
			return static_cast<uint16_t>(node->value[0] >> 16);
		}

		return 0x01FE;
	}

	void FillTemporaryTunnelListNode(
		RawTunnelListNode& node,
		const Endpoint& first,
		const Endpoint& second,
		const RawTunnelMapNode* sourceRecord,
		uint16_t mask)
	{
		std::fill(std::begin(node.value), std::end(node.value), 0);
		if (sourceRecord)
		{
			std::copy(std::begin(sourceRecord->value), std::end(sourceRecord->value), std::begin(node.value));
		}

		uint8_t* const bytes = reinterpret_cast<uint8_t*>(node.value);
		bytes[0] = static_cast<uint8_t>(first.x);
		bytes[1] = static_cast<uint8_t>(first.z);
		bytes[2] = static_cast<uint8_t>(second.x);
		bytes[3] = static_cast<uint8_t>(second.z);
		*reinterpret_cast<uint16_t*>(bytes + 12) = mask;
	}

	class TemporaryTunnelConnectionRecordScope
	{
	public:
		TemporaryTunnelConnectionRecordScope(
			cISC4TrafficSimulator* trafficSimulator,
			const Endpoint& first,
			const Endpoint& second)
			: sentinel(GetTunnelListSentinel(trafficSimulator)),
			  oldNext(nullptr),
			  oldPrevious(nullptr),
			  linked(false)
		{
			if (!sentinel)
			{
				return;
			}

			const RawTunnelMapNode* const forwardRecord = FindTunnelRecord(trafficSimulator, first);
			const RawTunnelMapNode* const reverseRecord = FindTunnelRecord(trafficSimulator, second);
			const uint16_t forwardMask = forwardRecord ? static_cast<uint16_t>(forwardRecord->value[0] >> 16) : 0x01FE;
			const uint16_t reverseMask = reverseRecord ? static_cast<uint16_t>(reverseRecord->value[0] >> 16) : 0x01FE;
			FillTemporaryTunnelListNode(forwardNode, first, second, forwardRecord, forwardMask);
			FillTemporaryTunnelListNode(reverseNode, second, first, reverseRecord, reverseMask);
			oldNext = sentinel->next;
			oldPrevious = sentinel->previous;
			if (!oldNext)
			{
				oldNext = sentinel;
			}
			if (!oldPrevious)
			{
				oldPrevious = sentinel;
			}

			forwardNode.next = &reverseNode;
			forwardNode.previous = sentinel;
			reverseNode.next = oldNext;
			reverseNode.previous = &forwardNode;
			oldNext->previous = &reverseNode;
			sentinel->next = &forwardNode;
			if (oldPrevious == sentinel)
			{
				sentinel->previous = &reverseNode;
			}
			linked = true;

			Logger::GetInstance().WriteLineFormatted(
				LogLevel::Trace,
				"TunnelPortalTool: inserted temporary tunnel connection records forward=%p reverse=%p sentinel=%p endpoints=(%u,%u)->(%u,%u) masks=0x%04X/0x%04X.",
				&forwardNode,
				&reverseNode,
				sentinel,
				first.x,
				first.z,
				second.x,
				second.z,
				static_cast<uint32_t>(forwardMask),
				static_cast<uint32_t>(reverseMask));
		}

		~TemporaryTunnelConnectionRecordScope()
		{
			if (linked)
			{
				sentinel->next = oldNext;
				sentinel->previous = oldPrevious;
				if (oldNext)
				{
					oldNext->previous = sentinel;
				}
			}
		}

	private:
		RawTunnelListNode forwardNode{};
		RawTunnelListNode reverseNode{};
		RawTunnelListNode* sentinel;
		RawTunnelListNode* oldNext;
		RawTunnelListNode* oldPrevious;
		bool linked;
	};

	void TraceNetworkToolPlacementContext(const char* label, cSC4NetworkTool* tool)
	{
		Logger& logger = Logger::GetInstance();

		if (!tool)
		{
			logger.WriteLineFormatted(LogLevel::Trace,
				"TunnelPortalTool: %s tool context is null.", label);
			return;
		}

		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: %s tool=%p placing=%u stage=%u failure=0x%08X draggedSteps=%u draggedCells=%u solvedCells=%u tunnelRecords=%u occManager=%p.",
			label,
			tool,
			static_cast<uint32_t>(FieldAt<uint8_t>(tool, kNetworkToolPlacingModeOffset)),
			FieldAt<uint32_t>(tool, kNetworkToolPlacementStageOffset),
			FieldAt<uint32_t>(tool, kNetworkToolFailureCodeOffset),
			static_cast<uint32_t>(VectorCountAt(tool, offsetof(cSC4NetworkTool, draggedSteps), sizeof(cSC4NetworkTool::tDraggedStep))),
			static_cast<uint32_t>(VectorCountAt(tool, offsetof(cSC4NetworkTool, draggedCells), sizeof(SC4Point<uint32_t>))),
			static_cast<uint32_t>(VectorCountAt(tool, offsetof(cSC4NetworkTool, solvedCells), sizeof(cSC4NetworkTool::tSolvedCell))),
			static_cast<uint32_t>(VectorCountAt(tool, kNetworkToolTunnelRecordsOffset, 0x10)),
			FieldAt<void*>(tool, 0x48));
	}

	void TraceNetworkToolTunnelRecords(const char* label, cSC4NetworkTool* tool)
	{
		if (!tool)
		{
			return;
		}

		const uint8_t* const vectorBase = reinterpret_cast<const uint8_t*>(tool) + kNetworkToolTunnelRecordsOffset;
		const auto start = *reinterpret_cast<const uintptr_t*>(vectorBase);
		const auto end = *reinterpret_cast<const uintptr_t*>(vectorBase + sizeof(uintptr_t));
		const auto capacity = *reinterpret_cast<const uintptr_t*>(vectorBase + sizeof(uintptr_t) * 2);
		if (!start || end < start)
		{
			return;
		}

		const uintptr_t byteSize = end - start;
		const uint32_t count16 = static_cast<uint32_t>(byteSize / 0x10);
		Logger& logger = Logger::GetInstance();
		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: %s tool tunnelRecords start=%p end=%p cap=%p bytes=%u count16=%u.",
			label,
			reinterpret_cast<const void*>(start),
			reinterpret_cast<const void*>(end),
			reinterpret_cast<const void*>(capacity),
			static_cast<uint32_t>(byteSize),
			count16);

		const uint32_t recordsToDump = std::min<uint32_t>(count16, 4);
		for (uint32_t index = 0; index < recordsToDump; ++index)
		{
			const auto* const words = reinterpret_cast<const uint32_t*>(start + index * 0x10);
			logger.WriteLineFormatted(
				LogLevel::Trace,
				"TunnelPortalTool: %s tool tunnelRecords[%u]=%08X/%08X/%08X/%08X.",
				label,
				index,
				words[0],
				words[1],
				words[2],
				words[3]);
		}
	}

	void PrepareTunnelEndpointCell(
		const char* label,
		cSC4NetworkCellInfo* cellInfo,
		cISC4NetworkOccupant::eNetworkType networkType,
		uint8_t tunnelPieceDirection,
		uint8_t pathStitchDirection)
	{
		if (!cellInfo)
		{
			return;
		}

		const uint32_t oldNetworkFlags = cellInfo->networkTypeFlags;
		const uint32_t oldCombinedEdges = cellInfo->edgeFlagsCombined;
		const uint32_t oldNetworkEdges = cellInfo->edgesPerNetwork[static_cast<uint32_t>(networkType)];
		// MakeTunnelPaths computes ONE cardinal direction from cell coordinates and filters
		// path entries by it. For mixed-axis portals the coordinate direction may differ from
		// the portal's facing, so whichever axis native stitching picks it must find entries on
		// both sides. Providing all four direction flags ensures entries for every possible
		// computed direction exist on both portals, making bidirectional stitching work regardless
		// of axis alignment.
		const uint32_t preparedEdges = kNorthSouthPortalEdges | kEastWestPortalEdges;

		cellInfo->networkTypeFlags |= kTunnelCellNetworkFlag;
		cellInfo->edgeFlagsCombined |= preparedEdges;
		cellInfo->edgesPerNetwork[static_cast<uint32_t>(networkType)] |= preparedEdges;
		FieldAt<uint8_t>(cellInfo, 0x51) = 1;
		FieldAt<uint8_t>(cellInfo, 0x53) = 1;

		Logger::GetInstance().WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: prepared %s tunnel endpoint cell direction=%u flags 0x%08X->0x%08X edges 0x%08X/0x%08X->0x%08X/0x%08X.",
			label,
			static_cast<uint32_t>(tunnelPieceDirection),
			oldNetworkFlags,
			cellInfo->networkTypeFlags,
			oldCombinedEdges,
			oldNetworkEdges,
			cellInfo->edgeFlagsCombined,
			cellInfo->edgesPerNetwork[static_cast<uint32_t>(networkType)]);
	}

	bool QueryTunnelOccupant(cISC4NetworkOccupant* occupant, cRZAutoRefCount<cIGZUnknown>& tunnelOccupant)
	{
		if (!occupant)
		{
			return false;
		}

		return occupant->QueryInterface(kNetworkTunnelOccupantID, tunnelOccupant.AsPPVoid()) && tunnelOccupant;
	}

	void TraceCellInfoState(
		const char* label,
		const cSC4NetworkCellInfo* cellInfo,
		cISC4NetworkOccupant::eNetworkType networkType)
	{
		Logger& logger = Logger::GetInstance();

		if (!cellInfo)
		{
			logger.WriteLineFormatted(LogLevel::Trace,
				"TunnelPortalTool: %s cell info is null.", label);
			return;
		}

		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: %s cell (%u,%u) flags=0x%08X edgeFlags=0x%08X networkEdge=0x%08X occ=%p idx=%d insert=%u bytes51-57=%02X %02X %02X %02X %02X %02X %02X vertices=%d,%d,%d,%d.",
			label,
			cellInfo->x,
			cellInfo->z,
			cellInfo->networkTypeFlags,
			cellInfo->edgeFlagsCombined,
			cellInfo->edgesPerNetwork[static_cast<uint32_t>(networkType)],
			cellInfo->networkOccupant,
			cellInfo->idxInCellsBuffer,
			static_cast<uint32_t>(FieldAt<uint8_t>(cellInfo, kCellInfoInsertResultOffset) != 0),
			static_cast<uint32_t>(FieldAt<uint8_t>(cellInfo, 0x51)),
			static_cast<uint32_t>(FieldAt<uint8_t>(cellInfo, 0x52)),
			static_cast<uint32_t>(FieldAt<uint8_t>(cellInfo, 0x53)),
			static_cast<uint32_t>(FieldAt<uint8_t>(cellInfo, 0x54)),
			static_cast<uint32_t>(FieldAt<uint8_t>(cellInfo, 0x55)),
			static_cast<uint32_t>(FieldAt<uint8_t>(cellInfo, 0x56)),
			static_cast<uint32_t>(FieldAt<uint8_t>(cellInfo, 0x57)),
			cellInfo->vertices[0],
			cellInfo->vertices[1],
			cellInfo->vertices[2],
			cellInfo->vertices[3]);
	}

	void TraceTunnelExemplarState(cSC4NetworkTool* tool)
	{
		Logger& logger = Logger::GetInstance();

		if (!tool)
		{
			return;
		}

		const uint32_t* const* const exemplars = reinterpret_cast<const uint32_t* const*>(
			reinterpret_cast<const uint8_t*>(tool) + kNetworkToolTunnelExemplarsOffset);
		const uint32_t* const exemplarStart = exemplars[0];
		const uint32_t* const exemplarEnd = exemplars[1];
		const uint32_t* const exemplarCapacity = exemplars[2];

		const uint32_t* const* const rotationOffsets = reinterpret_cast<const uint32_t* const*>(
			reinterpret_cast<const uint8_t*>(tool) + kNetworkToolTunnelRotationOffsetsOffset);
		const uint32_t* const rotationStart = rotationOffsets[0];
		const uint32_t* const rotationEnd = rotationOffsets[1];

		uint32_t defaultGroup = 0;
		const void* const defaultGroupBlock = FieldAt<const void*>(tool, kNetworkToolDefaultExemplarGroupOffset);
		if (defaultGroupBlock)
		{
			defaultGroup = FieldAt<uint32_t>(defaultGroupBlock, 0x0c);
		}

		const size_t exemplarCount = exemplarStart && exemplarEnd && exemplarEnd >= exemplarStart
			? static_cast<size_t>(exemplarEnd - exemplarStart)
			: 0;
		const size_t rotationCount = rotationStart && rotationEnd && rotationEnd >= rotationStart
			? static_cast<size_t>(rotationEnd - rotationStart)
			: 0;

		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: tunnel exemplar vector start=%p end=%p cap=%p count=%u rotationStart=%p rotationEnd=%p rotationCount=%u defaultGroup=0x%08X.",
			exemplarStart,
			exemplarEnd,
			exemplarCapacity,
			static_cast<uint32_t>(exemplarCount),
			rotationStart,
			rotationEnd,
			static_cast<uint32_t>(rotationCount),
			defaultGroup);

		const size_t traceCount = std::min<size_t>(exemplarCount, 12);
		for (size_t i = 0; i < traceCount; ++i)
		{
			const uint32_t rotationOffset = i < rotationCount ? rotationStart[i] : 0xffffffff;
			cIGZUnknown* const exemplar = GetOccupantExemplar(tool, exemplarStart[i], 0);
			logger.WriteLineFormatted(
				LogLevel::Trace,
				"TunnelPortalTool: tunnel exemplar[%u]=0x%08X rotationOffset=%u resolved=%p.",
				static_cast<uint32_t>(i),
				exemplarStart[i],
				rotationOffset,
				exemplar);
			if (exemplar)
			{
				exemplar->Release();
			}
		}
	}

	void TraceNetworkOccupantState(const char* label, cISC4NetworkOccupant* occupant)
	{
		Logger& logger = Logger::GetInstance();

		if (!occupant)
		{
			logger.WriteLineFormatted(LogLevel::Trace,
				"TunnelPortalTool: %s network occupant is null.", label);
			return;
		}

		cRZAutoRefCount<cIGZUnknown> preBuiltModel;
		const bool hasPreBuiltModel = occupant->QueryInterface(
			GZCLSID::kcSC4NetworkOccupantWithPreBuiltModel,
			preBuiltModel.AsPPVoid()) && preBuiltModel;

		uint32_t occupiedX = 0;
		uint32_t occupiedZ = 0;
		occupant->GetOccupiedCell(occupiedX, occupiedZ);

		cISC4Occupant* const baseOccupant = occupant->AsOccupant();
		const uint32_t baseFlags = baseOccupant ? baseOccupant->GetFlags() : 0;
		void* const placeableObject = baseOccupant ? baseOccupant->GetPlaceableObject() : nullptr;

		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: %s occupant=%p prebuilt=%u piece=0x%08X rot=%u flip=%u rf=%u variation=%u networkFlags=0x%08X usable=%u placed=%u viewDependent=%u occupied=(%u,%u) base=%p baseFlags=0x%08X placeable=%p pathInfo=0x%08X.",
			label,
			occupant,
			static_cast<uint32_t>(hasPreBuiltModel),
			occupant->PieceId(),
			static_cast<uint32_t>(occupant->GetRotation()),
			static_cast<uint32_t>(occupant->GetFlip()),
			static_cast<uint32_t>(occupant->GetRotationAndFlip()),
			static_cast<uint32_t>(occupant->GetVariation()),
			occupant->GetNetworkFlag(),
			static_cast<uint32_t>(occupant->IsUsable()),
			static_cast<uint32_t>(occupant->IsPlaced()),
			static_cast<uint32_t>(occupant->HasViewDependentModel()),
			occupiedX,
			occupiedZ,
			baseOccupant,
			baseFlags,
			placeableObject,
			static_cast<uint32_t>(occupant->GetPathInfo()));
	}

	cISC4NetworkOccupant* __fastcall Hook_InsertTunnelPiece(
		cSC4NetworkTool* tool,
		void*,
		uint8_t direction,
		uint8_t sequenceIndex,
		cSC4NetworkCellInfo* cellInfo);

	Patching::InlineHook sInsertTunnelPieceHook{
		0x00628390,
		reinterpret_cast<void*>(&Hook_InsertTunnelPiece),
		{ 0x83, 0xEC, 0x60, 0x56, 0x8B, 0xF1 },
		true
	};

	cISC4NetworkOccupant* __fastcall Hook_InsertTunnelPiece(
		cSC4NetworkTool* tool,
		void*,
		uint8_t direction,
		uint8_t sequenceIndex,
		cSC4NetworkCellInfo* cellInfo)
	{
		Logger& logger = Logger::GetInstance();
		const char* const source = sCustomTunnelInsertionDepth != 0 ? "custom" : "native";

		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: InsertTunnelPiece hook enter source=%s tool=%p direction=%u sequence=%u cell=%p.",
			source,
			tool,
			static_cast<uint32_t>(direction),
			static_cast<uint32_t>(sequenceIndex),
			cellInfo);
		TraceNetworkToolPlacementContext("InsertTunnelPiece hook before", tool);
		TraceNetworkToolTunnelRecords("InsertTunnelPiece hook before", tool);

		if (cellInfo)
		{
			TraceCellInfoState(
				"InsertTunnelPiece hook before",
				cellInfo,
				cSC4NetworkTool::GetFirstNetworkTypeFromFlags(cellInfo->networkTypeFlags));
		}

		const auto original = reinterpret_cast<InsertTunnelPieceFn>(sInsertTunnelPieceHook.trampoline);
		cISC4NetworkOccupant* const result = original
			? original(tool, direction, sequenceIndex, cellInfo)
			: nullptr;

		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: InsertTunnelPiece hook exit source=%s result=%p.",
			source,
			result);

		if (cellInfo)
		{
			TraceCellInfoState(
				"InsertTunnelPiece hook after",
				cellInfo,
				cSC4NetworkTool::GetFirstNetworkTypeFromFlags(cellInfo->networkTypeFlags));
		}
		TraceNetworkOccupantState("InsertTunnelPiece hook result", result);
		TraceNetworkToolTunnelRecords("InsertTunnelPiece hook after", tool);

		return result;
	}

	// Native PlaceNetwork calls cSC4NetworkConstructionCrew::MarkOccupantsUsable
	// after InsertTunnelPieces. Direct insertion bypasses that commit-list pass.
	void MarkNetworkOccupantUsable(cISC4NetworkOccupant* occupant, const char* label)
	{
		Logger& logger = Logger::GetInstance();

		if (!occupant)
		{
			logger.WriteLineFormatted(LogLevel::Error,
				"TunnelPortalTool: cannot mark %s occupant usable, occupant is null.", label);
			return;
		}

		cISC4Occupant* const baseOccupant = occupant->AsOccupant();
		if (!baseOccupant)
		{
			logger.WriteLineFormatted(LogLevel::Error,
				"TunnelPortalTool: cannot mark %s occupant visible, AsOccupant returned null.", label);
			return;
		}

		occupant->ClearNetworkFlag(0x4000);
		const uint8_t visibilityResult = baseOccupant->SetVisibility(true, true);
		occupant->SetNetworkFlag(0x10000000);

		logger.WriteLineFormatted(LogLevel::Trace,
			"TunnelPortalTool: marked %s tunnel occupant usable/visible, visibility result=%u flags=0x%08X.",
			label,
			static_cast<uint32_t>(visibilityResult),
			occupant->GetNetworkFlag());
	}

	// Trampoline pointer used by the naked assembly stub below.
	// Set by InstallDiagnostics after sInitTunnelPathHook is installed.
	void* sInitTunnelPathTrampolinePtr = nullptr;
	void* sPeerPathLookupTrampolinePtr = nullptr;
	void* sDoConnectionsChangedEntryTrampolinePtr = nullptr;

	void __stdcall TraceDoConnectionsChangedEntry(cISC4TrafficSimulator* trafficSimulator)
	{
		TraceAnyTunnelConnectionList("DoConnectionsChanged entry", trafficSimulator);
	}

	// Mid-function hook targeting the 6-byte "call [edx+0xC4]" at 0x0053FDEE inside
	// cSC4PathInfo::MakeTunnelPaths (confirmed function entry: 0x0053FD70).
	//
	// MakeTunnelPaths computes a cardinal direction from A's cell to B's cell and stores it
	// as a byte at [esp+0x58] (param_1 slot reused as a local after the arg is loaded into
	// registers). Prologue: sub esp,0x44 + push ebx,ebp,esi,edi = 0x54 total; param_1 at
	// entry+4 → [esp+0x58] inside the function. All five direction-assignment instructions
	// (C6 44 24 58 00/01/02/03) complete before 0x0053FDEE. By overwriting [esp+0x58] here,
	// after the coordinate comparison but before the loop reads it, we redirect path stitching
	// to the portal's actual facing direction instead of the coordinate-derived one.
	//
	// Register state at 0x0053FDEE (set by the two preceding instructions at 0x0053FDEA/EC):
	//   edx = vtable of esi (otherEnd's vtable) → used by the original call [edx+0xC4]
	//   ecx = esi = otherEnd                    → thiscall receiver for GetPathInfo
	//   eax = clobbered freely by our mov al    → overwritten by the call's return value
	//   esi = otherEnd (preserved, not touched)
	//   edi = pathInfo/this (preserved, not touched)
	NAKED_FUN void Hook_InitTunnelPath()
	{
		__asm {
			inc  dword ptr [sInitTunnelPathHookHitCount]
			mov  al, byte ptr [esp+0x58]
			mov  byte ptr [sLastInitTunnelPathOriginalDirection], al
			cmp  byte ptr [sCustomPortalFacingOverride], 0xFF
			je   done
			mov  al, byte ptr [sCustomPortalFacingOverride]
			mov  byte ptr [sLastInitTunnelPathOverrideDirection], al
			inc  dword ptr [sInitTunnelPathOverrideCount]
			mov  byte ptr [esp+0x58], al
		done:
			jmp  dword ptr [sInitTunnelPathTrampolinePtr]
		}
	}

	// Hook site: 0x0053FDEE — the 6-byte "call [edx+0xC4]" instruction inside MakeTunnelPaths.
	// This is the first instruction after all direction-assignment branches have completed.
	Patching::InlineHook sInitTunnelPathHook{
		0x0053FDEE,
		reinterpret_cast<void*>(&Hook_InitTunnelPath),
		{ 0xFF, 0x92, 0xC4, 0x00, 0x00, 0x00 },
		true
	};

	// Hook site: 0x0053FE31 - "push eax; mov ecx,ebx; lea ebp,[esi+8]".
	// This is immediately before MakeTunnelPaths calls peerPathInfo->GetPath(key, 0).
	// For mixed-facing portals, the peer path uses a different low word:
	// low byte = peer exit direction, next byte = opposite peer direction.
	NAKED_FUN void Hook_PeerPathLookupKey()
	{
		__asm {
			inc  dword ptr [sPeerPathLookupHookHitCount]
			mov  dword ptr [sLastPeerPathLookupOriginalKey], eax
			cmp  word ptr [sCustomPeerPathLookupKeyLowWord], 0xFFFF
			je   done
			mov  ax, word ptr [sCustomPeerPathLookupKeyLowWord]
			mov  dword ptr [sLastPeerPathLookupOverrideKey], eax
			inc  dword ptr [sPeerPathLookupOverrideCount]
		done:
			jmp  dword ptr [sPeerPathLookupTrampolinePtr]
		}
	}

	Patching::InlineHook sPeerPathLookupHook{
		0x0053FE31,
		reinterpret_cast<void*>(&Hook_PeerPathLookupKey),
		{ 0x50, 0x8B, 0xCB, 0x8D, 0x6E, 0x08 },
		true
	};

	NAKED_FUN void Hook_DoConnectionsChangedEntry()
	{
		__asm {
			pushfd
			pushad
			push ecx
			call TraceDoConnectionsChangedEntry
			popad
			popfd
			jmp dword ptr [sDoConnectionsChangedEntryTrampolinePtr]
		}
	}

	Patching::InlineHook sDoConnectionsChangedEntryHook{
		0x0071A867,
		reinterpret_cast<void*>(&Hook_DoConnectionsChangedEntry),
		{ 0x56, 0x8B, 0xF1, 0x83, 0xC0, 0xFE },
		true
	};

	int __fastcall Hook_AddTrafficConnection(
		cISC4TrafficSimulator* trafficSimulator,
		void*,
		int x,
		uint32_t z,
		int fromDirection,
		int toDirection,
		int travelMode);

	Patching::InlineHook sAddTrafficConnectionHook{
		0x0070eab0,
		reinterpret_cast<void*>(&Hook_AddTrafficConnection),
		{ 0x53, 0x8B, 0x5C, 0x24, 0x08, 0x55 },
		true
	};

	int __fastcall Hook_AddTrafficConnection(
		cISC4TrafficSimulator* trafficSimulator,
		void*,
		int x,
		uint32_t z,
		int fromDirection,
		int toDirection,
		int travelMode)
	{
		if (sTraceTrafficEdgeInsertions && IsTrafficEdgeTraceCell(static_cast<uint32_t>(x), z))
		{
			Logger::GetInstance().WriteLineFormatted(
				LogLevel::Trace,
				"TunnelPortalTool: AddTrafficConnection cell=(%d,%u) fromDir=%d toDir=%d travelMode=%d first=(%u,%u) second=(%u,%u).",
				x,
				z,
				fromDirection,
				toDirection,
				travelMode,
				sTraceFirstEndpoint.x,
				sTraceFirstEndpoint.z,
				sTraceSecondEndpoint.x,
				sTraceSecondEndpoint.z);
		}

		const auto original = reinterpret_cast<AddTrafficConnectionFn>(sAddTrafficConnectionHook.trampoline);
		return original
			? original(trafficSimulator, x, z, fromDirection, toDirection, travelMode)
			: 0;
	}

	uint32_t sAddTunnelTripNodeHookHitCount = 0;

	void __fastcall Hook_AddTunnelTripNode(
		void* pathFinder,
		void*,
		uint32_t x,
		uint32_t z,
		uint8_t edge,
		uint8_t travelMode,
		float cost,
		int currentNode,
		float heuristic,
		char outOfBounds)
	{
		++sAddTunnelTripNodeHookHitCount;

		const uint32_t currentX = currentNode
			? static_cast<uint32_t>(static_cast<uint16_t>(FieldAt<int16_t>(reinterpret_cast<void*>(currentNode), 0x14)))
			: 0xFFFF;
		const uint32_t currentZ = currentNode
			? static_cast<uint32_t>(static_cast<uint16_t>(FieldAt<int16_t>(reinterpret_cast<void*>(currentNode), 0x16)))
			: 0xFFFF;

		// Log every call (throttled) so we can verify the hook fires.
		const uint32_t hitNum = sAddTunnelTripNodeHookHitCount;
		if (hitNum <= 20 || (hitNum % 200) == 0)
		{
			Logger::GetInstance().WriteLineFormatted(
				LogLevel::Trace,
				"TunnelPortalTool: Hook_AddTunnelTripNode hit #%u current=(%u,%u) dest=(%u,%u) edge=%u travelMode=%u currentNode=%p.",
				hitNum,
				currentX,
				currentZ,
				x,
				z,
				static_cast<uint32_t>(edge),
				static_cast<uint32_t>(travelMode),
				reinterpret_cast<void*>(currentNode));
		}

		uint8_t replacementEdge = 0xFF;
		if (currentNode && TryGetCustomTunnelArrivalEdge(currentX, currentZ, x, z, replacementEdge))
		{
			if ((edge & 3) != replacementEdge)
			{
				Logger::GetInstance().WriteLineFormatted(
					LogLevel::Trace,
					"TunnelPortalTool: rewrote pathfinder tunnel AddTripNode edge current=(%u,%u) destination=(%u,%u) travelMode=%u edge=%u->%u.",
					currentX,
					currentZ,
					x,
					z,
					static_cast<uint32_t>(travelMode),
					static_cast<uint32_t>(edge),
					static_cast<uint32_t>(replacementEdge));
				edge = static_cast<uint8_t>((edge & ~0x03u) | replacementEdge);
			}
		}

		AddTripNode(pathFinder, x, z, edge, travelMode, cost, currentNode, heuristic, outOfBounds);
	}

	// FloodSubnetwork tunnel edge fix.
	//
	// FloodSubnetwork (Windows: 0x00717ec0) stamps subnet IDs on reachable
	// network edges using a DFS. Its tunnel branch (pseudo-direction 5) looks up
	// a tunnel connection via the map at this+0xc8, then jumps the flood to the
	// peer portal cell. It reuses the current node's "side" as the arrival side
	// at the peer — correct for same-axis tunnels, wrong for mixed-axis ones.
	//
	// We fix this by redirecting the CALL to GetNetworkInfo at 0x00718215 (inside
	// the tunnel branch) to a wrapper that calls the original, then overwrites
	// uStack_68 on the caller's stack with the correct arrival edge when a
	// registered custom tunnel portal pair is involved.
	//
	// Stack layout at hook entry (ESP = E):
	//   [E+0x00] return address (0x71821A)
	//   [E+0x04] networkType (PUSH EDX)
	//   [E+0x08] peerX       (PUSH EBX)
	//   [E+0x0C] peerY       (PUSH ECX)
	//   [E+0x10..] FloodSubnetwork's adjusted_ESP locals
	//     [E+0x24] = packed state (byte0=x, byte1=y, byte2=side, byte3=nextDir)
	//     [E+0x30] = uStack_68 (the edge value to fix)
	//   ECX = this (cSC4TrafficSimulator*)
	//   EBX = peerX (preserved across calls)

	using FloodGetNetworkInfoFn = void* (__thiscall*)(void*, uint32_t, uint32_t, uint32_t);
	FloodGetNetworkInfoFn sOriginalFloodGetNetworkInfo =
		reinterpret_cast<FloodGetNetworkInfoFn>(0x0070FB30);

	uint32_t sFloodSubnetworkHookHitCount = 0;
	uint32_t sFloodSubnetworkFixCallCount = 0;

	void __stdcall FloodSubnetworkFixTunnelEdge(
		uint32_t currentX,
		uint32_t currentY,
		uint32_t peerX,
		uint32_t peerY,
		uint32_t* edgePtr)
	{
		++sFloodSubnetworkFixCallCount;
		const uint32_t edgeValue = edgePtr ? (*edgePtr & 0xFF) : 0xDEAD;

		// Log every call so we can verify the hook fires and reads correct values.
		// Use a throttle to avoid flooding: log the first 20 calls, then every 100th.
		const uint32_t callNum = sFloodSubnetworkFixCallCount;
		if (callNum <= 20 || (callNum % 100) == 0)
		{
			Logger::GetInstance().WriteLineFormatted(
				LogLevel::Trace,
				"TunnelPortalTool: FloodSubnetworkFixTunnelEdge call #%u hookHits=%u current=(%u,%u) peer=(%u,%u) edgePtr=%p edgeValue=%u.",
				callNum,
				sFloodSubnetworkHookHitCount,
				currentX,
				currentY,
				peerX,
				peerY,
				edgePtr,
				edgeValue);
		}

		uint8_t arrivalEdge = 0xFF;
		if (TryGetCustomTunnelArrivalEdge(currentX, currentY, peerX, peerY, arrivalEdge))
		{
			const uint32_t currentEdge = *edgePtr & 0xFF;
			Logger::GetInstance().WriteLineFormatted(
				LogLevel::Trace,
				"TunnelPortalTool: FloodSubnetwork tunnel edge MATCH current=(%u,%u) peer=(%u,%u) edge=%u arrivalEdge=%u.",
				currentX,
				currentY,
				peerX,
				peerY,
				currentEdge,
				static_cast<uint32_t>(arrivalEdge));
			if (currentEdge != arrivalEdge)
			{
				Logger::GetInstance().WriteLineFormatted(
					LogLevel::Trace,
					"TunnelPortalTool: rewrote FloodSubnetwork tunnel edge current=(%u,%u) peer=(%u,%u) edge=%u->%u.",
					currentX,
					currentY,
					peerX,
					peerY,
					currentEdge,
					static_cast<uint32_t>(arrivalEdge));
				*edgePtr = (*edgePtr & ~0x03u) | arrivalEdge;
			}
		}
	}

	NAKED_FUN void Hook_FloodSubnetworkTunnelGetNetworkInfo()
	{
		__asm {
			// Increment hit counter (no flags/registers affected meaningfully).
			inc  dword ptr [sFloodSubnetworkHookHitCount]

			// Phase 1: Call original GetNetworkInfo(this, networkType, peerX, peerY).
			// ECX = this, stack = [retaddr, netType, peerX, peerY].
			// Re-push args for the real thiscall (callee cleans 12 bytes).
			push dword ptr [esp+0x0C]
			push dword ptr [esp+0x0C]
			push dword ptr [esp+0x0C]
			call dword ptr [sOriginalFloodGetNetworkInfo]
			// EAX = peer NetworkInfo*, ESP = E (3 re-pushed args cleaned by callee).

			// Phase 2: Fix uStack_68 on the caller's stack.
			push eax                          // save GetNetworkInfo result, ESP = E-4
			push ecx                          // ESP = E-8
			push edx                          // ESP = E-12

			// Key locations relative to hook entry ESP (= E):
			//   [E+0x08]  = peerX  (original caller arg)
			//   [E+0x0C]  = peerY  (original caller arg)
			//   [E+0x24]  = uStack_74 byte 0 = currentX
			//   [E+0x25]  = uStack_74 byte 1 = currentY
			//   [E+0x30]  = uStack_68 (arrival edge to fix)
			// NOTE: ESP shifts with each push; offsets below account for that.

			lea  eax, [esp + 0x3C]            // &uStack_68,       ESP = E-12
			push eax                          // arg5: edgePtr,    ESP = E-16
			push dword ptr [esp + 0x1C]       // arg4: peerY,      ESP = E-20  [E-16+0x1C]=[E+0x0C]
			push dword ptr [esp + 0x1C]       // arg3: peerX,      ESP = E-24  [E-20+0x1C]=[E+0x08]
			movzx eax, byte ptr [esp + 0x3D]  // currentY (byte 1 of packed state at E+0x25) ESP = E-24
			push eax                          // arg2: currentY,   ESP = E-28
			movzx eax, byte ptr [esp + 0x40]  // currentX (byte 0 of packed state at E+0x24) ESP = E-28
			push eax                          // arg1: currentX,   ESP = E-32
			call FloodSubnetworkFixTunnelEdge  // __stdcall, cleans 20 bytes → ESP = E-12

			pop  edx
			pop  ecx
			pop  eax                          // restored GetNetworkInfo result, ESP = E

			ret  0x0C                         // clean 3 original args, like the real GetNetworkInfo
		}
	}

	// Windows InsertTunnelPieces (0x006287d0) QueryInterfaces both occupants to
	// kcSC4NetworkTunnelOccupant, then calls tunnel->GetPathInfo (+0xc4) and
	// pathInfo->InitTunnelPath (+0x80) with the two tunnel-interface pointers.
	//
	// selfFacing: the portal's actual facing direction (0=N 1=E 2=S 3=W), used to set
	// sCustomPortalFacingOverride so Hook_InitTunnelPath can substitute the correct axis
	// instead of the coordinate-derived direction that MakeTunnelPaths computes internally.
	void RefreshTunnelPathInfo(
		cIGZUnknown* self,
		cIGZUnknown* otherEnd,
		uint8_t selfFacing,
		uint8_t otherFacing,
		uint8_t selfLookupPathDirection,
		uint16_t peerPathKeyLowWord)
	{
		Logger& logger = Logger::GetInstance();

		if (!self || !otherEnd)
		{
			logger.WriteLineFormatted(
				LogLevel::Error,
				"TunnelPortalTool: cannot refresh path info, self=%p otherEnd=%p.",
				self,
				otherEnd);
			return;
		}

		void** vtable = *reinterpret_cast<void***>(self);
		GetPathInfoFn getPathInfo = reinterpret_cast<GetPathInfoFn>(vtable[0x31]);
		void* pathInfo = getPathInfo(self);
		void** otherVtable = *reinterpret_cast<void***>(otherEnd);
		GetPathInfoFn getOtherPathInfo = reinterpret_cast<GetPathInfoFn>(otherVtable[0x31]);
		void* otherPathInfo = getOtherPathInfo(otherEnd);

		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: GetPathInfo self=%p otherEnd=%p pathInfo=%p otherPathInfo=%p.",
			self,
			otherEnd,
			pathInfo,
			otherPathInfo);

		if (!pathInfo)
		{
			return;
		}

		void** pathInfoVtable = *reinterpret_cast<void***>(pathInfo);
		InitTunnelPathFn initTunnelPath = reinterpret_cast<InitTunnelPathFn>(pathInfoVtable[0x20]);

		// Log the concrete function address on first call. Copy this value to sInitTunnelPathHook.address.
		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: InitTunnelPath concrete address=%p selfFacing=%u.",
			reinterpret_cast<void*>(initTunnelPath),
			static_cast<uint32_t>(selfFacing));

		const uint8_t selfPathDirection = TunnelPieceDirectionToPathDirection(selfFacing);
		const uint8_t otherPathDirection = TunnelPieceDirectionToPathDirection(otherFacing);
		const uint8_t peerLookupPathDirection = static_cast<uint8_t>(peerPathKeyLowWord);
		TracePathInfoKeyMap("before InitTunnelPath", pathInfo, otherPathInfo, selfLookupPathDirection);
		const uint32_t hookHitsBefore = sInitTunnelPathHookHitCount;
		const uint32_t hookOverridesBefore = sInitTunnelPathOverrideCount;
		const uint32_t peerLookupHitsBefore = sPeerPathLookupHookHitCount;
		const uint32_t peerLookupOverridesBefore = sPeerPathLookupOverrideCount;
		sLastInitTunnelPathOriginalDirection = 0xFF;
		sLastInitTunnelPathOverrideDirection = 0xFF;
		sLastPeerPathLookupOriginalKey = 0;
		sLastPeerPathLookupOverrideKey = 0;
		sCustomPortalFacingOverride = selfLookupPathDirection;
		sCustomPeerPathLookupKeyLowWord = peerPathKeyLowWord;
		initTunnelPath(pathInfo, self, otherEnd);
		sCustomPortalFacingOverride = 0xFF;
		sCustomPeerPathLookupKeyLowWord = 0xFFFF;
		const uint32_t hookHitDelta = sInitTunnelPathHookHitCount - hookHitsBefore;
		const uint32_t hookOverrideDelta = sInitTunnelPathOverrideCount - hookOverridesBefore;
		const uint32_t peerLookupHitDelta = sPeerPathLookupHookHitCount - peerLookupHitsBefore;
		const uint32_t peerLookupOverrideDelta = sPeerPathLookupOverrideCount - peerLookupOverridesBefore;
		if (sLastPeerPathLookupOriginalKey != 0)
		{
			TracePathKeyDetails("self lookup source", pathInfo, sLastPeerPathLookupOriginalKey);
			TracePathKeyDetails("peer rewritten target", otherPathInfo, sLastPeerPathLookupOverrideKey);
			TracePathKeyDetails(
				"peer opposite target",
				otherPathInfo,
				ReplacePathKeyLowWord(sLastPeerPathLookupOriginalKey, TunnelPathKeyLowWord(otherPathDirection ^ 2)));
		}
		TracePathInfoKeyMap("after InitTunnelPath", pathInfo, otherPathInfo, selfLookupPathDirection);

		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: refreshed path info pathInfo=%p otherPathInfo=%p self=%p otherEnd=%p facing=%u pathDirection=%u selfLookupPathDirection=%u otherFacing=%u otherPathDirection=%u peerLookupPathDirection=%u peerKeyLowWord=0x%04X hookHits=%u hookOverrides=%u originalDirection=%u overrideDirection=%u peerLookupHits=%u peerLookupOverrides=%u peerOriginalKey=0x%08X peerOverrideKey=0x%08X.",
			pathInfo,
			otherPathInfo,
			self,
			otherEnd,
			static_cast<uint32_t>(selfFacing),
			static_cast<uint32_t>(selfPathDirection),
			static_cast<uint32_t>(selfLookupPathDirection),
			static_cast<uint32_t>(otherFacing),
			static_cast<uint32_t>(otherPathDirection),
			static_cast<uint32_t>(peerLookupPathDirection),
			static_cast<uint32_t>(peerPathKeyLowWord),
			hookHitDelta,
			hookOverrideDelta,
			static_cast<uint32_t>(sLastInitTunnelPathOriginalDirection),
			static_cast<uint32_t>(sLastInitTunnelPathOverrideDirection),
			peerLookupHitDelta,
			peerLookupOverrideDelta,
			sLastPeerPathLookupOriginalKey,
			sLastPeerPathLookupOverrideKey);
	}

	void NotifyTrafficSimulatorForLinkedTunnels(
		const Endpoint& first,
		const Endpoint& second,
		cIGZUnknown* firstTunnel,
		cIGZUnknown* secondTunnel)
	{
		Logger& logger = Logger::GetInstance();
		cISC4TrafficSimulator* const trafficSimulator = GetTrafficSimulator();

		if (!trafficSimulator)
		{
			logger.WriteLine(LogLevel::Error, "TunnelPortalTool: cannot notify traffic simulator, no traffic simulator is available.");
			return;
		}

		TraceTunnelRecordMap("before tunnel changed reset", trafficSimulator, first, second);
		TraceTunnelConnectionList("before tunnel changed reset", trafficSimulator, first, second);
		DoTunnelChanged(trafficSimulator, firstTunnel, false);
		DoTunnelChanged(trafficSimulator, secondTunnel, false);
		TraceTunnelRecordMap("after tunnel changed remove", trafficSimulator, first, second);
		TraceTunnelConnectionList("after tunnel changed remove", trafficSimulator, first, second);
		DoTunnelChanged(trafficSimulator, firstTunnel, true);
		TraceTunnelRecordMap("after first tunnel changed add", trafficSimulator, first, second);
		TraceTunnelConnectionList("after first tunnel changed add", trafficSimulator, first, second);
		DoTunnelChanged(trafficSimulator, secondTunnel, true);
		TraceTunnelRecordMap("after second tunnel changed add", trafficSimulator, first, second);
		TraceTunnelConnectionList("after second tunnel changed add", trafficSimulator, first, second);

		TemporaryTunnelConnectionRecordScope temporaryTunnelConnectionRecord(trafficSimulator, first, second);
		TraceTunnelConnectionList("after temporary tunnel connection record insert", trafficSimulator, first, second);

		const bool oldTraceTrafficEdgeInsertions = sTraceTrafficEdgeInsertions;
		const bool oldSuppressConnectionsChangedEntryTrace = sSuppressConnectionsChangedEntryTrace;
		const Endpoint oldTraceFirstEndpoint = sTraceFirstEndpoint;
		const Endpoint oldTraceSecondEndpoint = sTraceSecondEndpoint;
		sTraceTrafficEdgeInsertions = true;
		sSuppressConnectionsChangedEntryTrace = true;
		sTraceFirstEndpoint = first;
		sTraceSecondEndpoint = second;
		DoConnectionsChanged(trafficSimulator, first.x, first.z, first.x, first.z);
		DoConnectionsChanged(trafficSimulator, second.x, second.z, second.x, second.z);
		DoConnectionsChanged(
			trafficSimulator,
			std::min(first.x, second.x),
			std::min(first.z, second.z),
			std::max(first.x, second.x),
			std::max(first.z, second.z));
		sTraceTrafficEdgeInsertions = oldTraceTrafficEdgeInsertions;
		sSuppressConnectionsChangedEntryTrace = oldSuppressConnectionsChangedEntryTrace;
		sTraceFirstEndpoint = oldTraceFirstEndpoint;
		sTraceSecondEndpoint = oldTraceSecondEndpoint;
		TraceTunnelRecordMap("after connection rebuilds", trafficSimulator, first, second);
		TraceTunnelConnectionList("after connection rebuilds", trafficSimulator, first, second);

		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: reset/notified traffic simulator for linked tunnel endpoints (%u,%u) and (%u,%u), including bounding rebuild.",
			first.x,
			first.z,
			second.x,
			second.z);
	}

	bool PlacePortalPair(const Endpoint& first, const Endpoint& second)
	{
		Logger& logger = Logger::GetInstance();

		logger.WriteLineFormatted(
			LogLevel::Debug,
			"TunnelPortalTool: attempting to place %s portal pair at (%u,%u) -> (%u,%u).",
			NetworkTypeName(first.networkType),
			first.x, first.z,
			second.x, second.z);

		if (first.networkType != second.networkType)
		{
			logger.WriteLine(LogLevel::Error, "Tunnel portal endpoints must be on the same network type.");
			return false;
		}

		cISC4City* city = GetCity();
		cISC4NetworkManager* networkManager = city ? city->GetNetworkManager() : nullptr;
		cSC4NetworkTool* tool = GetNetworkTool(networkManager, first.networkType);

		if (!tool)
		{
			logger.WriteLine(LogLevel::Error, "Could not acquire the network tool for tunnel portal placement.");
			return false;
		}

		cSC4NetworkCellInfo* firstCell = GetEndpointCell(tool, first);
		cSC4NetworkCellInfo* secondCell = GetEndpointCell(tool, second);

		if (!firstCell || !secondCell)
		{
			logger.WriteLine(LogLevel::Error, "Tunnel portal placement requires both endpoints to be existing compatible network tiles.");
			return false;
		}

		TraceTunnelExemplarState(tool);
		TraceCellInfoState("first before tunnel insertion", firstCell, first.networkType);
		TraceCellInfoState("second before tunnel insertion", secondCell, second.networkType);

		const uint8_t firstVectorDirection = InferTunnelPieceDirection(first, second);
		const uint8_t secondVectorDirection = InferTunnelPieceDirection(second, first);
		uint8_t firstDirection = firstVectorDirection;
		uint8_t secondDirection = secondVectorDirection;

		cISC4NetworkOccupant* firstOccupant = nullptr;
		cISC4NetworkOccupant* secondOccupant = nullptr;
		{
			NetworkToolPlacementStateScope placementState(tool);
			CustomTunnelInsertionScope customInsertion;
			const bool inferredFirstDirection = TryInferTunnelPieceDirectionFromSurfaceApproach(
				"first",
				firstCell,
				first.networkType,
				firstDirection);
			const bool inferredSecondDirection = TryInferTunnelPieceDirectionFromSurfaceApproach(
				"second",
				secondCell,
				second.networkType,
				secondDirection);
			logger.WriteLineFormatted(
				LogLevel::Trace,
				"TunnelPortalTool: tunnel piece directions first=%u (%s) second=%u (%s), vectorFallback first=%u second=%u.",
				static_cast<uint32_t>(firstDirection),
				inferredFirstDirection ? "surface approach" : "vector",
				static_cast<uint32_t>(secondDirection),
				inferredSecondDirection ? "surface approach" : "vector",
				static_cast<uint32_t>(firstVectorDirection),
				static_cast<uint32_t>(secondVectorDirection));
			PrepareTunnelEndpointCell("first", firstCell, first.networkType, firstDirection, firstVectorDirection);
			PrepareTunnelEndpointCell("second", secondCell, second.networkType, secondDirection, secondVectorDirection);
			TraceCellInfoState("first prepared for tunnel insertion", firstCell, first.networkType);
			TraceCellInfoState("second prepared for tunnel insertion", secondCell, second.networkType);
			firstOccupant = InsertTunnelPiece(tool, firstDirection, 0, firstCell);
			secondOccupant = InsertTunnelPiece(tool, secondDirection, 0, secondCell);
		}

		if (!firstOccupant || !secondOccupant)
		{
			logger.WriteLine(LogLevel::Error, "Tunnel portal placement did not create two tunnel occupants.");
			return false;
		}

		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: InsertTunnelPiece returned first=%p second=%p.",
			firstOccupant,
			secondOccupant);
		TraceCellInfoState("first after tunnel insertion", firstCell, first.networkType);
		TraceCellInfoState("second after tunnel insertion", secondCell, second.networkType);
		TraceNetworkOccupantState("first after tunnel insertion", firstOccupant);
		TraceNetworkOccupantState("second after tunnel insertion", secondOccupant);
		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: occupant manager insert flags first=%u second=%u.",
			static_cast<uint32_t>(FieldAt<uint8_t>(firstCell, kCellInfoInsertResultOffset) != 0),
			static_cast<uint32_t>(FieldAt<uint8_t>(secondCell, kCellInfoInsertResultOffset) != 0));

		cRZAutoRefCount<cIGZUnknown> firstTunnel;
		cRZAutoRefCount<cIGZUnknown> secondTunnel;
		if (!QueryTunnelOccupant(firstOccupant, firstTunnel) || !QueryTunnelOccupant(secondOccupant, secondTunnel))
		{
			logger.WriteLineFormatted(
				LogLevel::Error,
				"TunnelPortalTool: created occupants are not tunnel occupants, first=%p second=%p firstTunnel=%p secondTunnel=%p.",
				firstOccupant,
				secondOccupant,
				static_cast<cIGZUnknown*>(firstTunnel),
				static_cast<cIGZUnknown*>(secondTunnel));
			return false;
		}

		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: tunnel interfaces first=%p second=%p.",
			static_cast<cIGZUnknown*>(firstTunnel),
			static_cast<cIGZUnknown*>(secondTunnel));

		SetOtherEndOccupant(firstTunnel, secondOccupant);
		SetOtherEndOccupant(secondTunnel, firstOccupant);

		void* firstPathInfo = nullptr;
		void* secondPathInfo = nullptr;
		{
			void** firstTunnelVtable = *reinterpret_cast<void***>(static_cast<cIGZUnknown*>(firstTunnel));
			void** secondTunnelVtable = *reinterpret_cast<void***>(static_cast<cIGZUnknown*>(secondTunnel));
			firstPathInfo = reinterpret_cast<GetPathInfoFn>(firstTunnelVtable[0x31])(firstTunnel);
			secondPathInfo = reinterpret_cast<GetPathInfoFn>(secondTunnelVtable[0x31])(secondTunnel);
		}

		const RawPathMapNode* const firstOutgoingPath = FindPathWithMouthPoint(firstPathInfo, firstDirection, true);
		const RawPathMapNode* const secondIncomingPath = FindPathWithMouthPoint(secondPathInfo, secondDirection, false);
		const RawPathMapNode* const secondOutgoingPath = FindPathWithMouthPoint(secondPathInfo, secondDirection, true);
		const RawPathMapNode* const firstIncomingPath = FindPathWithMouthPoint(firstPathInfo, firstDirection, false);
		const uint8_t firstOutPathDirection = firstOutgoingPath ? static_cast<uint8_t>(firstOutgoingPath->key) : TunnelPieceDirectionToPathDirection(firstDirection);
		const uint16_t secondInPathKeyLowWord = secondIncomingPath ? static_cast<uint16_t>(secondIncomingPath->key) : TunnelPathKeyLowWord(TunnelPieceDirectionToPathDirection(secondDirection));
		const uint8_t secondOutPathDirection = secondOutgoingPath ? static_cast<uint8_t>(secondOutgoingPath->key) : TunnelPieceDirectionToPathDirection(secondDirection);
		const uint16_t firstInPathKeyLowWord = firstIncomingPath ? static_cast<uint16_t>(firstIncomingPath->key) : TunnelPathKeyLowWord(TunnelPieceDirectionToPathDirection(firstDirection));
		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: directed tunnel path pairing firstOutKey=0x%08X secondInKey=0x%08X secondOutKey=0x%08X firstInKey=0x%08X.",
			firstOutgoingPath ? firstOutgoingPath->key : 0,
			secondIncomingPath ? secondIncomingPath->key : 0,
			secondOutgoingPath ? secondOutgoingPath->key : 0,
			firstIncomingPath ? firstIncomingPath->key : 0);
		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: directed tunnel path directions firstOut=%u secondInLowWord=0x%04X secondOut=%u firstInLowWord=0x%04X.",
			static_cast<uint32_t>(firstOutPathDirection),
			static_cast<uint32_t>(secondInPathKeyLowWord),
			static_cast<uint32_t>(secondOutPathDirection),
			static_cast<uint32_t>(firstInPathKeyLowWord));

		RegisterCustomTunnelRouteEdgeFix(
			first,
			second,
			TunnelPieceDirectionToPathDirection(firstDirection),
			TunnelPieceDirectionToPathDirection(secondDirection),
			(firstDirection & 1) != (secondDirection & 1));
		RefreshTunnelPathInfo(firstTunnel, secondTunnel, firstDirection, secondDirection, firstOutPathDirection, secondInPathKeyLowWord);
		RefreshTunnelPathInfo(secondTunnel, firstTunnel, secondDirection, firstDirection, secondOutPathDirection, firstInPathKeyLowWord);
		MarkNetworkOccupantUsable(firstOccupant, "first");
		MarkNetworkOccupantUsable(secondOccupant, "second");
		TraceNetworkOccupantState("first after mark usable", firstOccupant);
		TraceNetworkOccupantState("second after mark usable", secondOccupant);
		NotifyTrafficSimulatorForLinkedTunnels(first, second, firstTunnel, secondTunnel);
		logger.WriteLine(
			LogLevel::Trace,
			"TunnelPortalTool: skipped synthetic occupant-added messages; traffic simulator was notified after tunnel linking.");

		logger.WriteLineFormatted(
			LogLevel::Info,
			"Placed experimental %s tunnel portal pair at (%u,%u) and (%u,%u).",
			NetworkTypeName(first.networkType),
			first.x,
			first.z,
			second.x,
			second.z);

		return true;
	}

	class TunnelPortalViewInputControl final : public cSC4BaseViewInputControl
	{
	public:
		TunnelPortalViewInputControl()
			: cSC4BaseViewInputControl(kTunnelPortalViewInputControlID)
		{
		}

		bool Init() override
		{
			const bool result = cSC4BaseViewInputControl::Init();
			if (result)
			{
				Logger::GetInstance().WriteLine(LogLevel::Debug, "TunnelPortalTool: activated, waiting for first endpoint.");
				ShowPrompt("Select first portal endpoint", "Left click a road/network tile. Esc or right click cancels.");
			}

			return result;
		}

		bool OnKeyDown(int32_t vkCode, uint32_t modifiers) override
		{
			if (vkCode == VK_ESCAPE)
			{
				Logger::GetInstance().WriteLine(LogLevel::Debug, "TunnelPortalTool: cancelled via Escape.");
				ClearFeedback();
				EndInput();
				return true;
			}

			return cSC4BaseViewInputControl::OnKeyDown(vkCode, modifiers);
		}

		bool OnMouseDownR(int32_t x, int32_t z, uint32_t modifiers) override
		{
			Logger::GetInstance().WriteLine(LogLevel::Debug, "TunnelPortalTool: cancelled via right click.");
			ClearFeedback();
			EndInput();
			return true;
		}

		bool OnMouseDownL(int32_t screenX, int32_t screenZ, uint32_t modifiers) override
		{
			if (!IsOnTop())
			{
				return false;
			}

			Endpoint endpoint;
			if (!PickEndpoint(screenX, screenZ, endpoint))
			{
				Logger::GetInstance().WriteLineFormatted(
					LogLevel::Debug,
					"TunnelPortalTool: no compatible network at screen (%d,%d).",
					screenX, screenZ);
				ShowPrompt("No compatible network tile", "Pick an existing surface network tile.");
				return true;
			}

			if (!hasFirstEndpoint)
			{
				firstEndpoint = endpoint;
				hasFirstEndpoint = true;

				Logger::GetInstance().WriteLineFormatted(
					LogLevel::Debug,
					"TunnelPortalTool: first endpoint set - %s at (%u,%u).",
					NetworkTypeName(endpoint.networkType),
					endpoint.x,
					endpoint.z);

				cRZBaseString title;
				title.Sprintf(
					"First %s portal: (%u,%u)",
					NetworkTypeName(endpoint.networkType),
					endpoint.x,
					endpoint.z);

				ShowPrompt(title.Data(), "Select second endpoint on the same network.");
				return true;
			}

			if (endpoint.x == firstEndpoint.x && endpoint.z == firstEndpoint.z)
			{
				Logger::GetInstance().WriteLineFormatted(
					LogLevel::Debug,
					"TunnelPortalTool: second endpoint (%u,%u) is the same tile as the first - rejected.",
					endpoint.x,
					endpoint.z);
				ShowPrompt("Invalid second endpoint", "Select a different network tile.");
				return true;
			}

			if (endpoint.networkType != firstEndpoint.networkType)
			{
				Logger::GetInstance().WriteLineFormatted(
					LogLevel::Debug,
					"TunnelPortalTool: network mismatch - first is %s, second is %s at (%u,%u).",
					NetworkTypeName(firstEndpoint.networkType),
					NetworkTypeName(endpoint.networkType),
					endpoint.x,
					endpoint.z);
				ShowPrompt("Network mismatch", "Select the second endpoint on the same network type.");
				return true;
			}

			const bool placed = PlacePortalPair(firstEndpoint, endpoint);
			ShowPrompt(
				placed ? "Tunnel portals linked" : "Tunnel portal placement failed",
				placed ? "Experimental portal pair committed." : "See NAM.log for details.");

			if (placed)
			{
				EndInput();
			}

			return true;
		}

		void Deactivate() override
		{
			ClearFeedback();
			cSC4BaseViewInputControl::Deactivate();
		}

	private:
		bool PickEndpoint(int32_t screenX, int32_t screenZ, Endpoint& endpoint)
		{
			if (!view3D)
			{
				return false;
			}

			float worldCoords[3] = { 0.0f, 0.0f, 0.0f };
			if (!view3D->PickTerrain(screenX, screenZ, worldCoords, view3D->GetTerrainQueryEnabled()))
			{
				Logger::GetInstance().WriteLineFormatted(
					LogLevel::Trace,
					"TunnelPortalTool: PickTerrain failed at screen (%d,%d).",
					screenX, screenZ);
				return false;
			}

			Logger::GetInstance().WriteLineFormatted(
				LogLevel::Trace,
				"TunnelPortalTool: screen (%d,%d) -> world (%.1f, %.1f, %.1f).",
				screenX, screenZ, worldCoords[0], worldCoords[1], worldCoords[2]);

			cISC4City* city = GetCity();
			if (!city)
			{
				return false;
			}

			const uint32_t maxX = city->CellCountX();
			const uint32_t maxZ = city->CellCountZ();

			if (maxX == 0 || maxZ == 0)
			{
				return false;
			}

			endpoint.x = std::min(static_cast<uint32_t>(std::max(worldCoords[0], 0.0f) / 16.0f), maxX - 1);
			endpoint.z = std::min(static_cast<uint32_t>(std::max(worldCoords[2], 0.0f) / 16.0f), maxZ - 1);

			Logger::GetInstance().WriteLineFormatted(
				LogLevel::Trace,
				"TunnelPortalTool: world (%.1f,%.1f) -> cell (%u,%u), city size %ux%u.",
				worldCoords[0], worldCoords[2], endpoint.x, endpoint.z, maxX, maxZ);

			return TryFindNetworkAtTile(endpoint.x, endpoint.z, endpoint.networkType);
		}

		void ShowPrompt(const char* titleText, const char* detailText)
		{
			if (!view3D)
			{
				return;
			}

			cRZBaseString title(titleText);
			cRZBaseString detail(detailText);
			view3D->SetCursorText(kPrimaryCursorTextID, 0, &detail, &title, 0);

			cRZBaseString hint("Right click or Esc: cancel");
			cRZBaseString mode("NAM tunnel portal tool");
			view3D->SetCursorText(kSecondaryCursorTextID, 0, &hint, &mode, 0);
		}

		void ClearFeedback()
		{
			if (view3D)
			{
				view3D->ClearCursorText(kPrimaryCursorTextID);
				view3D->ClearCursorText(kSecondaryCursorTextID);
			}
		}

		Endpoint firstEndpoint;
		bool hasFirstEndpoint = false;
	};
}

bool TunnelPortalTool::Activate(cISC4View3DWin* view3D)
{
	Logger::GetInstance().WriteLineFormatted(
		LogLevel::Debug,
		"TunnelPortalTool: activation requested, view3D=%p.",
		view3D);

	if (!view3D)
	{
		return false;
	}

	static cRZAutoRefCount<cISC4ViewInputControl> sActiveControl;

	TunnelPortalViewInputControl* control = new TunnelPortalViewInputControl();
	control->AddRef();
	sActiveControl = static_cast<cISC4ViewInputControl*>(control);
	control->Release();

	const bool activated = view3D->SetCurrentViewInputControl(
		control,
		cISC4View3DWin::ViewInputControlStackOperation_RemoveCurrentControl);

	if (!activated)
	{
		Logger::GetInstance().WriteLine(LogLevel::Error, "TunnelPortalTool: failed to set view input control.");
		sActiveControl.Reset();
	}
	else
	{
		Logger::GetInstance().WriteLine(LogLevel::Debug, "TunnelPortalTool: view input control installed.");
	}

	return activated;
}

void TunnelPortalTool::InstallDiagnostics()
{
	Patching::InstallInlineHook(sInsertTunnelPieceHook);
	Patching::InstallInlineHook(sInitTunnelPathHook);
	sInitTunnelPathTrampolinePtr = sInitTunnelPathHook.trampoline;
	Patching::InstallInlineHook(sPeerPathLookupHook);
	sPeerPathLookupTrampolinePtr = sPeerPathLookupHook.trampoline;
	Patching::InstallInlineHook(sDoConnectionsChangedEntryHook);
	sDoConnectionsChangedEntryTrampolinePtr = sDoConnectionsChangedEntryHook.trampoline;
	Patching::InstallInlineHook(sAddTrafficConnectionHook);

	constexpr uint32_t kFindPathTunnelAddTripNodeCall = 0x006d9aCF;
	constexpr uint32_t kFindPathTunnelAddTripNodeOriginalRel = 0xFFFFF4CC;
	const uint32_t tunnelAddTripNodeHookRel =
		reinterpret_cast<uint32_t>(&Hook_AddTunnelTripNode) - (kFindPathTunnelAddTripNodeCall + 5);
	Patching::PatchImmediate32(
		kFindPathTunnelAddTripNodeCall + 1,
		kFindPathTunnelAddTripNodeOriginalRel,
		tunnelAddTripNodeHookRel);

	// Verify the FindPath tunnel patch.
	{
		const uint8_t* const patchSite = reinterpret_cast<const uint8_t*>(kFindPathTunnelAddTripNodeCall);
		const uint32_t patchedRel = *reinterpret_cast<const uint32_t*>(patchSite + 1);
		const uint32_t resolvedTarget = kFindPathTunnelAddTripNodeCall + 5 + patchedRel;
		Logger::GetInstance().WriteLineFormatted(
			LogLevel::Info,
			"TunnelPortalTool: FindPath tunnel AddTripNode hook installed at 0x%08X, opcode=0x%02X rel32=0x%08X target=0x%08X hookFn=0x%08X.",
			kFindPathTunnelAddTripNodeCall,
			static_cast<uint32_t>(patchSite[0]),
			patchedRel,
			resolvedTarget,
			reinterpret_cast<uint32_t>(&Hook_AddTunnelTripNode));
	}

	// Patch FloodSubnetwork tunnel branch GetNetworkInfo call to our hook.
	constexpr uint32_t kFloodSubnetworkTunnelGetNetworkInfoCall = 0x00718215;
	constexpr uint32_t kFloodSubnetworkTunnelGetNetworkInfoOriginalRel = 0xFFFF7916;
	const uint32_t floodTunnelHookRel =
		reinterpret_cast<uint32_t>(&Hook_FloodSubnetworkTunnelGetNetworkInfo) - (kFloodSubnetworkTunnelGetNetworkInfoCall + 5);
	Patching::PatchImmediate32(
		kFloodSubnetworkTunnelGetNetworkInfoCall + 1,
		kFloodSubnetworkTunnelGetNetworkInfoOriginalRel,
		floodTunnelHookRel);

	// Verify the patch was applied by reading back the patched bytes.
	{
		const uint8_t* const patchSite = reinterpret_cast<const uint8_t*>(kFloodSubnetworkTunnelGetNetworkInfoCall);
		const uint32_t patchedRel = *reinterpret_cast<const uint32_t*>(patchSite + 1);
		const uint32_t resolvedTarget = kFloodSubnetworkTunnelGetNetworkInfoCall + 5 + patchedRel;
		Logger::GetInstance().WriteLineFormatted(
			LogLevel::Info,
			"TunnelPortalTool: FloodSubnetwork tunnel hook installed at 0x%08X, opcode=0x%02X rel32=0x%08X target=0x%08X hookFn=0x%08X.",
			kFloodSubnetworkTunnelGetNetworkInfoCall,
			static_cast<uint32_t>(patchSite[0]),
			patchedRel,
			resolvedTarget,
			reinterpret_cast<uint32_t>(&Hook_FloodSubnetworkTunnelGetNetworkInfo));
	}
}
