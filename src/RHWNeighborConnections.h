#pragma once
#include <cstdint>

namespace RHWNeighborConnections
{
	void Install(
		uint32_t maxSearchDistance,
		uint32_t maxGroupingGap,
		bool enableOWRNeighborConnectionSubpatch,
		bool enableDebugLogging);
}
