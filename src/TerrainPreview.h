#pragma once

// While hovering a puzzle piece, the intersection tool already solves the terrain heights of a placement without committing them.
// This copies that solution into the vertices drawn by the terrain view, so the terrain is shown as it will be after placement.
// The terrain itself is not modified, so nothing in the city reacts to it.
namespace TerrainPreview
{
	// the changed cells are outlined in the cut or fill color (R,G,B), with the given opacity for large height changes
	void Install(float opacity, const int (&cutColor)[3], const int (&fillColor)[3]);

	// call around the intersection tool's OnMouseMove, which only solves a placement when the hovered cell changes
	void BeginMouseMove();
	void EndMouseMove();

	// restores the terrain view to the actual terrain
	void Clear();

	// height difference between the previewed and the actual terrain at a world position
	float GetHeightChange(float x, float z);
}
