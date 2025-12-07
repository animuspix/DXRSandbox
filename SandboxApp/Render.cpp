#include "Render.h"
#include "..\GPUResource.h"
#include "..\Shaders\materials.h"
#include "..\Math.h"
#include "Materials.h"
#include "..\CPUMemory.h"

#include "..\Shaders\SharedConstants.h"
#include "..\Shaders\SharedPRNG_Code.h"
#include "..\Shaders\SharedStructs.h"

#include "RenderDebug.h"

#include <cassert>
#include <algorithm>
#include <chrono>
#include <thread>
#include <random>
#include <stdio.h>

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

// Compute/hybrid/shader-table constants (CPU copies)
CPUMemory::SingleAllocHandle<ComputeConstants> computeConstants;
CPUMemory::SingleAllocHandle<HybridTypes::HybridConstants> hybridConstants;
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
	memcpy(&computeConstants->screenAndLensOptions.sceneTransform, &frameConstants->sceneTransform, sizeof(transform));
}

// Sampling atlassed data
// Spectral data (in structbuffer) - straightforward, load constrained quad containing data & interpolate manually
// Roughness data (in texture) - sample as normal (so we get GPU interpolation), but constrain UVs to just inside each atlas entry to prevent bleeding (so (width-1, height-1))
// Roughness processing might be somewhat easier with manual texel loading & blending, unsure

void Render::Init(HWND hwnd, RENDER_MODE mode, XPlatUtils::BakedGeoBuffers& sceneGeo, XPlatUtils::BakedGeoBuffers& viewGeo, Material& material, CPUMemory::SingleAllocHandle<FrameConstants> frameConstants)
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

	// Initialize frame
	compute_frame.init({ false, false, true });
	computeConstants = CPUMemory::AllocateSingle<ComputeConstants>();

	// Transforms, when I get around to them -> cbuffer
	//
	// Geometry vbuffer/ibuffer are always present
	// Compute raytracing -> vbuffer/ibuffer & structbuffers (separately initialized), structbuffers initialize an AS (...probably just a grid...) that we use for software intersection testing
	// Hybrid raytracing -> vbuffer/ibuffer used to construct hardware AS, we raster the originals to make a primary-ray mask, then we trace bounce rays out of the mask against the hardware AS
	// Hardware raytracing -> vbuffer/ibuffer used to construct hardware AS; we path-trace the hardware AS for all bounces

	// First compute stage (spatial hashing)
	//////////////////////////////////////

	// Resource registration
	GPUResource<ResourceViews::CBUFFER>::resrc_desc computeCBufDesc;

	// Constants!

	// Generic
	UpdateComputeConstants(frameConstants);

	// Materials
	computeConstants->screenAndLensOptions.materialAtlasDims = float4(material.spectralTexX, material.spectralTexY, material.roughnessTexX, material.roughnessTexY);

	// CBuffer init/registry
	computeCBufDesc.initForCBuffer<ComputeConstants>(L"computeConstants", computeConstants);
	compute_frame.RegisterCBuffer(computeCBufDesc);

	// Vbuffer/Ibuffer
	GPUResource<ResourceViews::STRUCTBUFFER_RW>::resrc_desc structuredVbufferDesc;
	structuredVbufferDesc.dimensions[0] = sceneGeo.vbufferDesc.dimensions[0];
	structuredVbufferDesc.initForStructBuffer(sceneGeo.vbufferDesc.dimensions[0], sceneGeo.vbufferDesc.stride, L"structuredVbuffer", sceneGeo.vbufferDesc.srcData);

	const GPUResrcPermSetGeneric resrcRW_Permissions = (GENERIC_RESRC_ACCESS_DIRECT_READS | GENERIC_RESRC_ACCESS_DIRECT_WRITES);
	auto structuredVBuffer = compute_frame.RegisterPersistentResource<ResourceViews::STRUCTBUFFER_RW>(structuredVbufferDesc, resrcRW_Permissions);

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
	auto triBuffer = compute_frame.RegisterPersistentResource<ResourceViews::STRUCTBUFFER_RW>(structuredTribufferDesc, resrcRW_Permissions);

	// AS write-out (seeing if I can get away with just Morton-order tri-indices)
	GPUResource<ResourceViews::STRUCTBUFFER_RW>::resrc_desc as_Desc;
	CPUMemory::ArrayAllocHandle<uint> bvhAS = CPUMemory::AllocateArray<uint>(numTris);

	// Simple zero-init; cells are populated on the GPU
	CPUMemory::ZeroData(bvhAS);

	as_Desc.initForStructBuffer<uint>(numTris, L"octreeAS", bvhAS);
	auto asBinding = compute_frame.RegisterPersistentResource<ResourceViews::STRUCTBUFFER_RW>(as_Desc, resrcRW_Permissions);

	// Atomic U32 buffer setup
	// Useful for dispatch-wide synchronization (e.g. total ordering for Morton-sorted positions ;p)
	GPUResource<ResourceViews::STRUCTBUFFER_RW>::resrc_desc atomics_desc;

	// Arbitrary element count, anything up to 16k (D3D12 buffer alignment) is probably good
	atomics_desc.initForStructBuffer(1024u, 4u, L"atomicUIntBuffer", CPUMemory::EmptyByteSpan());
	auto atomicsBuffer = compute_frame.RegisterPersistentResource<ResourceViews::STRUCTBUFFER_RW>(atomics_desc, resrcRW_Permissions);

	// Keys!
	// Mark-up to guarantee persistent order for shared resources like this will be the next step
	// hmm
	// - cbuffer order is preserved automatically on reuse (only one cbuffer/pipeline anyway)
	// - remainder are shared through overloads on RegisterXXXXX
	// - could modify those overloads to enforce persistent order?
	// - will probably work, just very janky
	// - still think passing resources through frames before sending them to pipelines would be best
	//   (followed by a Frame::Finalize() or w/e to consolidate everything)
	// --> In that situation resources would be registered on the frame, which in practice would be filling up an array of pairs [desc | pipeline] and [desc | shared]
	// --> Finalizing the frame would mean binding all the shared resources at the front of each heap for each pipeline, then following those blocks with the 
	//	   specialized per-pipeline resources (in their respective heaps, again, so the descriptor heaps would have layouts like [shared][others])
	// --> After populating specialized resources per-pipeline, root-sig setup would go ahead as normal
	//
	// --> Can possibly implement without overly changing ResolveRootSignature(...); the minimal change would be to pass separate global/shared and per-resource pipelines,
	//	   then just make sure the global resources are always 

	// Bindless rendering references
	// https://github.com/DDreher/BasicBindlessRendering
	// https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html

	auto spatialHashingPass = compute_frame.RegisterComputeShader(COMPUTE_SPATIAL_HASHING, "ComputeSpatialHashing.cso", std::max(numTris / 64u, 1u), 1u, 1u);
	compute_frame.RegisterShaderExec(spatialHashingPass);

	// Third compute stage (ray-tracing, output to compute target)
	///////////////////////////////////////////////////////////////

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

	spectralAtlas.srcData = material.spectralData.GetByteSpan();
	roughnessAtlas.srcData = material.roughnessData.GetByteSpan();

	// Bind materials & material metadata ^_^
	auto spectralTex = compute_frame.RegisterPerStageResource<ResourceViews::STRUCTBUFFER_RW>(spectralAtlas, COMPUTE_STAGES::COMPUTE_LT, resrcRW_Permissions);
	auto roughnessTex = compute_frame.RegisterPerStageResource<ResourceViews::TEXTURE_SUPPORTS_SAMPLING>(roughnessAtlas, COMPUTE_STAGES::COMPUTE_LT, TEXTURE_ACCESS_DIRECT_READS);

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

	GPUResource<ResourceViews::STRUCTBUFFER_RW>::resrc_desc prng_Desc;
	prng_Desc.initForStructBuffer<GPU_PRNG_Channel>(screenWidth * screenHeight, L"prngState", prngState);
	auto prng = compute_frame.RegisterPerStageResource<ResourceViews::STRUCTBUFFER_RW>(prng_Desc, COMPUTE_LT, resrcRW_Permissions);

	GPUResrcPermSetTextures textureRW_Permissions = (TEXTURE_ACCESS_DIRECT_READS | TEXTURE_ACCESS_DIRECT_WRITES);
	auto sampleCounter = compute_frame.RegisterPerStageResource<ResourceViews::TEXTURE_DIRECT_WRITE>(sppCounter, COMPUTE_LT, textureRW_Permissions);

	auto computeTarget = compute_frame.RegisterPerStageResource<ResourceViews::TEXTURE_DIRECT_WRITE>(uavTexDesc, COMPUTE_LT, TEXTURE_ACCESS_DIRECT_WRITES | TEXTURE_ACCESS_DIRECT_READS);

	auto computeLightPass = compute_frame.RegisterComputeShader(COMPUTE_LT, "ComputeLightTransport.cso", screenWidth / 8, screenHeight / 8, 1); // 64 threads
	compute_frame.RegisterShaderExec(computeLightPass);

	// Fourth compute stage (presentation, a graphics stage in practice)
	auto vbuffer = compute_frame.RegisterPerStageResource<ResourceViews::VBUFFER>(viewGeo.vbufferDesc, COMPUTE_BLIT, GENERIC_RESRC_ACCESS_DIRECT_READS);
	auto ibuffer = compute_frame.RegisterPerStageResource<ResourceViews::IBUFFER>(viewGeo.ibufferDesc, COMPUTE_BLIT, GENERIC_RESRC_ACCESS_DIRECT_READS);

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

	compute_frame.RegisterPerStageResource<ResourceViews::TEXTURE_DEPTH_STENCIL>(depthTexDesc, COMPUTE_BLIT, TEXTURE_ACCESS_AS_DEPTH_STENCIL);
	auto computeTargetAsBlitSource = compute_frame.TransitionResource<ResourceViews::TEXTURE_DIRECT_WRITE, ResourceViews::TEXTURE_SUPPORTS_SAMPLING>(computeTarget, COMPUTE_BLIT);
	compute_frame.EnableStaticSamplers(COMPUTE_BLIT);

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

	auto computeFragStage = compute_frame.RegisterGraphicsShader(COMPUTE_BLIT, "ComputePresentation.vso", "ComputePresentation.pso", rasterSettings);
	compute_frame.RegisterShaderExec(computeFragStage);

	// Resolve assorted frame bindings/events into concrete GPU work
	compute_frame.Finalize();

	// Recover & sort shared resource keys
	computeConstants->computeResourceKeys.atomicsLookup = compute_frame.GetGPUKeyForPersistentBinding(atomicsBuffer);
	computeConstants->computeResourceKeys.bvhLookup = compute_frame.GetGPUKeyForPersistentBinding(asBinding);
	computeConstants->computeResourceKeys.structuredVBufferLookup = compute_frame.GetGPUKeyForPersistentBinding(structuredVBuffer);
	computeConstants->computeResourceKeys.triBufferLookup = compute_frame.GetGPUKeyForPersistentBinding(triBuffer);

	// Recover & sort per-stage resource keys
	decltype(compute_frame)::SortedResourceKeys computeResourceKeys = {};

	// First index is shared keys, see SharedStructs.h (PerStageResourceKeys)
	// Compute target is written by light transport, read by blit/presentation
	const uint32_t computeCBufferKey = compute_frame.GetGPUKeyForCBuffer();
	const uint32_t computeTargetKey = compute_frame.GetGPUKeyForStageBinding(computeTarget);

	SpatialHashBindings spatialBindings;
	spatialBindings.sharedKeys = computeCBufferKey;

	LightTransportBindings ltBindings = {};
	ltBindings.sharedKeys = computeCBufferKey;
	ltBindings.outputTextureLookup = computeTargetKey;
	ltBindings.sampleCounterLookup = compute_frame.GetGPUKeyForStageBinding(sampleCounter);
	ltBindings.roughnessLookup = compute_frame.GetGPUKeyForStageBinding(roughnessTex);
	ltBindings.spectralLookup = compute_frame.GetGPUKeyForStageBinding(spectralTex);
	ltBindings.prngStreamsLookup = compute_frame.GetGPUKeyForStageBinding(prng);

	ComputePresentationBindings presentBindings = {};
	presentBindings.sharedKeys = computeCBufferKey;
	presentBindings.colorBufferLookup = compute_frame.GetGPUKeyForStageRebinding(computeTargetAsBlitSource);

	// Easy memcpy
	memcpy(computeResourceKeys.constants[COMPUTE_SPATIAL_HASHING].data(), &spatialBindings, sizeof(spatialBindings));
	memcpy(computeResourceKeys.constants[COMPUTE_LT].data(), &ltBindings, sizeof(ltBindings));
	memcpy(computeResourceKeys.constants[COMPUTE_BLIT].data(), &presentBindings, sizeof(presentBindings));
	computeResourceKeys.numActiveConstants[COMPUTE_SPATIAL_HASHING] = 1;
	computeResourceKeys.numActiveConstants[COMPUTE_LT] = sizeof(ltBindings) / sizeof(uint32_t);
	computeResourceKeys.numActiveConstants[COMPUTE_BLIT] = sizeof(presentBindings) / sizeof(uint32_t);

	// Bake command-lists
	compute_frame.BakeCmdLists(computeResourceKeys);

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
	//CPUMemory::Free(prngState);
	//CPUMemory::Free(bvhAS);
	//CPUMemory::Free(tribufferMemory);
}

void Render::UpdateFrameConstants(CPUMemory::SingleAllocHandle<FrameConstants> frameConstants)
{
	switch (currMode)
	{
	case RENDER_MODE::MODE_COMPUTE:
		UpdateComputeConstants(frameConstants);
		compute_frame.UpdateCBuffer(computeConstants.GetByteSpan());
		break;
	case RENDER_MODE::MODE_HYBRID:
		//UpdateHybridConstants(frameConstants);
		break;
	case RENDER_MODE::MODE_SHADER_TABLES:
		//UpdateShaderTableConstants(frameConstants);
		break;
	default:
		assert(false); // Missing case!
		break;
	}
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
		compute_frame.SubmitPipes();
		break;
	case RENDER_MODE::MODE_HYBRID:
		hybrid_frame.SubmitPipes();
		break;
	case RENDER_MODE::MODE_SHADER_TABLES:
		shader_table_frame.SubmitPipes();
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
