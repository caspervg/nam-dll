#include "TransitAccess.h"
#include "cISC4City.h"
#include "cISC4Lot.h"
#include "cISC4TrafficSimulator.h"
#include "Patching.h"
#include "SC4Rect.h"
#include "SC4Vector.h"
#include "TransitAccessPatch.h"
#include "TransitAccessSupport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <Windows.h>

namespace
{
	constexpr size_t kGetConnectedDestinationCountVTableIndex = 0x98 / sizeof(void*);
	constexpr size_t kGetSubnetworksForLotVTableIndex = 0x9c / sizeof(void*);

	using namespace TransitAccessSupport;

}

namespace TransitAccess
{
	TransitAccessPatch patch;

	bool __fastcall HookCalculateRoadAccess(cISC4Lot* lot, void*)
	{
		return patch.CalculateRoadAccess(lot);
	}

	bool __fastcall HookGetSubnetworksForLot(
		cISC4TrafficSimulator* trafficSimulator,
		void*,
		cISC4Lot* lot,
		SC4Vector<uint32_t>& subnetworks)
	{
		return patch.GetSubnetworksForLot(trafficSimulator, lot, subnetworks);
	}

	uint32_t __fastcall HookGetConnectedDestinationCount(
		cISC4TrafficSimulator* trafficSimulator,
		void*,
		cISC4Lot* lot,
		int purpose)
	{
		return patch.GetConnectedDestinationCount(trafficSimulator, lot, purpose);
	}

	bool __fastcall HookCreateStartNodes(void* pathFinder, void*)
	{
		return patch.CreateStartNodes(pathFinder);
	}

	bool __fastcall HookSetupPathFinderForLot(void* trafficSimulator, void*, void* pathFinder, cISC4Lot* lot)
	{
		return patch.SetupPathFinderForLot(trafficSimulator, pathFinder, lot);
	}
}

namespace TransitAccess
{
	TransitAccessPatch::ScopedPathFinderSourceLotRecordErase::ScopedPathFinderSourceLotRecordErase(
		TransitAccessPatch& patch,
		void* pathFinder) :
		patch(patch),
		pathFinder(pathFinder)
	{
	}

	TransitAccessPatch::ScopedPathFinderSourceLotRecordErase::~ScopedPathFinderSourceLotRecordErase()
	{
		patch.EraseRecordedSourceLotForPathFinder(pathFinder);
	}

	void TransitAccessPatch::RecordSourceLotForPathFinder(void* pathFinder, cISC4Lot* lot)
	{
		if (!pathFinder || !lot)
		{
			return;
		}

		pathFinderSourceLots[pathFinder] = lot;
	}

	cISC4Lot* TransitAccessPatch::FindRecordedSourceLotForPathFinder(void* pathFinder)
	{
		if (!pathFinder)
		{
			return nullptr;
		}

		const auto it = pathFinderSourceLots.find(pathFinder);
		return it != pathFinderSourceLots.end() ? it->second : nullptr;
	}

	void TransitAccessPatch::EraseRecordedSourceLotForPathFinder(void* pathFinder)
	{
		if (!pathFinder)
		{
			return;
		}

		pathFinderSourceLots.erase(pathFinder);
	}

	void TransitAccessPatch::ClearRecordedSourceLots()
	{
		pathFinderSourceLots.clear();
	}

	bool TransitAccessPatch::GetLotSubnetworks(cISC4TrafficSimulator* trafficSimulator, cISC4Lot* lot, SC4Vector<uint32_t>& subnetworks)
	{
		subnetworks.clear();
		if (!trafficSimulator || !lot)
		{
			return false;
		}

		// Internal callers need the game's real result, not our vtable fallback.
		return originalGetSubnetworksForLot
			? originalGetSubnetworksForLot(trafficSimulator, lot, subnetworks)
			: trafficSimulator->GetSubnetworksForLot(lot, subnetworks);
	}

	bool TransitAccessPatch::GatherAdjacentTransitSubnetworks(
		cISC4TrafficSimulator* trafficSimulator,
		cISC4Lot* lot,
		SC4Vector<uint32_t>& subnetworks)
	{
		subnetworks.clear();
		if (!trafficSimulator || !lot || !IsResidentialSourceLot(lot))
		{
			return false;
		}

		std::unordered_set<uint32_t> seen;
		const std::vector<cISC4Lot*> candidates = GetNearbySideLots(lot, 1);
		for (cISC4Lot* candidate : candidates)
		{
			// Adjacent TE lots already participate in the traffic simulator, so
			// reuse their subnetworks instead of synthesizing new identifiers.
			if (!IsTransitEnabledLot(candidate))
			{
				continue;
			}

			SC4Vector<uint32_t> candidateSubnetworks;
			if (!GetLotSubnetworks(trafficSimulator, candidate, candidateSubnetworks))
			{
				continue;
			}

			for (uint32_t subnetwork : candidateSubnetworks)
			{
				if (seen.insert(subnetwork).second)
				{
					subnetworks.push_back(subnetwork);
				}
			}
		}

		return !subnetworks.empty();
	}

	uint32_t TransitAccessPatch::GetAdjacentTransitEnabledDestinationCount(cISC4TrafficSimulator* trafficSimulator, cISC4Lot* lot, int purpose)
	{
		if (!trafficSimulator || !lot || !IsResidentialSourceLot(lot) || !originalGetConnectedDestinationCount)
		{
			return 0;
		}

		uint32_t bestCount = 0;
		const std::vector<cISC4Lot*> candidates = GetNearbySideLots(lot, 1);
		for (cISC4Lot* candidate : candidates)
		{
			// For commute purposes the blocked residential lot can use the best
			// destination count exposed by a neighboring transit-enabled lot.
			if (IsTransitEnabledLot(candidate))
			{
				bestCount = std::max(bestCount, originalGetConnectedDestinationCount(trafficSimulator, candidate, purpose));
			}
		}

		return bestCount;
	}

	cISC4Lot* TransitAccessPatch::FindSourceLotForPathFinder(void* pathFinder)
	{
		cISC4City* const city = GetCity();
		cISC4LotManager* const lotManager = city ? city->GetLotManager() : nullptr;
		if (!pathFinder || !city || !lotManager)
		{
			return nullptr;
		}

		const SC4Rect<int32_t> sourceBounds = GetPathFinderSourceRect(pathFinder);
		if (!IsValidRect(sourceBounds))
		{
			return nullptr;
		}

		const auto cityWidth = static_cast<int32_t>(city->CellCountX());
		const auto cityHeight = static_cast<int32_t>(city->CellCountZ());
		if (cityWidth <= 0 || cityHeight <= 0)
		{
			return nullptr;
		}

		const int32_t minX = std::max(0, sourceBounds.topLeftX);
		const int32_t minZ = std::max(0, sourceBounds.topLeftY);
		const int32_t maxX = sourceBounds.bottomRightX >= cityWidth ? cityWidth - 1 : sourceBounds.bottomRightX;
		const int32_t maxZ = sourceBounds.bottomRightY >= cityHeight ? cityHeight - 1 : sourceBounds.bottomRightY;
		if (minX > maxX || minZ > maxZ)
		{
			return nullptr;
		}

		cISC4Lot* fallback = nullptr;
		std::unordered_set<cISC4Lot*> seenLots;
		for (int32_t z = minZ; z <= maxZ; ++z)
		{
			for (int32_t x = minX; x <= maxX; ++x)
			{
				cISC4Lot* const candidate = lotManager->GetLot(x, z, true);
				if (!candidate || !seenLots.insert(candidate).second)
				{
					continue;
				}

				SC4Rect<int32_t> candidateBounds;
				if (candidate->GetBoundingRect(candidateBounds) && RectsEqual(candidateBounds, sourceBounds))
				{
					return candidate;
				}

				if (!fallback)
				{
					fallback = candidate;
				}
			}
		}

		return fallback;
	}

	bool TransitAccessPatch::RetryCreateStartNodesThroughTransitEnabledLot(void* pathFinder, cISC4Lot* sourceLot)
	{
		if (!IsResidentialSourceLot(sourceLot))
		{
			return false;
		}

		std::vector<cISC4Lot*> candidates;
		std::unordered_set<cISC4Lot*> seenCandidates;
		CollectAdjacentTransitEnabledRoadAccessLots(sourceLot, candidates, seenCandidates);
		if (candidates.empty())
		{
			return false;
		}

		const SC4Rect<int32_t> originalRect = GetPathFinderSourceRect(pathFinder);
		for (cISC4Lot* candidate : candidates)
		{
			// CreateStartNodes reads the source bounds directly from the pathfinder.
			// Temporarily point it at the TE lot footprint, then restore the lot.
			SC4Rect<int32_t> candidateBounds;
			if (!candidate || !candidate->GetBoundingRect(candidateBounds) || !IsValidRect(candidateBounds))
			{
				continue;
			}

			SetPathFinderSourceRect(pathFinder, candidateBounds);
			const bool retrySucceeded = originalCreateStartNodes && originalCreateStartNodes(pathFinder);
			SetPathFinderSourceRect(pathFinder, originalRect);

			if (retrySucceeded)
			{
				return true;
			}
		}

		SetPathFinderSourceRect(pathFinder, originalRect);
		return false;
	}

	bool TransitAccessPatch::CalculateRoadAccess(cISC4Lot* lot)
	{
		InstallTrafficSimulatorHook();

		if (!lot)
		{
			return false;
		}

		if (originalCalculateRoadAccess && originalCalculateRoadAccess(lot))
		{
			return true;
		}

		// Only override the cached road-access bit when the regular road-access
		// calculation failed and a neighboring TE lot has a road-like network.
		if (HasAdjacentTransitEnabledRoadAccess(lot))
		{
			SetRoadAccessCache(lot, true);
			return true;
		}

		return false;
	}

	bool TransitAccessPatch::GetSubnetworksForLot(
		cISC4TrafficSimulator* trafficSimulator,
		cISC4Lot* lot,
		SC4Vector<uint32_t>& subnetworks)
	{
		bool vanillaResult = false;
		if (originalGetSubnetworksForLot)
		{
			vanillaResult = originalGetSubnetworksForLot(trafficSimulator, lot, subnetworks);
		}
		else
		{
			subnetworks.clear();
		}

		if (vanillaResult && !subnetworks.empty())
		{
			return true;
		}

		// Lots with TE access need the same subnetwork list the adjacent TE lot
		// would have returned, otherwise later destination checks still fail.
		SC4Vector<uint32_t> fallbackSubnetworks;
		if (GatherAdjacentTransitSubnetworks(trafficSimulator, lot, fallbackSubnetworks))
		{
			subnetworks = fallbackSubnetworks;
			return true;
		}

		return vanillaResult;
	}

	uint32_t TransitAccessPatch::GetConnectedDestinationCount(
		cISC4TrafficSimulator* trafficSimulator,
		cISC4Lot* lot,
		int purpose)
	{
		const uint32_t vanillaCount = originalGetConnectedDestinationCount
			? originalGetConnectedDestinationCount(trafficSimulator, lot, purpose)
			: 0;

		if (vanillaCount != 0 || purpose < 0 || purpose > 1)
		{
			return vanillaCount;
		}

		// Purpose 0/1 are the commute destination queries affected by road access.
		const uint32_t fallbackCount = GetAdjacentTransitEnabledDestinationCount(trafficSimulator, lot, purpose);
		if (fallbackCount != 0)
		{
			return fallbackCount;
		}

		return 0;
	}

	bool TransitAccessPatch::CreateStartNodes(void* pathFinder)
	{
		if (!pathFinder)
		{
			return false;
		}

		ScopedPathFinderSourceLotRecordErase sourceLotRecordErase(*this, pathFinder);
		if (originalCreateStartNodes && originalCreateStartNodes(pathFinder))
		{
			return true;
		}

		if (insideCreateStartNodesRetry)
		{
			return false;
		}

		// The retry is deliberately last: if the vanilla source lot can create
		// pathfinder start nodes, the patch leaves that result untouched.
		cISC4Lot* sourceLot = FindRecordedSourceLotForPathFinder(pathFinder);
		if (!sourceLot)
		{
			sourceLot = FindSourceLotForPathFinder(pathFinder);
		}
		if (!sourceLot)
		{
			return false;
		}

		ScopedBoolFlag retryGuard(insideCreateStartNodesRetry);
		return RetryCreateStartNodesThroughTransitEnabledLot(pathFinder, sourceLot);
	}

	bool TransitAccessPatch::SetupPathFinderForLot(void* trafficSimulator, void* pathFinder, cISC4Lot* lot)
	{
		const bool setupSucceeded = originalSetupPathFinderForLot
			&& originalSetupPathFinderForLot(trafficSimulator, pathFinder, lot);

		// Later start-node creation only receives the pathfinder pointer. Keep a
		// short-lived association so the retry can recover the source lot cheaply.
		if (setupSucceeded && pathFinder && lot)
		{
			RecordSourceLotForPathFinder(pathFinder, lot);
		}
		else
		{
			EraseRecordedSourceLotForPathFinder(pathFinder);
		}

		return setupSucceeded;
	}

	bool TransitAccessPatch::InstallTrafficSimulatorHook()
	{
		if (trafficSimulatorHookInstalled)
		{
			return true;
		}

		cISC4TrafficSimulator* const trafficSimulator = GetTrafficSimulator();
		if (!trafficSimulator)
		{
			return false;
		}

		// The simulator instance is created by the city, so its vtable hooks are
		// installed lazily once gzcom-dll can expose the live city object.
		auto** const vtable = *reinterpret_cast<void***>(trafficSimulator);
		if (!vtable || !vtable[kGetConnectedDestinationCountVTableIndex] || !vtable[kGetSubnetworksForLotVTableIndex])
		{
			return false;
		}

		void** const destinationSlot = &vtable[kGetConnectedDestinationCountVTableIndex];
		void** const subnetworksSlot = &vtable[kGetSubnetworksForLotVTableIndex];
		trafficSimulatorVTable = vtable;

		DWORD oldProtect;
		if (*destinationSlot != reinterpret_cast<void*>(&HookGetConnectedDestinationCount))
		{
			if (!VirtualProtect(destinationSlot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
			{
				return false;
			}

			originalGetConnectedDestinationCountSlot = *destinationSlot;
			originalGetConnectedDestinationCount =
				reinterpret_cast<GetConnectedDestinationCountFn>(originalGetConnectedDestinationCountSlot);
			*destinationSlot = reinterpret_cast<void*>(&HookGetConnectedDestinationCount);
			FlushInstructionCache(GetCurrentProcess(), destinationSlot, sizeof(void*));
			VirtualProtect(destinationSlot, sizeof(void*), oldProtect, &oldProtect);
		}

		if (*subnetworksSlot != reinterpret_cast<void*>(&HookGetSubnetworksForLot))
		{
			if (!VirtualProtect(subnetworksSlot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
			{
				return false;
			}

			originalGetSubnetworksForLotSlot = *subnetworksSlot;
			originalGetSubnetworksForLot = reinterpret_cast<GetSubnetworksForLotFn>(originalGetSubnetworksForLotSlot);
			*subnetworksSlot = reinterpret_cast<void*>(&HookGetSubnetworksForLot);
			FlushInstructionCache(GetCurrentProcess(), subnetworksSlot, sizeof(void*));
			VirtualProtect(subnetworksSlot, sizeof(void*), oldProtect, &oldProtect);
		}

		trafficSimulatorHookInstalled = true;
		return true;
	}

	void TransitAccessPatch::UninstallTrafficSimulatorHook()
	{
		if (!trafficSimulatorHookInstalled || !trafficSimulatorVTable)
		{
			return;
		}

		DWORD oldProtect;
		if (originalGetConnectedDestinationCountSlot)
		{
			void** const destinationSlot = &trafficSimulatorVTable[kGetConnectedDestinationCountVTableIndex];
			if (VirtualProtect(destinationSlot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
			{
				if (*destinationSlot == reinterpret_cast<void*>(&HookGetConnectedDestinationCount))
				{
					*destinationSlot = originalGetConnectedDestinationCountSlot;
					FlushInstructionCache(GetCurrentProcess(), destinationSlot, sizeof(void*));
				}
				VirtualProtect(destinationSlot, sizeof(void*), oldProtect, &oldProtect);
			}
		}

		if (originalGetSubnetworksForLotSlot)
		{
			void** const subnetworksSlot = &trafficSimulatorVTable[kGetSubnetworksForLotVTableIndex];
			if (VirtualProtect(subnetworksSlot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
			{
				if (*subnetworksSlot == reinterpret_cast<void*>(&HookGetSubnetworksForLot))
				{
					*subnetworksSlot = originalGetSubnetworksForLotSlot;
					FlushInstructionCache(GetCurrentProcess(), subnetworksSlot, sizeof(void*));
				}
				VirtualProtect(subnetworksSlot, sizeof(void*), oldProtect, &oldProtect);
			}
		}

		trafficSimulatorVTable = nullptr;
		originalGetConnectedDestinationCountSlot = nullptr;
		originalGetConnectedDestinationCount = nullptr;
		originalGetSubnetworksForLotSlot = nullptr;
		originalGetSubnetworksForLot = nullptr;
		trafficSimulatorHookInstalled = false;
	}

	void TransitAccessPatch::Install()
	{
		Patching::InstallInlineHook(calculateRoadAccessHook);
		originalCalculateRoadAccess = reinterpret_cast<CalculateRoadAccessFn>(calculateRoadAccessHook.trampoline);

		InstallTrafficSimulatorHook();

		Patching::InstallInlineHook(createStartNodesHook);
		originalCreateStartNodes = reinterpret_cast<CreateStartNodesFn>(createStartNodesHook.trampoline);

		Patching::InstallInlineHook(setupPathFinderForLotHook);
		originalSetupPathFinderForLot = reinterpret_cast<SetupPathFinderForLotFn>(setupPathFinderForLotHook.trampoline);
	}

	void TransitAccessPatch::Shutdown()
	{
		UninstallTrafficSimulatorHook();
		Patching::UninstallInlineHook(setupPathFinderForLotHook);
		originalSetupPathFinderForLot = nullptr;
		Patching::UninstallInlineHook(createStartNodesHook);
		originalCreateStartNodes = nullptr;
		Patching::UninstallInlineHook(calculateRoadAccessHook);
		originalCalculateRoadAccess = nullptr;
		ClearRecordedSourceLots();
	}
}

void TransitAccess::Install()
{
	patch.Install();
}

void TransitAccess::Shutdown()
{
	patch.Shutdown();
}
