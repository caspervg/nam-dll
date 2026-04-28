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
#include "cIS3DModelInstance.h"
#include "cRZAutoRefCount.h"
#include "cRZBaseString.h"
#include "cSC4BaseViewInputControl.h"
#include "GZCLSIDDefs.h"
#include "Patching.h"

#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <array>
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

	InsertTunnelPieceFn InsertTunnelPiece = reinterpret_cast<InsertTunnelPieceFn>(0x00628390);
	SetOtherEndOccupantFn SetOtherEndOccupant = reinterpret_cast<SetOtherEndOccupantFn>(0x00647530);
	DoTunnelChangedFn DoTunnelChanged = reinterpret_cast<DoTunnelChangedFn>(0x007140e0);
	DoConnectionsChangedFn DoConnectionsChanged = reinterpret_cast<DoConnectionsChangedFn>(0x0071a860);
	GetOccupantExemplarFn GetOccupantExemplar = reinterpret_cast<GetOccupantExemplarFn>(0x006244b0);

	uint32_t sCustomTunnelInsertionDepth = 0;

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

	const RawPathMapNode* FindFirstRawPathKeyForDirection(void* pathInfo, uint8_t pathDirection)
	{
		const RawPathMap* const map = GetPathMap(pathInfo);
		if (!IsTraceablePathMap(map))
		{
			return nullptr;
		}

		uint32_t totalKeys = 0;
		for (RawPathMapNode** bucket = map->start; bucket != map->end && totalKeys < kMaxPathInfoNodesToTrace; ++bucket)
		{
			for (RawPathMapNode* node = *bucket, *next = nullptr; node && totalKeys < kMaxPathInfoNodesToTrace; node = next)
			{
				next = node->next;
				++totalKeys;
				if (static_cast<uint8_t>(node->key) == pathDirection && CountRawPathPoints(node->path) > 0)
				{
					return node;
				}
			}
		}

		return nullptr;
	}

	float Cross2D(const RawPathPoint& a, const RawPathPoint& b, const RawPathPoint& c)
	{
		const float abx = b.x - a.x;
		const float abz = b.z - a.z;
		const float acx = c.x - a.x;
		const float acz = c.z - a.z;
		return abx * acz - abz * acx;
	}

	float DistanceSquared2D(const RawPathPoint& a, const RawPathPoint& b)
	{
		const float dx = b.x - a.x;
		const float dz = b.z - a.z;
		return dx * dx + dz * dz;
	}

	bool SegmentsCross2D(const RawPathPoint& a, const RawPathPoint& b, const RawPathPoint& c, const RawPathPoint& d)
	{
		const float c1 = Cross2D(a, b, c);
		const float c2 = Cross2D(a, b, d);
		const float c3 = Cross2D(c, d, a);
		const float c4 = Cross2D(c, d, b);
		return (c1 * c2 < 0.0f) && (c3 * c4 < 0.0f);
	}

	bool TryScoreTunnelLanePairing(
		void* firstPathInfo,
		void* secondPathInfo,
		uint8_t firstPathDirection,
		uint8_t secondPathDirection,
		uint8_t firstPeerLookupDirection,
		uint8_t secondPeerLookupDirection,
		float& scoreOut)
	{
		const RawPathMapNode* const firstSelf = FindFirstRawPathKeyForDirection(firstPathInfo, firstPathDirection);
		const RawPathMapNode* const secondSelf = FindFirstRawPathKeyForDirection(secondPathInfo, secondPathDirection);
		const RawPathMapNode* const firstPeer = FindFirstRawPathKeyForDirection(firstPathInfo, secondPeerLookupDirection);
		const RawPathMapNode* const secondPeer = FindFirstRawPathKeyForDirection(secondPathInfo, firstPeerLookupDirection);

		const RawPathPoint* const a0 = firstSelf ? LastRawPathPoint(firstSelf->path) : nullptr;
		const RawPathPoint* const b0 = secondPeer ? FirstRawPathPoint(secondPeer->path) : nullptr;
		const RawPathPoint* const b1 = secondSelf ? LastRawPathPoint(secondSelf->path) : nullptr;
		const RawPathPoint* const a1 = firstPeer ? FirstRawPathPoint(firstPeer->path) : nullptr;

		if (!a0 || !b0 || !b1 || !a1)
		{
			return false;
		}

		scoreOut = DistanceSquared2D(*a0, *b0) + DistanceSquared2D(*b1, *a1);
		if (SegmentsCross2D(*a0, *b0, *b1, *a1))
		{
			scoreOut += 1000000000.0f;
		}

		return true;
	}

	void ChooseTunnelLanePairing(
		void* firstPathInfo,
		void* secondPathInfo,
		uint8_t firstPathDirection,
		uint8_t secondPathDirection,
		uint8_t& firstPeerLookupDirection,
		uint8_t& secondPeerLookupDirection)
	{
		Logger& logger = Logger::GetInstance();
		const uint8_t firstCandidates[2] = { secondPathDirection, static_cast<uint8_t>(secondPathDirection ^ 2) };
		const uint8_t secondCandidates[2] = { firstPathDirection, static_cast<uint8_t>(firstPathDirection ^ 2) };
		float bestScore = 3.4e38f;
		bool found = false;

		firstPeerLookupDirection = secondPathDirection;
		secondPeerLookupDirection = firstPathDirection;

		for (uint8_t firstCandidate : firstCandidates)
		{
			for (uint8_t secondCandidate : secondCandidates)
			{
				float score = 0.0f;
				if (TryScoreTunnelLanePairing(
					firstPathInfo,
					secondPathInfo,
					firstPathDirection,
					secondPathDirection,
					firstCandidate,
					secondCandidate,
					score))
				{
					logger.WriteLineFormatted(
						LogLevel::Trace,
						"TunnelPortalTool: lane pairing candidate firstPeerDirection=%u secondPeerDirection=%u score=%.1f.",
						static_cast<uint32_t>(firstCandidate),
						static_cast<uint32_t>(secondCandidate),
						score);
					if (!found || score < bestScore)
					{
						found = true;
						bestScore = score;
						firstPeerLookupDirection = firstCandidate;
						secondPeerLookupDirection = secondCandidate;
					}
				}
			}
		}

		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: selected lane pairing firstPeerDirection=%u secondPeerDirection=%u score=%.1f found=%u.",
			static_cast<uint32_t>(firstPeerLookupDirection),
			static_cast<uint32_t>(secondPeerLookupDirection),
			bestScore,
			found ? 1u : 0u);
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

	void TraceModelInstances(const char* label, cISC4NetworkOccupant* occupant)
	{
		Logger& logger = Logger::GetInstance();

		if (!occupant)
		{
			return;
		}

		for (int32_t zoom = 0; zoom <= 5; ++zoom)
		{
			cIS3DModelInstance* modelInstances[4] = {};
			int32_t modelInstanceCount = 0;
			occupant->GetModelInstances(zoom, 0, modelInstances, 4, &modelInstanceCount);

			logger.WriteLineFormatted(
				LogLevel::Trace,
				"TunnelPortalTool: %s GetModelInstances zoom=%d count=%d first=%p.",
				label,
				zoom,
				modelInstanceCount,
				modelInstances[0]);

			for (int32_t i = 0; i < modelInstanceCount && i < 4; ++i)
			{
				if (modelInstances[i])
				{
					modelInstances[i]->Release();
				}
			}
		}
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
		uint8_t peerLookupPathDirection)
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
		const uint16_t peerPathKeyLowWord = TunnelPathKeyLowWord(peerLookupPathDirection);
		TracePathInfoKeyMap("before InitTunnelPath", pathInfo, otherPathInfo, selfPathDirection);
		const uint32_t hookHitsBefore = sInitTunnelPathHookHitCount;
		const uint32_t hookOverridesBefore = sInitTunnelPathOverrideCount;
		const uint32_t peerLookupHitsBefore = sPeerPathLookupHookHitCount;
		const uint32_t peerLookupOverridesBefore = sPeerPathLookupOverrideCount;
		sLastInitTunnelPathOriginalDirection = 0xFF;
		sLastInitTunnelPathOverrideDirection = 0xFF;
		sLastPeerPathLookupOriginalKey = 0;
		sLastPeerPathLookupOverrideKey = 0;
		sCustomPortalFacingOverride = selfPathDirection;
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
		TracePathInfoKeyMap("after InitTunnelPath", pathInfo, otherPathInfo, selfPathDirection);

		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: refreshed path info pathInfo=%p otherPathInfo=%p self=%p otherEnd=%p facing=%u pathDirection=%u otherFacing=%u otherPathDirection=%u peerLookupPathDirection=%u peerKeyLowWord=0x%04X hookHits=%u hookOverrides=%u originalDirection=%u overrideDirection=%u peerLookupHits=%u peerLookupOverrides=%u peerOriginalKey=0x%08X peerOverrideKey=0x%08X.",
			pathInfo,
			otherPathInfo,
			self,
			otherEnd,
			static_cast<uint32_t>(selfFacing),
			static_cast<uint32_t>(selfPathDirection),
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

		DoTunnelChanged(trafficSimulator, firstTunnel, false);
		DoTunnelChanged(trafficSimulator, secondTunnel, false);
		DoTunnelChanged(trafficSimulator, firstTunnel, true);
		DoTunnelChanged(trafficSimulator, secondTunnel, true);
		DoConnectionsChanged(trafficSimulator, first.x, first.z, first.x, first.z);
		DoConnectionsChanged(trafficSimulator, second.x, second.z, second.x, second.z);
		DoConnectionsChanged(
			trafficSimulator,
			std::min(first.x, second.x),
			std::min(first.z, second.z),
			std::max(first.x, second.x),
			std::max(first.z, second.z));

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
		TraceModelInstances("first after tunnel insertion", firstOccupant);
		TraceModelInstances("second after tunnel insertion", secondOccupant);
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

		const uint8_t firstPathDirection = TunnelPieceDirectionToPathDirection(firstDirection);
		const uint8_t secondPathDirection = TunnelPieceDirectionToPathDirection(secondDirection);
		uint8_t firstPeerLookupPathDirection = secondPathDirection;
		uint8_t secondPeerLookupPathDirection = firstPathDirection;
		ChooseTunnelLanePairing(
			firstPathInfo,
			secondPathInfo,
			firstPathDirection,
			secondPathDirection,
			firstPeerLookupPathDirection,
			secondPeerLookupPathDirection);

		RefreshTunnelPathInfo(firstTunnel, secondTunnel, firstDirection, secondDirection, firstPeerLookupPathDirection);
		RefreshTunnelPathInfo(secondTunnel, firstTunnel, secondDirection, firstDirection, secondPeerLookupPathDirection);
		MarkNetworkOccupantUsable(firstOccupant, "first");
		MarkNetworkOccupantUsable(secondOccupant, "second");
		TraceNetworkOccupantState("first after mark usable", firstOccupant);
		TraceNetworkOccupantState("second after mark usable", secondOccupant);
		TraceModelInstances("first after mark usable", firstOccupant);
		TraceModelInstances("second after mark usable", secondOccupant);
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
}
