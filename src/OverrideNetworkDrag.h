#pragma once
#include "cIOverrideNetworkDrag.h"

// Drags an override network directly, without a starter piece (see cIOverrideNetworkDrag). Requires the RUL2 engine
// patch, which seeds each row of the network (see Rul2Engine::SetOverrideDragSeed).
class OverrideNetworkDrag final : public cIOverrideNetworkDrag
{
public:
	static void Install();

	// cIGZUnknown

	bool QueryInterface(uint32_t riid, void** ppvObj) override;
	uint32_t AddRef() override;
	uint32_t Release() override;

	// cIOverrideNetworkDrag

	void SetOverrideNetwork(const TileRow* rows, uint32_t rowCount) override;
	void ClearOverrideNetwork() override;

private:
	uint32_t refCount = 0;
};
