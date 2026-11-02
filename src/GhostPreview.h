#pragma once

// Shows the parts of a RUL0 puzzle-piece preview that are hidden by terrain as a
// dimmed "ghost".
//
// Preview effects pin their single model particle to the terrain at one point
// (`source -pinToTerrain`), so any terrain under the rest of the piece that is
// higher than that point hides part of the model. Instead of moving the preview,
// this patch spawns a second instance of the same effect whose model is drawn
// with depth test GREATER and no depth writes, so it only appears where the real
// preview is occluded.
namespace GhostPreview
{
	// depthTolerance: distance (in meters, along the view direction) by which preview and ghost are moved towards the camera
	// while rendering, so that geometry only slightly below the terrain is still drawn as the regular preview
	void Install(float depthTolerance);
}
