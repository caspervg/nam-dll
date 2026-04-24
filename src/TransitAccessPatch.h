#pragma once

#include "Patching.h"
#include "SC4Vector.h"

#include <cstdint>
#include <unordered_map>

class cISC4Lot;
class cISC4TrafficSimulator;

namespace TransitAccess
{
	// The naked hooks stay as free functions so the patch can use the game's
	// calling conventions and immediately delegate into the stateful patch class.
	bool __fastcall HookCalculateRoadAccess(cISC4Lot* lot, void*);
	bool __fastcall HookGetSubnetworksForLot(cISC4TrafficSimulator* trafficSimulator, void*, cISC4Lot* lot, SC4Vector<uint32_t>& subnetworks);
	uint32_t __fastcall HookGetConnectedDestinationCount(cISC4TrafficSimulator* trafficSimulator, void*, cISC4Lot* lot, int purpose);
	bool __fastcall HookCreateStartNodes(void* pathFinder, void*);
	bool __fastcall HookSetupPathFinderForLot(void* trafficSimulator, void*, void* pathFinder, cISC4Lot* lot);

	// Implements the TE-lot access patch. Residential lots still ask the vanilla
	// simulator first; the patch only borrows access, subnetworks, and start
	// nodes from adjacent transit-enabled lots when the vanilla result is empty.
	class TransitAccessPatch final
	{
		class ScopedPathFinderSourceLotRecordErase
		{
		public:
			ScopedPathFinderSourceLotRecordErase(TransitAccessPatch& patch, void* pathFinder);
			ScopedPathFinderSourceLotRecordErase(const ScopedPathFinderSourceLotRecordErase&) = delete;
			ScopedPathFinderSourceLotRecordErase& operator=(const ScopedPathFinderSourceLotRecordErase&) = delete;
			~ScopedPathFinderSourceLotRecordErase();

		private:
			TransitAccessPatch& patch;
			void* pathFinder;
		};

		friend class ScopedPathFinderSourceLotRecordErase;

	public:
		void Install();
		void Shutdown();

		bool CalculateRoadAccess(cISC4Lot* lot);
		bool GetSubnetworksForLot(cISC4TrafficSimulator* trafficSimulator, cISC4Lot* lot, SC4Vector<uint32_t>& subnetworks);
		uint32_t GetConnectedDestinationCount(cISC4TrafficSimulator* trafficSimulator, cISC4Lot* lot, int purpose);
		bool CreateStartNodes(void* pathFinder);
		bool SetupPathFinderForLot(void* trafficSimulator, void* pathFinder, cISC4Lot* lot);

	private:
		using CalculateRoadAccessFn = bool(__thiscall*)(cISC4Lot*);
		using CreateStartNodesFn = bool(__thiscall*)(void*);
		using SetupPathFinderForLotFn = bool(__thiscall*)(void*, void*, cISC4Lot*);
		using GetConnectedDestinationCountFn = uint32_t(__thiscall*)(cISC4TrafficSimulator*, cISC4Lot*, int);
		using GetSubnetworksForLotFn = bool(__thiscall*)(cISC4TrafficSimulator*, cISC4Lot*, SC4Vector<uint32_t>&);

		void RecordSourceLotForPathFinder(void* pathFinder, cISC4Lot* lot);
		cISC4Lot* FindRecordedSourceLotForPathFinder(void* pathFinder);
		void EraseRecordedSourceLotForPathFinder(void* pathFinder);
		void ClearRecordedSourceLots();

		bool InstallTrafficSimulatorHook();
		void UninstallTrafficSimulatorHook();

		bool GetLotSubnetworks(cISC4TrafficSimulator* trafficSimulator, cISC4Lot* lot, SC4Vector<uint32_t>& subnetworks);
		bool GatherAdjacentTransitSubnetworks(cISC4TrafficSimulator* trafficSimulator, cISC4Lot* lot, SC4Vector<uint32_t>& subnetworks);
		uint32_t GetAdjacentTransitEnabledDestinationCount(cISC4TrafficSimulator* trafficSimulator, cISC4Lot* lot, int purpose);
		cISC4Lot* FindSourceLotForPathFinder(void* pathFinder);
		bool RetryCreateStartNodesThroughTransitEnabledLot(void* pathFinder, cISC4Lot* sourceLot);

		CalculateRoadAccessFn originalCalculateRoadAccess = nullptr;
		CreateStartNodesFn originalCreateStartNodes = nullptr;
		SetupPathFinderForLotFn originalSetupPathFinderForLot = nullptr;
		GetConnectedDestinationCountFn originalGetConnectedDestinationCount = nullptr;
		GetSubnetworksForLotFn originalGetSubnetworksForLot = nullptr;

		// Path searches can later reach CreateStartNodes with only the pathfinder
		// pointer. This side table remembers the source lot from SetupPathFinderForLot.
		std::unordered_map<void*, cISC4Lot*> pathFinderSourceLots;
		bool insideCreateStartNodesRetry = false;

		void** trafficSimulatorVTable = nullptr;
		void* originalGetConnectedDestinationCountSlot = nullptr;
		void* originalGetSubnetworksForLotSlot = nullptr;
		bool trafficSimulatorHookInstalled = false;

		Patching::InlineHook calculateRoadAccessHook{
			.address = 0x006c1a30,
			.hookFunction = reinterpret_cast<void*>(&HookCalculateRoadAccess),
			.expectedPrologue = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8},
			.hasExpectedPrologue = true,
			.patchAddress = 0,
			.original = {},
			.trampoline = nullptr,
			.installed = false
		};

		Patching::InlineHook createStartNodesHook{
			.address = 0x006d8a90,
			.hookFunction = reinterpret_cast<void*>(&HookCreateStartNodes),
			.expectedPrologue = {0x83, 0xec, 0x5c, 0x53, 0x55, 0x56},
			.hasExpectedPrologue = true,
			.patchAddress = 0,
			.original = {},
			.trampoline = nullptr,
			.installed = false
		};

		Patching::InlineHook setupPathFinderForLotHook{
			.address = 0x00711610,
			.hookFunction = reinterpret_cast<void*>(&HookSetupPathFinderForLot),
			.expectedPrologue = {0x83, 0xec, 0x14, 0x53, 0x55, 0x56},
			.hasExpectedPrologue = true,
			.patchAddress = 0,
			.original = {},
			.trampoline = nullptr,
			.installed = false
		};
	};

	extern TransitAccessPatch patch;
}
