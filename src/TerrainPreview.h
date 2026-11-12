#pragma once
#include <array>
#include <unordered_map>

class Settings;
class cSC4ModelBasedParticlesEffect;

// Shows the terrain heights that the intersection tool solves for a hovered puzzle piece, without changing the actual terrain.
// They are drawn either as a wireframe over the actual terrain, or by copying them into the vertices of the terrain view.
namespace TerrainPreview
{
	void Install(const Settings& settings);

	// call around the intersection tool's OnMouseMove, which only solves a placement when the hovered cell changes
	void BeginMouseMove();
	void EndMouseMove();

	// restores the terrain view to the actual terrain
	void Clear();

	// also releases the wireframe effects, when the tool goes away
	void Shutdown();

	// color of a wireframe particle effect, nullptr for any other one
	const std::array<float, 3>* GetWireframeColor(cSC4ModelBasedParticlesEffect* particles);

	// all wireframe particle effects and their colors
	const std::unordered_map<cSC4ModelBasedParticlesEffect*, std::array<float, 3>>& GetWireframeModels();

	// height difference between the previewed and the actual terrain at a world position
	float GetHeightChange(float x, float z);
}
