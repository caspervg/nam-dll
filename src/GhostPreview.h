#pragma once
#include <cstdint>

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
	// depthOffset: SC4DrawContext depth offset applied to the ghost, so that geometry only slightly below the
	// terrain (e.g. ramp walls) is not shown
	void Install(int32_t depthOffset);
}
