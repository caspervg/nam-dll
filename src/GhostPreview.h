#pragma once
#include "Settings.h"

// Puzzle piece previews are pinned to the terrain at a single point, so higher terrain elsewhere hides parts of them.
// This draws a tinted copy of the preview wherever it is hidden.
namespace GhostPreview
{
	void Install(const Settings& settings);
}
