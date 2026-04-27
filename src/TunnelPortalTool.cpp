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
		uint8_t tunnelPieceDirection)
	{
		if (!cellInfo)
		{
			return;
		}

		const uint32_t oldNetworkFlags = cellInfo->networkTypeFlags;
		const uint32_t oldCombinedEdges = cellInfo->edgeFlagsCombined;
		const uint32_t oldNetworkEdges = cellInfo->edgesPerNetwork[static_cast<uint32_t>(networkType)];
		const uint32_t portalEdges = GetPortalAxisEdges(tunnelPieceDirection);

		cellInfo->networkTypeFlags |= kTunnelCellNetworkFlag;
		cellInfo->edgeFlagsCombined |= portalEdges;
		cellInfo->edgesPerNetwork[static_cast<uint32_t>(networkType)] |= portalEdges;
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

	// Windows InsertTunnelPieces (0x006287d0) QueryInterfaces both occupants to
	// kcSC4NetworkTunnelOccupant, then calls tunnel->GetPathInfo (+0xc4) and
	// pathInfo->InitTunnelPath (+0x80) with the two tunnel-interface pointers.
	void RefreshTunnelPathInfo(cIGZUnknown* self, cIGZUnknown* otherEnd)
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

		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: GetPathInfo self=%p otherEnd=%p pathInfo=%p.",
			self,
			otherEnd,
			pathInfo);

		if (!pathInfo)
		{
			return;
		}

		void** pathInfoVtable = *reinterpret_cast<void***>(pathInfo);
		InitTunnelPathFn initTunnelPath = reinterpret_cast<InitTunnelPathFn>(pathInfoVtable[0x20]);
		initTunnelPath(pathInfo, self, otherEnd);

		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: refreshed path info pathInfo=%p self=%p otherEnd=%p.",
			pathInfo,
			self,
			otherEnd);
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

		DoTunnelChanged(trafficSimulator, firstTunnel, true);
		DoTunnelChanged(trafficSimulator, secondTunnel, true);
		DoConnectionsChanged(trafficSimulator, first.x, first.z, first.x, first.z);
		DoConnectionsChanged(trafficSimulator, second.x, second.z, second.x, second.z);

		logger.WriteLineFormatted(
			LogLevel::Trace,
			"TunnelPortalTool: notified traffic simulator for linked tunnel endpoints (%u,%u) and (%u,%u).",
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

		cISC4NetworkOccupant* firstOccupant = nullptr;
		cISC4NetworkOccupant* secondOccupant = nullptr;
		{
			NetworkToolPlacementStateScope placementState(tool);
			CustomTunnelInsertionScope customInsertion;
			const uint8_t firstVectorDirection = InferTunnelPieceDirection(first, second);
			const uint8_t secondVectorDirection = InferTunnelPieceDirection(second, first);
			uint8_t firstDirection = firstVectorDirection;
			uint8_t secondDirection = secondVectorDirection;
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
			PrepareTunnelEndpointCell("first", firstCell, first.networkType, firstDirection);
			PrepareTunnelEndpointCell("second", secondCell, second.networkType, secondDirection);
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

		RefreshTunnelPathInfo(firstTunnel, secondTunnel);
		RefreshTunnelPathInfo(secondTunnel, firstTunnel);
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
}
