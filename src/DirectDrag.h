#pragma once
#include <cstdint>

// Drags an override network directly, without a starter piece. A network is one or more rows that lie side by side,
// as the tiles of its starter piece do. Each drag of the base network is repeated for every row, and the RUL2 engine
// seeds the row with its tile (see Rul2Engine::SetDirectDragSeed).
namespace DirectDrag
{
	// A row of the network, in the frame where the network heads south. This is the ABI of NamSetDirectDragRows.
	struct Row
	{
		uint32_t tile;     // orthogonal straight tile of the row
		int32_t offset;    // cells to the east of the row that follows the cursor
		uint32_t rotFlip;  // RotFlip of the tile
	};

	void Install();

	// Sets the network that drags produce from now on. No rows turns direct dragging off.
	void SetRows(const Row* rows, uint32_t count);

	// Diagnostics (DirectDragLog in NAM.ini): logs every committed drag, direct or not.
	void SetLogging(bool enabled);
}
