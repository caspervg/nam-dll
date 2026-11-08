#pragma once
#include <cstddef>
#include <cstdint>
#include "NetworkStubs.h"
#include "cIS3DModelInstance.h"
#include "cISC4EffectsManager.h"
#include "cISC4VisualEffect.h"
#include "cS3DCamera.h"
#include "cS3DTransform.h"
#include "cS3DVector3.h"
#include <vector>

inline cISC4EffectsManager** const sppEffectsManager = reinterpret_cast<cISC4EffectsManager**>(0xb43d1c);

// position is the translation of the camera transform at +0x28, vPos (+0x160) is the look-at target
inline cS3DVector3 GetCameraPosition(cS3DCamera* camera)
{
	return *reinterpret_cast<cS3DVector3*>(reinterpret_cast<uint8_t*>(camera) + 0x50);
}

class SC4DrawContext
{
	public:
		uint8_t RESERVED[0x3c];
		cS3DCamera* camera;
};
static_assert(offsetof(SC4DrawContext, camera) == 0x3c);

class cSC4VisualEffect
{
	public:
		class cComponentRec
		{
			public:
				uint8_t RESERVED[0x38];
				void* component;
				uint8_t RESERVED[0x8];
		};
		static_assert(sizeof(cComponentRec) == 0x44);

		uint8_t RESERVED[0x114];
		cComponentRec* componentsBegin;
		cComponentRec* componentsEnd;
};
static_assert(offsetof(cSC4VisualEffect, componentsBegin) == 0x114);

class cSC4EffectsParticle
{
	public:
		uint8_t RESERVED[0x34];
		float alphaFactor;
		float colorFactor[3];
};
static_assert(offsetof(cSC4EffectsParticle, colorFactor) == 0x38);

class cSC4ParticleListNode
{
	public:
		cSC4ParticleListNode* next;
		uint8_t RESERVED[0x4];
		cSC4EffectsParticle* particle;
};
static_assert(offsetof(cSC4ParticleListNode, particle) == 0x8);

class cSC4ModelBasedParticlesEffect
{
	public:
		static constexpr uint32_t kVTable = 0xaa0c58;
		static constexpr uint32_t kRenderActiveParticlesSlot = 0x44;
		static constexpr uint32_t kRenderActiveParticles = 0x5add50;
		typedef void (__thiscall* pfn_RenderActiveParticles)(cSC4ModelBasedParticlesEffect* pThis, SC4DrawContext* drawContext);

		void* vtable;
		uint8_t RESERVED[0x10];
		cSC4ParticleListNode* particles;  // sentinel of a circular list
		uint8_t RESERVED[0xac];
		cS3DTransform transform;  // world transform used for rendering
		uint8_t RESERVED[0xd4];
		float alphaScale;  // SetFloatParams(3)
		uint8_t RESERVED[0x4c];
		cIS3DModelInstance* modelInstance;
		uint8_t RESERVED[0x5c];
};
static_assert(offsetof(cSC4ModelBasedParticlesEffect, particles) == 0x14);
static_assert(offsetof(cSC4ModelBasedParticlesEffect, transform) == 0xc4);
static_assert(offsetof(cSC4ModelBasedParticlesEffect, alphaScale) == 0x1d0);
static_assert(offsetof(cSC4ModelBasedParticlesEffect, modelInstance) == 0x220);
static_assert(sizeof(cSC4ModelBasedParticlesEffect) == 0x280);

class cSC4ViewInputControlNetworkIntxTool
{
	public:
		static constexpr uint32_t kVTable = 0xaaaf20;

		uint8_t RESERVED[0x2c];
		float cellSize;
		cS3DVector3 position;  // cursor position snapped to the cell center
		uint8_t RESERVED[0x4];
		cISC4VisualEffect* previewEffect;
		uint8_t RESERVED[0x4];  // cSC4LocalGridHelper
		nSC4Networks::cIntRule* intRule;  // rule of the current or last attempted placement
		uint32_t intRuleId;
		int32_t placementX;  // cell of the last valid placement
		int32_t placementZ;
		int64_t placementCost;
		uint8_t RESERVED[0x18];
		const char* previewNameBegin;  // name of the preview effect, with "_red" appended if the placement is not valid
		const char* previewNameEnd;
		uint8_t RESERVED[0x8];
		bool isPlacementValid;
};
static_assert(offsetof(cSC4ViewInputControlNetworkIntxTool, cellSize) == 0x2c);
static_assert(offsetof(cSC4ViewInputControlNetworkIntxTool, previewEffect) == 0x40);
static_assert(offsetof(cSC4ViewInputControlNetworkIntxTool, intRule) == 0x48);
static_assert(offsetof(cSC4ViewInputControlNetworkIntxTool, placementCost) == 0x58);
static_assert(offsetof(cSC4ViewInputControlNetworkIntxTool, previewNameBegin) == 0x78);
static_assert(offsetof(cSC4ViewInputControlNetworkIntxTool, isPlacementValid) == 0x88);

// the model particle components of an effect, which only exist once the effect has been started
inline std::vector<cSC4ModelBasedParticlesEffect*> GetModelParticles(cISC4VisualEffect* effect)
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
