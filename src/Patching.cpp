#include "Patching.h"
#include <cstring>
#include <limits>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "wil/result_macros.h"
#include "wil/win32_helpers.h"

namespace
{
	constexpr size_t kJumpByteCount = 5;

	uint32_t ResolvePatchAddress(uint32_t address)
	{
		uint32_t current = address;
		for (int i = 0; i < 6; ++i)
		{
			const auto* const p = reinterpret_cast<const uint8_t*>(current);
			if (p[0] == 0xe9)
			{
				const auto rel = *reinterpret_cast<const int32_t*>(p + 1);
				current = current + 5 + rel;
				continue;
			}
			if (p[0] == 0xeb)
			{
				const auto rel = static_cast<int8_t>(p[1]);
				current = current + 2 + rel;
				continue;
			}
			if (p[0] == 0xff && p[1] == 0x25)
			{
				const auto mem = *reinterpret_cast<const uint32_t*>(p + 2);
				current = *reinterpret_cast<const uint32_t*>(mem);
				continue;
			}
			break;
		}
		return current;
	}

	int32_t GetRelativeJumpOffset(uint32_t from, uint32_t to)
	{
		const auto delta = static_cast<intptr_t>(to) - static_cast<intptr_t>(from + kJumpByteCount);
		THROW_HR_IF(E_FAIL, delta < static_cast<intptr_t>(std::numeric_limits<int32_t>::min()));
		THROW_HR_IF(E_FAIL, delta > static_cast<intptr_t>(std::numeric_limits<int32_t>::max()));
		return static_cast<int32_t>(delta);
	}
}

void Patching::OverwriteMemory(void* address, uint8_t newValue)
{
	DWORD oldProtect;
	// Allow the executable memory to be written to.
	THROW_IF_WIN32_BOOL_FALSE(VirtualProtect(
		address,
		sizeof(newValue),
		PAGE_EXECUTE_READWRITE,
		&oldProtect));

	// Patch the memory at the specified address.
	*((uint8_t*)address) = newValue;
}

void Patching::OverwriteMemory(void* address, uint32_t newValue)
{
	DWORD oldProtect;
	// Allow the executable memory to be written to.
	THROW_IF_WIN32_BOOL_FALSE(VirtualProtect(
		address,
		sizeof(newValue),
		PAGE_EXECUTE_READWRITE,
		&oldProtect));

	// Patch the memory at the specified address.
	*((uint32_t*)address) = newValue;
}

void Patching::PatchImmediate32(const uint32_t address, const uint32_t expectedValue, const uint32_t newValue)
{
	auto* const immediate = reinterpret_cast<uint32_t*>(address);
	if (*immediate == newValue)
	{
		return;
	}

	THROW_HR_IF(E_FAIL, *immediate != expectedValue);

	DWORD oldProtect;
	THROW_IF_WIN32_BOOL_FALSE(VirtualProtect(
		immediate,
		sizeof(newValue),
		PAGE_EXECUTE_READWRITE,
		&oldProtect));

	std::memcpy(immediate, &newValue, sizeof(newValue));
	FlushInstructionCache(GetCurrentProcess(), immediate, sizeof(newValue));
	VirtualProtect(immediate, sizeof(newValue), oldProtect, &oldProtect);
}

void Patching::PatchPushImmediate32(const uint32_t address, const uint32_t expectedValue, const uint32_t newValue)
{
	const uint8_t* const instruction = reinterpret_cast<uint8_t*>(address);
	THROW_HR_IF(E_FAIL, instruction[0] != 0x68);
	PatchImmediate32(address + 1, expectedValue, newValue);
}

void Patching::PatchTestEaxImmediate32(const uint32_t address, const uint32_t expectedValue, const uint32_t newValue)
{
	const uint8_t* const instruction = reinterpret_cast<uint8_t*>(address);
	THROW_HR_IF(E_FAIL, instruction[0] != 0xa9);
	PatchImmediate32(address + 1, expectedValue, newValue);
}

void Patching::InstallHook(uint32_t address, void (*pfnFunc)(void))
{
	DWORD oldProtect;
	THROW_IF_WIN32_BOOL_FALSE(VirtualProtect((void*)address, 5, PAGE_EXECUTE_READWRITE, &oldProtect));

	*((uint8_t*)address) = 0xE9;
	*((uint32_t*)(address + 1)) = ((uint32_t)pfnFunc) - address - 5;
}

void Patching::InstallInlineHook(InlineHook& hook)
{
	if (hook.installed)
	{
		return;
	}

	hook.patchAddress = ResolvePatchAddress(hook.address);
	auto* const target = reinterpret_cast<uint8_t*>(hook.patchAddress);

	if (hook.hasExpectedPrologue)
	{
		THROW_HR_IF(
			E_FAIL,
			std::memcmp(target, hook.expectedPrologue.data(), hook.expectedPrologue.size()) != 0);
	}

	std::memcpy(hook.original.data(), target, hook.original.size());

	auto* const trampoline = static_cast<uint8_t*>(VirtualAlloc(
		nullptr,
		kInlineHookPatchByteCount + kJumpByteCount,
		MEM_RESERVE | MEM_COMMIT,
		PAGE_EXECUTE_READWRITE));
	THROW_LAST_ERROR_IF_NULL(trampoline);

	try
	{
		std::memcpy(trampoline, target, kInlineHookPatchByteCount);
		trampoline[kInlineHookPatchByteCount] = 0xe9;
		const int32_t trampolineRel = GetRelativeJumpOffset(
			reinterpret_cast<uint32_t>(trampoline + kInlineHookPatchByteCount),
			reinterpret_cast<uint32_t>(target + kInlineHookPatchByteCount));
		std::memcpy(trampoline + kInlineHookPatchByteCount + 1, &trampolineRel, sizeof(trampolineRel));

		const int32_t hookRel = GetRelativeJumpOffset(
			reinterpret_cast<uint32_t>(target),
			reinterpret_cast<uint32_t>(hook.hookFunction));

		DWORD oldProtect;
		THROW_IF_WIN32_BOOL_FALSE(VirtualProtect(
			target,
			kInlineHookPatchByteCount,
			PAGE_EXECUTE_READWRITE,
			&oldProtect));

		target[0] = 0xe9;
		std::memcpy(target + 1, &hookRel, sizeof(hookRel));
		for (size_t i = kJumpByteCount; i < kInlineHookPatchByteCount; ++i)
		{
			target[i] = 0x90;
		}

		FlushInstructionCache(GetCurrentProcess(), target, kInlineHookPatchByteCount);
		VirtualProtect(target, kInlineHookPatchByteCount, oldProtect, &oldProtect);

		hook.trampoline = trampoline;
		hook.installed = true;
	}
	catch (...)
	{
		VirtualFree(trampoline, 0, MEM_RELEASE);
		throw;
	}
}

void Patching::UninstallInlineHook(InlineHook& hook)
{
	if (!hook.installed)
	{
		return;
	}

	auto* const target = reinterpret_cast<uint8_t*>(hook.patchAddress);
	DWORD oldProtect;
	if (VirtualProtect(target, kInlineHookPatchByteCount, PAGE_EXECUTE_READWRITE, &oldProtect))
	{
		std::memcpy(target, hook.original.data(), hook.original.size());
		FlushInstructionCache(GetCurrentProcess(), target, kInlineHookPatchByteCount);
		VirtualProtect(target, kInlineHookPatchByteCount, oldProtect, &oldProtect);
	}

	if (hook.trampoline)
	{
		VirtualFree(hook.trampoline, 0, MEM_RELEASE);
	}

	hook.patchAddress = 0;
	hook.trampoline = nullptr;
	hook.installed = false;
}
