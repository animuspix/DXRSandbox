#pragma once

#include "DXWrapper.h"
#include <type_traits>
#include <tuple>
#include <stdint.h>
#include <cassert>
#include <typeinfo>
#include "ResourceEnums.h"
#include "RasterSettings.h"

#include "CPUMemory.h"

template<ResourceViews variant>
struct GPUResource
{
	// For these purposes mesh "dimensions" refer to the length of a vertex/index buffer in vertices/indices, not the  volume of its bounding box/rect/line
	static constexpr uint32_t numDimensions = variant == ResourceViews::TEXTURE_DIRECT_WRITE ? 2 :
											  variant == ResourceViews::TEXTURE_SUPPORTS_SAMPLING ? 2 :
											  variant == ResourceViews::TEXTURE_RENDER_TARGET ? 2 :
											  variant == ResourceViews::TEXTURE_DEPTH_STENCIL ? 2 :
											  1;


	// Dynamically selected constructor argument layouts, depending on resource type
	struct resrc_descs_shared
	{
		LPCWSTR resrcName;
	};
	
	struct resrc_desc_custom_fmt : public resrc_descs_shared
	{
		// Regular old zero-initialization constructor
		resrc_desc_custom_fmt()
		{
			stride = 0;
			for (uint32_t i = 0; i < numDimensions; i++)
			{
				dimensions[i] = 0;
			}
			this->resrcName = L"unnamed";
		}
		template<typename bufferType>
		void initForCBuffer(LPCWSTR name, CPUMemory::SingleAllocHandle<bufferType> _srcData)
		{
			stride = sizeof(bufferType);
			dimensions[0] = 1;
			srcData.handle = _srcData.handle;
			srcData.arrayLen = sizeof(bufferType);
			srcData.dataOffset = 0;
			this->resrcName = name;
		}

		template<typename v>
		void initForStructBuffer(uint32_t numElts, LPCWSTR name, CPUMemory::ArrayAllocHandle<v> _srcData)
		{
			stride = sizeof(v);
			dimensions[0] = numElts;
			
			srcData.handle = _srcData.handle;
			srcData.arrayLen = _srcData.arrayLen * sizeof(v);
			srcData.dataOffset = _srcData.dataOffset;
			
			this->resrcName = name;
		}

		void initForStructBuffer(uint32_t numElts, uint32_t eltStride, LPCWSTR name, CPUMemory::ArrayAllocHandle<uint8_t> _srcData)
		{
			stride = eltStride;
			dimensions[0] = numElts;
			srcData = _srcData;
			this->resrcName = name;
		}

		uint32_t stride;
		uint32_t dimensions[numDimensions]; // Resource width/heights in number of elements, not bytes
		CPUMemory::ArrayAllocHandle<uint8_t> srcData = {}; // Imported resource data, either procedural or loaded from disk; nullptr (undefined/unused) by default
	};
	struct resrc_desc_texture_fmt : public resrc_descs_shared
	{
		using fmtType = typename std::conditional<variant == ResourceViews::TEXTURE_DEPTH_STENCIL, StandardDepthStencilFormats, StandardResrcFmts>::type;
		fmtType fmt;
		uint32_t stride; // No reason I can't precompute this here and make it constant for regular resources/textures, its just a lot of code
		uint32_t dimensions[numDimensions]; // Resource width/heights in number of elements, not bytes
		RasterSettings::MSAASettings msaa;
		CPUMemory::ArrayAllocHandle<uint8_t> srcData = {}; // Imported resource data, either procedural or loaded from disk; nullptr (undefined/unused) by default
	};
	struct resrc_desc_vbuffer_fmt : public resrc_descs_shared
	{
		uint32_t stride;
		uint32_t dimensions[numDimensions];
		CPUMemory::ArrayAllocHandle<uint8_t> srcData = {}; // Imported resource data, either procedural or loaded from disk; nullptr (undefined/unused) by default
		StandardResrcFmts eltFmts[XPlatConstants::maxVBufferStride / XPlatConstants::eltSizeInBytes];
		VertexEltSemantics eltSemantics[XPlatConstants::maxVBufferStride / XPlatConstants::eltSizeInBytes];
		uint32_t numEltsPerVert;

		template<typename v>
		void init(StandardResrcFmts _eltFmts[sizeof(v) / XPlatConstants::eltSizeInBytes],
				  VertexEltSemantics _eltSemantics[sizeof(v) / XPlatConstants::eltSizeInBytes],
				  CPUMemory::ArrayAllocHandle<uint8_t> _srcData,
				  uint32_t numVerts,
				  LPCWSTR name) // Semantic formats are expected to increase from the first to the last vertex element (so no POSITION4 then POSITION0 or w/e)
		{
			stride = sizeof(v);
			numEltsPerVert = sizeof(v) / XPlatConstants::eltSizeInBytes;
			memcpy(eltFmts, _eltFmts, numEltsPerVert * sizeof(StandardResrcFmts));
			memcpy(eltSemantics, _eltSemantics, numEltsPerVert * sizeof(VertexEltSemantics));
			srcData = _srcData;
			dimensions[0] = numVerts;
			this->resrcName = name;
		}
	};
	struct resrc_desc_ibuffer_fmt : public resrc_descs_shared
	{
		StandardIBufferFmts fmt;
		uint32_t stride;
		uint32_t dimensions[numDimensions];
		CPUMemory::ArrayAllocHandle<uint8_t> srcData = {}; // Imported resource data, either procedural or loaded from disk; nullptr (undefined/unused) by default
	};
	struct resrc_desc_accelStruct_fmt : public resrc_descs_shared
	{
		DXWrapper::DataHandle<D3D_VBUFFER> src_vbuf;
		DXWrapper::DataHandle<D3D_IBUFFER>* src_ibuf;
		XPlatUtils::AccelStructConfig config;
	};

	// The type of description associated with a specific resource
	static constexpr bool typedBuffer = variant == ResourceViews::CBUFFER || variant == ResourceViews::STRUCTBUFFER_RW;
	static constexpr bool texture = variant == ResourceViews::TEXTURE_SUPPORTS_SAMPLING || variant == ResourceViews::TEXTURE_DIRECT_WRITE ||
									variant == ResourceViews::TEXTURE_STAGING || variant == ResourceViews::TEXTURE_RENDER_TARGET || variant == ResourceViews::TEXTURE_DEPTH_STENCIL;
	static constexpr bool stdTexture = texture && !(variant == ResourceViews::TEXTURE_DEPTH_STENCIL);
	static constexpr bool depthStencil = variant == ResourceViews::TEXTURE_DEPTH_STENCIL;
	static constexpr bool ibuffer = variant == ResourceViews::IBUFFER;
	static constexpr bool vbuffer = variant == ResourceViews::VBUFFER;

	using vbufferSelector = typename std::conditional<vbuffer, resrc_desc_vbuffer_fmt, resrc_desc_accelStruct_fmt>::type;
	using ibufferSelector = typename std::conditional<ibuffer, resrc_desc_ibuffer_fmt, vbufferSelector>::type;
	using textureSelector = typename std::conditional<texture, resrc_desc_texture_fmt, ibufferSelector>::type;
	using resrc_desc = typename std::conditional<typedBuffer, resrc_desc_custom_fmt, textureSelector>::type;

	using AccessPermissions = typename std::conditional<texture, GPUResrcPermSetTextures, GPUResrcPermSetGeneric>::type;
	private:
		// Expecting to expand parameter structs here once I've generated each interface in DXWrapper
		/////////////////////////////////////////////////////////////////////////////////////////////

		struct TypedBufferGenerator
		{
			void operator()(resrc_desc_custom_fmt desc, DXWrapper::DataHandle<variant == ResourceViews::CBUFFER ? D3D_CBUFFER : D3D_STRUCTBUFFER> *buffer_out, AccessPermissions accessSettings, uint32_t pipelineID)
			{
				if constexpr (variant == ResourceViews::CBUFFER)
				{
					*buffer_out = DXWrapper::GenerateConstantBuffer(desc.stride, accessSettings, desc.srcData, pipelineID);
				}
				else if constexpr (variant == ResourceViews::STRUCTBUFFER_RW)
				{
					*buffer_out = DXWrapper::GenerateStructuredBuffer(desc.stride * desc.dimensions[0], desc.stride, desc.dimensions[0], accessSettings, desc.srcData, pipelineID);
				}

				DXWrapper::NameResource(*buffer_out, desc.resrcName);
			}
		};

		struct StdTextureGenerator
		{
			void operator()(resrc_desc_texture_fmt desc, DXWrapper::DataHandle<D3D_TEXTURE>* texture_out, AccessPermissions accessSettings, uint32_t pipelineID)
			{
				// Should maybe use a switch here
				if (variant == ResourceViews::TEXTURE_DIRECT_WRITE)
				{ *texture_out = DXWrapper::GenerateStandardTexture(desc.dimensions[0], desc.dimensions[1], desc.fmt, desc.msaa, accessSettings, TextureViews::DIRECT_WRITE, desc.srcData, pipelineID); }

				else if (variant == ResourceViews::TEXTURE_SUPPORTS_SAMPLING)
				{ *texture_out = DXWrapper::GenerateStandardTexture(desc.dimensions[0], desc.dimensions[1], desc.fmt, desc.msaa, accessSettings, TextureViews::SUPPORTS_SAMPLING, desc.srcData, pipelineID); }

				else if (variant == ResourceViews::TEXTURE_STAGING)
				{ *texture_out = DXWrapper::GenerateStandardTexture(desc.dimensions[0], desc.dimensions[1], desc.fmt, desc.msaa, accessSettings, TextureViews::STAGING, desc.srcData, pipelineID); }

				else if (variant == ResourceViews::TEXTURE_RENDER_TARGET)
				{ *texture_out = DXWrapper::GenerateStandardTexture(desc.dimensions[0], desc.dimensions[1], desc.fmt, desc.msaa, accessSettings, TextureViews::RENDER_TARGET, desc.srcData, pipelineID); }

				else if (variant == ResourceViews::TEXTURE_DEPTH_STENCIL)
				{ *texture_out = DXWrapper::GenerateStandardTexture(desc.dimensions[0], desc.dimensions[1], desc.fmt, desc.msaa, accessSettings, TextureViews::DEPTH_STENCIL, desc.srcData, pipelineID); }

				else
				{
					assert(false); // Either the submitted variant isn't a recognized TextureVariant, or you somehow called this generator without a texture resource ^_^'
				}

				DXWrapper::NameResource(*texture_out, desc.resrcName);
			}
		};

		struct DepthTextureGenerator
		{
			void operator()(resrc_desc_texture_fmt desc, DXWrapper::DataHandle<D3D_TEXTURE>* texture_out, AccessPermissions accessSettings, uint32_t pipelineID)
			{
				*texture_out = DXWrapper::GenerateDepthStencilTexture(desc.dimensions[0], desc.dimensions[1], desc.fmt, desc.msaa, accessSettings, desc.srcData, pipelineID);
				DXWrapper::NameResource(*texture_out, desc.resrcName);
			}
		};

		struct IBufferGenerator
		{
			void operator()(resrc_desc_ibuffer_fmt desc, DXWrapper::DataHandle<D3D_IBUFFER>* ibuffer_out, AccessPermissions accessSettings, uint32_t pipelineID)
			{
				*ibuffer_out = DXWrapper::GenerateIndexBuffer(desc.stride * desc.dimensions[0], desc.fmt, accessSettings, desc.srcData, pipelineID);
				DXWrapper::NameResource(*ibuffer_out, desc.resrcName);
			}
		};

		struct VBufferGenerator
		{
			void operator()(resrc_desc_vbuffer_fmt desc, DXWrapper::DataHandle<D3D_VBUFFER>* vbuffer_out, AccessPermissions accessSettings, uint32_t pipelineID)
			{
				*vbuffer_out = DXWrapper::GenerateVertexBuffer(desc.stride * desc.dimensions[0], desc.stride, desc.numEltsPerVert, desc.eltFmts, accessSettings, desc.srcData, pipelineID);
				DXWrapper::NameResource(*vbuffer_out, desc.resrcName);
			}
		};

		struct AccelStructGenerator
		{
			void operator()(resrc_desc_accelStruct_fmt desc, std::pair<DXWrapper::DataHandle<D3D_ACCELSTRUCT_BLAS>, DXWrapper::DataHandle<D3D_ACCELSTRUCT_TLAS>>* accelStruct_out, AccessPermissions accessSettings, uint32_t pipelineID)
			{
				DXWrapper::GenerateAccelStructForGeometry(desc.src_vbuf, desc.src_ibuf, &accelStruct_out->first, &accelStruct_out->second, accessSettings, desc.config, pipelineID);

				// Resolve specific tlas/blas names; assume our resource names are much shorter than 64 chars, hopefully they are...^_^'
				wchar_t blasName[64] = {};
				wchar_t tlasName[64] = {}; 
				wsprintf(blasName, L"%s_blas", desc.resrcName);
				wsprintf(tlasName, L"%s_tlas", desc.resrcName);

				// Name blas/tlas resources
				DXWrapper::NameResource(accelStruct_out->first, blasName);
				DXWrapper::NameResource(accelStruct_out->second, tlasName);
			}
		};

		using vbufferGen = typename std::conditional<vbuffer, VBufferGenerator, AccelStructGenerator>::type;
		using ibufferGen = typename std::conditional<ibuffer, IBufferGenerator, vbufferGen>::type;
		using stdTextureGen = typename std::conditional<stdTexture, StdTextureGenerator, ibufferGen>::type;
		using depthTextureGen = typename std::conditional<depthStencil, DepthTextureGenerator, stdTextureGen>::type;
		using resrc_gen = typename std::conditional<typedBuffer, TypedBufferGenerator, depthTextureGen>::type;

		static constexpr D3D_OBJ_FMT d3dClassicResrcFinder()
		{
			return variant == ResourceViews::VBUFFER ? D3D_VBUFFER :
				   variant == ResourceViews::IBUFFER ? D3D_IBUFFER :
				   variant == ResourceViews::STRUCTBUFFER_RW ? D3D_STRUCTBUFFER :
				   variant == ResourceViews::CBUFFER ? D3D_CBUFFER :
				   D3D_TEXTURE; // All remaining variants are non-classic resources (AS), or different types of texture
		}

		struct StandardResrcTransitionGenerator
		{
			void operator()(ResourceViews beforeVariant, ResourceViews afterVariant, DXWrapper::DataHandle<d3dClassicResrcFinder()> resrc, uint8_t pipelineID)
			{
				DXWrapper::InsertTransition(beforeVariant, afterVariant, resrc, pipelineID);
			}
		};

		struct AccelStructTransitionGenerator
		{
			void operator()(ResourceViews beforeVariant, ResourceViews afterVariant, std::pair<DXWrapper::DataHandle<D3D_ACCELSTRUCT_BLAS>, DXWrapper::DataHandle<D3D_ACCELSTRUCT_TLAS>>* resrc, uint8_t pipelineID)
			{
				// Transitions for acceleration structures are unsupported - hard to implement conceptually (which sub-AS should transition?), and overall challenging since GPU ASes use black-box formats that we'd have to 
				// reverse-engineer for incoming transitions. Outgoing transitions are technically possible, but awkward since ASes contain two resource handles (TLAS, BLAS) and all other resources contain just one
				assert(("Transitions to acceleration structures are unsupported", false));
			}
		};

		using transition_gen = typename std::conditional<variant != ResourceViews::RT_ACCEL_STRUCTURE, StandardResrcTransitionGenerator, AccelStructTransitionGenerator>::type;

		struct TransitionValidator
		{
			static void validate(ResourceViews beforeVariant, ResourceViews currentVariant, GPUResrcPermSetTextures resrcPermissions)
			{
				if constexpr (texture && variant != ResourceViews::TEXTURE_STAGING)
				{
					if (currentVariant == ResourceViews::TEXTURE_DIRECT_WRITE)
					{
						assert(("Direct-write/UAV resource requested without write permissions", resrcPermissions & GPU_RESRC_ACCESS_PERMISSIONS_TEXTURES::TEXTURE_ACCESS_DIRECT_WRITES));
					}
					else if (currentVariant == ResourceViews::TEXTURE_SUPPORTS_SAMPLING)
					{
						assert(("Sampled resource rquested without read permissions", resrcPermissions & GPU_RESRC_ACCESS_PERMISSIONS_TEXTURES::TEXTURE_ACCESS_DIRECT_READS));
					}
					else if (currentVariant == ResourceViews::TEXTURE_RENDER_TARGET)
					{
						assert(("Render-target resource requested without render-target permissions", resrcPermissions == GPU_RESRC_ACCESS_PERMISSIONS_TEXTURES::TEXTURE_ACCESS_AS_RENDER_TARGET));
					}
					else if (variant == ResourceViews::TEXTURE_DEPTH_STENCIL)
					{
						assert(("Depth-stencil resource requested without depth-stencil permissions", resrcPermissions == GPU_RESRC_ACCESS_PERMISSIONS_TEXTURES::TEXTURE_ACCESS_AS_DEPTH_STENCIL));
					}
				}
			}
			static void validate(ResourceViews beforeVariant, ResourceViews currentVariant, GPUResrcPermSetGeneric resrcPermissions)
			{
				// No valid transitions for non-texture resources
				assert(false);
			}
		};

	public:

	// Small default constructor to allow for arrays of uninitialized resources; just zeroes everything and returns
	GPUResource<variant>() { memset(this, 0, sizeof(GPUResource<variant>)); }

	// Interfaces
	void InitFromScratch(resrc_desc _desc, AccessPermissions accessSettings, uint32_t pipelineID)
	{
		// Realtime assertion hacks because no metaclasses :(
		if (variant == ResourceViews::TEXTURE_STAGING)
		{
			assert(accessSettings == TEXTURE_ACCESS_COPIES_ONLY); // Staging textures can only be used for copies to & from the GPU
		}

		desc = _desc;
		resrc_gen()(_desc, &resrc, accessSettings, pipelineID);
		gpuAccessSettings = accessSettings;
	}

	template<ResourceViews srcVariant>
	void InitFromSharedResrc(GPUResource<srcVariant>* _src, uint8_t pipelineID) // Hopefully won't get circular references since I use a pointer here ^_^'
	{
		// Many resource variants can only be shared across pipelines under the same variant (i.e. straightforward buffer/texture reuse), usually ones with specialized
		// layouts that can't easily be reused, or can only be reused with resource types that we don't support (e.g. index buffers <-> 1D textures). Texture staging layouts are trivial
		// to reuse, but the resources themselves are allocated on system memory and specifically can't be accessed by the GPU (except through copies), so transitioning a staging resource
		// to/from any other variant is impossible
		// Vertex buffers and structured buffers can technically be transitioned easily, but vertex buffers come with a lot of implied state (input layout dependency, vertex element formats, vertex element counts...) that are difficult to supply for structbuffers
		// and would make the RHI much more complicated if vbuffer/structbuffer transitions were supported
		// Transitions for these resources can still be simulated with copies, e.g. vertex data can be copied into a structured buffer to allow alternating between hardware and software rasterization
		if (variant == ResourceViews::IBUFFER || variant == ResourceViews::RT_ACCEL_STRUCTURE || variant == ResourceViews::TEXTURE_STAGING || variant == ResourceViews::CBUFFER || variant == ResourceViews::VBUFFER || variant == ResourceViews::STRUCTBUFFER_RW)
		{
			assert(srcVariant == variant);
		}

		// Everything will be defined already, so initialization is fast & easy
		memcpy(this, _src, sizeof(GPUResource<srcVariant>)); // Incredibly sketchy, but effectively subverts access restrictions ^_^'

		// Insert a (validated) transition from the previous state/variant to the current state/variant on the API's background command-list
		// (only if not transitioning from an accel structure + not reusing a resource with the same variant)
		if constexpr (variant != ResourceViews::RT_ACCEL_STRUCTURE && (variant != srcVariant))
		{
			TransitionValidator::validate(srcVariant, variant, _src->GetGPUAccessSettings());
			transition_gen()(srcVariant, variant, resrc, pipelineID);
		}
	}

	void UpdateData(CPUMemory::ArrayAllocHandle<uint8_t> data)
	{
		DXWrapper::UpdateResrcData(resrc, data);
	}

	// Resource properties
	// We don't really want to modify these after setup
	///////////////////////////////////////////////////

	const resrc_desc GetDesc() const { return desc; }
	const auto GetResrcHandle() { return resrc; }
	const AccessPermissions GetGPUAccessSettings() const { return gpuAccessSettings; }

	private:
		resrc_desc desc; // Resource summary (element type, stride, width/height, etc)
		using resrc_ty = typename std::conditional<variant != ResourceViews::RT_ACCEL_STRUCTURE,
												   DXWrapper::DataHandle<d3dClassicResrcFinder()>,
												   std::pair<DXWrapper::DataHandle<D3D_ACCELSTRUCT_BLAS>, DXWrapper::DataHandle<D3D_ACCELSTRUCT_TLAS>>>::type;

		resrc_ty resrc; // A handle to the actual API resource each "GPUResource" represents
						// Conditional std::pair allows us to sneakily use two resources behind-the-scenes here
		AccessPermissions gpuAccessSettings;
};

namespace XPlatUtils
{
	struct BakedGeoBuffers
	{
		GPUResource<ResourceViews::VBUFFER>::resrc_desc_vbuffer_fmt vbufferDesc;
		GPUResource<ResourceViews::IBUFFER>::resrc_desc_ibuffer_fmt ibufferDesc;
	};
}
