#include "TerrainPreview.h"
#include "EffectStubs.h"
#include "Logger.h"
#include "NetworkStubs.h"
#include "Patching.h"
#include "Settings.h"
#include "cISC4App.h"
#include "cISC4City.h"
#include "cISTETerrain.h"
#include "cISTETerrainView.h"
#include "cGZPersistResourceKey.h"
#include "GZServPtrs.h"
#include "wil/result_macros.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
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

	// RedisplayTerrain (0x749850) would also copy the actual terrain into the view again, so only its redraw message is posted
	constexpr uint32_t redrawTerrainRegionMessage = 0x870bc918;
	typedef void (__cdecl* pfn_PostTerrainMessage)(uint32_t messageType, uint32_t data1, uint32_t data2, uint32_t data3, uint32_t data4);
	pfn_PostTerrainMessage PostTerrainMessage = reinterpret_cast<pfn_PostTerrainMessage>(0x743380);

	constexpr uint32_t InsertIsolatedHighwayIntersection_CallSite = 0x66166f;  // in cSC4ViewInputControlNetworkIntxTool::OnMouseMove
	constexpr uint32_t InsertIsolatedHighwayIntersection_Target = 0x62cd50;
	constexpr float cellSize = 16.0f;
	constexpr float minHeightChange = 0.01f;

	struct VertexChange
	{
		float actualHeight;
		float previewHeight;
	};

	// changed vertices by their index into the terrain view points
	std::unordered_map<uint32_t, VertexChange> pending;
	uint32_t pendingVertexCountZ = 0;
	bool solveAttempted = false;

	std::unordered_map<uint32_t, VertexChange> applied;
	cSTETerrainViewPoint* appliedPoints = nullptr;
	uint32_t appliedVertexCountX = 0;
	uint32_t appliedVertexCountZ = 0;
	bool cellsOutlined = false;

	float vertexHeightChange(uint32_t x, uint32_t z)
	{
		if (x >= appliedVertexCountX || z >= appliedVertexCountZ) {
			return 0.0f;
		}
		auto search = applied.find(z * appliedVertexCountX + x);
		return search != applied.end() ? search->second.previewHeight - search->second.actualHeight : 0.0f;
	}

	// The cut and filled regions each get one outline in their color, which is more opaque where the terrain changes more.
	// The outline textures (tools/grading_outlines.py) are white and draw the sides of a cell given by their instance ID.
	constexpr uint32_t outlineTextureType = 0x7ab50e44;
	constexpr uint32_t outlineTextureGroup = 0x4e414d47;
	enum OutlineSide : uint32_t {
		outlineXMin = 1 << 0,
		outlineXMax = 1 << 1,
		outlineZMin = 1 << 2,
		outlineZMax = 1 << 3,
		outlineCornerXMinZMin = 1 << 4,  // connects the outline at concave corners
		outlineCornerXMinZMax = 1 << 5,
		outlineCornerXMaxZMin = 1 << 6,
		outlineCornerXMaxZMax = 1 << 7,
	};

	// Textured selections share one combiner, whose second RGB source is the vertex color instead of the selection color.
	// While the outlines are shown, it uses the constant color, so the outline texture is modulated with the selection color.
	uint8_t* const texturedSelectionRGBSource1 = reinterpret_cast<uint8_t*>(0xb0d6f2);  // cGDCombiner at 0xb0d6f0
	constexpr uint8_t sourcePrimaryColor = 3;
	constexpr uint8_t sourceConstant = 2;
	constexpr int32_t combineModeModulate = 1;  // also halves the selection alpha
	bool combinerSourceReplaced = false;
	uint8_t cutColor[4];
	uint8_t fillColor[4];
	constexpr float fullOpacityHeightChange = 16.0f;
	constexpr float minOpacityFactor = 0.15f;
	float groundOutlineOpacity = 1.0f;  // fainter next to the wireframe

	// logarithmic, so that each doubling of the change is an equal step, and doubled as the outline combine mode halves it
	float opacityFor(float heightChange)
	{
		const uint8_t* color = heightChange < 0 ? cutColor : fillColor;
		float strength = (std::min)(std::log1p(std::abs(heightChange)) / std::log1p(fullOpacityHeightChange), 1.0f);
		float opacity = color[3] / 255.0f * (minOpacityFactor + (1 - minOpacityFactor) * strength);
		return (std::min)(opacity * 2, 1.0f);
	}

	cISTETerrainView* getTerrainView()
	{
		cISC4AppPtr pApp;
		cISC4City* pCity = pApp ? pApp->GetCity() : nullptr;
		cISTETerrain* pTerrain = pCity ? pCity->GetTerrain() : nullptr;
		return pTerrain ? pTerrain->GetView() : nullptr;
	}

	// outlines the cells around the changed vertices, to show that this terrain is only a preview
	void outlineCells()
	{
		cISTETerrainView* pView = getTerrainView();
		if (pView == nullptr || appliedVertexCountX < 2) {
			return;
		}
		int cellCountX = static_cast<int>(appliedVertexCountX) - 1;
		int cellCountZ = static_cast<int>(appliedVertexCountZ) - 1;
		std::set<std::pair<int, int>> cells;
		for (auto& entry : applied) {
			int x = entry.first % appliedVertexCountX;
			int z = entry.first / appliedVertexCountX;
			for (int dz = -1; dz <= 0; dz++) {
				for (int dx = -1; dx <= 0; dx++) {
					if (x + dx >= 0 && x + dx < cellCountX && z + dz >= 0 && z + dz < cellCountZ) {
						cells.insert({x + dx, z + dz});
					}
				}
			}
		}
		// the corners of a cell can be raised and lowered at the same time, so the largest change decides
		std::map<std::pair<int, int>, float> cellChanges;
		for (auto& [x, z] : cells) {
			float change = 0.0f;
			for (float cornerChange : {vertexHeightChange(x, z), vertexHeightChange(x + 1, z), vertexHeightChange(x, z + 1), vertexHeightChange(x + 1, z + 1)}) {
				if (std::abs(cornerChange) > std::abs(change)) {
					change = cornerChange;
				}
			}
			cellChanges[{x, z}] = change;
		}
		auto isCut = [](float change) { return change < 0; };
		auto inRegion = [&cellChanges, &isCut](int x, int z, bool cut) {
			auto search = cellChanges.find({x, z});
			return search != cellChanges.end() && isCut(search->second) == cut;
		};

		for (auto& [cell, change] : cellChanges) {
			auto [x, z] = cell;
			bool cut = isCut(change);
			bool xMin = !inRegion(x - 1, z, cut), xMax = !inRegion(x + 1, z, cut);
			bool zMin = !inRegion(x, z - 1, cut), zMax = !inRegion(x, z + 1, cut);
			uint32_t sides = (xMin ? outlineXMin : 0) | (xMax ? outlineXMax : 0) | (zMin ? outlineZMin : 0) | (zMax ? outlineZMax : 0);
			if (!xMin && !zMin && !inRegion(x - 1, z - 1, cut)) {
				sides |= outlineCornerXMinZMin;
			}
			if (!xMin && !zMax && !inRegion(x - 1, z + 1, cut)) {
				sides |= outlineCornerXMinZMax;
			}
			if (!xMax && !zMin && !inRegion(x + 1, z - 1, cut)) {
				sides |= outlineCornerXMaxZMin;
			}
			if (!xMax && !zMax && !inRegion(x + 1, z + 1, cut)) {
				sides |= outlineCornerXMaxZMax;
			}
			if (sides == 0) {
				continue;  // inside the region
			}

			const uint8_t* color = cut ? cutColor : fillColor;
			uint32_t rgba = color[0] << 24 | color[1] << 16 | color[2] << 8 | static_cast<uint8_t>(groundOutlineOpacity * opacityFor(change) * 255);
			SC4Point<int> point = {x, z};
			cGZPersistResourceKey texture(outlineTextureType, outlineTextureGroup, sides);
			pView->MarkSelected(point, texture, 0, false, rgba, combineModeModulate, false);
			cellsOutlined = true;
		}
		if (cellsOutlined && !combinerSourceReplaced && *texturedSelectionRGBSource1 == sourcePrimaryColor) {
			*texturedSelectionRGBSource1 = sourceConstant;
			combinerSourceReplaced = true;
		}
	}

	// the view only redraws regions that changed, so the edited vertices are redrawn explicitly
	void redraw()
	{
		if (applied.empty()) {
			return;
		}
		uint32_t minX = UINT32_MAX, minZ = UINT32_MAX, maxX = 0, maxZ = 0;
		float minY = applied.begin()->second.actualHeight, maxY = minY;
		for (auto& [index, change] : applied) {
			uint32_t x = index % appliedVertexCountX;
			uint32_t z = index / appliedVertexCountX;
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
		maxX = (std::min)(maxX + 1, appliedVertexCountX - 1);
		maxZ = (std::min)(maxZ + 1, appliedVertexCountZ - 1);
		auto heightToData = [](float y) { return static_cast<uint32_t>(std::clamp(y, 0.0f, 65535.0f)); };
		PostTerrainMessage(
			redrawTerrainRegionMessage,
			minX << 16 | maxX,
			minZ << 16 | maxZ,
			heightToData(std::floor(minY) - 1) << 16 | heightToData(std::ceil(maxY) + 1),
			0);
	}

	// The wireframe draws the graded terrain as lines along the cell edges, with dotted lines down to the actual terrain.
	// Each line or dash is an effect with one model particle (tools/grading_outlines.py), with a fainter copy where it is hidden.
	bool wireframeEnabled = false;
	constexpr const char* lineEffectName = "nam_grading_line";  // 16 m along x
	constexpr const char* dashEffectName = "nam_grading_dash";  // 0.8 m up along y
	constexpr float dashLength = 0.8f;
	constexpr float dashSpacing = 1.6f;
	constexpr float minDottedLineHeight = 1.0f;
	constexpr float lineOpacity = 0.6f;  // keeps the lines about as faint as the piece preview
	constexpr float dottedLineOpacity = 0.4f;
	constexpr float hiddenOpacity = 0.5f;  // relative to the visible part
	constexpr float lineLift = 0.1f;  // keeps lines over unchanged terrain from flickering into it

	struct WireframeItem
	{
		cS3DTransform transform;
		float heightChange;  // decides the color and opacity
	};

	struct WireframeEffect
	{
		cISC4VisualEffect* visible = nullptr;
		cISC4VisualEffect* hidden = nullptr;
		bool started = false;
		std::vector<cSC4ModelBasedParticlesEffect*> models;  // of both copies, once the particles exist
		std::array<float, 3> color;
	};

	std::vector<WireframeEffect> lineEffects;
	std::vector<WireframeEffect> dashEffects;
	bool effectsMissing = false;

	// the color parameter of an effect does not reach model particles, so the particles are colored while they are drawn
	std::unordered_map<cSC4ModelBasedParticlesEffect*, std::array<float, 3>> modelColors;
	bool modelsPending = false;

	// finds the particles of a started effect, which may only exist after a frame
	bool findModels(WireframeEffect& effect)
	{
		if (effect.models.empty()) {
			auto visibleModels = GetModelParticles(effect.visible);
			auto hiddenModels = GetModelParticles(effect.hidden);
			if (visibleModels.empty() || hiddenModels.empty()) {
				return false;
			}
			for (auto particles : hiddenModels) {
				SC4DrawAttributes attributes = particles->modelInstance->GetDrawAttributes();
				attributes.flags |= SC4DrawAttributes::kDepthGreaterNoWrite;
				particles->modelInstance->SetDrawAttributes(attributes);
			}
			effect.models = visibleModels;
			effect.models.insert(effect.models.end(), hiddenModels.begin(), hiddenModels.end());
		}
		for (auto particles : effect.models) {
			modelColors[particles] = effect.color;
		}
		return true;
	}

	void findPendingModels()
	{
		modelsPending = false;
		for (auto effects : {&lineEffects, &dashEffects}) {
			for (auto& effect : *effects) {
				if (effect.started && effect.models.empty() && !findModels(effect)) {
					modelsPending = true;
				}
			}
		}
	}

	cISC4VisualEffect* createEffect(const char* name)
	{
		cISC4VisualEffect* effect = nullptr;
		if (*sppEffectsManager == nullptr || !(*sppEffectsManager)->CreateVisualEffect(name, &effect)) {
			effect = nullptr;
		}
		if (effect == nullptr && !effectsMissing) {
			Logger::GetInstance().WriteLineFormatted(LogLevel::Error, "The grading wireframe effect %s was not found. Is the NAM grading preview package installed?", name);
			effectsMissing = true;
		}
		return effect;
	}

	void showItems(std::vector<WireframeEffect>& effects, const char* name, const std::vector<WireframeItem>& items, float opacity)
	{
		for (size_t i = 0; i < items.size(); i++) {
			if (i == effects.size()) {
				effects.push_back({createEffect(name), createEffect(name)});
			}
			auto& effect = effects[i];
			if (effect.visible == nullptr || effect.hidden == nullptr) {
				continue;
			}
			const uint8_t* color = items[i].heightChange < 0 ? cutColor : fillColor;
			effect.color = {color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f};
			float visibleAlpha = opacity * opacityFor(items[i].heightChange);
			float hiddenAlpha = visibleAlpha * hiddenOpacity;
			for (auto [instance, alpha] : {std::pair{effect.visible, visibleAlpha}, std::pair{effect.hidden, hiddenAlpha}}) {
				instance->SetEffectTransform(items[i].transform);
				instance->SetFloatParams(cISC4VisualEffect::tParamType::Alpha, &alpha, 1);
				if (!effect.started) {
					instance->Start(cISC4VisualEffect::tTransitionType::Hard);
				}
			}
			effect.started = true;
			if (!findModels(effect)) {
				modelsPending = true;
			}
		}
		for (size_t i = items.size(); i < effects.size(); i++) {
			auto& effect = effects[i];
			if (effect.started) {
				effect.visible->Stop(cISC4VisualEffect::tTransitionType::Hard);
				effect.hidden->Stop(cISC4VisualEffect::tTransitionType::Hard);
				effect.started = false;
				for (auto particles : effect.models) {
					modelColors.erase(particles);
				}
				effect.models.clear();  // found again once the particles are back
			}
		}
	}

	void releaseEffects(std::vector<WireframeEffect>& effects)
	{
		for (auto& effect : effects) {
			for (auto particles : effect.models) {
				modelColors.erase(particles);
			}
			for (auto instance : {effect.visible, effect.hidden}) {
				if (instance != nullptr) {
					instance->Stop(cISC4VisualEffect::tTransitionType::Hard);
					instance->Release();
				}
			}
		}
		effects.clear();
	}

	// The line model points along x. Its x axis is stretched onto the edge, and the other two stay perpendicular to it.
	cS3DTransform lineTransform(const cS3DVector3& from, const cS3DVector3& to)
	{
		float dx = to.fX - from.fX, dy = to.fY - from.fY, dz = to.fZ - from.fZ;
		float length = std::sqrt(dx * dx + dy * dy + dz * dz);
		float horizontal = std::sqrt(dx * dx + dz * dz);
		float columns[3][3] = {
			{dx / cellSize, dy / cellSize, dz / cellSize},  // along the edge
			{-dx / horizontal * dy / length, horizontal / length, -dz / horizontal * dy / length},  // up, tilted with the edge
			{-dz / horizontal, 0.0f, dx / horizontal},  // across the edge
		};
		cS3DTransform transform(cS3DVector3(from.fX, from.fY + lineLift, from.fZ));
		transform.flags |= cS3DTransform::kRotation;
		for (int column = 0; column < 3; column++) {
			for (int row = 0; row < 3; row++) {
				transform.rotation[row][column] = columns[column][row];
			}
		}
		return transform;
	}

	void hideWireframe()
	{
		showItems(lineEffects, lineEffectName, {}, 0.0f);
		showItems(dashEffects, dashEffectName, {}, 0.0f);
	}

	void updateWireframe()
	{
		std::vector<WireframeItem> lines;
		std::vector<WireframeItem> dashes;
		cSTETerrainViewPoint* points = appliedPoints;
		uint32_t countX = appliedVertexCountX;
		uint32_t countZ = appliedVertexCountZ;
		if (points != nullptr && points == *spPoints && countX > 1 && countZ > 1) {
			auto gradedPosition = [&](uint32_t x, uint32_t z) {
				return cS3DVector3(x * cellSize, points[z * countX + x].y + vertexHeightChange(x, z), z * cellSize);
			};

			// the edges of all cells with a changed corner, each in the color of its larger change
			std::set<std::pair<uint32_t, uint32_t>> vertices;
			for (auto& entry : applied) {
				uint32_t x = entry.first % countX;
				uint32_t z = entry.first / countX;
				for (uint32_t vz = z > 0 ? z - 1 : 0; vz <= (std::min)(z + 1, countZ - 1); vz++) {
					for (uint32_t vx = x > 0 ? x - 1 : 0; vx <= (std::min)(x + 1, countX - 1); vx++) {
						vertices.insert({vx, vz});
					}
				}
			}
			for (auto& [x, z] : vertices) {
				for (auto [nx, nz] : {std::pair{x + 1, z}, std::pair{x, z + 1}}) {
					if (vertices.count({nx, nz}) == 0) {
						continue;
					}
					float change = vertexHeightChange(x, z);
					if (std::abs(vertexHeightChange(nx, nz)) > std::abs(change)) {
						change = vertexHeightChange(nx, nz);
					}
					if (std::abs(change) > minHeightChange) {
						lines.push_back({lineTransform(gradedPosition(x, z), gradedPosition(nx, nz)), change});
					}
				}
			}

			// dotted lines between the actual and the graded terrain at the changed corners, centered between both
			for (auto& [index, change] : applied) {
				float height = change.previewHeight - change.actualHeight;
				if (std::abs(height) < minDottedLineHeight) {
					continue;
				}
				float bottom = (std::min)(change.actualHeight, change.previewHeight);
				int count = static_cast<int>((std::abs(height) - dashLength) / dashSpacing) + 1;
				float first = bottom + (std::abs(height) - (count - 1) * dashSpacing - dashLength) / 2;
				float x = (index % countX) * cellSize;
				float z = (index / countX) * cellSize;
				for (int i = 0; i < count; i++) {
					dashes.push_back({cS3DTransform(cS3DVector3(x, first + i * dashSpacing, z)), height});
				}
			}
		}
		showItems(lineEffects, lineEffectName, lines, lineOpacity);
		showItems(dashEffects, dashEffectName, dashes, dottedLineOpacity);
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

		pendingVertexCountZ = networkTool->numVerticesZ;
		auto satisfier = &networkTool->vertexHtConstraintSatisfier2;
		cSC4VertexHtConstraintSatisfier::RewindSolvedVertexList(satisfier);
		int32_t vertex;
		float height;
		while (cSC4VertexHtConstraintSatisfier::GetNextSolvedVertexAndItsHeight(satisfier, &vertex, &height)) {
			if (vertex < 0 || static_cast<uint32_t>(vertex) / numVerticesX >= networkTool->numVerticesZ) {
				continue;
			}
			uint32_t index = static_cast<uint32_t>(vertex);
			auto previous = applied.find(index);  // the terrain style still shows the previous preview
			float actualHeight = previous != applied.end() ? previous->second.actualHeight : points[index].y;
			if (std::abs(height - actualHeight) > minHeightChange) {
				pending[index] = {actualHeight, height};
			}
		}
	}

	void apply()
	{
		cSTETerrainViewPoint* points = *spPoints;
		if (points == nullptr || pending.empty()) {
			return;
		}
		if (!wireframeEnabled) {
			for (auto& [index, change] : pending) {
				points[index].y = change.previewHeight;
			}
		}
		applied = std::move(pending);
		appliedPoints = points;
		appliedVertexCountX = *spVertexCountX;
		appliedVertexCountZ = pendingVertexCountZ;
		outlineCells();
		if (!wireframeEnabled) {
			redraw();
		}
	}

	void clearTerrain()
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
		if (!wireframeEnabled && appliedPoints != nullptr && appliedPoints == *spPoints) {
			for (auto& [index, change] : applied) {
				if (appliedPoints[index].y == change.previewHeight) {
					appliedPoints[index].y = change.actualHeight;
				}
			}
			redraw();
		}
		applied.clear();
		appliedPoints = nullptr;
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
		clearTerrain();
		apply();
		pending.clear();
		// the effects are reused for the new placement, which keeps them from flickering
		if (wireframeEnabled) {
			updateWireframe();
		}
	}
}

void TerrainPreview::Clear()
{
	clearTerrain();
	hideWireframe();
}

void TerrainPreview::Shutdown()
{
	Clear();
	releaseEffects(lineEffects);
	releaseEffects(dashEffects);
}

const std::array<float, 3>* TerrainPreview::GetWireframeColor(cSC4ModelBasedParticlesEffect* particles)
{
	auto search = modelColors.find(particles);
	if (search == modelColors.end() && modelsPending) {
		findPendingModels();
		search = modelColors.find(particles);
	}
	return search != modelColors.end() ? &search->second : nullptr;
}

const std::unordered_map<cSC4ModelBasedParticlesEffect*, std::array<float, 3>>& TerrainPreview::GetWireframeModels()
{
	if (modelsPending) {
		findPendingModels();
	}
	return modelColors;
}

float TerrainPreview::GetHeightChange(float x, float z)
{
	if (applied.empty() || x < 0 || z < 0) {
		return 0.0f;
	}
	float fx = x / cellSize;
	float fz = z / cellSize;
	uint32_t vx = static_cast<uint32_t>(fx);
	uint32_t vz = static_cast<uint32_t>(fz);
	float tx = fx - vx;
	float tz = fz - vz;
	float north = vertexHeightChange(vx, vz) * (1 - tx) + vertexHeightChange(vx + 1, vz) * tx;
	float south = vertexHeightChange(vx, vz + 1) * (1 - tx) + vertexHeightChange(vx + 1, vz + 1) * tx;
	return north * (1 - tz) + south * tz;
}

void TerrainPreview::Install(const Settings& settings)
{
	wireframeEnabled = settings.ghostPreviewWireframe;
	groundOutlineOpacity = wireframeEnabled ? 0.6f : 1.0f;
	std::copy(std::begin(settings.ghostPreviewCutColor), std::end(settings.ghostPreviewCutColor), cutColor);
	std::copy(std::begin(settings.ghostPreviewFillColor), std::end(settings.ghostPreviewFillColor), fillColor);

	auto callSite = reinterpret_cast<uint8_t*>(InsertIsolatedHighwayIntersection_CallSite);
	uint32_t target = InsertIsolatedHighwayIntersection_CallSite + 5 + *reinterpret_cast<int32_t*>(callSite + 1);
	THROW_HR_IF(E_FAIL, callSite[0] != 0xe8 || target != InsertIsolatedHighwayIntersection_Target);
	Patching::InstallCallHook(InsertIsolatedHighwayIntersection_CallSite, reinterpret_cast<void (*)(void)>(&Hook_InsertIsolatedHighwayIntersection));
}
