#include "OverrideNetworkDrag.h"
#include "Rul2Engine.h"
#include "NetworkStubs.h"
#include "Patching.h"
#include "RotFlip.h"
#include "Logger.h"
#include <algorithm>
#include <vector>
#define NOMINMAX
#include <Windows.h>
#include "wil/result_macros.h"

namespace
{
	// Windows SimCity 4.exe 1.1.641. Every network tool vtable has ClearCellPreviews at +0x40 and PlaceAllSegments at
	// +0x4c. The UI sets the anchor cells of the drag (tool + 0x118) and calls PlaceAllSegments, for the preview on
	// every mouse move and once more to commit. PlaceAllSegments clears the cell previews before it draws.
	constexpr uint32_t kVtables[] = {0xaa8748, 0xaa99a8, 0xaa9f30, 0xaaa5c0, 0xaaa950};
	constexpr uint32_t kSlotClearCellPreviews = 0x40;
	constexpr uint32_t kSlotPlaceAllSegments = 0x4c;
	constexpr uint32_t kClearCellPreviews = 0x625580;
	constexpr uint32_t kPlaceAllSegments = 0x63bdc0;
	constexpr uint32_t kAnchorPointsOffset = 0x118;

	typedef void (__thiscall* pfn_ClearCellPreviews)(cSC4NetworkTool* pThis);
	typedef void (__thiscall* pfn_PlaceAllSegments)(cSC4NetworkTool* pThis, uint8_t commit, uint8_t straightOnly, uint8_t unknown, uint32_t networkType);
	const auto ClearCellPreviews = reinterpret_cast<pfn_ClearCellPreviews>(kClearCellPreviews);
	const auto PlaceAllSegments = reinterpret_cast<pfn_PlaceAllSegments>(kPlaceAllSegments);

	struct AnchorPoints  // the game's vector layout
	{
		SC4Point<uint32_t>* start;
		SC4Point<uint32_t>* end;
		SC4Point<uint32_t>* capacity;
	};

	typedef cIOverrideNetworkDrag::TileRow TileRow;

	std::vector<TileRow> sRows;  // the row that follows the cursor comes first
	bool sKeepPreviews = false;  // rows after the first of a drag: the previews of the earlier rows stay

	void __fastcall Hook_ClearCellPreviews(cSC4NetworkTool* pThis, void* edx)
	{
		if (!sKeepPreviews) {
			ClearCellPreviews(pThis);
		}
	}

	void placeRow(cSC4NetworkTool* pThis, const TileRow& row, uint8_t commit, uint8_t straightOnly, uint8_t unknown, uint32_t networkType)
	{
		const AnchorPoints* anchors = reinterpret_cast<AnchorPoints*>(reinterpret_cast<uint8_t*>(pThis) + kAnchorPointsOffset);
		const bool hasStart = anchors->start != nullptr && anchors->end > anchors->start;
		Rul2Engine::SetOverrideDragSeed(row.straightTileId, static_cast<RotFlip>(row.rotationFlip & 0x83),
				hasStart ? (anchors->start[0].y << 16) | anchors->start[0].x : 0xFFFFFFFF);
		PlaceAllSegments(pThis, commit, straightOnly, unknown, networkType);
		Rul2Engine::SetOverrideDragSeed(0, R0F0);
	}

	void __fastcall Hook_PlaceAllSegments(cSC4NetworkTool* pThis, void* edx, uint8_t commit, uint8_t straightOnly, uint8_t unknown, uint32_t networkType)
	{
		Rul2Engine::SetDragLog(commit != 0 && Logger::GetInstance().IsEnabled(LogLevel::Debug));
		if (sRows.empty()) {
			return PlaceAllSegments(pThis, commit, straightOnly, unknown, networkType);
		}
		AnchorPoints* anchors = reinterpret_cast<AnchorPoints*>(reinterpret_cast<uint8_t*>(pThis) + kAnchorPointsOffset);
		const bool isSegment = anchors->start != nullptr && anchors->end - anchors->start == 2;
		const SC4Point<uint32_t> start = isSegment ? anchors->start[0] : SC4Point<uint32_t>{};
		const SC4Point<uint32_t> end = isSegment ? anchors->start[1] : SC4Point<uint32_t>{};
		const int32_t dx = end.x - start.x;
		const int32_t dz = end.y - start.y;
		if (sRows.size() == 1 || !isSegment || (dx == 0) == (dz == 0)) {
			// TODO the other rows only follow a straight orthogonal drag. A bend or a diagonal needs a parallel
			// curve for every row.
			return placeRow(pThis, sRows.front(), commit, straightOnly, unknown, networkType);
		}

		// One step to the east in the frame where the network heads south, turned to the heading of the drag.
		const int32_t eastX = dz > 0 ? 1 : dz < 0 ? -1 : 0;
		const int32_t eastZ = dx > 0 ? -1 : dx < 0 ? 1 : 0;
		const int32_t maxX = static_cast<int32_t>(pThis->numCellsX) - 1;
		const int32_t maxZ = static_cast<int32_t>(pThis->numCellsZ) - 1;
		for (const TileRow& row : sRows) {
			const int32_t x = row.offset * eastX;
			const int32_t z = row.offset * eastZ;
			const int32_t x0 = start.x + x, z0 = start.y + z, x1 = end.x + x, z1 = end.y + z;
			if (std::min(x0, x1) < 0 || std::min(z0, z1) < 0 || std::max(x0, x1) > maxX || std::max(z0, z1) > maxZ) {
				continue;  // this row is off the map
			}
			anchors->start[0] = {static_cast<uint32_t>(x0), static_cast<uint32_t>(z0)};
			anchors->start[1] = {static_cast<uint32_t>(x1), static_cast<uint32_t>(z1)};
			placeRow(pThis, row, commit, straightOnly, unknown, networkType);
			sKeepPreviews = true;
		}
		sKeepPreviews = false;
		anchors->start[0] = start;
		anchors->start[1] = end;
	}
}

void OverrideNetworkDrag::Install()
{
	for (uint32_t vtable : kVtables) {
		// another DLL may have patched it already
		THROW_HR_IF(E_FAIL, *reinterpret_cast<uint32_t*>(vtable + kSlotPlaceAllSegments) != kPlaceAllSegments);
	}
	for (uint32_t vtable : kVtables) {
		Patching::OverwriteMemory(reinterpret_cast<void*>(vtable + kSlotPlaceAllSegments), reinterpret_cast<uint32_t>(&Hook_PlaceAllSegments));
		// The power line and underground tools have their own ClearCellPreviews.
		if (*reinterpret_cast<uint32_t*>(vtable + kSlotClearCellPreviews) == kClearCellPreviews) {
			Patching::OverwriteMemory(reinterpret_cast<void*>(vtable + kSlotClearCellPreviews), reinterpret_cast<uint32_t>(&Hook_ClearCellPreviews));
		}
	}
}

bool OverrideNetworkDrag::QueryInterface(uint32_t riid, void** ppvObj)
{
	if (riid == GZIID_cIOverrideNetworkDrag) {
		*ppvObj = static_cast<cIOverrideNetworkDrag*>(this);
	} else if (riid == GZIID_cIGZUnknown) {
		*ppvObj = static_cast<cIGZUnknown*>(this);
	} else {
		return false;
	}
	AddRef();
	return true;
}

uint32_t OverrideNetworkDrag::AddRef()
{
	return ++refCount;
}

uint32_t OverrideNetworkDrag::Release()
{
	// the director owns the only instance
	if (refCount > 0) {
		--refCount;
	}
	return refCount;
}

void OverrideNetworkDrag::SetOverrideNetwork(const TileRow* rows, uint32_t rowCount)
{
	sRows.assign(rows, rows + (rows != nullptr ? rowCount : 0));
	std::stable_partition(sRows.begin(), sRows.end(), [](const TileRow& row) { return row.offset == 0; });
}

void OverrideNetworkDrag::ClearOverrideNetwork()
{
	sRows.clear();
}
