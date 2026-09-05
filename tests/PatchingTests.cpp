// Run from the repository root in an x86 Native Tools command prompt:
// cl /nologo /std:c++20 /EHsc /I src /I vendor/wil/include tests/PatchingTests.cpp src/Patching.cpp /Fe:cmake-build-transit-check/PatchingTests.exe /Fo:cmake-build-transit-check/ /link ole32.lib
// cmake-build-transit-check\PatchingTests.exe
#include "Patching.h"
#include <Windows.h>
#include <cassert>
#include <cstring>

int main()
{
	static_assert(sizeof(void*) == 4);
	constexpr uint8_t prologue[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};
	auto* target = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
	assert(target);
	std::memcpy(target, prologue, sizeof(prologue));
	Patching::InlineHook hook{
		.address = reinterpret_cast<uint32_t>(target),
		.hookFunction = target + 32,
		.expectedPrologue = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8},
		.hasExpectedPrologue = true
	};
	Patching::InstallInlineHook(hook);
	assert(hook.installed && target[0] == 0xe9);
	void* const trampoline = hook.trampoline;
	Patching::InstallInlineHook(hook);
	assert(hook.trampoline == trampoline);

	// A reserved, uncommitted target makes VirtualProtect fail deterministically.
	assert(VirtualFree(target, 4096, MEM_DECOMMIT));
	Patching::UninstallInlineHook(hook);
	assert(hook.installed && hook.trampoline == trampoline);
	MEMORY_BASIC_INFORMATION memory{};
	assert(VirtualQuery(trampoline, &memory, sizeof(memory)));
	assert(memory.State == MEM_COMMIT);

	assert(VirtualAlloc(target, 4096, MEM_COMMIT, PAGE_EXECUTE_READWRITE) == target);
	Patching::UninstallInlineHook(hook);
	assert(!hook.installed && !hook.trampoline);
	assert(std::memcmp(target, prologue, sizeof(prologue)) == 0);
	Patching::UninstallInlineHook(hook);
	assert(VirtualFree(target, 0, MEM_RELEASE));
}
