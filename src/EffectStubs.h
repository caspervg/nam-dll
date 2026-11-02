#pragma once
#include <cstddef>
#include <cstdint>
#include "NetworkStubs.h"
#include "cIS3DModelInstance.h"
#include "cISC4VisualEffect.h"
#include "cS3DCamera.h"
#include "cS3DVector3.h"

class cS3DTransform
{
	public:
		uint8_t RESERVED[0x38];
};
static_assert(sizeof(cS3DTransform) == 0x38);

class SC4DrawAttributes
{
	public:
		enum Flags : uint8_t {
			kApplyAlpha = 0x02,
			kSelfLit = 0x04,
			kDepthGreaterNoWrite = 0x20,  // only draws where something else is in front
		};

		uint8_t flags;
		uint8_t RESERVED[3];
		float color[3];
		float alpha;
};
static_assert(sizeof(SC4DrawAttributes) == 0x14);

// gzcom-dll declares this without arguments, but the game returns the attributes through a hidden out-pointer
inline SC4DrawAttributes GetDrawAttributes(cIS3DModelInstance* modelInstance)
{
	typedef SC4DrawAttributes* (__thiscall* pfn_GetDrawAttributes)(cIS3DModelInstance* pThis, SC4DrawAttributes* out);
	auto getDrawAttributes = reinterpret_cast<pfn_GetDrawAttributes>((*reinterpret_cast<void***>(modelInstance))[0x3c / 4]);
	SC4DrawAttributes attributes = {};
	return *getDrawAttributes(modelInstance, &attributes);
}

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
		uint8_t RESERVED[0xad];
		uint8_t transformFlags;  // world transform at +0xc4; 0 if identity, in which case the translation is unused
		uint8_t RESERVED[0x26];
		float transformTranslation[3];
		uint8_t RESERVED[0x128];
		cIS3DModelInstance* modelInstance;
};
static_assert(offsetof(cSC4ModelBasedParticlesEffect, particles) == 0x14);
static_assert(offsetof(cSC4ModelBasedParticlesEffect, transformFlags) == 0xc5);
static_assert(offsetof(cSC4ModelBasedParticlesEffect, transformTranslation) == 0xec);
static_assert(offsetof(cSC4ModelBasedParticlesEffect, modelInstance) == 0x220);

class cSC4ViewInputControlNetworkIntxTool
{
	public:
		static constexpr uint32_t kVTable = 0xaaaf20;

		uint8_t RESERVED[0x40];
		cISC4VisualEffect* previewEffect;
		uint8_t RESERVED[0x34];
		const char* previewNameBegin;
		const char* previewNameEnd;
};
static_assert(offsetof(cSC4ViewInputControlNetworkIntxTool, previewEffect) == 0x40);
static_assert(offsetof(cSC4ViewInputControlNetworkIntxTool, previewNameBegin) == 0x78);
