#include "Render.h"
#include "..\GPUResource.h"
#include "..\Shaders\materials.h"
#include "..\Math.h"
#include "Materials.h"
#include "..\CPUMemory.h"

#include "..\Shaders\SharedConstants.h"
#include "..\Shaders\SharedPRNG_Code.h"

#include "RenderDebug.h"

#include <cassert>
#include <algorithm>
#include <chrono>
#include <thread>
#include <random>
#include <stdio.h>

// Namespaced cbuffers for each rendering mode (compute, hybrid, shader-table)
//////////////////////////////////////////////////////////////////////////////

namespace ComputeTypes
{
	struct ComputeConstants
	{
		GenericRenderConstants screenAndLensOptions;
	};
}

namespace HybridTypes
{
	struct HybridConstants
	{
		GenericRenderConstants screenAndLensOptions;
	};
}

namespace ShaderTableTypes
{
	struct ShaderTableConstants
	{
		GenericRenderConstants screenAndLensOptions;
	};
}

// Compute/hybrid/shader-table constants
PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> computeCBufHandle;
CPUMemory::SingleAllocHandle<ComputeTypes::ComputeConstants> computeConstants;

PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> hybridCBufHandle;
CPUMemory::SingleAllocHandle<HybridTypes::HybridConstants> hybridConstants;

PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> shaderTableCBufHandle;
CPUMemory::SingleAllocHandle<ShaderTableTypes::ShaderTableConstants> shaderTableComputeConstants;


// Generate PRNG seeds using the SplitMix64 generator recommended in the Xoshiro128+ implementation above
// Cycle the generator twice and stride the output across uints (xoshiro128+ has four uint32s of state)
// https://www.boost.org/doc/libs/1_82_0/boost/core/detail/splitmix64.hpp
GPU_PRNG_Channel GetGPU_PRNG_Seed(uint64_t currentTime)
{
	uint64_t seed = currentTime;
	GPU_PRNG_Channel channel = {};
	constexpr uint8_t numSeedCycles = 2;
	for (uint32_t i = 0; i < numSeedCycles; i += 2) // Generator implemented from Boost, see https://www.boost.org/doc/libs/1_82_0/boost/core/detail/splitmix64.hpp
	{
		seed += (0x9e3779b9ull << 32) + 0x7f4a7c15u;

		uint64_t z = seed;

		z ^= z >> 30;
		z *= (0xbf58476dull << 32) + 0x1ce4e5b9u;
		z ^= z >> 27;
		z *= (0x94d049bbull << 32) + 0x133111ebu;
		z ^= z >> 31;

		channel.state[i] = seed & 0xffffffff; // Lower bits in first state bits of each pair
		channel.state[i + 1] = static_cast<uint32_t>(seed & (0xffffffffull << 32)); // Upper bits in second state bits
	};

	return channel;
}

// Jump function from xoshiro128+ implementation, here: https://prng.di.unimi.it/xoshiro512plus.c
void GPU_PRNG_SeedAndJump(GPU_PRNG_Channel* channel, const GPU_PRNG_Channel seed, uint32_t numJumps)
{
	*channel = seed;
	GPU_PRNG_Channel u = *channel;
	for (uint32_t i = 0; i < numJumps; i++)
	{
		static const uint64_t JUMP[GPU_PRNG_STREAM_STATE_SIZE] = { 0x8764000b, 0xf542d2d3, 0x6fa035c3, 0x77f2db5b };

		GPU_PRNG_Channel t = {};
		for (int i = 0; i < GPU_PRNG_STREAM_STATE_SIZE; i++)
		{
			for (int b = 0; b < 32; b++)
			{
				if (JUMP[i] & UINT64_C(1) << b)
				{
					t.state[0] ^= u.state[0];
					t.state[1] ^= u.state[1];
					t.state[2] ^= u.state[2];
					t.state[3] ^= u.state[3];
				}

				GPU_PRNG_Next(u);
			}
		}

		u = t;
	}
	*channel = u;
}

void PRNGThreadInterface(uint32_t screenWidth, CPUMemory::ArrayAllocHandle<GPU_PRNG_Channel> prngState, uint32_t screenRowNdx, uint32_t screenHeight)
{
	for (uint32_t j = 0; j < screenWidth; j++)
	{
		std::ranlux48 rngSeeder;

		const uint64_t currTime = std::chrono::steady_clock::now().time_since_epoch().count();
		rngSeeder.seed(currTime);

		const GPU_PRNG_Channel prngSeed = GetGPU_PRNG_Seed(rngSeeder());
		GPU_PRNG_SeedAndJump(&prngState[j + screenRowNdx * screenHeight], prngSeed, 16);
	}
}

void UpdateComputeConstants(CPUMemory::SingleAllocHandle<Render::FrameConstants> frameConstants) // Excludes material atlas dimensions (only changed on setup/init)
{
	computeConstants->screenAndLensOptions.screenAndTime = float4(frameConstants->screenWidth, frameConstants->screenHeight, frameConstants->timeSeconds, 0.0f);
	computeConstants->screenAndLensOptions.lensSettings = float4(frameConstants->fov, frameConstants->focalDepth, frameConstants->aberration, frameConstants->spp);
	memcpy(&computeConstants->screenAndLensOptions.filmSPD, &frameConstants->filmSPD, sizeof(FilmSPD_Piecewise));

	computeConstants->screenAndLensOptions.sceneBoundsMin = frameConstants->sceneBoundsMin;
	computeConstants->screenAndLensOptions.sceneBoundsMax = frameConstants->sceneBoundsMax;
	computeConstants->screenAndLensOptions.cameraTransform = frameConstants->cameraTransform;
	memcpy(computeConstants->screenAndLensOptions.sceneTransforms, frameConstants->sceneTransforms, sizeof(transform) * frameConstants->numTransforms);
}

// Sampling atlassed data
// Spectral data (in structbuffer) - straightforward, load constrained quad containing data & interpolate manually
// Roughness data (in texture) - sample as normal (so we get GPU interpolation), but constrain UVs to just inside each atlas entry to prevent bleeding (so (width-1, height-1))
// Roughness processing might be somewhat easier with manual texel loading & blending, unsure

void Render::Init(HWND hwnd, RENDER_MODE mode, XPlatUtils::BakedGeoBuffers& sceneGeo, XPlatUtils::BakedGeoBuffers& viewGeo, CPUMemory::ArrayAllocHandle<Material> sceneMaterials, uint32_t sceneMaterialCount, CPUMemory::SingleAllocHandle<FrameConstants> frameConstants)
{
	// Store the active render mode
	currMode = mode;

	// Initialize render debug support
	RenderDebug::Init();

	// Initialize the active API wrapper
	// To be replaced with an intermediate RHI, eventually
	const uint32_t screenWidth = static_cast<uint32_t>(frameConstants->screenWidth), screenHeight = static_cast<uint32_t>(frameConstants->screenHeight);
#ifdef DX12
	DXWrapper::Init(hwnd, screenWidth, screenHeight, true);
#else
	VKWrapper::Init(hwnd, screenWidth, screenHeight, true);
#endif

	// Compute
	//////////

	computeConstants = CPUMemory::AllocateSingle<ComputeTypes::ComputeConstants>();

	// Transforms, when I get around to them -> cbuffer
	//
	// Geometry vbuffer/ibuffer are always present
	// Compute raytracing -> vbuffer/ibuffer & structbuffers (separately initialized), structbuffers initialize an AS (...probably just a grid...) that we use for software intersection testing
	// Hybrid raytracing -> vbuffer/ibuffer used to construct hardware AS, we raster the originals to make a primary-ray mask, then we trace bounce rays out of the mask against the hardware AS
	// Hardware raytracing -> vbuffer/ibuffer used to construct hardware AS; we path-trace the hardware AS for all bounces

	// First compute stage (spatial hashing)
	//////////////////////////////////////

	// Initialize pipeline
	compute_frame.pipes[COMPUTE_STAGES::SPATIAL_HASHING].init(false);

	// Resource registration
	GPUResource<ResourceViews::CBUFFER>::resrc_desc computeCBufDesc;

	// Constants!

	// Generic
	UpdateComputeConstants(frameConstants);

	// Materials
	// Current packing algorithm sits on the x-axis
	// Should drive runtime atlas dimensions from calculations here
	computeConstants->screenAndLensOptions.materialAtlasDims = float4(0.0f, sceneMaterials[0].spectralTexY, 0.0f, sceneMaterials[0].roughnessTexY);
	for (uint32_t i = 0; i < sceneMaterialCount; i++)
	{
		computeConstants->screenAndLensOptions.materialAtlasDims.x += sceneMaterials[0].spectralTexX;
		computeConstants->screenAndLensOptions.materialAtlasDims.y = std::max(computeConstants->screenAndLensOptions.materialAtlasDims.y, static_cast<float>(sceneMaterials[i].spectralTexY));

		computeConstants->screenAndLensOptions.materialAtlasDims.z += sceneMaterials[0].roughnessTexX;
		computeConstants->screenAndLensOptions.materialAtlasDims.w = std::max(computeConstants->screenAndLensOptions.materialAtlasDims.w, static_cast<float>(sceneMaterials[i].roughnessTexY));
	}

	computeCBufDesc.initForCBuffer<ComputeTypes::ComputeConstants>(L"computeConstants", computeConstants);
	computeCBufHandle = compute_frame.pipes[COMPUTE_STAGES::SPATIAL_HASHING].RegisterCBuffer(computeCBufDesc, GENERIC_RESRC_ACCESS_DIRECT_READS);

	// Vbuffer/Ibuffer
	GPUResource<ResourceViews::STRUCTBUFFER_RW>::resrc_desc structuredVbufferDesc;
	structuredVbufferDesc.dimensions[0] = sceneGeo.vbufferDesc.dimensions[0];
	structuredVbufferDesc.initForStructBuffer(sceneGeo.vbufferDesc.dimensions[0], sceneGeo.vbufferDesc.stride, L"structuredVbuffer", sceneGeo.vbufferDesc.srcData);

	constexpr int resrcRW_Permissions = GENERIC_RESRC_ACCESS_DIRECT_READS | GENERIC_RESRC_ACCESS_DIRECT_WRITES;
	auto structuredVbuffer = compute_frame.pipes[COMPUTE_STAGES::SPATIAL_HASHING].RegisterStructBuffer(structuredVbufferDesc, resrcRW_Permissions);

	const uint32_t numTris = sceneGeo.ibufferDesc.dimensions[0] / 3;
	GPUResource<ResourceViews::STRUCTBUFFER_RW>::resrc_desc structuredTribufferDesc;

	// Sorting z-order indices problematic on GPU; doable, but messy, and likely to involve RHI work
	// Also problematic on CPU; messy RHI work needed for the repeated gpu updates, and overall not
	// ideal when we're aiming for a GPU-driven architecture (like many modern rendering engines)

	// Alternative; renormalization
	// The triangle order is what matters, not the numbers! So we can find the most distant scene pos
	// (= largest possible z-value), divide smaller z-values against it, then scale back out using
	// the nunber of triangles

	// Might need to put the "most distant" scene pos in our constant buffer, but hopefully that isn't
	// too high-overhead

	// Not needed after all; we can compute & stash the max z-value entirely on GPU using some barrier/InterlockedX trickery

	CPUMemory::MemSize sourceNdxFootprint = 0;
	uint32_t* sourceNdces = static_cast<uint32_t*>(sceneGeo.ibufferDesc.srcData.Bytes(sourceNdxFootprint));
	auto tribufferMemory = CPUMemory::AllocateArray<IndexedTriangle>(numTris);
	for (uint32_t i = 0; i < numTris; i++)
	{
		const uint32_t indexProvoking = i * 3;
		tribufferMemory[i].xyz.x = sourceNdces[indexProvoking];
		tribufferMemory[i].xyz.y = sourceNdces[indexProvoking + 1];
		tribufferMemory[i].xyz.z = sourceNdces[indexProvoking + 2];
		tribufferMemory[i].xyz.w = 0;
	}

	structuredTribufferDesc.initForStructBuffer(numTris, sizeof(IndexedTriangle), L"structuredTribuffer", tribufferMemory.GetByteSpan());
	auto tribufferHandle = compute_frame.pipes[COMPUTE_STAGES::SPATIAL_HASHING].RegisterStructBuffer(structuredTribufferDesc, resrcRW_Permissions);

	GPUResource<ResourceViews::STRUCTBUFFER_RW>::resrc_desc mortonHashmapDesc;
	mortonHashmapDesc.initForStructBuffer(MORTON_HASHMAP_BUCKET_COUNT, sizeof(MortonHashBucket), L"mortonHashmap", CPUMemory::EmptyByteSpan());
	auto mortonHashmapHandle = compute_frame.pipes[COMPUTE_STAGES::SPATIAL_HASHING].RegisterStructBuffer(mortonHashmapDesc, resrcRW_Permissions);

	compute_frame.pipes[COMPUTE_STAGES::SPATIAL_HASHING].ResolveRootSignature();

	auto csMorton_ResolutionHandle = compute_frame.pipes[COMPUTE_STAGES::SPATIAL_HASHING].RegisterComputeShader("ComputeSpatialHashing.cso", std::max(numTris / 64u, 1u), 1u, 1u);
	compute_frame.pipes[COMPUTE_STAGES::SPATIAL_HASHING].AppendComputeExec(csMorton_ResolutionHandle);
	compute_frame.pipes[COMPUTE_STAGES::SPATIAL_HASHING].BakeCmdList();

	// Second compute stage; AS generation
	compute_frame.pipes[COMPUTE_STAGES::AS_GENERATION].init(false);

	// Bind resources shared with spatial hashing
	compute_frame.pipes[COMPUTE_STAGES::AS_GENERATION].RegisterCBuffer(computeCBufHandle);
	compute_frame.pipes[COMPUTE_STAGES::AS_GENERATION].RegisterStructBuffer(structuredVbuffer);
	compute_frame.pipes[COMPUTE_STAGES::AS_GENERATION].RegisterStructBuffer(tribufferHandle);

	// AS write-out (16M cells, at most two children each)
	GPUResource<ResourceViews::STRUCTBUFFER_RW>::resrc_desc as_Desc;

	auto computeNumBvhCells = [](const uint32_t numTris)
	{
		uint32_t numCells = 0;
		uint32_t numChildren = numTris;
		
		while (numChildren > 0)
		{
			numChildren /= AS_NODE_CHILDCOUNT;
			numCells += numChildren;
		}

		return numCells;
	};
	
	const CPUMemory::MemSize numCells = computeNumBvhCells(numTris); // Up to eight children per node, supporting more than 1M triangles feels unnecessary
	CPUMemory::ArrayAllocHandle<ComputeAS_Node> bvhAS = CPUMemory::AllocateArray<ComputeAS_Node>(numCells);
 
	// GPU bvh setup plans:
	// - Buffer structure goes from rank 0 (root node) down to rank N (last before triangles), left-to-right (better for cache, easier to read)
	// - Implicit clustering using z-ordered indices (see above)
	// - Nodes have bounds, two children (not 8 - relying on the implicit 3D nature of the z-curve to use a simpler binary search), and a flag
	//   indicating if their children are leaves/branches
	// - "Children" in leaf parents are an offset + count (up to 16) into the z-ordered tribuffer
	// 
	// - Expanding on binary search impl, root node covers the entire scene; next two cover half each, their children cover 1/4 each, etc
	// - Branches are tesselating cubes, so bounds should be predictable; check old octree buffer setup for ideas
	// 
	// - Traversal
	// -- Assume sky hits/scene misses if no intersection with the root node
	// -- Otherwise chase node children
	// --- Every time we pick a child, increment the current rank & update our traversal coordinates in a history buffer
	// --- No point tracking children of leaf nodes since they aren't nodes themselves; treat the whole 16-tri intersection test as a simple
	//     yes/no hit query for the containing leaf
	// 
	// -- If we hit a triangle eventually, success!
	// -- Otherwise (for any miss event, tris or nodes), switch the child selected in the previous rank and try again
	// -- If we backtrack all the way to the root node, assume the ray intersects the bounding volume but not the actual geometry
	// 
	// - Buffer setup should still be GPU-side as much as possible, for simplicity
	// -- We could put the tribuffer sorting on the gpu using a copy into a second buffer, instead of hardcoding like above; worth
	//    considering since it would support animations more easily hmmm
	//

	// Simple zero-init; cells are populated on the GPU
	CPUMemory::ZeroData(bvhAS);

	as_Desc.initForStructBuffer<ComputeAS_Node>(numCells, L"octreeAS", bvhAS);
	auto customAS = compute_frame.pipes[COMPUTE_STAGES::AS_GENERATION].RegisterStructBuffer(as_Desc, resrcRW_Permissions);

	// GPU PRNG state (one stream per-pixel/ray-path)
	CPUMemory::ArrayAllocHandle<GPU_PRNG_Channel> prngState = CPUMemory::AllocateArray<GPU_PRNG_Channel>(screenWidth * screenHeight);
	
	const uint32_t groupSize = std::min(64u, std::thread::hardware_concurrency());
	for (uint32_t i = 0; i < screenHeight; i += groupSize)
	{
		std::thread threads[64] = {};
		for (uint32_t t = 0; t < groupSize; t++)
		{
			threads[t] = std::thread(PRNGThreadInterface, screenWidth, prngState, i + t, screenHeight);
		}
		
		for (uint32_t t = 0; t < groupSize; t++)
		{
			threads[t].join();
		}
	}

	// Bind GPU PRNG
	GPUResource<ResourceViews::STRUCTBUFFER_RW>::resrc_desc prng_Desc;
	prng_Desc.initForStructBuffer<GPU_PRNG_Channel>(screenWidth * screenHeight, L"prngState", prngState);
	auto gpuPRNG = compute_frame.pipes[COMPUTE_STAGES::AS_GENERATION].RegisterStructBuffer(prng_Desc, resrcRW_Permissions);

	// Bind morton hashmap, needed for LBVH setup, then resolve root signature
	compute_frame.pipes[COMPUTE_STAGES::AS_GENERATION].RegisterStructBuffer(mortonHashmapHandle);
	compute_frame.pipes[COMPUTE_STAGES::AS_GENERATION].ResolveRootSignature();

	// Shader registration
	// Move onto this after verifying resource set-up
	
	// One thread/triangle
	auto csAS_ResolutionHandle = compute_frame.pipes[COMPUTE_STAGES::AS_GENERATION].RegisterComputeShader("ComputeAS_Resolve.cso", std::max(numTris / 64u, 1u), 1u, 1u);
	compute_frame.pipes[COMPUTE_STAGES::AS_GENERATION].AppendComputeExec(csAS_ResolutionHandle);

	// Work-submission legwork quietly automates when we call (or JIT if we wait until SubmitCmdList, whichever)
	compute_frame.pipes[COMPUTE_STAGES::AS_GENERATION].BakeCmdList();

	// Third compute stage (ray-tracing, output to compute target)
	///////////////////////////////////////////////////////////////

	compute_frame.pipes[COMPUTE_STAGES::LT].init(false);
	compute_frame.pipes[COMPUTE_STAGES::LT].RegisterCBuffer(computeCBufHandle);
	compute_frame.pipes[COMPUTE_STAGES::LT].RegisterStructBuffer(structuredVbuffer);

	// Load/bind materials
	//////////////////////

	// Local material type definitions
	using spectralType = decltype(Material::spectralData)::innerType;
	using roughnessType = decltype(Material::roughnessData)::innerType;

	// Material resource descriptions
	GPUResource<ResourceViews::STRUCTBUFFER_RW>::resrc_desc spectralAtlas = {};
	GPUResource<ResourceViews::TEXTURE_SUPPORTS_SAMPLING>::resrc_desc roughnessAtlas = {};

	// Set material properties known ahead of time
	spectralAtlas.stride = sizeof(spectralType);
	roughnessAtlas.fmt = StandardResrcFmts::FP32_1; // For now
	roughnessAtlas.msaa.enabled = false;
	roughnessAtlas.msaa.expectedSamples = 1;
	roughnessAtlas.msaa.forcedSamples = 1;
	roughnessAtlas.msaa.qualityTier = 0;
	roughnessAtlas.stride = sizeof(roughnessType);

	roughnessAtlas.resrcName = L"roughnessAtlas";
	spectralAtlas.resrcName = L"spectralAtlas";

	// Compute material dimensions
	spectralAtlas.dimensions[0] = static_cast<uint32_t>(computeConstants->screenAndLensOptions.materialAtlasDims.x * computeConstants->screenAndLensOptions.materialAtlasDims.y);
	roughnessAtlas.dimensions[0] = static_cast<uint32_t>(computeConstants->screenAndLensOptions.materialAtlasDims.z);
	roughnessAtlas.dimensions[1] = static_cast<uint32_t>(computeConstants->screenAndLensOptions.materialAtlasDims.w);

	// Material memory allocations
	const uint32_t spectralAtlasFootprint = spectralAtlas.dimensions[0] * spectralAtlas.stride;
	const uint32_t roughnessAtlasFootprint = roughnessAtlas.dimensions[0] * roughnessAtlas.dimensions[1] * roughnessAtlas.stride;

	// Incredibly hacky to use byte arrays here; we should use typed arrays & array subsets instead, then extract ByteSpans for upload
	CPUMemory::ArrayAllocHandle<uint8_t> spectralAtlasData = CPUMemory::AllocateArray<uint8_t>(spectralAtlasFootprint);
	CPUMemory::ArrayAllocHandle<uint8_t> roughnessAtlasData = CPUMemory::AllocateArray<uint8_t>(roughnessAtlasFootprint);
	CPUMemory::ArrayAllocHandle<MaterialPropertyEntry> materialEntries = CPUMemory::AllocateArray<MaterialPropertyEntry>(sceneMaterialCount);

	// Atlas into allocated memory
	// Very naive packing, all on X
	uint16_t atlasX_OffsSpectral = 0;
	uint16_t atlasX_OffsRoughness = 0;
	for (uint32_t i = 0; i < sceneMaterialCount; i++)
	{
		// Atlas spectral data
		//////////////////////

		const uint32_t spectralSubresrcWidth = spectralAtlas.stride * sceneMaterials[i].spectralTexX;
		const uint32_t spectralResrcWidth = spectralAtlas.stride * spectralAtlas.dimensions[0];

		materialEntries[i].spectralWidth = sceneMaterials[i].spectralTexX;
		materialEntries[i].spectralHeight = sceneMaterials[i].spectralTexY;
		materialEntries[i].spectralOffsetU = static_cast<float>(atlasX_OffsSpectral) / spectralResrcWidth;
		materialEntries[i].spectralOffsetV = 0; // For now! May change if we use a different packing algorithm

		uint16_t atlasY_OffsSpectral = 0;
		for (uint32_t y = 0; y < sceneMaterials[i].spectralTexY; y++)
		{
			memcpy(&spectralAtlasData[0] + atlasX_OffsSpectral + atlasY_OffsSpectral, &sceneMaterials[i].spectralData[0], spectralSubresrcWidth);
			atlasY_OffsSpectral += spectralResrcWidth;
		}
		atlasX_OffsSpectral += spectralSubresrcWidth;

		// Atlas roughness data
		///////////////////////

		const uint32_t roughnessSubresrcWidth = roughnessAtlas.stride * sceneMaterials[i].roughnessTexX;
		const uint32_t roughnessResrcWidth = roughnessAtlas.stride * roughnessAtlas.dimensions[0];

		materialEntries[i].roughnessWidth = sceneMaterials[i].roughnessTexX;
		materialEntries[i].roughnessHeight = sceneMaterials[i].roughnessTexY;
		materialEntries[i].roughnessOffsetU = static_cast<float>(atlasX_OffsRoughness) / roughnessResrcWidth;
		materialEntries[i].roughnessOffsetV = 0; // For now! May change if we use a different packing algorithm

		uint16_t atlasY_OffsRoughness = 0;
		for (uint32_t y = 0; y < sceneMaterials[i].roughnessTexY; y++)
		{
			memcpy(&roughnessAtlasData[0] + atlasX_OffsRoughness + atlasY_OffsRoughness, &sceneMaterials[i].roughnessData[0] + (roughnessSubresrcWidth * y), roughnessSubresrcWidth);
			atlasY_OffsRoughness += roughnessResrcWidth;
		}
		atlasX_OffsRoughness += roughnessSubresrcWidth;
	}

	spectralAtlas.srcData = spectralAtlasData.GetByteSpan();
	roughnessAtlas.srcData = roughnessAtlasData.GetByteSpan();

	// Bind materials & material metadata ^_^
	GPUResource<ResourceViews::STRUCTBUFFER_RW>::resrc_desc materialTable;
	materialTable.initForStructBuffer<MaterialPropertyEntry>(sceneMaterialCount, L"materialTable", materialEntries);
	compute_frame.pipes[COMPUTE_STAGES::LT].RegisterStructBuffer(materialTable, resrcRW_Permissions); // Kind of incredibly cumbersome - should add support for read-only structbuffers (are they new? they feel new)
	compute_frame.pipes[COMPUTE_STAGES::LT].RegisterStructBuffer(spectralAtlas, resrcRW_Permissions);
	compute_frame.pipes[COMPUTE_STAGES::LT].RegisterTextureSampleable(roughnessAtlas, TEXTURE_ACCESS_DIRECT_READS);

	GPUResource<ResourceViews::TEXTURE_DIRECT_WRITE>::resrc_desc sppCounter;
	sppCounter.fmt = StandardResrcFmts::U32_1;
	sppCounter.stride = sizeof(uint32_t);
	sppCounter.dimensions[0] = screenWidth;
	sppCounter.dimensions[1] = screenHeight;
	sppCounter.msaa.enabled = false;
	sppCounter.msaa.expectedSamples = 1;
	sppCounter.msaa.forcedSamples = 1;
	sppCounter.msaa.qualityTier = 0;
	sppCounter.resrcName = L"sampleCountsPerPixel";
	sppCounter.srcData = {};

	GPUResource<ResourceViews::TEXTURE_DIRECT_WRITE>::resrc_desc uavTexDesc;
	uavTexDesc.fmt = StandardResrcFmts::FP16_4;
	uavTexDesc.stride = 8; // 4 channels, 2 bytes/channels

	uavTexDesc.dimensions[0] = screenWidth;
	uavTexDesc.dimensions[1] = screenHeight;

	uavTexDesc.msaa.enabled = false;
	uavTexDesc.msaa.expectedSamples = 1;
	uavTexDesc.msaa.forcedSamples = 1;
	uavTexDesc.msaa.qualityTier = 0;

	uavTexDesc.srcData = {};

	uavTexDesc.resrcName = L"computeTarget";

	compute_frame.pipes[COMPUTE_STAGES::LT].RegisterStructBuffer(tribufferHandle);
	compute_frame.pipes[COMPUTE_STAGES::LT].RegisterStructBuffer(customAS);
	compute_frame.pipes[COMPUTE_STAGES::LT].RegisterStructBuffer(gpuPRNG);

	compute_frame.pipes[COMPUTE_STAGES::LT].RegisterTextureDirectWrite(sppCounter, resrcRW_Permissions);

	auto computeTarget = compute_frame.pipes[1].RegisterTextureDirectWrite(uavTexDesc, resrcRW_Permissions);
	compute_frame.pipes[COMPUTE_STAGES::LT].ResolveRootSignature();

	auto csTestHandle = compute_frame.pipes[1].RegisterComputeShader("ComputeLightTransport.cso", screenWidth / 8, screenHeight / 8, 1); // 64 threads
	compute_frame.pipes[COMPUTE_STAGES::LT].AppendComputeExec(csTestHandle);
	compute_frame.pipes[COMPUTE_STAGES::LT].BakeCmdList();

	// Fourth compute stage (presentation, a graphics stage in practice)
	compute_frame.pipes[COMPUTE_STAGES::BLIT].init(true);
	compute_frame.pipes[COMPUTE_STAGES::BLIT].RegisterCBuffer(computeCBufHandle);
	compute_frame.pipes[COMPUTE_STAGES::BLIT].RegisterVBuffer(viewGeo.vbufferDesc, GPU_RESRC_ACCESS_PERMISSIONS_GENERIC::GENERIC_RESRC_ACCESS_DIRECT_READS);
	compute_frame.pipes[COMPUTE_STAGES::BLIT].RegisterIBuffer(viewGeo.ibufferDesc, GPU_RESRC_ACCESS_PERMISSIONS_GENERIC::GENERIC_RESRC_ACCESS_DIRECT_READS);

	GPUResource<ResourceViews::TEXTURE_DEPTH_STENCIL>::resrc_desc depthTexDesc;
	depthTexDesc.fmt = StandardDepthStencilFormats::DEPTH_16_UNORM_NO_STENCIL;
	depthTexDesc.stride = 2;

	depthTexDesc.dimensions[0] = screenWidth;
	depthTexDesc.dimensions[1] = screenHeight;

	depthTexDesc.msaa.enabled = false;
	depthTexDesc.msaa.expectedSamples = 1;
	depthTexDesc.msaa.forcedSamples = 1;
	depthTexDesc.msaa.qualityTier = 0;

	depthTexDesc.srcData = {};

	depthTexDesc.resrcName = L"depthTex";

	compute_frame.pipes[COMPUTE_STAGES::BLIT].RegisterDepthStencil(depthTexDesc, GPU_RESRC_ACCESS_PERMISSIONS_TEXTURES::TEXTURE_ACCESS_AS_DEPTH_STENCIL);

	compute_frame.pipes[COMPUTE_STAGES::BLIT].EnableStaticSamplers();
	compute_frame.pipes[COMPUTE_STAGES::BLIT].RegisterTextureSampleable(computeTarget);

	compute_frame.pipes[COMPUTE_STAGES::BLIT].ResolveRootSignature();

	RasterSettings rasterSettings = {};
	rasterSettings.stencil.enabled = false; // No stencilling, not sure if leaving other settings at 0 is ok

	rasterSettings.depth.enabled = false;//true;
	rasterSettings.depth.depthTest = RasterSettings::DEPTH_STENCIL_TEST_TYPES::LESS;

	rasterSettings.coreRaster.clipDistant = false;
	rasterSettings.coreRaster.conservativeRaster = false;
	rasterSettings.coreRaster.fillMode = RasterSettings::FILL_SOLID;
	rasterSettings.coreRaster.cullMode = RasterSettings::CULL_BACK;
	rasterSettings.coreRaster.windMode = RasterSettings::WIND_CW; // Not sure about this setting

	rasterSettings.msaaSettings.enabled = false; // No MSAA (we will implement reprojected TAA, eventually)
	rasterSettings.msaaSettings.expectedSamples = 1;
	rasterSettings.msaaSettings.forcedSamples = 0; // Not sure about that
	rasterSettings.msaaSettings.qualityTier = 0;

	auto computeFragStage = compute_frame.pipes[COMPUTE_STAGES::BLIT].RegisterGraphicsShader("ComputePresentation.vso", "ComputePresentation.pso", rasterSettings);
	compute_frame.pipes[COMPUTE_STAGES::BLIT].AppendGFX_Exec(computeFragStage);

	// We require state that varies per-frame for presentation (thx swapchain), so we can't bake this cmdlist on-init
	//compute_frame.pipes[2].BakeCmdList();

	// Hybrid
	/////////

	// Primary rays emulated with GFX draw-pass, remainder bounced with TraceRays()
	//hybrid_frame.pipes[0].RegisterVBuffer(sceneGeo.vbufferDesc, GENERIC_RESRC_ACCESS_DIRECT_READS);
	//hybrid_frame.pipes[0].RegisterIBuffer(sceneGeo.ibufferDesc, GENERIC_RESRC_ACCESS_DIRECT_READS);
	//hybrid_frame.pipes[0].BakeCmdList();

	// Shader-tables
	//GPUResource<ResourceVariants::CBUFFER>::resrc_desc shaderTableCBufDesc;
	//auto shaderTableCBufHandle = compute_frame.pipes[0].RegisterCBuffer(shaderTableCBufDesc, GENERIC_RESRC_ACCESS_DIRECT_READS);
	//shader_table_frame.pipes[0].RegisterRaytracingShader("ShaderTableTracing.cso", L"raygeneration", L"closesthit", L"miss", 2 * sizeof(float), 4 * sizeof(float), 7); // Seven might be overkill, we'll see how we go
	//																																								 // Could also do russian-roulette inside our raygen stage...got some notes in the ray library about that now
	//shader_table_frame.pipes[0].BakeCmdList();

	// Memory clean-up
	CPUMemory::Free(spectralAtlasData);
	CPUMemory::Free(roughnessAtlasData);
	CPUMemory::Free(materialEntries);
	CPUMemory::Free(prngState);
	CPUMemory::Free(bvhAS);
	CPUMemory::Free(tribufferMemory);
}

void Render::UpdateFrameConstants(CPUMemory::SingleAllocHandle<FrameConstants> frameConstants)
{
	UpdateComputeConstants(frameConstants);

	auto cbufResrc = compute_frame.pipes[0].DecodeCBufferHandle(computeCBufHandle);
	cbufResrc->UpdateData(computeConstants.GetByteSpan());
}

void Render::Draw()
{
#ifdef PROFILE
	static bool capturedFirstFrame = false;
	if (!capturedFirstFrame)
	{
		RenderDebug::BeginCapture();
	}
#endif

	switch (currMode)
	{
		case RENDER_MODE::MODE_COMPUTE:
			compute_frame.pipes[0].SubmitCmdList(false);
			compute_frame.pipes[1].SubmitCmdList(false);
			compute_frame.pipes[2].SubmitCmdList(false);
			break;
		case RENDER_MODE::MODE_HYBRID:
			hybrid_frame.pipes[0].SubmitCmdList(false);
			break;
		case RENDER_MODE::MODE_SHADER_TABLES:
			shader_table_frame.pipes[0].SubmitCmdList(false);
			break;
		default:
			assert(false); // Unsupported mode (spooky, indicates possible memory corruption)
			break;
	}
	DXWrapper::PresentLastFrame();

#ifdef PROFILE
	if (!capturedFirstFrame)
	{
		RenderDebug::EndCapture();
		capturedFirstFrame = true;
	}
#endif
}
