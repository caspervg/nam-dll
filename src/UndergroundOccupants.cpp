#include "UndergroundOccupants.h"
#include "Logger.h"
#include "Patching.h"
#include "cISC4NetworkOccupant.h"
#include <cstring>
#include <Windows.h>
#include "wil/result_macros.h"

namespace
{
	// When the terrain of some cells changes, cSC4NetworkManager::DoMessage collects the network occupants of those
	// cells that are of type Pipe (4) or Subway (7), so that the underground views can rebuild their geometry (0x6588d0).
	// That code treats each of them as a cSC4UndergroundOccupant and calls one of its virtual methods (vtable offset
	// 0x178). A model-based occupant that has Pipe or Subway among its networks, such as the Vanishing Construction Piece
	// that the Network Eraser leaves behind on an erased FLUP starter, has a different method in that slot. It adjusts
	// its this pointer by whatever is in memory in front of the occupant, which crashes the game.
	// We leave such occupants out of the pipe and subway lists, as they have no underground geometry.
	constexpr uint32_t TerrainChanged_InjectPoint = 0x613e09;
	constexpr uint32_t TerrainChanged_PipeList = 0x613e0e;  // adds the occupant to the pipe list
	constexpr uint32_t TerrainChanged_SubwayCheck = 0x613e20;  // cmp esi, 7
	constexpr uint32_t TerrainChanged_NextNetworkType = 0x613e3a;  // inc esi

	constexpr uint8_t TerrainChanged_ExpectedBytes[] = {
		0x83, 0xfe, 0x04,  // cmp esi, 4
		0x75, 0x12,  // jne 0x613e20
	};

	constexpr uint32_t kPipeNetworkType = 4;
	constexpr uint32_t kSubwayNetworkType = 7;
	constexpr uint32_t kNetworkTypesMask = 0x1fff;  // one bit per network type, in the low bits of the network flags

	// answered by cSC4UndergroundOccupant::QueryInterface (0x65a080), which the subway and pipe occupants derive from
	constexpr uint32_t kUndergroundOccupantIID = 0x4a10afc2;

	bool IsUndergroundOccupant(cISC4NetworkOccupant* networkOccupant, uint32_t networkType)
	{
		cIGZUnknown* undergroundOccupant = nullptr;
		if (networkOccupant->QueryInterface(kUndergroundOccupantIID, reinterpret_cast<void**>(&undergroundOccupant))) {
			undergroundOccupant->Release();
			return true;
		}

		uint32_t cellX = 0;
		uint32_t cellZ = 0;
		networkOccupant->GetOccupiedCell(cellX, cellZ);
		const uint32_t flags = networkOccupant->GetNetworkFlag();
		Logger::GetInstance().WriteLineFormatted(
			LogLevel::Info,
			"Skipped a %s occupant at cell (%u, %u) that is not an underground occupant while updating the underground "
			"after a terrain change (model 0x%08X, networks 0x%04X, flags 0x%08X). Such occupants are usually left "
			"behind by the Network Eraser, and would otherwise crash the game.",
			networkType == kPipeNetworkType ? "Pipe" : "Subway",
			cellX,
			cellZ,
			networkOccupant->PieceId(),  // the model of a model-based occupant
			flags & kNetworkTypesMask,
			flags);
		return false;
	}

	void NAKED_FUN Hook_TerrainChanged(void)
	{
		__asm {
			// esi = network type, edi = cISC4NetworkOccupant* that is of this network type
			cmp esi, kPipeNetworkType;
			je checkUnderground;
			cmp esi, kSubwayNetworkType;
			jne continueOriginal;
checkUnderground:
			// eax, ecx and edx are free here: the game reloads them before using them again
			push esi;  // networkType
			push edi;  // networkOccupant
			call IsUndergroundOccupant;
			add esp, 8;
			test al, al;
			jnz continueOriginal;
			push TerrainChanged_NextNetworkType;  // leave the occupant out of the pipe and subway lists
			ret;
continueOriginal:
			cmp esi, kPipeNetworkType;
			jne checkSubway;
			push TerrainChanged_PipeList;
			ret;
checkSubway:
			push TerrainChanged_SubwayCheck;
			ret;
		}
	}
}

void UndergroundOccupants::Install()
{
	THROW_HR_IF(E_FAIL, std::memcmp(
		reinterpret_cast<const void*>(TerrainChanged_InjectPoint),
		TerrainChanged_ExpectedBytes,
		sizeof(TerrainChanged_ExpectedBytes)) != 0);
	Patching::InstallHook(TerrainChanged_InjectPoint, Hook_TerrainChanged);
}
