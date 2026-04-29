#pragma once

#include "cISC4NetworkOccupant.h"

#include <cstdint>

namespace TunnelPortalToolPlacement
{
	// Minimal API shared between the UI control and the placement implementation.
	// Hook state, raw game layouts, and traffic repair details stay private to
	// TunnelPortalTool.cpp.
	struct Endpoint
	{
		uint32_t x = 0;
		uint32_t z = 0;
		cISC4NetworkOccupant::eNetworkType networkType = cISC4NetworkOccupant::Road;
	};

	const char* NetworkTypeName(cISC4NetworkOccupant::eNetworkType type);
	bool TryFindNetworkAtTile(
		uint32_t x,
		uint32_t z,
		cISC4NetworkOccupant::eNetworkType& networkTypeOut);
	bool PlacePortalPair(const Endpoint& first, const Endpoint& second);
}
