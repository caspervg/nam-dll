#include "TerrainPreview.h"
#include "NetworkStubs.h"
#include "Patching.h"
#include "wil/result_macros.h"
#include <cmath>
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
			uint8_t RESERVED[0x14];
	};
	static_assert(sizeof(cSTETerrainViewPoint) == 0x20);

	cSTETerrainViewPoint** const spPoints = reinterpret_cast<cSTETerrainViewPoint**>(0xb4c758);
	const uint32_t* const spVertexCountX = reinterpret_cast<uint32_t*>(0xb4c74c);

	constexpr uint32_t InsertIsolatedHighwayIntersection_CallSite = 0x66166f;  // in cSC4ViewInputControlNetworkIntxTool::OnMouseMove
	constexpr uint32_t kInsertIsolatedHighwayIntersection = 0x62cd50;
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
	}

	typedef bool (__thiscall* pfn_InsertIsolatedHighwayIntersection)(cSC4NetworkTool* pThis, int32_t x, int32_t z, uint32_t ruleId, uint32_t commit);
	pfn_InsertIsolatedHighwayIntersection InsertIsolatedHighwayIntersection = reinterpret_cast<pfn_InsertIsolatedHighwayIntersection>(kInsertIsolatedHighwayIntersection);

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
	// the terrain view copies the actual terrain again when it changes, so only restore what is still ours
	if (appliedPoints != nullptr && appliedPoints == *spPoints) {
		for (auto& change : applied) {
			if (appliedPoints[change.index].y == change.previewHeight) {
				appliedPoints[change.index].y = change.actualHeight;
			}
		}
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

void TerrainPreview::Install()
{
	auto callSite = reinterpret_cast<uint8_t*>(InsertIsolatedHighwayIntersection_CallSite);
	uint32_t target = InsertIsolatedHighwayIntersection_CallSite + 5 + *reinterpret_cast<int32_t*>(callSite + 1);
	THROW_HR_IF(E_FAIL, callSite[0] != 0xe8 || target != kInsertIsolatedHighwayIntersection);
	Patching::InstallCallHook(InsertIsolatedHighwayIntersection_CallSite, reinterpret_cast<void (*)(void)>(&Hook_InsertIsolatedHighwayIntersection));
}
