#include "BridgePatches.h"
#include "Patching.h"
#include "NetworkStubs.h"
#include "cISTETerrain.h"
#include <algorithm>

// How the network tool places bridges (cSC4NetworkTool, from the Mac binary, verified on Windows):
//
// IdentifyBridgeCells (0x632830) scans the dragged steps from index 2 for the first step containing
// water (cISTETerrain::CellHasWater, true if any corner of the cell is under water). The step before it
// becomes the near abutment. SelectBridgeStartAndEnd (0x630570) then walks forward over the water,
// checking each step with IsStepGoodForBridge (0x62fdb0) and CheckBridgeStepEdgeTypes (0x62eb80),
// until it reaches the first completely dry step, which becomes the far abutment.
//
// IsStepGoodForBridge rejects a step if any existing surface-transport network occupant lies within
// the cells of the step, expanded by one tile in every direction.
namespace
{
	// IsStepGoodForBridge(cISC4OccupantFilter*, int x1, int z1, int x2, int z2) (free function),
	// custom calling convention: occupant manager in esi, arguments on stack, caller cleans up.
	constexpr uint32_t IsStepGoodForBridge = 0x62fdb0;

	// Patch 1: Last-tile abutment
	//
	// SelectBridgeStartAndEnd requires the far abutment to lie before the last dragged step:
	//   mov edx, dword ptr [edi + 0x78]  ; number of valid dragged steps
	//   dec edx
	//   cmp eax, edx                     ; current step < validSteps - 1
	// so the drag has to extend one tile beyond the far abutment, otherwise the whole drag fails.
	// The near abutment has no such requirement as it may be a partially submerged shore tile.
	// Removing the `dec` matches the bound that PadStepsToMakeMinBridgeSpan (0x62ecd0) already uses
	// when it extends a bridge forwards, so bridges ending on the last dragged step already occur.
	constexpr uint32_t SelectBridgeStartAndEnd_ValidStepsBound = 0x6305af;
	constexpr uint32_t SelectBridgeStartAndEnd_DecValidSteps = 0x6305b2;

	// Patch 2: Far abutment clearance
	//
	// In SelectBridgeStartAndEnd, the IsStepGoodForBridge check also runs on the far abutment, before it
	// is known to be dry. Its one-tile margin then includes the tile beyond the far abutment. The
	// abutment row itself is already covered by the margin of the last water step, and the near
	// abutment never gets this check, so only the tile row behind the far abutment is affected.
	constexpr uint32_t SelectBridgeStartAndEnd_IsStepGoodForBridgeCall = 0x630604;

	bool __cdecl isFarAbutmentStep(const cSC4NetworkTool* networkTool, uint32_t x1, uint32_t z1, uint32_t x2, uint32_t z2)
	{
		cISTETerrain* const terrain = networkTool->terrain;
		if (terrain == nullptr) {
			return false;
		}
		// The bounding box of the first and last cell of the step. For diagonal steps, this may
		// include cells not belonging to the step, which errs on the side of the vanilla check.
		for (uint32_t x = std::min(x1, x2); x <= std::max(x1, x2); x++) {
			for (uint32_t z = std::min(z1, z2); z <= std::max(z1, z2); z++) {
				if (terrain->CellHasWater(x, z)) {
					return false;  // water step, not the far abutment
				}
			}
		}
		return true;  // first dry step after the water, i.e. the far abutment
	}

	void NAKED_FUN Hook_SelectBridgeStartAndEnd_IsStepGoodForBridge(void)
	{
		// esi: cISC4OccupantManager*, edi: cSC4NetworkTool*
		// [esp+0x4] filter, [esp+0x8] x1, [esp+0xc] z1, [esp+0x10] x2, [esp+0x14] z2
		__asm {
			push dword ptr [esp + 0x14];  // z2
			push dword ptr [esp + 0x14];  // x2
			push dword ptr [esp + 0x14];  // z1
			push dword ptr [esp + 0x14];  // x1
			push edi;  // networkTool
			call isFarAbutmentStep;  // (cdecl)
			add esp, 0x14;
			test al, al;
			jz vanillaCheck;
			ret;  // al = true: skip the clearance check for the far abutment

vanillaCheck:
			push IsStepGoodForBridge;  // tail-call with the original arguments
			ret;
		}
	}

	// Patch 3: Parallel bridges (experimental)
	//
	// IsStepGoodForBridge queries the occupants in the rectangle
	//   [min(x1,x2) - 1, max(x1,x2) + 1] × [min(z1,z2) - 1, max(z1,z2) + 1]
	// via cISC4OccupantManager::GetOccupantsByStandardCityCells (vtable slot 0xa0), filtered by a
	// cSC4NetworkORFilter for the surface-transport networks (0x1f4f). We shrink this rectangle back
	// to the step cells across the drag direction, but keep the margin along the drag direction.
	constexpr uint32_t IsStepGoodForBridge_GetOccupantsCall = 0x62fe87;

	// tStepBound::sbPenDirIsX, written by cSC4NetworkTool::ComputeDraggedCells (0x639790) for every
	// drag. True if the cross-section of the dragged steps extends along x, i.e. the drag runs along z.
	const bool* const sbPenDirIsX = reinterpret_cast<const bool*>(0xb464f0);

	void __cdecl removeLateralClearance(SC4Point<int32_t>* northWestCell, SC4Point<int32_t>* southEastCell)
	{
		if (*sbPenDirIsX) {
			northWestCell->x += 1;
			southEastCell->x -= 1;
		} else {
			northWestCell->y += 1;
			southEastCell->y -= 1;
		}
	}

	void NAKED_FUN Hook_IsStepGoodForBridge_GetOccupants(void)
	{
		// ecx: cISC4OccupantManager*
		// [esp+0x4] occupant list, [esp+0x8] northWestCell, [esp+0xc] southEastCell, [esp+0x10] filter, [esp+0x14] flags
		__asm {
			push ecx;  // store
			push dword ptr [esp + 0x10];  // southEastCell
			push dword ptr [esp + 0x10];  // northWestCell
			call removeLateralClearance;  // (cdecl)
			add esp, 0x8;
			pop ecx;  // restore
			mov eax, dword ptr [ecx];
			jmp dword ptr [eax + 0xa0];  // GetOccupantsByStandardCityCells, returns to the original call site
		}
	}
}

void BridgePatches::InstallLastTileAbutmentPatch()
{
	Patching::ExpectBytes(SelectBridgeStartAndEnd_ValidStepsBound, {0x8b, 0x57, 0x78, 0x4a, 0x3b, 0xc2});
	Patching::PatchByte(SelectBridgeStartAndEnd_DecValidSteps, 0x4a, 0x90);  // dec edx -> nop
}

void BridgePatches::InstallFarAbutmentClearancePatch()
{
	Patching::ExpectBytes(SelectBridgeStartAndEnd_IsStepGoodForBridgeCall, {0xe8, 0xa7, 0xf7, 0xff, 0xff});  // call IsStepGoodForBridge
	Patching::InstallCallHook(SelectBridgeStartAndEnd_IsStepGoodForBridgeCall, Hook_SelectBridgeStartAndEnd_IsStepGoodForBridge);
}

void BridgePatches::InstallParallelBridgesPatch()
{
	Patching::ExpectBytes(IsStepGoodForBridge_GetOccupantsCall, {0xff, 0x92, 0xa0, 0x00, 0x00, 0x00});  // call dword ptr [edx + 0xa0]
	Patching::InstallCallHook(IsStepGoodForBridge_GetOccupantsCall, Hook_IsStepGoodForBridge_GetOccupants);
	Patching::OverwriteMemory((void*)(IsStepGoodForBridge_GetOccupantsCall + 5), (uint8_t)0x90);  // pad the 6-byte instruction
}
