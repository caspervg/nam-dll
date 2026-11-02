#include "GhostPreview.h"
#include "Patching.h"
#include "EffectStubs.h"
#include "cISC4EffectsManager.h"
#include "wil/result_macros.h"
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

// The intersection tool (puzzle pieces) keeps its preview effect in a member and re-creates it whenever the
// effect name changes, e.g. for the "_red" variant of invalid placements. Instead of hooking into its
// OnMouseMove, we wrap the virtual methods of the tool and, after each call, mirror the preview effect the
// tool currently holds into a second "ghost" instance of the same effect.
//
// The model of the ghost is drawn with SC4DrawAttributes::kDepthGreaterNoWrite, so it only shows where the
// actual preview is hidden behind terrain or buildings. The preview models do not write depth themselves,
// so the two instances never overlap.
namespace
{
	cISC4EffectsManager** const sppEffectsManager = reinterpret_cast<cISC4EffectsManager**>(0xb43d1c);

	constexpr uint32_t particleParamAlphaScale = 3;  // cSC4ParticlesEffect::SetFloatParams
	constexpr float ghostAlphaScale = 0.75f;
	constexpr float ghostColor[3] = {0.55f, 0.8f, 1.0f};  // light blue tint

	constexpr size_t s3dTransformSize = 0x38;

	constexpr uint32_t IntxTool_Shutdown_Slot = 0x10;
	constexpr uint32_t IntxTool_OnKeyDown_Slot = 0x38;
	constexpr uint32_t IntxTool_OnMouseDownL_Slot = 0x40;
	constexpr uint32_t IntxTool_OnMouseUpL_Slot = 0x48;
	constexpr uint32_t IntxTool_OnMouseMove_Slot = 0x50;
	constexpr uint32_t IntxTool_OnMouseExit_Slot = 0x58;
	constexpr uint32_t IntxTool_DeletingDtor_Slot = 0x68;

	constexpr uint32_t IntxTool_Shutdown = 0x660d30;
	constexpr uint32_t IntxTool_OnKeyDown = 0x660e80;
	constexpr uint32_t IntxTool_OnMouseDownL = 0x661010;
	constexpr uint32_t IntxTool_OnMouseUpL = 0x660c30;
	constexpr uint32_t IntxTool_OnMouseMove = 0x661260;
	constexpr uint32_t IntxTool_OnMouseExit = 0x660de0;
	constexpr uint32_t IntxTool_DeletingDtor = 0x6611e0;

	typedef bool (__thiscall* pfn_OnNoArgs)(cSC4ViewInputControlNetworkIntxTool* pThis);
	typedef bool (__thiscall* pfn_OnKey)(cSC4ViewInputControlNetworkIntxTool* pThis, int32_t vkCode, uint32_t modifiers);
	typedef bool (__thiscall* pfn_OnMouse)(cSC4ViewInputControlNetworkIntxTool* pThis, int32_t x, int32_t z, uint32_t modifiers);
	typedef void* (__thiscall* pfn_DeletingDtor)(cSC4ViewInputControlNetworkIntxTool* pThis, uint32_t flags);

	pfn_OnNoArgs origShutdown = nullptr;
	pfn_OnKey origOnKeyDown = nullptr;
	pfn_OnMouse origOnMouseDownL = nullptr;
	pfn_OnMouse origOnMouseUpL = nullptr;
	pfn_OnMouse origOnMouseMove = nullptr;
	pfn_OnNoArgs origOnMouseExit = nullptr;
	pfn_DeletingDtor origDeletingDtor = nullptr;
	cSC4ModelBasedParticlesEffect::pfn_RenderActiveParticles origRenderActiveParticles = nullptr;

	struct GhostState
	{
		cSC4ViewInputControlNetworkIntxTool* tool = nullptr;
		cISC4VisualEffect* source = nullptr;
		cISC4VisualEffect* ghost = nullptr;
		bool modelsFlagged = false;
		std::vector<cSC4ModelBasedParticlesEffect*> components;
	};
	GhostState sGhost;

	void destroyGhost()
	{
		if (sGhost.ghost != nullptr) {
			sGhost.ghost->Stop(cISC4VisualEffect::tTransitionType::Unknown1);
			sGhost.ghost->Release();
		}
		sGhost = GhostState{};
	}

	// Components and their models are created lazily after Start, so this is retried until something was flagged.
	bool flagGhostModels(cISC4VisualEffect* ghost)
	{
		auto visualEffect = reinterpret_cast<cSC4VisualEffect*>(ghost);
		bool flagged = false;
		for (auto rec = visualEffect->componentsBegin; rec != nullptr && rec < visualEffect->componentsEnd; rec++) {
			auto component = reinterpret_cast<cSC4ModelBasedParticlesEffect*>(rec->component);
			if (component == nullptr || reinterpret_cast<uint32_t>(component->vtable) != cSC4ModelBasedParticlesEffect::kVTable) {
				continue;  // only model particles carry a model instance
			}
			cIS3DModelInstance* modelInstance = component->modelInstance;
			if (modelInstance != nullptr) {
				auto getDrawAttributes = reinterpret_cast<pfn_GetDrawAttributes>((*reinterpret_cast<void***>(modelInstance))[kModelInstanceGetDrawAttributesSlot]);
				SC4DrawAttributes current = {};
				SC4DrawAttributes attributes = *getDrawAttributes(modelInstance, &current);
				attributes.flags |= SC4DrawAttributes::kDepthGreaterNoWrite;  // the particle renderer keeps this flag on every frame
				modelInstance->SetDrawAttributes(attributes);
				sGhost.components.push_back(component);
				flagged = true;
			}
		}
		return flagged;
	}

	void createGhost(cSC4ViewInputControlNetworkIntxTool* tool, cISC4VisualEffect* source)
	{
		sGhost.tool = tool;
		sGhost.source = source;

		cISC4EffectsManager* effectsManager = *sppEffectsManager;
		if (effectsManager == nullptr || tool->previewNameBegin == nullptr || tool->previewNameEnd <= tool->previewNameBegin) {
			return;
		}

		const std::string effectName(tool->previewNameBegin, tool->previewNameEnd);
		cISC4VisualEffect* ghost = nullptr;
		if (effectsManager->CreateVisualEffect(effectName.c_str(), &ghost) && ghost != nullptr) {
			ghost->SetFloatParams(particleParamAlphaScale, &ghostAlphaScale, 1);
			ghost->Start(cISC4VisualEffect::tTransitionType::Unknown1);
			sGhost.ghost = ghost;
		}
	}

	void syncGhost(cSC4ViewInputControlNetworkIntxTool* tool)
	{
		if (sGhost.tool != nullptr && sGhost.tool != tool) {
			destroyGhost();
		}

		cISC4VisualEffect* source = tool->previewEffect;
		if (source != sGhost.source) {
			destroyGhost();
			if (source != nullptr) {
				createGhost(tool, source);
			}
		}

		if (sGhost.ghost != nullptr && source != nullptr) {
			alignas(16) uint8_t transform[s3dTransformSize] = {};
			source->GetEffectTransform(*reinterpret_cast<cS3DTransform*>(transform));
			sGhost.ghost->SetEffectTransform(*reinterpret_cast<cS3DTransform*>(transform));

			if (!sGhost.modelsFlagged) {
				sGhost.modelsFlagged = flagGhostModels(sGhost.ghost);
			}
		}
	}

	bool __fastcall Hook_Shutdown(cSC4ViewInputControlNetworkIntxTool* pThis, void* edxUnused)
	{
		bool result = origShutdown(pThis);
		syncGhost(pThis);
		return result;
	}

	bool __fastcall Hook_OnKeyDown(cSC4ViewInputControlNetworkIntxTool* pThis, void* edxUnused, int32_t vkCode, uint32_t modifiers)
	{
		bool result = origOnKeyDown(pThis, vkCode, modifiers);
		syncGhost(pThis);
		return result;
	}

	bool __fastcall Hook_OnMouseDownL(cSC4ViewInputControlNetworkIntxTool* pThis, void* edxUnused, int32_t x, int32_t z, uint32_t modifiers)
	{
		bool result = origOnMouseDownL(pThis, x, z, modifiers);
		syncGhost(pThis);
		return result;
	}

	bool __fastcall Hook_OnMouseUpL(cSC4ViewInputControlNetworkIntxTool* pThis, void* edxUnused, int32_t x, int32_t z, uint32_t modifiers)
	{
		bool result = origOnMouseUpL(pThis, x, z, modifiers);
		syncGhost(pThis);
		return result;
	}

	bool __fastcall Hook_OnMouseMove(cSC4ViewInputControlNetworkIntxTool* pThis, void* edxUnused, int32_t x, int32_t z, uint32_t modifiers)
	{
		bool result = origOnMouseMove(pThis, x, z, modifiers);
		syncGhost(pThis);
		return result;
	}

	bool __fastcall Hook_OnMouseExit(cSC4ViewInputControlNetworkIntxTool* pThis, void* edxUnused)
	{
		bool result = origOnMouseExit(pThis);
		syncGhost(pThis);
		return result;
	}

	// Shared by all model-based particle effects, so only the components of the ghost are tinted.
	void __fastcall Hook_RenderActiveParticles(cSC4ModelBasedParticlesEffect* pThis, void* edxUnused, SC4DrawContext* drawContext)
	{
		if (std::find(sGhost.components.begin(), sGhost.components.end(), pThis) != sGhost.components.end()) {
			// particles are re-emitted continuously, so their color factors are overwritten before every frame
			for (auto node = pThis->particles->next; node != pThis->particles; node = node->next) {
				std::copy(std::begin(ghostColor), std::end(ghostColor), node->particle->colorFactor);
			}
		}
		origRenderActiveParticles(pThis, drawContext);
	}

	void* __fastcall Hook_DeletingDtor(cSC4ViewInputControlNetworkIntxTool* pThis, void* edxUnused, uint32_t flags)
	{
		if (sGhost.tool == pThis) {
			destroyGhost();
		}
		return origDeletingDtor(pThis, flags);
	}

	template <typename Fn, typename Hook>
	void patchToolSlot(uint32_t slot, uint32_t expected, Fn& original, Hook hook)
	{
		original = reinterpret_cast<Fn>(Patching::PatchVTableSlot(
				cSC4ViewInputControlNetworkIntxTool::kVTable + slot, expected, reinterpret_cast<void*>(hook)));
	}
}

void GhostPreview::Install()
{
	// verify every slot before patching any, so that a mismatch leaves the vtable untouched
	constexpr std::pair<uint32_t, uint32_t> expectedSlots[] = {
		{IntxTool_Shutdown_Slot, IntxTool_Shutdown},
		{IntxTool_OnKeyDown_Slot, IntxTool_OnKeyDown},
		{IntxTool_OnMouseDownL_Slot, IntxTool_OnMouseDownL},
		{IntxTool_OnMouseUpL_Slot, IntxTool_OnMouseUpL},
		{IntxTool_OnMouseMove_Slot, IntxTool_OnMouseMove},
		{IntxTool_OnMouseExit_Slot, IntxTool_OnMouseExit},
		{IntxTool_DeletingDtor_Slot, IntxTool_DeletingDtor},
	};
	for (auto&& [slot, expected] : expectedSlots) {
		THROW_HR_IF(E_FAIL, *reinterpret_cast<const uint32_t*>(cSC4ViewInputControlNetworkIntxTool::kVTable + slot) != expected);
	}
	constexpr uint32_t renderSlotAddress = cSC4ModelBasedParticlesEffect::kVTable + cSC4ModelBasedParticlesEffect::kRenderActiveParticlesSlot;
	THROW_HR_IF(E_FAIL, *reinterpret_cast<const uint32_t*>(renderSlotAddress) != cSC4ModelBasedParticlesEffect::kRenderActiveParticles);

	patchToolSlot(IntxTool_Shutdown_Slot, IntxTool_Shutdown, origShutdown, &Hook_Shutdown);
	patchToolSlot(IntxTool_OnKeyDown_Slot, IntxTool_OnKeyDown, origOnKeyDown, &Hook_OnKeyDown);
	patchToolSlot(IntxTool_OnMouseDownL_Slot, IntxTool_OnMouseDownL, origOnMouseDownL, &Hook_OnMouseDownL);
	patchToolSlot(IntxTool_OnMouseUpL_Slot, IntxTool_OnMouseUpL, origOnMouseUpL, &Hook_OnMouseUpL);
	patchToolSlot(IntxTool_OnMouseMove_Slot, IntxTool_OnMouseMove, origOnMouseMove, &Hook_OnMouseMove);
	patchToolSlot(IntxTool_OnMouseExit_Slot, IntxTool_OnMouseExit, origOnMouseExit, &Hook_OnMouseExit);
	patchToolSlot(IntxTool_DeletingDtor_Slot, IntxTool_DeletingDtor, origDeletingDtor, &Hook_DeletingDtor);
	origRenderActiveParticles = reinterpret_cast<cSC4ModelBasedParticlesEffect::pfn_RenderActiveParticles>(Patching::PatchVTableSlot(
			renderSlotAddress, cSC4ModelBasedParticlesEffect::kRenderActiveParticles, reinterpret_cast<void*>(&Hook_RenderActiveParticles)));
}
