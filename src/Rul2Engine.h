#pragma once
#include <cstdint>

namespace Rul2Engine
{
	void Install();

	// Direct drag: drags of the base network turn into the override network with this orthogonal straight tile,
	// without a starter piece. 0 turns it off.
	void SetDirectDragTile(uint32_t tileId);
}
