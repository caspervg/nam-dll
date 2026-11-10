#pragma once
#include <cstdint>
#include "RotFlip.h"

namespace Rul2Engine
{
	void Install();

	// Override network drag: the drags solved from now on turn into the override network with this orthogonal
	// straight tile, as if a starter piece lay behind the start of the drag. The orientation is the one the tile has
	// when the network heads south. A tile of 0 turns it off. startXz is the cell where the drag starts (z << 16 | x), or
	// 0xFFFFFFFF when that is not known. See OverrideNetworkDrag, which sets this for each row it places.
	void SetOverrideDragSeed(uint32_t tileId, RotFlip rotFlip, uint32_t startXz = 0xFFFFFFFF);

	// Diagnostics: logs the cells of the drags solved from now on, before and after the overrides (log level Debug).
	void SetDragLog(bool enabled);
}
