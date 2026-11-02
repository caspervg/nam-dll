#include "TerrainPreview.h"
#include "NetworkStubs.h"
#include "Patching.h"
#include "cISC4App.h"
#include "cISC4City.h"
#include "cISTETerrain.h"
#include "cISTETerrainView.h"
#include "cGZPersistResourceKey.h"
#include "GZServPtrs.h"
#include "wil/result_macros.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>
#include <vector>

namespace
{
	// vertex of the mesh drawn by cSTETerrainView3D, which is copied from the terrain whenever the terrain changes
	class cSTETerrainViewPoint
	{
		public:
			float x;
			float y;
			float z;
			uint32_t color;  // BGRA, the alpha is always 0xff
			float textureCoords[2];  // set by AssignTerrainAndWaterTextureCoordinates
			uint8_t RESERVED[0x8];  // zeroed on init, probably a second texture coordinate
	};
	static_assert(offsetof(cSTETerrainViewPoint, color) == 0xc);
	static_assert(sizeof(cSTETerrainViewPoint) == 0x20);

	cSTETerrainViewPoint** const spPoints = reinterpret_cast<cSTETerrainViewPoint**>(0xb4c758);
	const uint32_t* const spVertexCountX = reinterpret_cast<uint32_t*>(0xb4c74c);

	// The view only redraws regions that changed, so the edited vertices have to be redrawn explicitly.
	// RedisplayTerrain (0x749850) would also copy the actual terrain into the view again, so only its redraw message is posted.
	constexpr uint32_t redrawTerrainRegionMessage = 0x870bc918;
	typedef void (__cdecl* pfn_PostTerrainMessage)(uint32_t messageType, uint32_t data1, uint32_t data2, uint32_t data3, uint32_t data4);
	pfn_PostTerrainMessage PostTerrainMessage = reinterpret_cast<pfn_PostTerrainMessage>(0x743380);

	constexpr uint32_t InsertIsolatedHighwayIntersection_CallSite = 0x66166f;  // in cSC4ViewInputControlNetworkIntxTool::OnMouseMove
	constexpr uint32_t InsertIsolatedHighwayIntersection_Target = 0x62cd50;
	constexpr float cellSize = 16.0f;
	constexpr float minHeightChange = 0.01f;

	struct VertexChange
	{
		uint32_t index;  // into sPoints
		float actualHeight;
		float previewHeight;
	};

	std::vector<VertexChange> pending;
	bool solveAttempted = false;

	std::vector<VertexChange> applied;
	std::unordered_map<uint32_t, float> appliedHeightChanges;
	cSTETerrainViewPoint* appliedPoints = nullptr;
	uint32_t appliedVertexCountX = 0;
	bool cellsOutlined = false;

	// The changed cells are outlined with the tile outline texture of the local_tile_outline effect, which is white so it can be tinted.
	// Cells that are cut get the cut color, cells that are filled the fill color, and the opacity grows with the height change.
	const cGZPersistResourceKey outlineTexture(0x7ab50e44, 0x1abe787d, 0x144161da);

	// Textured selections share one combiner, of which only the RGB mode is set per selection. Its second RGB source is the
	// vertex color, so the color of a selection is never used. While the outlines are shown, the constant color is used instead,
	// which with the modulate mode gives the outline texture times the color of the selection.
	uint8_t* const texturedSelectionRGBSource1 = reinterpret_cast<uint8_t*>(0xb0d6f2);  // cGDCombiner at 0xb0d6f0
	constexpr uint8_t sourcePrimaryColor = 3;
	constexpr uint8_t sourceConstant = 2;
	constexpr int32_t combineModeModulate = 1;  // also halves the selection alpha
	bool combinerSourceReplaced = false;
	constexpr uint8_t cutColor[3] = {255, 140, 90};
	constexpr uint8_t fillColor[3] = {120, 200, 255};
	constexpr float fullOpacityHeightChange = 8.0f;
	constexpr float minOpacityFactor = 0.3f;
	float outlineOpacity;

	cISTETerrainView* getTerrainView()
	{
		cISC4AppPtr pApp;
		cISC4City* pCity = pApp ? pApp->GetCity() : nullptr;
		cISTETerrain* pTerrain = pCity ? pCity->GetTerrain() : nullptr;
		return pTerrain ? pTerrain->GetView() : nullptr;
	}

	// outlines the cells around the changed vertices, to show that this terrain is only a preview
	void outlineCells(const std::vector<VertexChange>& changes, uint32_t vertexCountX)
	{
		cISTETerrainView* pView = getTerrainView();
		if (pView == nullptr || vertexCountX < 2) {
			return;
		}
		std::set<std::pair<int, int>> cells;
		for (auto& change : changes) {
			int x = change.index % vertexCountX;
			int z = change.index / vertexCountX;
			for (int dz = -1; dz <= 0; dz++) {
				for (int dx = -1; dx <= 0; dx++) {
					if (x + dx >= 0 && x + dx < static_cast<int>(vertexCountX) - 1 && z + dz >= 0) {
						cells.insert({x + dx, z + dz});
					}
				}
			}
		}
		auto heightChange = [vertexCountX](int x, int z) {
			auto search = appliedHeightChanges.find(z * vertexCountX + x);
			return search != appliedHeightChanges.end() ? search->second : 0.0f;
		};
		for (auto& [x, z] : cells) {
			float change = (heightChange(x, z) + heightChange(x + 1, z) + heightChange(x, z + 1) + heightChange(x + 1, z + 1)) / 4;
			const uint8_t* color = change < 0 ? cutColor : fillColor;
			float opacity = outlineOpacity * (minOpacityFactor + (1 - minOpacityFactor) * (std::min)(std::abs(change) / fullOpacityHeightChange, 1.0f));
			uint32_t rgba = color[0] << 24 | color[1] << 16 | color[2] << 8 | static_cast<uint8_t>((std::min)(opacity * 2, 1.0f) * 255);
			SC4Point<int> cell = {x, z};
			pView->MarkSelected(cell, outlineTexture, 0, false, rgba, combineModeModulate, false);
		}
		cellsOutlined = !cells.empty();
		if (cellsOutlined && !combinerSourceReplaced && *texturedSelectionRGBSource1 == sourcePrimaryColor) {
			*texturedSelectionRGBSource1 = sourceConstant;
			combinerSourceReplaced = true;
		}
	}

	void redraw(const std::vector<VertexChange>& changes, uint32_t vertexCountX)
	{
		if (changes.empty() || vertexCountX == 0) {
			return;
		}
		uint32_t minX = UINT32_MAX, minZ = UINT32_MAX, maxX = 0, maxZ = 0;
		float minY = changes[0].actualHeight, maxY = changes[0].actualHeight;
		for (auto& change : changes) {
			uint32_t x = change.index % vertexCountX;
			uint32_t z = change.index / vertexCountX;
			minX = (std::min)(minX, x);
			maxX = (std::max)(maxX, x);
			minZ = (std::min)(minZ, z);
			maxZ = (std::max)(maxZ, z);
			minY = (std::min)({minY, change.actualHeight, change.previewHeight});
			maxY = (std::max)({maxY, change.actualHeight, change.previewHeight});
		}
		// one cell of margin, as the triangles around the edited vertices change as well
		minX = minX > 0 ? minX - 1 : 0;
		minZ = minZ > 0 ? minZ - 1 : 0;
		maxX = (std::min)(maxX + 1, vertexCountX - 1);
		maxZ += 1;
		auto heightToData = [](float y) { return static_cast<uint32_t>(std::clamp(y, 0.0f, 65535.0f)); };
		PostTerrainMessage(
			redrawTerrainRegionMessage,
			minX << 16 | maxX,
			minZ << 16 | maxZ,
			heightToData(std::floor(minY) - 1) << 16 | heightToData(std::ceil(maxY) + 1),
			0);
	}

	float getActualHeight(cSTETerrainViewPoint* points, uint32_t index)
	{
		for (auto& change : applied) {
			if (change.index == index) {
				return change.actualHeight;
			}
		}
		return points[index].y;
	}

	// The tool tries to place the piece without committing it, which still solves the terrain heights.
	// This is the same solution that UpdateTerrainHeights (0x644530) writes to the terrain when the piece is placed.
	void capture(cSC4NetworkTool* networkTool)
	{
		pending.clear();
		cSTETerrainViewPoint* points = *spPoints;
		uint32_t numVerticesX = networkTool->numVerticesX;
		if (points == nullptr || numVerticesX == 0 || numVerticesX != *spVertexCountX) {
			return;
		}

		auto satisfier = &networkTool->vertexHtConstraintSatisfier2;
		cSC4VertexHtConstraintSatisfier::RewindSolvedVertexList(satisfier);
		int32_t vertex;
		float height;
		while (cSC4VertexHtConstraintSatisfier::GetNextSolvedVertexAndItsHeight(satisfier, &vertex, &height)) {
			if (vertex < 0 || static_cast<uint32_t>(vertex) / numVerticesX >= networkTool->numVerticesZ) {
				continue;
			}
			uint32_t index = static_cast<uint32_t>(vertex);
			float actualHeight = getActualHeight(points, index);
			if (std::abs(height - actualHeight) > minHeightChange) {
				pending.push_back({index, actualHeight, height});
			}
		}
	}

	void apply(const std::vector<VertexChange>& changes)
	{
		cSTETerrainViewPoint* points = *spPoints;
		if (points == nullptr || changes.empty()) {
			return;
		}
		for (auto& change : changes) {
			points[change.index].y = change.previewHeight;
			appliedHeightChanges[change.index] = change.previewHeight - change.actualHeight;
		}
		applied = changes;
		appliedPoints = points;
		appliedVertexCountX = *spVertexCountX;
		outlineCells(applied, appliedVertexCountX);
		redraw(applied, appliedVertexCountX);
	}

	typedef bool (__thiscall* pfn_InsertIsolatedHighwayIntersection)(cSC4NetworkTool* pThis, int32_t x, int32_t z, uint32_t ruleId, uint32_t commit);
	pfn_InsertIsolatedHighwayIntersection InsertIsolatedHighwayIntersection = reinterpret_cast<pfn_InsertIsolatedHighwayIntersection>(InsertIsolatedHighwayIntersection_Target);

	bool __fastcall Hook_InsertIsolatedHighwayIntersection(cSC4NetworkTool* pThis, void*, int32_t x, int32_t z, uint32_t ruleId, uint32_t commit)
	{
		bool result = InsertIsolatedHighwayIntersection(pThis, x, z, ruleId, commit);
		solveAttempted = true;
		if (result) {
			capture(pThis);
		}
		return result;
	}
}

void TerrainPreview::BeginMouseMove()
{
	solveAttempted = false;
	pending.clear();
}

void TerrainPreview::EndMouseMove()
{
	if (solveAttempted) {
		Clear();
		apply(pending);
		pending.clear();
	}
}

void TerrainPreview::Clear()
{
	if (cellsOutlined) {
		if (cISTETerrainView* pView = getTerrainView()) {
			pView->ClearCurrentSelections();
		}
		cellsOutlined = false;
		if (combinerSourceReplaced) {
			*texturedSelectionRGBSource1 = sourcePrimaryColor;
			combinerSourceReplaced = false;
		}
	}
	// the terrain view copies the actual terrain again when it changes, so only restore what is still ours
	if (appliedPoints != nullptr && appliedPoints == *spPoints) {
		for (auto& change : applied) {
			if (appliedPoints[change.index].y == change.previewHeight) {
				appliedPoints[change.index].y = change.actualHeight;
			}
		}
		redraw(applied, appliedVertexCountX);
	}
	applied.clear();
	appliedHeightChanges.clear();
	appliedPoints = nullptr;
}

float TerrainPreview::GetHeightChange(float x, float z)
{
	if (applied.empty() || x < 0 || z < 0) {
		return 0.0f;
	}
	auto heightChange = [](uint32_t vx, uint32_t vz) {
		auto search = appliedHeightChanges.find(vz * appliedVertexCountX + vx);
		return search != appliedHeightChanges.end() ? search->second : 0.0f;
	};
	float fx = x / cellSize;
	float fz = z / cellSize;
	uint32_t vx = static_cast<uint32_t>(fx);
	uint32_t vz = static_cast<uint32_t>(fz);
	float tx = fx - vx;
	float tz = fz - vz;
	float north = heightChange(vx, vz) * (1 - tx) + heightChange(vx + 1, vz) * tx;
	float south = heightChange(vx, vz + 1) * (1 - tx) + heightChange(vx + 1, vz + 1) * tx;
	return north * (1 - tz) + south * tz;
}

void TerrainPreview::Install(float opacity)
{
	outlineOpacity = std::clamp(opacity, 0.0f, 1.0f);

	auto callSite = reinterpret_cast<uint8_t*>(InsertIsolatedHighwayIntersection_CallSite);
	uint32_t target = InsertIsolatedHighwayIntersection_CallSite + 5 + *reinterpret_cast<int32_t*>(callSite + 1);
	THROW_HR_IF(E_FAIL, callSite[0] != 0xe8 || target != InsertIsolatedHighwayIntersection_Target);
	Patching::InstallCallHook(InsertIsolatedHighwayIntersection_CallSite, reinterpret_cast<void (*)(void)>(&Hook_InsertIsolatedHighwayIntersection));
}
