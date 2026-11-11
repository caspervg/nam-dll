#pragma once

#include "PortalFootprint.h"
#include "PortalGeometry.h"
#include "Style.h"
#include "cISC4NetworkOccupant.h"

#include <array>
#include <cstddef>
#include <cstdint>

class cIGZUnknown;

// Debug logging for the tunnel portal tool. Everything here writes at Debug log
// level, so it's a no-op in normal runs; it keeps the verbose trace strings out
// of the code that does the actual work.
namespace TunnelPortal::Debug
{
	// Dumps up to 24 path entries of a tunnel occupant's path map (key fields,
	// point count, first/last point). `label` tags each line.
	void TracePathMap(const char* label, cIGZUnknown* tunnel);

	// One line stating which pair is about to be placed.
	void LogPlacementAttempt(
		const Endpoint& first,
		const Endpoint& second,
		const Styles::Style& style);

	// The two-tile pairing decision and the numbers behind it.
	void LogPairingDecision(
		bool reverse,
		bool fromPathSemantics,
		uint32_t directPathScore,
		uint32_t reversePathScore,
		const std::array<uint32_t, 2>& firstFacingExitPathCounts,
		const std::array<uint32_t, 2>& secondFacingExitPathCounts,
		int64_t directPairingDistance,
		int64_t reversePairingDistance);

	// Full state of one stitched lane pair (cells, occupants, edges, facings).
	void LogLanePair(
		size_t laneIndex,
		bool reverse,
		const PortalCell& firstCell,
		const PortalCell& secondCell,
		cISC4NetworkOccupant::eNetworkType networkType,
		uint8_t firstDirection,
		uint8_t secondDirection);

	// Logs the call chain that led here as return addresses into the game's
	// code. SC4 omits frame pointers, so this scans the stack upward from
	// `stackPosition` and keeps values that point just after a call
	// instruction in the executable's code section. Expect the occasional
	// stale return address from an earlier call; resolve them with Ghidra.
	void LogCallStack(const char* label, const void* stackPosition);

	// Redirects the only call to cSC4TrafficNetworkMap::InitializeTunnelPaths
	// so every run logs its call stack before running unchanged.
	void InstallInitializeTunnelPathsTrace();

	// Peer path-key resolution stats from one RefreshTunnelPathInfo call.
	void LogPathRefresh(
		uint8_t selfLookupPathDirection,
		uint16_t peerPathKeyLowWord,
		uint32_t exactKeyCount,
		uint32_t remappedUniqueKeyCount,
		uint32_t unresolvedKeyCount,
		const std::array<uint32_t, 4>& unresolvedOriginalKeys);
}
