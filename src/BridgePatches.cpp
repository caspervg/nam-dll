#include "BridgePatches.h"
#include "Logger.h"
#include "Patching.h"
#include "NetworkStubs.h"
#include "cISTETerrain.h"
#include <algorithm>
#include <string>

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

	bool sLastTileAbutmentInstalled = false;
	bool sFarAbutmentClearanceInstalled = false;
	bool sParallelBridgesInstalled = false;

	// Patch 1: Last-tile abutment
	//
	// SelectBridgeStartAndEnd requires the far abutment to lie before the last dragged step:
	//   mov edx, dword ptr [edi + 0x78]  ; number of valid dragged steps
	//   dec edx
	//   cmp eax, edx                     ; current step < validSteps - 1
	// so the drag has to extend one tile beyond the far abutment, otherwise the whole drag fails.
	// The near abutment has no such requirement as it may be a partially submerged shore tile.
	// Removing the `dec`s matches the bound that PadStepsToMakeMinBridgeSpan (0x62ecd0) already uses
	// when it extends a bridge forwards, so bridges ending on the last dragged step already occur.
	//
	// MSVC rotated the loop, so the bound appears twice: before the loop, for the first step after
	// the water starts, and at the bottom of the loop, for every following step.
	constexpr uint32_t SelectBridgeStartAndEnd_ValidStepsBound = 0x6305af;
	constexpr uint32_t SelectBridgeStartAndEnd_DecValidSteps = 0x6305b2;
	constexpr uint32_t SelectBridgeStartAndEnd_LoopValidStepsBound = 0x6306ad;
	constexpr uint32_t SelectBridgeStartAndEnd_LoopDecValidSteps = 0x6306b2;

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

	// Debug log
	//
	// Wraps IdentifyBridgeCells (called by PlaceNetwork) and SelectBridgeStartAndEnd (called by
	// IdentifyBridgeCells) to log the dragged steps and the bridges placed on them. When a bridge is
	// rejected, the checks of SelectBridgeStartAndEnd are repeated to find the first failing one.
	// Drag previews run this for every mouse move, so a report is only written when it changes.
	constexpr uint32_t PlaceNetwork_IdentifyBridgeCellsCall = 0x63b9e5;
	constexpr uint32_t IdentifyBridgeCells_SelectBridgeStartAndEndCall = 0x63298b;
	constexpr uint32_t kNetworkToolBridgeFilterOffset = 0x364;  // cSC4NetworkORFilter* passed to IsStepGoodForBridge
	constexpr uint32_t kNetworkToolNetworkTypeInfoOffset = 0x358;  // cSC4NetworkTypeInfo* of the dragged network
	constexpr uint32_t kNetworkToolWaterBridgeTypesOffset = 0x33c;  // vector<long> of bridge types that work over water
	constexpr uint32_t kNetworkToolPreferredBridgeOffset = 0x200;  // bridge type chosen in the UI, or -1
	constexpr uint32_t kNetworkTypeInfoBridgeTypesOffset = 0x30;  // vector<cSC4NetworkBridgeTypeInfo>
	constexpr uint32_t kBridgeTypeInfoSize = 0x178;
	constexpr uint32_t kBridgeTypeInfoMinSpanOffset = 0x4;
	constexpr uint32_t kBridgeTypeInfoMaxSpanOffset = 0x8;
	constexpr uint32_t kBridgeTypeInfoMinAltitudeOffset = 0xc8;  // minimum estimated elevation above water
	constexpr uint32_t kBridgeTypeInfoMaxAltitudeOffset = 0xcc;

	template <typename T>
	T readField(const void* object, uint32_t offset)
	{
		return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(object) + offset);
	}

	// Lists the bridge types FindBestBridgeType chooses from for a bridge over water.
	std::string describeWaterBridgeTypes(const cSC4NetworkTool* networkTool)
	{
		std::string description;
		char entry[120];
		const int32_t preferred = readField<int32_t>(networkTool, kNetworkToolPreferredBridgeOffset);
		snprintf(entry, sizeof(entry), "preferred type %d;", preferred);
		description += entry;
		const auto* const typeInfo = readField<const uint8_t*>(networkTool, kNetworkToolNetworkTypeInfoOffset);
		const auto* const bridgeTypes = readField<const uint8_t*>(typeInfo, kNetworkTypeInfoBridgeTypesOffset);
		const auto* const candidates = readField<const int32_t*>(networkTool, kNetworkToolWaterBridgeTypesOffset);
		const auto* const candidatesEnd = readField<const int32_t*>(networkTool, kNetworkToolWaterBridgeTypesOffset + 4);
		for (const int32_t* candidate = candidates; candidate != candidatesEnd; candidate++) {
			const uint8_t* const bridgeType = bridgeTypes + *candidate * kBridgeTypeInfoSize;
			snprintf(entry, sizeof(entry), " type %d: span %u-%u, altitude %.0f-%.0f;", *candidate,
					readField<uint32_t>(bridgeType, kBridgeTypeInfoMinSpanOffset),
					readField<uint32_t>(bridgeType, kBridgeTypeInfoMaxSpanOffset),
					readField<float>(bridgeType, kBridgeTypeInfoMinAltitudeOffset),
					readField<float>(bridgeType, kBridgeTypeInfoMaxAltitudeOffset));
			description += entry;
		}
		return description;
	}

	typedef void (__thiscall* pfn_IdentifyBridgeCells)(cSC4NetworkTool* pThis, int32_t* stepCount, int32_t networkType);
	typedef bool (__thiscall* pfn_SelectBridgeStartAndEnd)(cSC4NetworkTool* pThis, int32_t firstWaterStep, int32_t* start, int32_t* end, int32_t* bridgeType, float* deckHeight, int32_t networkType);
	typedef bool (__thiscall* pfn_CheckBridgeStepEdgeTypes)(cSC4NetworkTool* pThis, int32_t step, int32_t networkType);
	const auto IdentifyBridgeCells = reinterpret_cast<pfn_IdentifyBridgeCells>(0x632830);
	const auto SelectBridgeStartAndEnd = reinterpret_cast<pfn_SelectBridgeStartAndEnd>(0x630570);
	const auto CheckBridgeStepEdgeTypes = reinterpret_cast<pfn_CheckBridgeStepEdgeTypes>(0x62eb80);

	// Calls IsStepGoodForBridge, which expects the occupant manager in esi. (cdecl)
	bool NAKED_FUN callIsStepGoodForBridge(intptr_t occupantManager, intptr_t filter, int32_t x1, int32_t z1, int32_t x2, int32_t z2)
	{
		__asm {
			push esi;  // store
			mov esi, dword ptr [esp + 0x8];  // occupantManager
			push dword ptr [esp + 0x1c];  // z2
			push dword ptr [esp + 0x1c];  // x2
			push dword ptr [esp + 0x1c];  // z1
			push dword ptr [esp + 0x1c];  // x1
			push dword ptr [esp + 0x1c];  // filter
			mov eax, IsStepGoodForBridge;
			call eax;
			add esp, 0x14;
			pop esi;  // restore
			ret;
		}
	}

	bool isWaterStep(const cSC4NetworkTool* networkTool, int32_t step)
	{
		const cSC4NetworkTool::tDraggedStep draggedStep = networkTool->draggedSteps[step];
		for (uint32_t i = draggedStep.firstCellIndex; i <= draggedStep.lastCellIndex; i++) {
			const SC4Point<uint32_t> cell = networkTool->draggedCells[i];
			if (networkTool->terrain->CellHasWater(cell.x, cell.y)) {
				return true;
			}
		}
		return false;
	}

	bool isStepGoodForBridge(const cSC4NetworkTool* networkTool, int32_t step)
	{
		const cSC4NetworkTool::tDraggedStep draggedStep = networkTool->draggedSteps[step];
		const SC4Point<uint32_t> first = networkTool->draggedCells[draggedStep.firstCellIndex];
		const SC4Point<uint32_t> last = networkTool->draggedCells[draggedStep.lastCellIndex];
		const intptr_t filter = *reinterpret_cast<const intptr_t*>(reinterpret_cast<const uint8_t*>(networkTool) + kNetworkToolBridgeFilterOffset);
		return callIsStepGoodForBridge(networkTool->occupantManager, filter, first.x, first.y, last.x, last.y);
	}

	uint32_t stepType(const cSC4NetworkTool* networkTool, int32_t step)
	{
		return networkTool->draggedSteps[step].stepType;
	}

	// Repeats the checks of SelectBridgeStartAndEnd, including the patches installed here.
	std::string explainRejectedBridge(cSC4NetworkTool* networkTool, int32_t firstWaterStep, int32_t networkType)
	{
		char reason[200];
		const int32_t start = firstWaterStep - 1;
		if (stepType(networkTool, start) != 0) {
			snprintf(reason, sizeof(reason), "near abutment (step %d) is already a bridge or tunnel", start);
			return reason;
		}
		if (!CheckBridgeStepEdgeTypes(networkTool, start, networkType)) {
			snprintf(reason, sizeof(reason), "the existing network on the near abutment (step %d) does not allow a bridge", start);
			return reason;
		}
		const int32_t endBound = networkTool->validStepCount - (sLastTileAbutmentInstalled ? 0 : 1);
		for (int32_t end = firstWaterStep + 1; ; end++) {
			if (end >= endBound) {
				snprintf(reason, sizeof(reason), "no dry far abutment within the drag (searched up to step %d of %d valid steps)",
						endBound - 1, networkTool->validStepCount);
				return reason;
			}
			if (stepType(networkTool, end) != 0) {
				snprintf(reason, sizeof(reason), "step %d is already a bridge or tunnel", end);
				return reason;
			}
			const bool dry = !isWaterStep(networkTool, end);
			if (!(dry && sFarAbutmentClearanceInstalled) && !isStepGoodForBridge(networkTool, end)) {
				snprintf(reason, sizeof(reason), "an existing network lies within one tile of step %d%s", end, dry ? " (far abutment)" : "");
				return reason;
			}
			if (!CheckBridgeStepEdgeTypes(networkTool, end, networkType)) {
				snprintf(reason, sizeof(reason), "the existing network on step %d does not allow a bridge", end);
				return reason;
			}
			if (dry) {
				if (end >= networkTool->straightStepCount) {
					snprintf(reason, sizeof(reason), "the far abutment (step %d) lies beyond the bend of the drag (%d straight steps)",
							end, networkTool->straightStepCount);
				} else {
					snprintf(reason, sizeof(reason), "no bridge type fits steps %d to %d (span %d, before padding to the minimum span); ",
							start, end, end - start + 1);
					return reason + describeWaterBridgeTypes(networkTool);
				}
				return reason;
			}
		}
	}

	std::string sReport;
	std::string sLastReport;
	bool sSelectCalled = false;

	void appendLine(std::string& report, const char* const line)
	{
		report += line;
		report += '\n';
	}

	bool __fastcall Hook_SelectBridgeStartAndEnd(cSC4NetworkTool* pThis, void* edx,
			int32_t firstWaterStep, int32_t* start, int32_t* end, int32_t* bridgeType, float* deckHeight, int32_t networkType)
	{
		const bool result = SelectBridgeStartAndEnd(pThis, firstWaterStep, start, end, bridgeType, deckHeight, networkType);
		sSelectCalled = true;
		char line[2048];
		if (result && *bridgeType >= 0) {
			snprintf(line, sizeof(line), "  water from step %d: bridge from step %d to %d, bridge type %d, deck at %.1f m",
					firstWaterStep, *start, *end, *bridgeType, *deckHeight);
		} else {
			snprintf(line, sizeof(line), "  water from step %d: rejected, %s",
					firstWaterStep, explainRejectedBridge(pThis, firstWaterStep, networkType).c_str());
		}
		appendLine(sReport, line);
		return result;
	}

	// Why IdentifyBridgeCells did not try to place a bridge at all.
	void explainSkippedBridges(cSC4NetworkTool* pThis, int32_t checkedSteps, int32_t networkType)
	{
		char line[300];
		if (checkedSteps < 4 || checkedSteps > pThis->straightStepCount) {
			snprintf(line, sizeof(line), "  no bridges tried: %d steps were checked, but bridges need 4 to %d (or the network has no bridges)",
					checkedSteps, pThis->straightStepCount);
			appendLine(sReport, line);
			return;
		}
		for (int32_t i = 0; i < checkedSteps; i++) {
			if (!isWaterStep(pThis, i)) {
				continue;
			}
			if (i < 2) {
				snprintf(line, sizeof(line), "  water at step %d, but bridges are only searched from step 2", i);
				appendLine(sReport, line);
				continue;
			}
			std::string reasons;
			if (stepType(pThis, i) != 0 || stepType(pThis, i - 1) != 0 || stepType(pThis, i - 2) != 0) {
				reasons += " already a bridge or tunnel nearby;";
			}
			if (!isStepGoodForBridge(pThis, i)) {
				reasons += " existing network within one tile;";
			}
			if (!CheckBridgeStepEdgeTypes(pThis, i, networkType)) {
				reasons += " existing network does not allow a bridge;";
			}
			snprintf(line, sizeof(line), "  water at step %d skipped:%s", i, reasons.c_str());
			appendLine(sReport, line);
			return;  // the first water step tells the story
		}
	}

	void __fastcall Hook_IdentifyBridgeCells(cSC4NetworkTool* pThis, void* edx, int32_t* stepCount, int32_t networkType)
	{
		const int32_t checkedSteps = *stepCount;
		sReport.clear();
		sSelectCalled = false;
		IdentifyBridgeCells(pThis, stepCount, networkType);

		if (pThis->terrain == nullptr) {
			return;
		}
		const int32_t totalSteps = static_cast<int32_t>(pThis->draggedSteps.size());
		std::string water;
		std::string bridges;
		bool hasWater = false;
		for (int32_t i = 0; i < totalSteps; i++) {
			const bool wet = isWaterStep(pThis, i);
			hasWater |= wet;
			water += wet ? '~' : '.';
			const uint32_t type = stepType(pThis, i);
			bridges += type == 1 ? 'B' : type == 2 ? 'T' : '.';
		}
		if (!hasWater) {
			return;  // nothing to do with bridges
		}
		if (!sSelectCalled) {
			explainSkippedBridges(pThis, checkedSteps, networkType);
		}

		char line[300];
		std::string report;
		snprintf(line, sizeof(line), "Bridge drag, network %d: %d steps, %d straight, %d valid, %d checked%s",
				networkType, totalSteps, pThis->straightStepCount, pThis->validStepCount, checkedSteps,
				*stepCount < checkedSteps ? " -> drag rejected" : "");
		appendLine(report, line);
		appendLine(report, ("  water:   " + water).c_str());
		appendLine(report, ("  bridges: " + bridges).c_str());
		report += sReport;

		if (report != sLastReport) {
			Logger& logger = Logger::GetInstance();
			size_t begin = 0;
			for (size_t newline = report.find('\n'); newline != std::string::npos; newline = report.find('\n', begin)) {
				logger.WriteLine(LogLevel::Info, report.substr(begin, newline - begin).c_str());
				begin = newline + 1;
			}
			sLastReport = std::move(report);
		}
	}
}

void BridgePatches::InstallLastTileAbutmentPatch()
{
	Patching::ExpectBytes(SelectBridgeStartAndEnd_ValidStepsBound, {0x8b, 0x57, 0x78, 0x4a, 0x3b, 0xc2});  // mov edx, [edi+0x78]; dec edx; cmp eax, edx
	Patching::ExpectBytes(SelectBridgeStartAndEnd_LoopValidStepsBound, {0x8b, 0x4f, 0x78, 0x8b, 0xc5, 0x49, 0x3b, 0xc1});  // mov ecx, [edi+0x78]; mov eax, ebp; dec ecx; cmp eax, ecx
	Patching::PatchByte(SelectBridgeStartAndEnd_DecValidSteps, 0x4a, 0x90);  // dec edx -> nop
	Patching::PatchByte(SelectBridgeStartAndEnd_LoopDecValidSteps, 0x49, 0x90);  // dec ecx -> nop
	sLastTileAbutmentInstalled = true;
}

void BridgePatches::InstallFarAbutmentClearancePatch()
{
	Patching::ExpectBytes(SelectBridgeStartAndEnd_IsStepGoodForBridgeCall, {0xe8, 0xa7, 0xf7, 0xff, 0xff});  // call IsStepGoodForBridge
	Patching::InstallCallHook(SelectBridgeStartAndEnd_IsStepGoodForBridgeCall, Hook_SelectBridgeStartAndEnd_IsStepGoodForBridge);
	sFarAbutmentClearanceInstalled = true;
}

void BridgePatches::InstallParallelBridgesPatch()
{
	Patching::ExpectBytes(IsStepGoodForBridge_GetOccupantsCall, {0xff, 0x92, 0xa0, 0x00, 0x00, 0x00});  // call dword ptr [edx + 0xa0]
	Patching::InstallCallHook(IsStepGoodForBridge_GetOccupantsCall, Hook_IsStepGoodForBridge_GetOccupants);
	Patching::OverwriteMemory((void*)(IsStepGoodForBridge_GetOccupantsCall + 5), (uint8_t)0x90);  // pad the 6-byte instruction
	sParallelBridgesInstalled = true;
}

void BridgePatches::InstallDebugLog()
{
	Patching::ExpectBytes(PlaceNetwork_IdentifyBridgeCellsCall, {0xe8, 0x46, 0x6e, 0xff, 0xff});  // call IdentifyBridgeCells
	Patching::ExpectBytes(IdentifyBridgeCells_SelectBridgeStartAndEndCall, {0xe8, 0xe0, 0xdb, 0xff, 0xff});  // call SelectBridgeStartAndEnd
	Patching::InstallCallHook(PlaceNetwork_IdentifyBridgeCellsCall, reinterpret_cast<void(*)(void)>(Hook_IdentifyBridgeCells));
	Patching::InstallCallHook(IdentifyBridgeCells_SelectBridgeStartAndEndCall, reinterpret_cast<void(*)(void)>(Hook_SelectBridgeStartAndEnd));
}
