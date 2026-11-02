#include "GhostPreview.h"
#include "Patching.h"
#include "TerrainPreview.h"
#include "EffectStubs.h"
#include "cISC4EffectsManager.h"
#include "wil/result_macros.h"
#include <algorithm>
#include <string>
#include <vector>

// Whenever the intersection tool has a preview effect, we create a second instance of the same effect (the ghost).
// Its model is drawn with depth test GREATER, so it only shows where the preview is hidden by terrain or buildings.
// The preview models write depth, so the ghost has to be drawn right before the preview each frame.
// With grading, the terrain is drawn as it will be after placement, and the preview is moved onto it.
namespace
{
	cISC4EffectsManager** const sppEffectsManager = reinterpret_cast<cISC4EffectsManager**>(0xb43d1c);

	constexpr uint32_t particleAlphaScaleParam = 3;  // SetFloatParams

	bool overlayEnabled;
	bool gradingEnabled;
	float ghostColor[3];
	float ghostAlphaScale;
	float depthTolerance;

	cISC4VisualEffect* ghost = nullptr;
	cISC4VisualEffect* ghostSource = nullptr;
	cSC4ViewInputControlNetworkIntxTool* ghostTool = nullptr;
	std::vector<cSC4ModelBasedParticlesEffect*> ghostModels;
	std::vector<cSC4ModelBasedParticlesEffect*> sourceModels;

	cSC4ModelBasedParticlesEffect::pfn_RenderActiveParticles RenderActiveParticles = nullptr;

	std::vector<cSC4ModelBasedParticlesEffect*> getModelParticles(cISC4VisualEffect* effect)
	{
		std::vector<cSC4ModelBasedParticlesEffect*> result;
		auto visualEffect = reinterpret_cast<cSC4VisualEffect*>(effect);
		for (auto rec = visualEffect->componentsBegin; rec != nullptr && rec < visualEffect->componentsEnd; rec++) {
			auto particles = reinterpret_cast<cSC4ModelBasedParticlesEffect*>(rec->component);
			if (particles != nullptr && reinterpret_cast<uint32_t>(particles->vtable) == cSC4ModelBasedParticlesEffect::kVTable && particles->modelInstance != nullptr) {
				result.push_back(particles);
			}
		}
		return result;
	}

	void destroyGhost()
	{
		if (ghost != nullptr) {
			ghost->Stop(cISC4VisualEffect::tTransitionType::Unknown1);
			ghost->Release();
		}
		ghost = nullptr;
		ghostSource = nullptr;
		ghostTool = nullptr;
		ghostModels.clear();
		sourceModels.clear();
	}

	void createGhost(cSC4ViewInputControlNetworkIntxTool* tool)
	{
		if (*sppEffectsManager == nullptr || tool->previewNameEnd <= tool->previewNameBegin) {
			return;
		}

		std::string name(tool->previewNameBegin, tool->previewNameEnd);
		if ((*sppEffectsManager)->CreateVisualEffect(name.c_str(), &ghost)) {
			ghost->SetFloatParams(particleAlphaScaleParam, &ghostAlphaScale, 1);
			ghost->Start(cISC4VisualEffect::tTransitionType::Unknown1);
		}
	}

	void updateGhost(cSC4ViewInputControlNetworkIntxTool* tool)
	{
		if (tool != ghostTool || tool->previewEffect != ghostSource) {
			destroyGhost();
			ghostTool = tool;
			ghostSource = tool->previewEffect;
			if (overlayEnabled && ghostSource != nullptr) {
				createGhost(tool);
			}
		}
		if (ghostSource == nullptr) {
			TerrainPreview::Clear();
			return;
		}
		// the particle models only exist once the effects have been started
		if (sourceModels.empty()) {
			sourceModels = getModelParticles(ghostSource);
		}
		if (ghost == nullptr) {
			return;
		}

		cS3DTransform transform = {};
		ghostSource->GetEffectTransform(transform);
		ghost->SetEffectTransform(transform);

		if (ghostModels.empty()) {
			ghostModels = getModelParticles(ghost);
			for (auto particles : ghostModels) {
				SC4DrawAttributes attributes = GetDrawAttributes(particles->modelInstance);
				attributes.flags |= SC4DrawAttributes::kDepthGreaterNoWrite;  // kept by the particle renderer
				particles->modelInstance->SetDrawAttributes(attributes);
			}
		}
	}

	// Moving the preview and ghost towards the camera only changes their depth, as the camera is orthographic.
	// Parts buried less than the tolerance are then drawn as the regular preview instead of the ghost.
	cS3DVector3 getToleranceShift(SC4DrawContext* drawContext)
	{
		cS3DVector3 shift(0.0f);
		if (depthTolerance != 0.0f && drawContext->camera != nullptr) {
			cS3DVector3 position = GetCameraPosition(drawContext->camera);
			cS3DVector3& lookAt = drawContext->camera->vPos;
			shift = cS3DVector3(position.fX - lookAt.fX, position.fY - lookAt.fY, position.fZ - lookAt.fZ);
			if (shift.Length() > 0.0f) {
				shift.SetLength(depthTolerance);
			}
		}
		return shift;
	}

	// Only the render transform is moved, as the particles are pinned to the terrain when they are emitted.
	void renderShifted(cSC4ModelBasedParticlesEffect* particles, SC4DrawContext* drawContext, const cS3DVector3& shift)
	{
		cS3DVector3& translation = particles->transform.translation;
		bool hasTranslation = particles->transform.flags != 0;
		if (hasTranslation) {
			translation.fX += shift.fX;
			translation.fY += shift.fY;
			translation.fZ += shift.fZ;
		}
		RenderActiveParticles(particles, drawContext);
		if (hasTranslation) {
			translation.fX -= shift.fX;
			translation.fY -= shift.fY;
			translation.fZ -= shift.fZ;
		}
	}

	bool contains(const std::vector<cSC4ModelBasedParticlesEffect*>& list, cSC4ModelBasedParticlesEffect* particles)
	{
		return std::find(list.begin(), list.end(), particles) != list.end();
	}

	void __fastcall Hook_RenderActiveParticles(cSC4ModelBasedParticlesEffect* pThis, void*, SC4DrawContext* drawContext)
	{
		if (contains(ghostModels, pThis)) {
			return;  // drawn together with the preview
		}
		if (!contains(sourceModels, pThis)) {
			RenderActiveParticles(pThis, drawContext);
			return;
		}

		// keep the preview's own color in the tint, so the ghost of a _red preview stays red
		float tint[3];
		std::copy(std::begin(ghostColor), std::end(ghostColor), tint);
		if (pThis->particles->next != pThis->particles) {
			const float* sourceColor = pThis->particles->next->particle->colorFactor;
			for (int i = 0; i < 3; i++) {
				tint[i] *= sourceColor[i];
			}
		}

		cS3DVector3 shift = getToleranceShift(drawContext);
		if (gradingEnabled && pThis->transform.flags != 0) {
			// the preview is pinned to the actual terrain, so it is moved onto the previewed terrain at its position
			shift.fY += TerrainPreview::GetHeightChange(pThis->transform.translation.fX, pThis->transform.translation.fZ);
		}
		for (auto particles : ghostModels) {
			// the particles are re-emitted all the time, so their color is set every frame
			for (auto node = particles->particles->next; node != particles->particles; node = node->next) {
				std::copy(std::begin(tint), std::end(tint), node->particle->colorFactor);
			}
			renderShifted(particles, drawContext, shift);
		}
		renderShifted(pThis, drawContext, shift);
	}

	typedef bool (__thiscall* pfn_Shutdown)(cSC4ViewInputControlNetworkIntxTool* pThis);
	typedef bool (__thiscall* pfn_OnKeyDown)(cSC4ViewInputControlNetworkIntxTool* pThis, int32_t vkCode, uint32_t modifiers);
	typedef bool (__thiscall* pfn_OnMouse)(cSC4ViewInputControlNetworkIntxTool* pThis, int32_t x, int32_t z, uint32_t modifiers);
	typedef bool (__thiscall* pfn_OnMouseExit)(cSC4ViewInputControlNetworkIntxTool* pThis);
	typedef void* (__thiscall* pfn_DeletingDtor)(cSC4ViewInputControlNetworkIntxTool* pThis, uint32_t flags);

	pfn_Shutdown Shutdown = nullptr;
	pfn_OnKeyDown OnKeyDown = nullptr;
	pfn_OnMouse OnMouseDownL = nullptr;
	pfn_OnMouse OnMouseUpL = nullptr;
	pfn_OnMouse OnMouseMove = nullptr;
	pfn_OnMouseExit OnMouseExit = nullptr;
	pfn_DeletingDtor DeletingDtor = nullptr;

	// The tool replaces or releases its preview effect in these methods, so the ghost is updated after each of them.
	bool __fastcall Hook_Shutdown(cSC4ViewInputControlNetworkIntxTool* pThis, void*)
	{
		bool result = Shutdown(pThis);
		updateGhost(pThis);
		return result;
	}

	bool __fastcall Hook_OnKeyDown(cSC4ViewInputControlNetworkIntxTool* pThis, void*, int32_t vkCode, uint32_t modifiers)
	{
		bool result = OnKeyDown(pThis, vkCode, modifiers);
		updateGhost(pThis);
		return result;
	}

	bool __fastcall Hook_OnMouseDownL(cSC4ViewInputControlNetworkIntxTool* pThis, void*, int32_t x, int32_t z, uint32_t modifiers)
	{
		TerrainPreview::Clear();  // placing the piece changes the actual terrain, which must not be restored afterwards
		bool result = OnMouseDownL(pThis, x, z, modifiers);
		updateGhost(pThis);
		return result;
	}

	bool __fastcall Hook_OnMouseUpL(cSC4ViewInputControlNetworkIntxTool* pThis, void*, int32_t x, int32_t z, uint32_t modifiers)
	{
		bool result = OnMouseUpL(pThis, x, z, modifiers);
		updateGhost(pThis);
		return result;
	}

	bool __fastcall Hook_OnMouseMove(cSC4ViewInputControlNetworkIntxTool* pThis, void*, int32_t x, int32_t z, uint32_t modifiers)
	{
		TerrainPreview::BeginMouseMove();
		bool result = OnMouseMove(pThis, x, z, modifiers);
		TerrainPreview::EndMouseMove();
		updateGhost(pThis);
		return result;
	}

	bool __fastcall Hook_OnMouseExit(cSC4ViewInputControlNetworkIntxTool* pThis, void*)
	{
		bool result = OnMouseExit(pThis);
		updateGhost(pThis);
		return result;
	}

	void* __fastcall Hook_DeletingDtor(cSC4ViewInputControlNetworkIntxTool* pThis, void*, uint32_t flags)
	{
		if (pThis == ghostTool) {
			destroyGhost();
			TerrainPreview::Clear();
		}
		return DeletingDtor(pThis, flags);
	}

	struct VTableHook
	{
		uint32_t slotAddress;
		uint32_t expected;
		void** original;
		void* hook;
	};

	constexpr uint32_t toolSlot(uint32_t offset)
	{
		return cSC4ViewInputControlNetworkIntxTool::kVTable + offset;
	}

	const VTableHook vtableHooks[] = {
		{toolSlot(0x10), 0x660d30, reinterpret_cast<void**>(&Shutdown), reinterpret_cast<void*>(&Hook_Shutdown)},
		{toolSlot(0x38), 0x660e80, reinterpret_cast<void**>(&OnKeyDown), reinterpret_cast<void*>(&Hook_OnKeyDown)},
		{toolSlot(0x40), 0x661010, reinterpret_cast<void**>(&OnMouseDownL), reinterpret_cast<void*>(&Hook_OnMouseDownL)},
		{toolSlot(0x48), 0x660c30, reinterpret_cast<void**>(&OnMouseUpL), reinterpret_cast<void*>(&Hook_OnMouseUpL)},
		{toolSlot(0x50), 0x661260, reinterpret_cast<void**>(&OnMouseMove), reinterpret_cast<void*>(&Hook_OnMouseMove)},
		{toolSlot(0x58), 0x660de0, reinterpret_cast<void**>(&OnMouseExit), reinterpret_cast<void*>(&Hook_OnMouseExit)},
		{toolSlot(0x68), 0x6611e0, reinterpret_cast<void**>(&DeletingDtor), reinterpret_cast<void*>(&Hook_DeletingDtor)},
		{
			cSC4ModelBasedParticlesEffect::kVTable + cSC4ModelBasedParticlesEffect::kRenderActiveParticlesSlot,
			cSC4ModelBasedParticlesEffect::kRenderActiveParticles,
			reinterpret_cast<void**>(&RenderActiveParticles),
			reinterpret_cast<void*>(&Hook_RenderActiveParticles)
		},
	};
}

void GhostPreview::Install(const Settings& settings)
{
	for (int i = 0; i < 3; i++) {
		ghostColor[i] = settings.ghostPreviewColor[i] / 255.0f;
	}
	overlayEnabled = settings.ghostPreviewOverlay;
	gradingEnabled = settings.ghostPreviewGrading;
	ghostAlphaScale = settings.ghostPreviewAlphaScale;
	depthTolerance = settings.ghostPreviewDepthTolerance;

	// check all slots first, so that nothing is patched if any of them is unexpected
	for (auto& vtableHook : vtableHooks) {
		THROW_HR_IF(E_FAIL, *reinterpret_cast<uint32_t*>(vtableHook.slotAddress) != vtableHook.expected);
	}
	if (gradingEnabled) {
		TerrainPreview::Install(settings.ghostPreviewGradingOpacity);
	}
	for (auto& vtableHook : vtableHooks) {
		*vtableHook.original = reinterpret_cast<void*>(Patching::PatchVTableSlot(vtableHook.slotAddress, vtableHook.expected, vtableHook.hook));
	}
}
