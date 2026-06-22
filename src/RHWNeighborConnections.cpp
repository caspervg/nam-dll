#include "RHWNeighborConnections.h"
#include "Patching.h"

namespace
{
	// cSC4TrafficSimulator::PerformPathSearch checks this mask before using the
	// adjacent tile as the return carriageway for a divided network connection.
	constexpr uint32_t kForwardPathNetworkMaskTestAddress = 0x0071c8c1;
	constexpr uint32_t kReturnPathNetworkMaskTestAddress = 0x0071c93d;

	constexpr uint16_t kVanillaDividedNetworkConnectionMask = 0x1108;
	constexpr uint16_t kDirtRoadNeighborConnectionMask = 0x0400;
	constexpr uint16_t kRHWDividedNetworkConnectionMask =
		kVanillaDividedNetworkConnectionMask | kDirtRoadNeighborConnectionMask;
}

void RHWNeighborConnections::Install()
{
	Patching::PatchTestWordPtrEaxImmediate16(
		kForwardPathNetworkMaskTestAddress,
		kVanillaDividedNetworkConnectionMask,
		kRHWDividedNetworkConnectionMask);
	Patching::PatchTestWordPtrEaxImmediate16(
		kReturnPathNetworkMaskTestAddress,
		kVanillaDividedNetworkConnectionMask,
		kRHWDividedNetworkConnectionMask);
}
