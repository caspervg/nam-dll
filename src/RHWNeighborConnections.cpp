#include "RHWNeighborConnections.h"
#include "Patching.h"
#include <algorithm>

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
	constexpr uint32_t kMinimumSearchDistance = 1;
	constexpr uint32_t kMaximumSearchDistance = 64;

	uint32_t g_maxSearchDistance = kMinimumSearchDistance;

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

	uint16_t GetNeighborConnectionMask(void* const pTrafficSimulator, const int32_t x, const int32_t z)
	{
		const void* const pConnection = pGetTrafficNeighborConnection(pTrafficSimulator, x, z);
		return pConnection ? *static_cast<const uint16_t*>(pConnection) : 0;
	}

	bool IsValidBoundaryCoordinate(void* const pTrafficSimulator, const int32_t x, const int32_t z)
	{
		const auto* const bytes = static_cast<const uint8_t*>(pTrafficSimulator);
		const int32_t maxX = *reinterpret_cast<const int32_t*>(bytes + 0x28);
		const int32_t maxZ = *reinterpret_cast<const int32_t*>(bytes + 0x2c);
		const bool xOutside = x < 0 || x > maxX;
		const bool zOutside = z < 0 || z > maxZ;

		if (xOutside == zOutside)
		{
			return false;
		}

		return xOutside ? z >= 0 && z <= maxZ : x >= 0 && x <= maxX;
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

		for (uint32_t distance = 1; distance <= g_maxSearchDistance; ++distance)
		{
			const int32_t candidateX = originX + stepX * static_cast<int32_t>(distance);
			const int32_t candidateZ = originZ + stepZ * static_cast<int32_t>(distance);
			if (!IsValidBoundaryCoordinate(pTrafficSimulator, candidateX, candidateZ))
			{
				break;
			}

			const uint16_t candidateMask =
				GetNeighborConnectionMask(pTrafficSimulator, candidateX, candidateZ);
			if ((candidateMask & kDirtRoadNeighborConnectionMask) != 0)
			{
				*pX = candidateX;
				*pZ = candidateZ;
				return;
			}
		}

		// Preserve ordinary one-tile DirtRoad/RHW-2 behavior when no paired
		// carriageway exists in the configured search range.
		*pX = originX;
		*pZ = originZ;
	}
}

void RHWNeighborConnections::Install(const uint32_t maxSearchDistance)
{
	g_maxSearchDistance = std::clamp(
		maxSearchDistance,
		kMinimumSearchDistance,
		kMaximumSearchDistance);

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
