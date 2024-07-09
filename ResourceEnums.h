#pragma once

#include <assert.h>
#include <type_traits>

enum class ResourceViews
{
	VBUFFER,
	IBUFFER,
	STRUCTBUFFER_RW, // No non-RW structbuffer, because D3D12 requires all structbuffers to be bound through read-writeable UAVs
	CBUFFER,

	// No unstructured buffer direct-write, because an unstructured buffer is just a 1D or 2D Nx1 texture without sampling support - hard to justify extra code for not much utility

	TEXTURE_DIRECT_WRITE,
	TEXTURE_SUPPORTS_SAMPLING,
	TEXTURE_STAGING,
	TEXTURE_RENDER_TARGET,
	TEXTURE_DEPTH_STENCIL,
	RT_ACCEL_STRUCTURE,
	NUM_VARIANTS
};

enum class TextureViews
{
	DIRECT_WRITE,
	SUPPORTS_SAMPLING,
	STAGING,
	RENDER_TARGET,
	DEPTH_STENCIL,
};

// Access permissions for different resources
// Not completely deduceable, because often we want to bind a texture resource as a render-target in one stage, and as a sampled resource (SRV) in another
enum GPU_RESRC_ACCESS_PERMISSIONS_GENERIC
{
	GENERIC_RESRC_ACCESS_DIRECT_READS = 1<<0,
	GENERIC_RESRC_ACCESS_DIRECT_WRITES = 1<<1
};

enum GPU_RESRC_ACCESS_PERMISSIONS_TEXTURES
{
	TEXTURE_ACCESS_DIRECT_READS = 1<<0,
	TEXTURE_ACCESS_DIRECT_WRITES = 1<<1,
	TEXTURE_ACCESS_AS_RENDER_TARGET = 1<<2,
	TEXTURE_ACCESS_AS_DEPTH_STENCIL = 1<<3,
	TEXTURE_ACCESS_COPIES_ONLY = 1<<4 // Required for TEXTURE_STAGING, not combinable with other flags
};

template<typename internalEnumType> requires(std::is_same<internalEnumType, GPU_RESRC_ACCESS_PERMISSIONS_GENERIC>::value ||
											 std::is_same<internalEnumType, GPU_RESRC_ACCESS_PERMISSIONS_TEXTURES>::value)
struct GPU_RESRC_ACCESS_PERMISSION_SET
{
	GPU_RESRC_ACCESS_PERMISSION_SET() : bitset(0) {}
	GPU_RESRC_ACCESS_PERMISSION_SET(const internalEnumType val) : bitset(static_cast<uint32_t>(val)) {};
	GPU_RESRC_ACCESS_PERMISSION_SET(const uint32_t val) : bitset(val)
	{
		constexpr bool texPerms = std::is_same<internalEnumType, GPU_RESRC_ACCESS_PERMISSIONS_TEXTURES>::value;
		constexpr bool genericPerms = std::is_same<internalEnumType, GPU_RESRC_ACCESS_PERMISSIONS_GENERIC>::value;
		if (texPerms)
		{
			assert(val < (TEXTURE_ACCESS_COPIES_ONLY << 1));
		}
		else if (genericPerms)
		{
			assert(val <= (GENERIC_RESRC_ACCESS_DIRECT_WRITES << 1));
		}
	};

	const bool operator==(const GPU_RESRC_ACCESS_PERMISSIONS_GENERIC v)
	{
		if (!std::is_same<internalEnumType, GPU_RESRC_ACCESS_PERMISSIONS_GENERIC>::value) return false;
		else
		{
			return bitset == static_cast<uint32_t>(v);
		}
	}

	const bool operator==(const GPU_RESRC_ACCESS_PERMISSIONS_TEXTURES v)
	{
		if (!std::is_same<internalEnumType, GPU_RESRC_ACCESS_PERMISSIONS_TEXTURES>::value) return false;
		else
		{
			return bitset == static_cast<uint32_t>(v);
		}
	}

	const bool operator&(const internalEnumType v)
	{
		return bitset & static_cast<uint32_t>(v);
	}

	// Can't see value in overloading for permission set RHS, yet

	const void operator|=(const internalEnumType v)
	{
		bitset |= static_cast<uint32_t>(v);
	}

	const void operator|=(const GPU_RESRC_ACCESS_PERMISSION_SET<internalEnumType> v)
	{
		bitset |= v.GetBitSet();
	}

	const GPU_RESRC_ACCESS_PERMISSION_SET<internalEnumType> operator|(GPU_RESRC_ACCESS_PERMISSION_SET<internalEnumType> v)
	{
		GPU_RESRC_ACCESS_PERMISSION_SET<internalEnumType> a(bitset);
		a |= v;
		return a;
	}

	const GPU_RESRC_ACCESS_PERMISSION_SET<internalEnumType> operator|(internalEnumType v)
	{
		GPU_RESRC_ACCESS_PERMISSION_SET<internalEnumType> a(bitset);
		a |= v;
		return a;
	}

	const uint32_t GetBitSet() const
	{
		return bitset;
	}

	private:
	uint32_t bitset = 0;
};

using GPUResrcPermSetGeneric = GPU_RESRC_ACCESS_PERMISSION_SET<GPU_RESRC_ACCESS_PERMISSIONS_GENERIC>;
using GPUResrcPermSetTextures = GPU_RESRC_ACCESS_PERMISSION_SET<GPU_RESRC_ACCESS_PERMISSIONS_TEXTURES>;

enum class StandardResrcFmts
{
	// 32bpc floating-point formats
	FP32_1,
	FP32_2,
	FP32_3,
	FP32_4,

	// 16bpc floating-point formats
	FP16_1,
	FP16_2,
	//FP16_3, // Unsupported in DX12 API code
	FP16_4,

	// 32bpc unsigned integer formats
	U32_1,
	U32_2,
	U32_3,
	U32_4,

	// 16bpc unsigned integer formats
	U16_1,
	U16_2,
	//U16_3, // Unsupported in DX12 API code
	U16_4,

	// 8bpc unsigned integer formats
	U8_1,
	U8_2,
	U8_3,
	U8_4,

	// 32bpc signed integer formats
	S32_1,
	S32_2,
	S32_3,
	S32_4,

	// 16bpc signed integer formats
	S16_1,
	S16_2,
	S16_3,
	S16_4,

	// 8bpc signed integer formats
	S8_1,
	S8_2,
	S8_3,
	S8_4,
};

// Just four supported formats for index buffers at the moment; might add more later
enum class StandardIBufferFmts
{
	U16,
	S16,
	U32,
	S32
};

// Depth/stencil texture formats supported by D3D12
// Depth/stencil textures are somewhat of a fixed-function hangover; they can be be bound as shader resources (after transitioning) and copied to/from other depth/stencil textures,
// but they don't inter-operate with any other resources and can't be bound for output through UAVs or render-targets
// I'm keeping them at the pipeline level instead of generating them just-in-time at the API level to enable binding them as a raster output in one pass and reading them in another, since there
// wouldn't be a clean way to do that if they weren't exposed to the user-facing Frame/Pipeline structures
enum StandardDepthStencilFormats
{
	DEPTH_16_UNORM_NO_STENCIL,
	DEPTH_24_UNORM_STENCIL_8,
	DEPTH_32_FLOAT_NO_STENCIL,
	DEPTH_32_FLOAT_STENCIL_8_PAD_24
};

// Vertex-buffer layout helper; valid labels for vertex elements, modelled on standard DirectX input semantics
// For convention & ease of programming all semantics are expected to be Float4
enum class VertexEltSemantics
{
	POSITION, // Float4 xyz pos, w can be anything
	COLOR, // Float4, either color or material properties by convention (can be anything)
	NORMAL, // Float4, xyz elements are either face or interpolated normals by convention (can be anything)
	TEXCOORD, // Float4, pairs of UV and atlas coordinates by convention (or extra material data), can be anything
	NUM_SUPPORTED_SEMANTICS
};
