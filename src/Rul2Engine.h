#pragma once
#include <cstdint>
#include "RotFlip.h"

namespace Rul2Engine
{
	void Install();

	// Direct drag: the drags solved from now on turn into the override network with this orthogonal straight tile,
	// as if a starter piece lay behind the start of the drag. The orientation is the one the tile has when the
	// network heads south. A tile of 0 turns it off. See DirectDrag, which sets this for each row it places.
	void SetDirectDragSeed(uint32_t tileId, RotFlip rotFlip);

	// Diagnostics: logs the cells of the drags solved from now on, before and after the overrides.
	void SetDragLog(bool enabled);
}
