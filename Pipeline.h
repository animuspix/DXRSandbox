#pragma once

#include "DXWrapper.h"
#include "GPUResource.h"
#include "Shader.h"
#include "RasterSettings.h"
#include "Math.h"

// Pipelines contain a group of shaders, a group of resources, and a command list
// Don't think writing command-lists from outside code is elegant or sensible
// Better to have a bunch of interfaces that mutate the command-list behind the scenes
// Visibility over pipeline resources is a problem - I think a simple public const summary buffer
// (private but accessed through a getter) could be an effective way to specify resources for different arguments without
// exposing their API handles to external code

enum class PIPELINE_OBJ_TYPES
{
	SHADER,
	RESRC
};

template<PIPELINE_OBJ_TYPES type>
struct PipelineObjectHandle
{
	uint32_t index;
	using objFmtType = typename std::conditional<type == PIPELINE_OBJ_TYPES::SHADER, SHADER_TYPES, ResourceViews>::type;
	objFmtType objFmt;
	uint64_t srcPipelineID;

	PipelineObjectHandle() {};
	PipelineObjectHandle(uint32_t _index, objFmtType _fmt, uint64_t _srcPipelineID)
	{
		index = _index;
		objFmt = _fmt;
		srcPipelineID = _srcPipelineID;
	}

	PIPELINE_OBJ_TYPES objType = type;
};

enum PIPELINE_DEPENDENCY_TYPES
{
	COPY_TO_WRITE, // Implies transition from COPY_DEST to UAV
	COPY_TO_READ, // Implies transition from COPY_DEST to SRV
	RASTER_TO_READ, // Implies transition from RTV to SRV (textures only)
	RASTER_TO_WRITE, // Implies transition from RTV to UAV (textures only)
	WRITE_TO_READ, // Implies transition from UAV to SRV (textures or buffers)
	READ_TO_WRITE // Implies transition from SRV to UAV (textures or buffers)
};

struct PipelineDependency
{
	PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> resrc;
	PIPELINE_DEPENDENCY_TYPES depType;
};

struct CopyEvent
{
	PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> src, dst;
	uint32_t copyWidth, copyHeight; // Height must be 1 for buffer resources
};

struct ClearEvent
{
	PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> resrc;
	float clearVal;
};

struct ExecEvent
{
	PipelineObjectHandle<PIPELINE_OBJ_TYPES::SHADER> shader;
	uint32_t pipelineID;
};

struct ComputeEvent : public ExecEvent
{
	uvec3 dispatchAxes;
};

struct GFX_Event : public ExecEvent
{
	uint32_t numNdces;
};

class Pipeline
{
	public:
		// Pipelines are set-up piecemeal, so constructor intentionally does nothing except generating & saving an ID value to help organize API work
		Pipeline() {};
		void init(bool isDynamic);

		// Not sure how to implement this one, might be type issues with resources
		~Pipeline() {};

		// Registration functions for each supported resource
		// Shared resources are automatically registered as dependencies, with appropriate transition barriers if needed
		// Certain resources can be shared with an implied change in resource variant (e.g. a read-only texture shared through RegisterRWTexture) - this pattern is how we detect & process resource state transitions
		// Descriptors still need to exist in-order for the pipeline's root signature to be valid, so the part where we generate those just-in-time is all fine - this just avoids duplicating the resources themselves
		////////////////////////////////////////////////////////////////////////////////////////////////////////////////

		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterCBuffer(GPUResource<ResourceViews::CBUFFER>::resrc_desc desc, GPUResrcPermSetGeneric accessSettings);
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterStructBuffer(GPUResource<ResourceViews::STRUCTBUFFER_RW>::resrc_desc desc, GPUResrcPermSetGeneric accessSettings);
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterTextureDirectWrite(GPUResource<ResourceViews::TEXTURE_DIRECT_WRITE>::resrc_desc desc, GPUResrcPermSetTextures accessSettings);
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterTextureSampleable(GPUResource<ResourceViews::TEXTURE_SUPPORTS_SAMPLING>::resrc_desc desc, GPUResrcPermSetTextures accessSettings);
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterRenderTarget(GPUResource<ResourceViews::TEXTURE_RENDER_TARGET>::resrc_desc desc, GPUResrcPermSetTextures accessSettings);
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterDepthStencil(GPUResource<ResourceViews::TEXTURE_DEPTH_STENCIL>::resrc_desc desc, GPUResrcPermSetTextures accessSettings);
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterStagingTexture(GPUResource<ResourceViews::TEXTURE_STAGING>::resrc_desc desc, GPUResrcPermSetTextures accessSettings);
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterVBuffer(GPUResource<ResourceViews::VBUFFER>::resrc_desc desc, GPUResrcPermSetGeneric accessSettings);
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterIBuffer(GPUResource<ResourceViews::IBUFFER>::resrc_desc desc, GPUResrcPermSetGeneric accessSettings);
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterAccelerationStructure(GPUResource<ResourceViews::RT_ACCEL_STRUCTURE>::resrc_desc desc, GPUResrcPermSetGeneric accessSettings);

		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterCBuffer(GPUResource<ResourceViews::CBUFFER>::resrc_desc desc, GPU_RESRC_ACCESS_PERMISSIONS_GENERIC accessSettings) { return RegisterCBuffer(desc, GPUResrcPermSetGeneric(accessSettings)); };
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterStructBuffer(GPUResource<ResourceViews::STRUCTBUFFER_RW>::resrc_desc desc, GPU_RESRC_ACCESS_PERMISSIONS_GENERIC accessSettings) { return RegisterStructBuffer(desc, GPUResrcPermSetGeneric(accessSettings)); };
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterTextureDirectWrite(GPUResource<ResourceViews::TEXTURE_DIRECT_WRITE>::resrc_desc desc, GPU_RESRC_ACCESS_PERMISSIONS_TEXTURES accessSettings)
		{
			return RegisterTextureDirectWrite(desc, GPUResrcPermSetTextures(accessSettings));
		};

		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterTextureSampleable(GPUResource<ResourceViews::TEXTURE_SUPPORTS_SAMPLING>::resrc_desc desc, GPU_RESRC_ACCESS_PERMISSIONS_TEXTURES accessSettings)
		{
			return RegisterTextureSampleable(desc, GPUResrcPermSetTextures(accessSettings));
		};

		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterRenderTarget(GPUResource<ResourceViews::TEXTURE_RENDER_TARGET>::resrc_desc desc, GPU_RESRC_ACCESS_PERMISSIONS_TEXTURES accessSettings)
		{
			return RegisterRenderTarget(desc, GPUResrcPermSetTextures(accessSettings));
		};

		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterDepthStencil(GPUResource<ResourceViews::TEXTURE_DEPTH_STENCIL>::resrc_desc desc, GPU_RESRC_ACCESS_PERMISSIONS_TEXTURES accessSettings)
		{
			return RegisterDepthStencil(desc, GPUResrcPermSetTextures(accessSettings));
		};

		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterStagingTexture(GPUResource<ResourceViews::TEXTURE_STAGING>::resrc_desc desc, GPU_RESRC_ACCESS_PERMISSIONS_TEXTURES accessSettings)
		{
			return RegisterStagingTexture(desc, GPUResrcPermSetTextures(accessSettings));
		};

		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterVBuffer(GPUResource<ResourceViews::VBUFFER>::resrc_desc desc, GPU_RESRC_ACCESS_PERMISSIONS_GENERIC accessSettings) { return RegisterVBuffer(desc, GPUResrcPermSetGeneric(accessSettings)); };
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterIBuffer(GPUResource<ResourceViews::IBUFFER>::resrc_desc desc, GPU_RESRC_ACCESS_PERMISSIONS_GENERIC accessSettings) { return RegisterIBuffer(desc, GPUResrcPermSetGeneric(accessSettings)); };

		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterAccelerationStructure(GPUResource<ResourceViews::RT_ACCEL_STRUCTURE>::resrc_desc desc, GPU_RESRC_ACCESS_PERMISSIONS_GENERIC accessSettings)
		{
			return RegisterAccelerationStructure(desc, GPUResrcPermSetGeneric(accessSettings));
		};

		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterCBuffer(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> sharedCBufferHandle);
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterStructBuffer(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> sharedStructBufferHandle);
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterTextureSampleable(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> sharedSampleableTextureHandle);
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterTextureDirectWrite(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> sharedDirectWriteTextureHandle);
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterRenderTarget(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> sharedRenderTargetHandle);
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterStagingTexture(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> sharedStagingTextureHandle);
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterVBuffer(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> sharedVbufferHandle);
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterIBuffer(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> sharedIbufferHandle);
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> RegisterAccelerationStructure(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> sharedAS_Handle);

		// Access a registered resource through its handle
		static GPUResource<ResourceViews::CBUFFER>* DecodeCBufferHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> cbufferHandle);
		static GPUResource<ResourceViews::STRUCTBUFFER_RW>* DecodeStructBufferHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> structBufferHandle);
		static GPUResource<ResourceViews::TEXTURE_SUPPORTS_SAMPLING>* DecodeReadOnlyTextureHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> readOnlyTextureHandle);
		static GPUResource<ResourceViews::TEXTURE_DIRECT_WRITE>* DecodeRWTextureHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> rwTextureHandle);
		static GPUResource<ResourceViews::TEXTURE_RENDER_TARGET>* DecodeRenderTargetHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> renderTargetHandle);
		static GPUResource<ResourceViews::TEXTURE_DEPTH_STENCIL>* DecodeDepthTexHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> renderTargetHandle);
		static GPUResource<ResourceViews::TEXTURE_STAGING>* DecodeStagingTextureHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> stagingTextureHandle);
		static GPUResource<ResourceViews::VBUFFER>* DecodeVBufferHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> vbufferHandle);
		static GPUResource<ResourceViews::IBUFFER>* DecodeIBufferHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> ibufferHandle);
		static GPUResource<ResourceViews::RT_ACCEL_STRUCTURE>* DecodeAccelStructHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> accelStructHandle);

		// Enable static point/linear samplers
		void EnableStaticSamplers();

		// Generate the descriptor range for this pipeline - to be called after resource registration
		void ResolveRootSignature();

		// Generate the input layout for this pipeline - required if a v-buffer is registered for this pipeline
		// (i.e. [RegisterVBuffer] has been called)
		void ResolveInputLayout();

		// Shader registration; expected to be invoked after resolving the descriptor layout for each pipeline
		// I believe I might be able to completely automate pipelines by registering shader dependencies here - the idea is that with fully ordered shaders + copy events I have everything I need
		// to generate commandlists (either just-in-time or on startup)
		// Very early into this work but see ShaderDependency, above
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::SHADER> RegisterComputeShader(const char* dxilPath, uint16_t dispatchX, uint16_t dispatchY, uint16_t dispatchZ);
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::SHADER> RegisterGraphicsShader(const char* dxilPathVertex, const char* dxilPathPixel, RasterSettings& gfxSettings);
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::SHADER> RegisterRaytracingShader(const char* dxilPathRTEffect, const wchar_t* raygenStageName, const wchar_t* closestHitStageName, const wchar_t* missStageName, uint32_t maxShaderAttributeByteSize, uint32_t maxRayPayloadByteSize, uint32_t recursionDepth);

		// Access a registered shader through its handle
		static Shader<SHADER_TYPES::COMPUTE>* DecodeComputeShaderHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::SHADER> handle);
		static Shader<SHADER_TYPES::GRAPHICS>* DecodeGfxShaderHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::SHADER> handle);
		static Shader<SHADER_TYPES::RAYTRACING>* DecodeRaytracingShaderHandle(PipelineObjectHandle<PIPELINE_OBJ_TYPES::SHADER> handle);

		// GPU work submission
		void AppendClear(ClearEvent clear);
		void AppendCopy(CopyEvent cpy);
		void AppendComputeExec(PipelineObjectHandle<PIPELINE_OBJ_TYPES::SHADER> shader);
		void AppendGFX_Exec(PipelineObjectHandle<PIPELINE_OBJ_TYPES::SHADER> shader);

		// If we're using a dynamically-baked pipeline to support multibuffering, we generally want to keep the same actual commands each time
		// (and defer buffer handling to the API implementation layer). But we might want to use dynamic baking to better handle fine-grained 
		// settings changes, and in that case it can be useful to clear our command backlog while still re-baking every frame
		// That's what this function supports - re-setting numEvents to zero, without requiring the current pipeline to be statically-baked
		void ResetStagingCmds();

		// Bake/submit work associated with this pipeline
		// (if an un-baked command list is submitted, it will be baked just before it passes to the API)
		void BakeCmdList(); // Bakes our scheduler down to an API command-list for submission (through ExecuteCommandLists() on DX12)
		void SubmitCmdList(bool synchronous); // "synchronous" indicates whether the API should wait after this cmd-list is submitted, in case a later pipeline depends on some of the work it performed

	private:
		PipelineDependency* dependencies = nullptr;
		uint32_t numDependencies = 0;
		static constexpr uint32_t maxNumDependencies = XPlatConstants::maxResourcesPerPipeline;

		// DXRSandbox shouldn't need dynamic samplers yet
		// (+ we'll be able to add more static samplers for a while before we get to that point)
		bool pointSamplerEnabled = false;
		bool linearSamplerEnabled = false;

		DXWrapper::DataHandle<D3D_ROOTSIG> rootSig;
		bool resolvedRootSig = false; // Set after resolving the root signature for the pipeline, asserted on shader registration

		// The actual ordered pipeline to fire off each frame
		struct PipelineEvent
		{
			enum PIPELINE_EVENT_TYPE
			{
				COPY,
				COMPUTE_EXEC,
				GRAPHICS_EXEC,
				CLEAR
			};

			PipelineEvent() : evtType(COMPUTE_EXEC)
			{
				memset(&cpy, 0, sizeof(CopyEvent));
				memset(&clear, 0, sizeof(ClearEvent));
				memset(&invocableCompute, 0, sizeof(ComputeEvent));
				memset(&invocableGFX, 0, sizeof(GFX_Event));
			}

			void AssignClear(ClearEvent evt)
			{
				evtType = CLEAR;
				clear = evt;
			}

			void AssignCopy(CopyEvent evt)
			{
				evtType = COPY;
				cpy = evt;
			}

			void AssignComputeExec(ComputeEvent _invocable)
			{
				evtType = COMPUTE_EXEC;
				invocableCompute = _invocable;
			}

			void AssignGraphicsExec(GFX_Event _invocable)
			{
				evtType = GRAPHICS_EXEC;
				invocableGFX = _invocable;
			}

			const PIPELINE_EVENT_TYPE GetEvtType()
			{
				return evtType;
			}

			void IssueToCmdList(DXWrapper::DataHandle<D3D_CMD_LIST> clientCmdList, DXWrapper::DataHandle<D3D_ROOTSIG> rootSig, uint32_t pipelineID); // Defined separately to reduce header leakages

			private:
				PIPELINE_EVENT_TYPE evtType;
				CopyEvent cpy;
				GFX_Event invocableGFX;
				ComputeEvent invocableCompute;
				ClearEvent clear;
		};
		static constexpr uint32_t maxPipelineDepth = 32;
		PipelineEvent events[maxPipelineDepth];
		uint32_t numEvents = 0;
		
		// Setting to record whether a pipeline has been baked or not, to decide whether (or not) to reset cmdlists before baking
		// (commandlists start open, and resetting an open cmdlist makes the debug layer unhappy)
		// Per-object (to avoid global messiness during setup)
		bool pipelineBaked = false;

		// Whether or not the pipeline is re-baked every frame - necessary for apps with extremely dynamic settings, and for any pipeline
		// which writes to the back-buffer (since we have to re-bake with a different target buffer & PSO every frame, unless we want to
		// do blits and use twice as much bandwidth + do single-buffering with extra steps)
		bool dynamicallyBakedPipeline = false;

		// Pipeline ID, generated at construction
		uint32_t id = 0;
};

