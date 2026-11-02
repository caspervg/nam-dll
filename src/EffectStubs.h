#pragma once
#include <cstddef>
#include <cstdint>
#include "NetworkStubs.h"
#include "cIS3DModelInstance.h"
#include "cISC4VisualEffect.h"

class SC4DrawAttributes
{
	public:
		enum Flags : uint8_t {
			kApplyAlpha = 0x02,
			kSelfLit = 0x04,
			kDepthGreaterNoWrite = 0x20,  // depth test GREATER, no depth writes: only draws occluded parts
		};

		uint8_t flags;
		uint8_t RESERVED[3];
		float color[3];
		float alpha;
};
static_assert(offsetof(SC4DrawAttributes, alpha) == 0x10);
static_assert(sizeof(SC4DrawAttributes) == 0x14);

// The gzcom-dll declaration of cIS3DModelInstance::GetDrawAttributes returns a pointer without arguments,
// but the game returns the struct by value through a hidden out-pointer (callee pops 4 bytes).
typedef SC4DrawAttributes* (__thiscall* pfn_GetDrawAttributes)(cIS3DModelInstance* pThis, SC4DrawAttributes* pOut);
constexpr uint32_t kModelInstanceGetDrawAttributesSlot = 0x3c / sizeof(void*);

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
		static_assert(offsetof(cComponentRec, component) == 0x38);
		static_assert(sizeof(cComponentRec) == 0x44);

		uint8_t RESERVED[0x114];
		cComponentRec* componentsBegin;
		cComponentRec* componentsEnd;
};
static_assert(offsetof(cSC4VisualEffect, componentsBegin) == 0x114);
static_assert(offsetof(cSC4VisualEffect, componentsEnd) == 0x118);

class cSC4EffectsParticle
{
	public:
		uint8_t RESERVED[0x34];
		float alphaFactor;  // multiplied with the alpha curve when rendering
		float colorFactor[3];  // multiplied with the color curve when rendering
};
static_assert(offsetof(cSC4EffectsParticle, alphaFactor) == 0x34);
static_assert(offsetof(cSC4EffectsParticle, colorFactor) == 0x38);

class cSC4ParticleListNode
{
	public:
		cSC4ParticleListNode* next;
		uint8_t RESERVED[0x4];
		cSC4EffectsParticle* particle;
};
static_assert(offsetof(cSC4ParticleListNode, particle) == 0x8);

class SC4DrawContext
{
	public:
		typedef void (__thiscall* pfn_SetDepthOffset)(SC4DrawContext* pThis, int32_t offset);
		static inline pfn_SetDepthOffset SetDepthOffset = reinterpret_cast<pfn_SetDepthOffset>(0x7d4480);

		uint8_t RESERVED[0xbc];
		int32_t depthOffset;  // positive values move geometry towards the camera (e.g. 2 for decals)
};
static_assert(offsetof(SC4DrawContext, depthOffset) == 0xbc);

class cSC4ModelBasedParticlesEffect
{
	public:
		static constexpr uint32_t kVTable = 0xaa0c58;
		static constexpr uint32_t kRenderActiveParticlesSlot = 0x44;
		static constexpr uint32_t kRenderActiveParticles = 0x5add50;
		typedef void (__thiscall* pfn_RenderActiveParticles)(cSC4ModelBasedParticlesEffect* pThis, SC4DrawContext* drawContext);

		void* vtable;
		uint8_t RESERVED[0x10];
		cSC4ParticleListNode* particles;  // sentinel node of a circular list
		uint8_t RESERVED[0x208];
		cIS3DModelInstance* modelInstance;
};
static_assert(offsetof(cSC4ModelBasedParticlesEffect, particles) == 0x14);
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
static_assert(offsetof(cSC4ViewInputControlNetworkIntxTool, previewNameEnd) == 0x7c);
