#pragma once
#include <array>
#include <cstddef>
#include "stdint.h"

#ifdef __clang__
#define NAKED_FUN __attribute__((naked))
#else
#define NAKED_FUN __declspec(naked)
#endif

namespace Patching
{
	constexpr size_t kInlineHookPatchByteCount = 6;

	struct InlineHook
	{
		uint32_t address;
		void* hookFunction;
		std::array<uint8_t, kInlineHookPatchByteCount> expectedPrologue;
		bool hasExpectedPrologue;
		uint32_t patchAddress;
		std::array<uint8_t, kInlineHookPatchByteCount> original;
		void* trampoline;
		bool installed;
	};

	void OverwriteMemory(void* address, uint8_t newValue);
	void OverwriteMemory(void* address, uint32_t newValue);
	void PatchImmediate32(uint32_t address, uint32_t expectedValue, uint32_t newValue);
	void PatchPushImmediate32(uint32_t address, uint32_t expectedValue, uint32_t newValue);
	void PatchTestEaxImmediate32(uint32_t address, uint32_t expectedValue, uint32_t newValue);

	void InstallHook(uint32_t address, void (*pfnFunc)(void));
	void InstallInlineHook(InlineHook& hook);
	void UninstallInlineHook(InlineHook& hook);
}
