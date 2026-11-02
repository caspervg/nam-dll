#pragma once
#include "stdint.h"

#ifdef __clang__
#define NAKED_FUN __attribute__((naked))
#else
#define NAKED_FUN __declspec(naked)
#endif

namespace Patching
{
	void OverwriteMemory(void* address, uint8_t newValue);
	void OverwriteMemory(void* address, uint32_t newValue);
	void PatchImmediate32(uint32_t address, uint32_t expectedValue, uint32_t newValue);
	void PatchPushImmediate32(uint32_t address, uint32_t expectedValue, uint32_t newValue);
	void PatchTestEaxImmediate32(uint32_t address, uint32_t expectedValue, uint32_t newValue);

	void InstallHook(uint32_t address, void (*pfnFunc)(void));
	void InstallCallHook(uint32_t address, void (*pfnFunc)(void));

	// Replaces a vtable slot after checking that it still holds the expected
	// function. Returns the previous slot value.
	uint32_t PatchVTableSlot(uint32_t slotAddress, uint32_t expectedValue, void* newValue);
}
