#pragma once
#include "stdint.h"
#include <array>
#include <cstddef>
#include <vector>
#include <initializer_list>

#ifdef __clang__
#define NAKED_FUN __attribute__((naked))
#else
#define NAKED_FUN __declspec(naked)
#endif

namespace Patching
{
	constexpr size_t kInlineHookPatchByteCount = 6;

	// Detours a function entry to hookFunction and leaves a trampoline behind, so
	// the hook can still run the displaced original. Patches are installed for the
	// lifetime of the process; SC4 keeps running until it exits.
	struct InlineHook
	{
		uint32_t address;
		void* hookFunction;
		std::array<uint8_t, kInlineHookPatchByteCount> expectedBytes;
		bool checkExpectedBytes;
		std::array<uint8_t, kInlineHookPatchByteCount> original;
		void* trampoline;
		bool installed;
	};

	// Replaces a single, statically known vtable slot. The slot has to still hold
	// expectedOriginal, which keeps a wrong address or a changed game build from
	// silently redirecting an unrelated method.
	struct VTableHook
	{
		uint32_t slotAddress;
		void* hookFunction;
		uint32_t expectedOriginal;
		void* original;
		bool installed;
	};

	void OverwriteMemory(void* address, uint8_t newValue);
	void OverwriteMemory(void* address, uint32_t newValue);
	void PatchImmediate16(uint32_t address, uint16_t expectedValue, uint16_t newValue);
	void ExpectBytes(uint32_t address, std::initializer_list<uint8_t> expectedBytes);
	void PatchByte(uint32_t address, uint8_t expectedValue, uint8_t newValue);
	void PatchImmediate32(uint32_t address, uint32_t expectedValue, uint32_t newValue);
	void PatchPushImmediate32(uint32_t address, uint32_t expectedValue, uint32_t newValue);
	void PatchTestEaxImmediate32(uint32_t address, uint32_t expectedValue, uint32_t newValue);
	void PatchTestWordPtrEaxImmediate16(uint32_t address, uint16_t expectedValue, uint16_t newValue);

	void InstallHook(uint32_t address, void (*pfnFunc)(void));
	void InstallCallHook(uint32_t address, void (*pfnFunc)(void));
	void InstallInlineHook(InlineHook& hook);
	void InstallVTableHook(VTableHook& hook);
	void PatchBytes(uint32_t address, const std::vector<uint8_t>& expected, const std::vector<uint8_t>& replacement);
	void ReplaceWithCall(uint32_t address, const std::vector<uint8_t>& expected, void (*pfnFunc)(void));

	// returns the previous slot value
	uint32_t PatchVTableSlot(uint32_t slotAddress, uint32_t expectedValue, void* newValue);
}
