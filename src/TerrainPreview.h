#pragma once
#include <array>
#include <unordered_map>

class cSC4ModelBasedParticlesEffect;

// While hovering a puzzle piece, the intersection tool already solves the terrain heights of a placement without committing them.
// This shows that solution either as a wireframe over the actual terrain, or by copying it into the vertices drawn by the terrain view.
// The terrain itself is not modified, so nothing in the city reacts to it.
namespace TerrainPreview
{
	// the changed cells are outlined in the cut or fill color (R,G,B), with the given opacity for large height changes
	void Install(float opacity, const int (&cutColor)[3], const int (&fillColor)[3], bool wireframe);

	// call around the intersection tool's OnMouseMove, which only solves a placement when the hovered cell changes
	void BeginMouseMove();
	void EndMouseMove();

	// restores the terrain view to the actual terrain
	void Clear();

	// also releases the wireframe effects, when the tool goes away
	void Shutdown();

	// color of a wireframe particle effect, false for any other one
	bool GetWireframeColor(cSC4ModelBasedParticlesEffect* particles, float (&color)[3]);

	// all wireframe particle effects and their colors
	const std::unordered_map<cSC4ModelBasedParticlesEffect*, std::array<float, 3>>& GetWireframeModels();

	// height difference between the previewed and the actual terrain at a world position
	float GetHeightChange(float x, float z);
}
