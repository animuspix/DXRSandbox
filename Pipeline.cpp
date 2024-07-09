#include "Pipeline.h"
#include "CPUMemory.h"
#include <algorithm>

// Pipeline counter to automate ID generation; saves users the work of creating & managing those numbers themselves
static uint32_t idGenerator = 0;

struct PipelineObjectBundle
{
	// Resources & composed root signature
	// Descriptor heaps can theoretically be filled by just one resource type (like 32 structbuffers), so size each registry to the total resource count (maxResourcesPerPipeline)
	GPUResource<ResourceViews::CBUFFER> cbuffer;
	bool cbufferRegistered = false;

	GPUResource<ResourceViews::STRUCTBUFFER_RW> structbuffers[XPlatConstants::maxResourcesPerPipeline];
	uint32_t numStructBuffers = 0;

	GPUResource<ResourceViews::TEXTURE_SUPPORTS_SAMPLING> texturesReadOnly[XPlatConstants::maxResourcesPerPipeline];
	uint32_t numTexturesReadOnly = 0;

	GPUResource<ResourceViews::TEXTURE_DIRECT_WRITE> texturesRW[XPlatConstants::maxResourcesPerPipeline];
	uint32_t numTexturesRW = 0;

	GPUResource<ResourceViews::TEXTURE_STAGING> texturesStaging[XPlatConstants::maxResourcesPerPipeline];
	uint32_t numTexturesStaging = 0;

	// At most one vbuffer & one ibuffer per-pipeline
	// Encourages geometry batching & grouping similar types of processing together, which should be good for performance
	// (and also simplifies my shader/pso setup code)
	GPUResource<ResourceViews::VBUFFER> vbuffer;
	GPUResource<ResourceViews::IBUFFER> ibuffer;
	DXWrapper::DataHandle<D3D_RASTER_INPUT_LAYOUT> ilayout;
	uint32_t numNdces = 0;
	bool vbufferRegistered = false;
	bool iBufferRegistered = false;
	bool resolvedIlayout = false;

	GPUResource<ResourceViews::RT_ACCEL_STRUCTURE> pipelineAS;
	bool asRegistered = false;

	GPUResource<ResourceViews::TEXTURE_RENDER_TARGET> renderTargets[XPlatConstants::maxNumRenderTargetsPerPipeline()];
	uint32_t numRenderTargets = 0;

	GPUResource<ResourceViews::TEXTURE_DEPTH_STENCIL> depthStencilTex;
	bool depthStencilTexRegistered = false;

	// Required because render-targets (& depth-stencils + vertex buffers + index buffers) are bound on the input-assembler, not the root-signature,
	// so there's no easy way to cache their descriptors while also generating them just-in-time (like we do in ResolveRootSignature for generic resourc descriptors)
	// No need to provide a public/surface interface creating these - we already have registry functions, we just need to hook those up generating the views behind
	// the scenes
	DXWrapper::DataHandle<D3D_DESCRIPTOR_HANDLE> renderTargetViews[XPlatConstants::maxNumRenderTargetsPerPipeline()];
	DXWrapper::DataHandle<D3D_DESCRIPTOR_HANDLE> depthStencilView;
	DXWrapper::DataHandle<D3D_DESCRIPTOR_HANDLE> vbufferView;
	DXWrapper::DataHandle<D3D_DESCRIPTOR_HANDLE> ibufferView;

	// Work to be done + work items to process in the pipeline
	DXWrapper::DataHandle<D3D_CMD_LIST> cmdList;
	Shader<SHADER_TYPES::COMPUTE> computeShaders[XPlatConstants::maxNumComputeShaders];
	uvec3 csDispatchAxes[XPlatConstants::maxNumComputeShaders];
	uint32_t numComputeShaders = 0;

	Shader<SHADER_TYPES::GRAPHICS> gfxShaders[XPlatConstants::maxNumGfxShaders];
	DXWrapper::RasterBindlist rasterBindingGroups[XPlatConstants::maxNumGfxShaders];
	uint32_t numGfxShaders = 0;

	Shader<SHADER_TYPES::RAYTRACING> raytracingShaders[XPlatConstants::maxNumRaytracingShaders];
	uint32_t numRaytracingShaders = 0;
};

CPUMemory::ArrayAllocHandle<PipelineObjectBundle> pipelineData;

void Pipeline::init(bool isDynamic)
{
	id = idGenerator;
	idGenerator++;

	// Allocate pipeline data just once, on setting up the first pipeline
	if (id == 0)
	{
		pipelineData = CPUMemory::AllocateArray<PipelineObjectBundle>(XPlatConstants::maxNumPipelines);
	}

	PipelineObjectBundle& currentPipelineBundle = pipelineData[id];
	
	currentPipelineBundle.cbufferRegistered = false;
	currentPipelineBundle.numStructBuffers = 0;
	currentPipelineBundle.numTexturesReadOnly = 0;
	currentPipelineBundle.numTexturesRW = 0;
	currentPipelineBundle.numRenderTargets = 0;
	currentPipelineBundle.numTexturesStaging = 0;

	currentPipelineBundle.vbufferRegistered = false;
	currentPipelineBundle.iBufferRegistered = false;
	currentPipelineBundle.resolvedIlayout = false;

	currentPipelineBundle.asRegistered = false;
	currentPipelineBundle.depthStencilTexRegistered = false;

	numDependencies = 0;
	pointSamplerEnabled = false;
	linearSamplerEnabled = false;

	resolvedRootSig = false;

	wchar_t cmdListLabel[32] = {};
	wsprintf(cmdListLabel, L"Sandbox pipeline %u", id);
	currentPipelineBundle.cmdList = DXWrapper::CreateCmdList(cmdListLabel);

	currentPipelineBundle.numComputeShaders = 0;
	currentPipelineBundle.numGfxShaders = 0;
	currentPipelineBundle.numRaytracingShaders = 0;

	numEvents = 0;

	pipelineBaked = false;

	if (isDynamic)
	{
		dynamicallyBakedPipeline = true;
	}
	else
	{
		dynamicallyBakedPipeline = false;
	}
}

PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> Pipeline::RegisterCBuffer(GPUResource<ResourceViews::CBUFFER>::resrc_desc desc, GPUResrcPermSetGeneric accessSettings)
{
	PipelineObjectBundle& currBundle = pipelineData[id];
	assert(!currBundle.cbufferRegistered);

	currBundle.cbuffer = GPUResource<ResourceViews::CBUFFER>();
	currBundle.cbuffer.InitFromScratch(desc, accessSettings, id);
	return PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC>(0, ResourceViews::CBUFFER, id);
}

PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> Pipeline::RegisterStructBuffer(GPUResource<ResourceViews::STRUCTBUFFER_RW>::resrc_desc desc, GPUResrcPermSetGeneric accessSettings)
{
	PipelineObjectBundle& currBundle = pipelineData[id];

	currBundle.structbuffers[currBundle.numStructBuffers] = GPUResource<ResourceViews::STRUCTBUFFER_RW>();
	currBundle.structbuffers[currBundle.numStructBuffers].InitFromScratch(desc, accessSettings, id);
	
	auto handle = PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC>(currBundle.numStructBuffers, ResourceViews::STRUCTBUFFER_RW, id);
	currBundle.numStructBuffers++;
	
	return handle;
}

PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> Pipeline::RegisterTextureDirectWrite(GPUResource<ResourceViews::TEXTURE_DIRECT_WRITE>::resrc_desc desc, GPUResrcPermSetTextures accessSettings)
{
	PipelineObjectBundle& currBundle = pipelineData[id];

	currBundle.texturesRW[currBundle.numTexturesRW] = GPUResource<ResourceViews::TEXTURE_DIRECT_WRITE>();
	currBundle.texturesRW[currBundle.numTexturesRW].InitFromScratch(desc, accessSettings, id);
	
	auto handle = PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC>(currBundle.numTexturesRW, ResourceViews::TEXTURE_DIRECT_WRITE, id);
	currBundle.numTexturesRW++;
	
	return handle;
}

PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> Pipeline::RegisterTextureSampleable(GPUResource<ResourceViews::TEXTURE_SUPPORTS_SAMPLING>::resrc_desc desc, GPUResrcPermSetTextures accessSettings)
{
	PipelineObjectBundle& currBundle = pipelineData[id];

	currBundle.texturesReadOnly[currBundle.numTexturesReadOnly] = GPUResource<ResourceViews::TEXTURE_SUPPORTS_SAMPLING>();
	currBundle.texturesReadOnly[currBundle.numTexturesReadOnly].InitFromScratch(desc, accessSettings, id);
	
	auto handle = PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC>(currBundle.numTexturesReadOnly, ResourceViews::TEXTURE_SUPPORTS_SAMPLING, id);
	currBundle.numTexturesReadOnly++;

	return handle;
}

PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> Pipeline::RegisterRenderTarget(GPUResource<ResourceViews::TEXTURE_RENDER_TARGET>::resrc_desc desc, GPUResrcPermSetTextures accessSettings)
{
	PipelineObjectBundle& currBundle = pipelineData[id];

	currBundle.renderTargets[currBundle.numRenderTargets] = GPUResource<ResourceViews::TEXTURE_RENDER_TARGET>();
	currBundle.renderTargets[currBundle.numRenderTargets].InitFromScratch(desc, accessSettings, id);
	
	auto handle = PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC>(currBundle.numRenderTargets, ResourceViews::TEXTURE_DIRECT_WRITE, id);
	currBundle.numTexturesReadOnly++;
	
	return handle;
}

PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> Pipeline::RegisterDepthStencil(GPUResource<ResourceViews::TEXTURE_DEPTH_STENCIL>::resrc_desc desc, GPUResrcPermSetTextures accessSettings)
{
	PipelineObjectBundle& currBundle = pipelineData[id];
	assert(!currBundle.depthStencilTexRegistered); // Only one depth-stencil supported per-pipeline
	
	currBundle.depthStencilTex = GPUResource<ResourceViews::TEXTURE_DEPTH_STENCIL>();
	currBundle.depthStencilTex.InitFromScratch(desc, accessSettings, id);
	currBundle.depthStencilTexRegistered = true;

	return PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC>(0, ResourceViews::TEXTURE_DEPTH_STENCIL, id);
}

PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> Pipeline::RegisterStagingTexture(GPUResource<ResourceViews::TEXTURE_STAGING>::resrc_desc desc, GPUResrcPermSetTextures accessSettings)
{
	PipelineObjectBundle& currBundle = pipelineData[id];

	currBundle.texturesStaging[currBundle.numTexturesStaging] = GPUResource<ResourceViews::TEXTURE_STAGING>();
	currBundle.texturesStaging[currBundle.numTexturesStaging].InitFromScratch(desc, accessSettings, id);
	auto handle = PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC>(currBundle.numTexturesStaging, ResourceViews::TEXTURE_STAGING, id);
	
	currBundle.numTexturesStaging++;
	return handle;
}

PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> Pipeline::RegisterVBuffer(GPUResource<ResourceViews::VBUFFER>::resrc_desc desc, GPUResrcPermSetGeneric accessSettings)
{
	PipelineObjectBundle& currBundle = pipelineData[id];

	currBundle.vbuffer = GPUResource<ResourceViews::VBUFFER>();
	currBundle.vbuffer.InitFromScratch(desc, accessSettings, id);
	currBundle.vbufferRegistered = true;

	currBundle.ilayout = DXWrapper::ResolveInputLayout(desc.eltFmts, desc.eltSemantics, desc.numEltsPerVert);
	currBundle.resolvedIlayout = true;

	return PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC>(0, ResourceViews::VBUFFER, id);
}

PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> Pipeline::RegisterIBuffer(GPUResource<ResourceViews::IBUFFER>::resrc_desc desc, GPUResrcPermSetGeneric accessSettings)
{
	PipelineObjectBundle& currBundle = pipelineData[id];

	currBundle.ibuffer = GPUResource<ResourceViews::IBUFFER>();
	currBundle.ibuffer.InitFromScratch(desc, accessSettings, id);
	currBundle.numNdces = desc.dimensions[0];
	currBundle.iBufferRegistered = true;
	
	return PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC>(0, ResourceViews::IBUFFER, id);
}

PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> Pipeline::RegisterAccelerationStructure(GPUResource<ResourceViews::RT_ACCEL_STRUCTURE>::resrc_desc desc, GPUResrcPermSetGeneric accessSettings)
{
	PipelineObjectBundle& currBundle = pipelineData[id];

	currBundle.pipelineAS = GPUResource<ResourceViews::RT_ACCEL_STRUCTURE>();
	currBundle.pipelineAS.InitFromScratch(desc, accessSettings, id);
	currBundle.asRegistered = true;

	return PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC>(0, ResourceViews::RT_ACCEL_STRUCTURE, id);
}

template<ResourceViews dstVariant>
PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterSharedResrc(GPUResource<dstVariant>& dst, PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> srcHandle, uint8_t callingPipeID, uint32_t handleOffset)
{
	dst = GPUResource<dstVariant>();
	switch (srcHandle.objFmt)
	{
		case ResourceViews::CBUFFER:
			assert(dstVariant == ResourceViews::CBUFFER || dstVariant == ResourceViews::STRUCTBUFFER_RW || dstVariant == ResourceViews::VBUFFER);
			dst.InitFromSharedResrc(Pipeline::DecodeCBufferHandle(srcHandle), callingPipeID);
			break;

		case ResourceViews::VBUFFER:
			assert(dstVariant == ResourceViews::VBUFFER || dstVariant == ResourceViews::STRUCTBUFFER_RW || dstVariant == ResourceViews::CBUFFER);
			dst.InitFromSharedResrc(Pipeline::DecodeVBufferHandle(srcHandle), callingPipeID);
			break;

		case ResourceViews::IBUFFER:
			assert(dstVariant == ResourceViews::IBUFFER || dstVariant == ResourceViews::CBUFFER);
			dst.InitFromSharedResrc(Pipeline::DecodeIBufferHandle(srcHandle), callingPipeID);
			break;

		case ResourceViews::STRUCTBUFFER_RW:
			assert(dstVariant == ResourceViews::STRUCTBUFFER_RW || dstVariant == ResourceViews::CBUFFER || dstVariant == ResourceViews::VBUFFER);
			dst.InitFromSharedResrc(Pipeline::DecodeStructBufferHandle(srcHandle), callingPipeID);
			break;

		case ResourceViews::TEXTURE_DIRECT_WRITE:
			assert(dstVariant == ResourceViews::TEXTURE_SUPPORTS_SAMPLING || dstVariant == ResourceViews::TEXTURE_RENDER_TARGET || dstVariant == ResourceViews::TEXTURE_DEPTH_STENCIL || dstVariant == ResourceViews::TEXTURE_DIRECT_WRITE);
			dst.InitFromSharedResrc(Pipeline::DecodeRWTextureHandle(srcHandle), callingPipeID);
			break;

		case ResourceViews::TEXTURE_SUPPORTS_SAMPLING:
			assert(dstVariant == ResourceViews::TEXTURE_SUPPORTS_SAMPLING || dstVariant == ResourceViews::TEXTURE_DIRECT_WRITE);
			dst.InitFromSharedResrc(Pipeline::DecodeReadOnlyTextureHandle(srcHandle), callingPipeID);
			break;

		case ResourceViews::TEXTURE_STAGING:
			assert(dstVariant == ResourceViews::TEXTURE_STAGING); // Conversion for this texture is not allowed (staging textures are defined in host memory and can't be accessed by the GPU, so they can't transition to any texture type that lives there)
																  // We still allow sharing staging textures between pipelines because it might be convenient to use them for e.g. debugging changes between effects on the CPU
			dst.InitFromSharedResrc(Pipeline::DecodeStagingTextureHandle(srcHandle), callingPipeID);
			break;

		case ResourceViews::TEXTURE_RENDER_TARGET:
			assert(dstVariant == ResourceViews::TEXTURE_DIRECT_WRITE || dstVariant == ResourceViews::TEXTURE_SUPPORTS_SAMPLING || dstVariant == ResourceViews::TEXTURE_RENDER_TARGET);
			dst.InitFromSharedResrc(Pipeline::DecodeRenderTargetHandle(srcHandle), callingPipeID);
			break;

		case ResourceViews::TEXTURE_DEPTH_STENCIL:
			assert(dstVariant == ResourceViews::TEXTURE_SUPPORTS_SAMPLING || dstVariant == ResourceViews::TEXTURE_DIRECT_WRITE || dstVariant == ResourceViews::TEXTURE_DEPTH_STENCIL);
			dst.InitFromSharedResrc(Pipeline::DecodeDepthTexHandle(srcHandle), callingPipeID);
			break;

		case ResourceViews::RT_ACCEL_STRUCTURE:
			assert(dstVariant == ResourceViews::RT_ACCEL_STRUCTURE);
			dst.InitFromSharedResrc(Pipeline::DecodeAccelStructHandle(srcHandle), callingPipeID);
			break;
	}
	return PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC>(handleOffset, dstVariant, callingPipeID);
}

PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> Pipeline::RegisterCBuffer(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> sharedCBufferHandle)
{
	PipelineObjectBundle& currBundle = pipelineData[id];
	return RegisterSharedResrc<ResourceViews::CBUFFER>(currBundle.cbuffer, sharedCBufferHandle, id, 0);
}

PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> Pipeline::RegisterStructBuffer(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> sharedStructBufferHandle)
{
	PipelineObjectBundle& currBundle = pipelineData[id];
	
	auto handle = RegisterSharedResrc<ResourceViews::STRUCTBUFFER_RW>(currBundle.structbuffers[currBundle.numStructBuffers], sharedStructBufferHandle, id, currBundle.numStructBuffers);
	currBundle.numStructBuffers++;
	
	return handle;
}

PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> Pipeline::RegisterTextureSampleable(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> sharedSampleableTextureHandle)
{
	PipelineObjectBundle& currBundle = pipelineData[id];

	auto handle = RegisterSharedResrc<ResourceViews::TEXTURE_SUPPORTS_SAMPLING>(currBundle.texturesReadOnly[currBundle.numTexturesReadOnly], sharedSampleableTextureHandle, id, currBundle.numTexturesReadOnly);
	currBundle.numTexturesReadOnly++;
	
	return handle;
}

PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> Pipeline::RegisterTextureDirectWrite(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> sharedDirectWriteTextureHandle)
{
	PipelineObjectBundle& currBundle = pipelineData[id];

	auto handle = RegisterSharedResrc<ResourceViews::TEXTURE_DIRECT_WRITE>(currBundle.texturesRW[currBundle.numTexturesRW], sharedDirectWriteTextureHandle, id, currBundle.numTexturesRW);
	currBundle.numTexturesRW++;
	
	return handle;
}

PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> Pipeline::RegisterRenderTarget(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> sharedRenderTargetHandle)
{
	PipelineObjectBundle& currBundle = pipelineData[id];

	auto handle = RegisterSharedResrc<ResourceViews::TEXTURE_RENDER_TARGET>(currBundle.renderTargets[currBundle.numRenderTargets], sharedRenderTargetHandle, id, currBundle.numRenderTargets);
	currBundle.numRenderTargets++;
	
	return handle;
}

PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> Pipeline::RegisterStagingTexture(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> sharedStagingTextureHandle)
{
	PipelineObjectBundle& currBundle = pipelineData[id];
	
	auto handle = RegisterSharedResrc<ResourceViews::TEXTURE_STAGING>(currBundle.texturesStaging[currBundle.numTexturesStaging], sharedStagingTextureHandle, id, currBundle.numTexturesStaging);
	currBundle.numTexturesStaging++;
	
	return handle;
}

PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> Pipeline::RegisterVBuffer(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> sharedVbufferHandle)
{
	PipelineObjectBundle& currBundle = pipelineData[id];
	return RegisterSharedResrc<ResourceViews::VBUFFER>(currBundle.vbuffer, sharedVbufferHandle, id, 0);
}

PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> Pipeline::RegisterIBuffer(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> sharedIbufferHandle)
{
	PipelineObjectBundle& currBundle = pipelineData[id];
	return RegisterSharedResrc<ResourceViews::IBUFFER>(currBundle.ibuffer, sharedIbufferHandle, id, 0);
}

PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterAccelerationStructure(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> sharedAS_Handle)
{
	return {}; // Unimplemented atm
}

GPUResource<ResourceViews::CBUFFER>* Pipeline::DecodeCBufferHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> cbufferHandle)
{
	assert(cbufferHandle.objFmt == ResourceViews::CBUFFER);
	return &pipelineData[cbufferHandle.srcPipelineID].cbuffer;
}

GPUResource<ResourceViews::STRUCTBUFFER_RW>* Pipeline::DecodeStructBufferHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> structbufferHandle)
{
	assert(structbufferHandle.objFmt == ResourceViews::STRUCTBUFFER_RW);
	return &pipelineData[structbufferHandle.srcPipelineID].structbuffers[structbufferHandle.index];
}

GPUResource<ResourceViews::TEXTURE_SUPPORTS_SAMPLING>* Pipeline::DecodeReadOnlyTextureHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> readOnlyTextureHandle)
{
	assert(readOnlyTextureHandle.objFmt == ResourceViews::TEXTURE_SUPPORTS_SAMPLING);
	return &pipelineData[readOnlyTextureHandle.srcPipelineID].texturesReadOnly[readOnlyTextureHandle.index];
}

GPUResource<ResourceViews::TEXTURE_DIRECT_WRITE>* Pipeline::DecodeRWTextureHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> rwTextureHandle)
{
	assert(rwTextureHandle.objFmt == ResourceViews::TEXTURE_DIRECT_WRITE);
	return &pipelineData[rwTextureHandle.srcPipelineID].texturesRW[rwTextureHandle.index];
}

GPUResource<ResourceViews::TEXTURE_RENDER_TARGET>* Pipeline::DecodeRenderTargetHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> renderTargetHandle)
{
	assert(renderTargetHandle.objFmt == ResourceViews::TEXTURE_RENDER_TARGET);
	return &pipelineData[renderTargetHandle.srcPipelineID].renderTargets[renderTargetHandle.index];
}

GPUResource<ResourceViews::TEXTURE_DEPTH_STENCIL>* Pipeline::DecodeDepthTexHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> depthTexHandle)
{
	assert(depthTexHandle.objFmt == ResourceViews::TEXTURE_DEPTH_STENCIL);
	return &pipelineData[depthTexHandle.srcPipelineID].depthStencilTex;
}

GPUResource<ResourceViews::TEXTURE_STAGING>* Pipeline::DecodeStagingTextureHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> stagingTextureHandle)
{
	assert(stagingTextureHandle.objFmt == ResourceViews::TEXTURE_STAGING);
	return &pipelineData[stagingTextureHandle.srcPipelineID].texturesStaging[stagingTextureHandle.index];
}

GPUResource<ResourceViews::VBUFFER>* Pipeline::DecodeVBufferHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> vbufferHandle)
{
	assert(vbufferHandle.objFmt == ResourceViews::VBUFFER);
	return &pipelineData[vbufferHandle.srcPipelineID].vbuffer;
}

GPUResource<ResourceViews::IBUFFER>* Pipeline::DecodeIBufferHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> ibufferHandle)
{
	assert(ibufferHandle.objFmt == ResourceViews::IBUFFER);
	return &pipelineData[ibufferHandle.srcPipelineID].ibuffer;
}

GPUResource<ResourceViews::RT_ACCEL_STRUCTURE>* Pipeline::DecodeAccelStructHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> accelStructHandle)
{
	assert(accelStructHandle.objFmt == ResourceViews::RT_ACCEL_STRUCTURE);
	return &pipelineData[accelStructHandle.srcPipelineID].pipelineAS;
}

void Pipeline::EnableStaticSamplers()
{
	pointSamplerEnabled = true;
	linearSamplerEnabled = true;
}

void Pipeline::ResolveRootSignature()
{
	PipelineObjectBundle& currBundle = pipelineData[id];

	DXWrapper::ResourceBindList bindList;
	bindList.cbuffer = currBundle.cbuffer.GetResrcHandle();
	bindList.cbufferEnabled = true;

	// Structured buffers
	for (uint32_t i = 0; i < currBundle.numStructBuffers; i++) bindList.structbuffers[i] = currBundle.structbuffers[i].GetResrcHandle();
	bindList.numStructbuffers = currBundle.numStructBuffers;

	// Read-only textures
	for (uint32_t i = 0; i < currBundle.numTexturesReadOnly; i++) bindList.readOnlyTextures[i] = currBundle.texturesReadOnly[i].GetResrcHandle();
	bindList.numReadOnlyTextures = currBundle.numTexturesReadOnly;

	// Read/write textures
	for (uint32_t i = 0; i < currBundle.numTexturesRW; i++) bindList.rwTextures[i] = currBundle.texturesRW[i].GetResrcHandle();
	bindList.numRWTextures = currBundle.numTexturesRW;

	// Acceleration structures
	bindList.topLevelAS = currBundle.pipelineAS.GetResrcHandle().second; // Store using the second handle because top-level ASes are bound to the pipeline, not bottom-level ones
															  // (should maybe change that declaration order, since TLAS are composed from BLAS, not the other way around)
	bindList.tlasEnabled = currBundle.asRegistered;

	// Samplers
	bindList.staticSamplersEnabled[0] = pointSamplerEnabled;
	bindList.staticSamplersEnabled[1] = linearSamplerEnabled;

	// Generate root signature, update root signature status, return ^_^
	rootSig = DXWrapper::ResolveRootSignature(bindList, currBundle.vbufferRegistered, id);
	resolvedRootSig = true;
}

void Pipeline::ResolveInputLayout()
{
	PipelineObjectBundle& currBundle = pipelineData[id];
	assert(currBundle.vbufferRegistered);

	GPUResource<ResourceViews::VBUFFER>::resrc_desc_vbuffer_fmt desc = currBundle.vbuffer.GetDesc();
	currBundle.ilayout = DXWrapper::ResolveInputLayout(desc.eltFmts, desc.eltSemantics, desc.numEltsPerVert);
}

PipelineObjectHandle<PIPELINE_OBJ_TYPES::SHADER> Pipeline::RegisterComputeShader(const char* dxilPath, uint16_t dispatchX, uint16_t dispatchY, uint16_t dispatchZ)
{
	// Can't meaningfully generate shaders/PSOs without an existing root-signature
	PipelineObjectBundle& currBundle = pipelineData[id];
	assert(resolvedRootSig);

	Shader<SHADER_TYPES::COMPUTE>::shader_desc desc;
	desc.precompiledSrcFilenames[0] = dxilPath;
	desc.descriptors = rootSig;

	currBundle.csDispatchAxes[currBundle.numComputeShaders].x = dispatchX;
	currBundle.csDispatchAxes[currBundle.numComputeShaders].y = dispatchY;
	currBundle.csDispatchAxes[currBundle.numComputeShaders].z = dispatchZ;
	currBundle.computeShaders[currBundle.numComputeShaders] = Shader<SHADER_TYPES::COMPUTE>(desc, id);
	
	auto handle = PipelineObjectHandle<PIPELINE_OBJ_TYPES::SHADER>(currBundle.numComputeShaders, SHADER_TYPES::COMPUTE, id);
	currBundle.numComputeShaders++;
	return handle;
}

PipelineObjectHandle<PIPELINE_OBJ_TYPES::SHADER> Pipeline::RegisterGraphicsShader(const char* dxilPathVertex, const char* dxilPathPixel, RasterSettings& gfxSettings)
{
	// Can't meaningfully generate shaders/PSOs without an existing root-signature
	PipelineObjectBundle& currBundle = pipelineData[id];
	assert(resolvedRootSig);

	// If we have an index buffer registered, require a vertex buffer
	if (currBundle.iBufferRegistered)
	{
		assert(currBundle.vbufferRegistered);
	}

	// If we have a vertex buffer registered, require an input layout
	if (currBundle.vbufferRegistered)
	{
		assert(currBundle.resolvedIlayout);
	}

	// Check for a registered depth/stencil buffer when depth-testing or stencilling are enabled
	if (gfxSettings.depth.enabled || gfxSettings.stencil.enabled)
	{
		assert(currBundle.depthStencilTexRegistered); // Register a depth/stencil buffer (through RegisterDepthStencilBuffer) before registering a GFX shader with depth-testing/stencilling
	}
	else
	{
		// Not having an explicit render-target is now supported - we use the swapchain in that case
		//assert(numRenderTargets > 0);
	}

	// Populate most shader settings
	Shader<SHADER_TYPES::GRAPHICS>::shader_desc desc;
	desc.precompiledSrcFilenames[0] = dxilPathVertex;
	desc.precompiledSrcFilenames[1] = dxilPathPixel;
	desc.gfxSettings = gfxSettings;
	desc.ilayout = currBundle.ilayout;
	desc.descriptors = rootSig;

	// Resolve output-merger/raster bindings
	uint32_t supportedRenderTargets = std::min(currBundle.numRenderTargets, XPlatConstants::maxNumRenderTargetsPerPipeline());
	DXWrapper::DataHandle<D3D_TEXTURE> pipelineRenderTargets[XPlatConstants::maxNumRenderTargetsPerPipeline()];
	for (uint32_t i = 0; i < supportedRenderTargets; i++)
	{
		pipelineRenderTargets[i] = currBundle.renderTargets[i].GetResrcHandle();
	}
	desc.rasterBindings.numRenderTargets = currBundle.numRenderTargets;
	desc.rasterBindings.renderTargets = pipelineRenderTargets;
	desc.rasterBindings.depthStencilTexture = currBundle.depthStencilTex.GetResrcHandle();

	// Store bindings for access when the current shader is being invoked/drawn
	currBundle.rasterBindingGroups[currBundle.numGfxShaders] = desc.rasterBindings; // Should rasterBindingGroups *really* be global?

	// Generate shader
	currBundle.gfxShaders[currBundle.numGfxShaders] = Shader<SHADER_TYPES::GRAPHICS>(desc, id);
	auto handle = PipelineObjectHandle<PIPELINE_OBJ_TYPES::SHADER>(currBundle.numGfxShaders, SHADER_TYPES::GRAPHICS, id);
	currBundle.numGfxShaders++;
	return handle;
}

PipelineObjectHandle<PIPELINE_OBJ_TYPES::SHADER> Pipeline::RegisterRaytracingShader(const char* dxilPathRTEffect, const wchar_t* raygenStageName, const wchar_t* closestHitStageName, const wchar_t* missStageName, uint32_t maxShaderAttributeByteSize, uint32_t maxRayPayloadByteSize, uint32_t recursionDepth)
{
	// Can't meaningfully generate shaders/PSOs without an existing root-signature
	PipelineObjectBundle& currBundle = pipelineData[id];
	assert(resolvedRootSig);

	Shader<SHADER_TYPES::RAYTRACING>::shader_desc desc;
	desc.precompiledSrcFilenames[0] = dxilPathRTEffect;
	desc.raygenStageName = raygenStageName;
	desc.closestHitStageName = closestHitStageName;
	desc.missStageName = missStageName;
	desc.maxShaderAttributeByteSize = maxShaderAttributeByteSize;
	desc.maxRayPayloadByteSize = maxRayPayloadByteSize;
	desc.recursionDepth = recursionDepth;

	currBundle.raytracingShaders[currBundle.numRaytracingShaders] = Shader<SHADER_TYPES::RAYTRACING>(desc, id);
	auto handle = PipelineObjectHandle<PIPELINE_OBJ_TYPES::SHADER>(currBundle.numRaytracingShaders, SHADER_TYPES::RAYTRACING, id);
	currBundle.numRaytracingShaders++;
	return handle;
}

Shader<SHADER_TYPES::COMPUTE>* Pipeline::DecodeComputeShaderHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::SHADER> handle)
{
	assert(handle.objFmt == SHADER_TYPES::COMPUTE);
	return &pipelineData[handle.srcPipelineID].computeShaders[handle.index];
}

Shader<SHADER_TYPES::GRAPHICS>* Pipeline::DecodeGfxShaderHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::SHADER> handle)
{
	PipelineObjectBundle& currBundle = pipelineData[handle.srcPipelineID];
	assert(handle.objFmt == SHADER_TYPES::GRAPHICS);
	assert(currBundle.gfxShaders[handle.index].type == SHADER_TYPES::GRAPHICS);
	return &currBundle.gfxShaders[handle.index];
}

Shader<SHADER_TYPES::RAYTRACING>* Pipeline::DecodeRaytracingShaderHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::SHADER> handle)
{
	assert(handle.objFmt == SHADER_TYPES::RAYTRACING);
	return &pipelineData[handle.srcPipelineID].raytracingShaders[handle.index];
}

void Pipeline::AppendClear(ClearEvent clear)
{
	events[numEvents].AssignClear(clear);
	numEvents++;
}

void Pipeline::AppendCopy(CopyEvent cpy)
{
	// Need verification here (DXWrapper::VerifyCopy() or similar)
//#ifdef _DEBUG
//	DXwrapper::VerifyCopy();
//#endif
	events[numEvents].AssignCopy(cpy);
	numEvents++;
}

void Pipeline::AppendComputeExec(PipelineObjectHandle<PIPELINE_OBJ_TYPES::SHADER> shader)
{
	events[numEvents].AssignComputeExec({ shader, id, pipelineData[id].csDispatchAxes[shader.index] });
	numEvents++;
}

void Pipeline::AppendGFX_Exec(PipelineObjectHandle<PIPELINE_OBJ_TYPES::SHADER> shader)
{
	events[numEvents].AssignGraphicsExec({ shader, id, pipelineData[id].numNdces });
	numEvents++;
}

bool computeSigBound = false;
bool gfxSigBound = false;
bool rtSigBound = false;

void Pipeline::PipelineEvent::IssueToCmdList(DXWrapper::DataHandle<D3D_CMD_LIST> clientCmdList, DXWrapper::DataHandle<D3D_ROOTSIG> rootSig, uint32_t pipelineID)
{
	if (evtType == COPY)
	{
		// Insert copy through DXWrapper
	}
	else if (evtType == CLEAR)
	{
		// Insert clear through DXWrapper
	}
	else if (evtType == COMPUTE_EXEC)
	{
		// Root-sig binding, then emit draw/dispatch
		if (!computeSigBound)
		{
			DXWrapper::BindComputeResources(clientCmdList, rootSig, pipelineID); // Sets compute root signature, binds compute descriptor tables
			computeSigBound = true;
		}

		auto shader_ptr = Pipeline::DecodeComputeShaderHandle(invocableCompute.shader);
		DXWrapper::SubmitComputeExec(clientCmdList, invocableCompute.dispatchAxes.x, invocableCompute.dispatchAxes.y, invocableCompute.dispatchAxes.z, shader_ptr->pso);
	}
	else if (evtType == GRAPHICS_EXEC)
	{
		if (!gfxSigBound)
		{
			DXWrapper::BindGFX_Resources(clientCmdList, rootSig, pipelineID); // Sets compute root signature, binds compute descriptor tables
			gfxSigBound = true;
		}

		auto shader_ptr = Pipeline::DecodeGfxShaderHandle(invocableGFX.shader);
		DXWrapper::SubmitGraphicsExec(clientCmdList, invocableGFX.numNdces, shader_ptr->pso, pipelineID);
	}

	// Raytracing support tba...
}

void Pipeline::ResetStagingCmds()
{
	numEvents = 0;
}

void Pipeline::BakeCmdList()
{
	// Reset pipeline commandlist before submitting anything
	// (in-case we're rebaking our command-list in-engine)
	auto cmdList = pipelineData[id].cmdList;

	if (pipelineBaked)
	{
		DXWrapper::ResetCmdList(cmdList);
	}

	// Transition barriers inserted before other render logic
	/////////////////////////////////////////////////////////

	// Issue each event we've recorded to the pipeline's internal command-list
	for (uint32_t i = 0; i < numEvents; i++)
	{
		events[i].IssueToCmdList(cmdList, rootSig, id);
	}

	// Close the command-list, now that we've populated it
	DXWrapper::CloseCmdList(cmdList);
	pipelineBaked = true;
	
	// Reset root-sig binding states
	gfxSigBound = false;
	computeSigBound = false;
	rtSigBound = false;

	// After baking command-list, reset pipeline event counter for static cmd-lists
	// (we want to preserve it for dynamic ones, since they'll be resubmitted with
	// slightly different parameters every frame)
	if (!dynamicallyBakedPipeline)
	{
		numEvents = 0;
	}
}

void Pipeline::SubmitCmdList(bool synchronous)
{
	// Allow baking command-lists immediately before the first frame instead of preparing them on startup (allows for more runtime changess &c before the frame logic is locked in)
	if (numEvents > 0)
	{
		BakeCmdList();
	}

	// Only issue work if a valid command list exists (i.e. the pipeline has been baked down)
	if (pipelineBaked)
	{
		// Submits command-list for API processing, with/out a hidden call to handle API infrastructure calls (possible copies for resource uploads, etc)
		DXWrapper::IssueWork(pipelineData[id].cmdList, synchronous, id);
	}
}
