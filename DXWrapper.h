#pragma once

#include <stdint.h>
#include <Windows.h>
#undef min
#undef max

#include "RasterSettings.h"
#include "ResourceEnums.h"
#include "XPlatformUtilities.h"
#include "Math.h"

#include "CPUMemory.h"

// Useful links
// https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/Samples/Desktop/D3D12Raytracing/src/D3D12RaytracingHelloWorld/D3D12RaytracingHelloWorld.cpp
// https://docs.microsoft.com/en-us/windows/win32/direct3d12/ray-generation-shader
// https://docs.microsoft.com/en-us/windows/win32/direct3d12/intersection-attributes
// https://docs.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_dispatch_rays_desc
// https://docs.microsoft.com/en-us/windows/win32/api/d3d12/nn-d3d12-id3d12pipelinestate
// https://docs.microsoft.com/en-us/windows/win32/direct3d12/any-hit-shader
// https://docs.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-executecommandlists
// https://docs.microsoft.com/en-us/windows/win32/api/d3d12/nn-d3d12-id3d12commandqueue
// https://docs.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copyresource
// https://docs.microsoft.com/en-us/windows/win32/direct3d12/managing-graphics-pipeline-state-in-direct3d-12
// https://docs.microsoft.com/en-us/windows/win32/api/dxgi/ne-dxgi-dxgi_swap_effect
// https://docs.microsoft.com/en-us/windows/win32/direct3d12/swap-chains

// This wrapper is strictly for API calls + handles and the core pipeline
// GPU memory/descriptor management, GPU resources, and shaders/effects should all be controlled from their own structures which
// delegate their final API objects & calls to be stored/invoked here

// Lots of stuff here (like max resources/pipeline and max stride/vertex) should really go in an intermediate layer while absolute core behaviours are implemented in API code
// When I get around to implementing VKWrapper I'll make sure to separate that out & hopefully reduce the amount that needs to be reimplemented for Vulkan

// All definitions that require dependencies restricted to DXWrapper.cpp to reduce header leaks
enum D3D_OBJ_FMT
{
	D3D_VBUFFER,
	D3D_IBUFFER,
	D3D_TEXTURE,
	D3D_STRUCTBUFFER,
	D3D_CBUFFER,
	D3D_ACCELSTRUCT_BLAS,
	D3D_ACCELSTRUCT_TLAS,
	D3D_SAMPLER,
	D3D_PSO,
	D3D_CMD_LIST,
	D3D_ROOTSIG,
	D3D_RASTER_INPUT_LAYOUT,
	D3D_DESCRIPTOR_HANDLE
};

template<D3D_OBJ_FMT objFmt>
concept d3dResrcObj = (objFmt == D3D_VBUFFER) || (objFmt == D3D_IBUFFER) ||
					  (objFmt == D3D_TEXTURE) || (objFmt == D3D_STRUCTBUFFER) ||
					  (objFmt == D3D_CBUFFER) || (objFmt == D3D_ACCELSTRUCT_BLAS) ||
					  (objFmt == D3D_ACCELSTRUCT_TLAS);

class DXWrapper
{
	public:
		// Initialize/deinitialize DX interface
		static bool Init(HWND hwnd, uint32_t screenWidth, uint32_t screenHeight, bool vsynced);
		static void Deinit();

		// Everything processed by DX ends up in a buffer within our DX-wrapper; we use these handles to identify them
		template<D3D_OBJ_FMT objFmt>
		struct DataHandle
		{
			static constexpr D3D_OBJ_FMT format = objFmt;
			uint64_t index;
		};

		// Supported static samplers
		enum STATIC_SAMPLERS
		{
			STATIC_SAMPLER_POINT,
			STATIC_SAMPLER_LINEAR,
			NUM_STATIC_SAMPLERS
		};

		// A list of expected ranges to help generate root signatures for each pipeline
		struct ResourceBindList
		{
			DataHandle<D3D_CBUFFER> cbuffer; // CBV
			bool cbufferEnabled;

			DataHandle<D3D_STRUCTBUFFER> structbuffers[XPlatConstants::maxResourcesPerPipeline]; // UAVs
			uint32_t numStructbuffers;

			// Vertex buffers & index buffers are bound directly on the input assembler, not through the root signature

			DataHandle<D3D_TEXTURE> rwTextures[XPlatConstants::maxResourcesPerPipeline]; // UAVs
			uint32_t numRWTextures;

			DataHandle<D3D_TEXTURE> readOnlyTextures[XPlatConstants::maxResourcesPerPipeline]; // SRVs
			uint32_t numReadOnlyTextures;

			// Staging textures cannot be accessed from shader code
			DataHandle<D3D_ACCELSTRUCT_TLAS> topLevelAS; // SRV; top-level AS are composed from bottom-level ASes, so only the top-level AS needs to be bound
														 // Only one AS per-pipeline (matches one vbo/ibo for each)
			bool tlasEnabled;

			// Static samplers are hardcoded, so users just switch each one on/off and the number to use is decoded when we resolve each pipeline's root-signature
			bool staticSamplersEnabled[NUM_STATIC_SAMPLERS];
		};
		static DataHandle<D3D_ROOTSIG> ResolveRootSignature(ResourceBindList bindings, bool mayUseGraphics, uint32_t pipelineID);
		static DataHandle<D3D_RASTER_INPUT_LAYOUT> ResolveInputLayout(StandardResrcFmts* elementFormats, VertexEltSemantics* semantics, uint32_t numEltsPerVert);

	private:
		static void InsertTransition(ResourceViews beforeVariant, ResourceViews afterVariant, uint64_t resrcNdx, uint8_t pipelineID);
		static void NameResourceInternal(uint64_t resrcID, LPCWSTR name);

		static void UpdateCBufferData(DataHandle<D3D_CBUFFER> handle, CPUMemory::ArrayAllocHandle<uint8_t> data);

	public:
		template<D3D_OBJ_FMT handleFmt>
		static void InsertTransition(ResourceViews beforeVariant, ResourceViews afterVariant, DXWrapper::DataHandle<handleFmt> resrc, uint8_t pipelineID)
		{
			InsertTransition(beforeVariant, afterVariant, resrc.index, pipelineID);
		}

		template<D3D_OBJ_FMT fmt>
		static void UpdateResrcData(DataHandle<fmt> handle, CPUMemory::ArrayAllocHandle<uint8_t> data)
		{
			static_assert(handle.format == D3D_CBUFFER || handle.format == D3D_IBUFFER || handle.format == D3D_IBUFFER || handle.format == D3D_STRUCTBUFFER || handle.format == D3D_TEXTURE, "UpdateResrcData can only be used on resource objects");

			switch (handle.format)
			{
				case D3D_CBUFFER:
					UpdateCBufferData(handle, data);
					break;
				//case D3D_IBUFFER:
				//	UpdateCBufferData();
				//	break;
				//case D3D_IBUFFER:
				//	UpdateCBufferData();
				//	break;
				//case D3D_STRUCTBUFFER:
				//	UpdateCBufferData();
				//	break;
				//case D3D_TEXTURE:
				//	UpdateTextureData();
				//	break;
				default:
					printf("unimplemented resource update\n");
					assert(false);
					break;
			}
		}

		// Get the highest multisampling quality level supported for a given format
		static uint32_t GetMaxMSAAQualityLevelForTexture(StandardResrcFmts fmt, uint32_t expectedSampleCount);

		// Compute PSO setup, nice and simplez
		static DataHandle<D3D_PSO> GenerateComputePSO(const char* precompiledSrcName, DataHandle<D3D_ROOTSIG> descriptors, uint32_t pipelineID);

		// Specialized GFX bindlist, for resources bound on the output-merger stage instead of the root signature
		struct RasterBindlist
		{
			uint32_t numRenderTargets = 1; // Assume at least one render-target (not depth-only)
			DataHandle<D3D_TEXTURE>* renderTargets;

			// Because the depth buffer is bound in the output-merger stage, not through shader root descriptors
			DataHandle<D3D_TEXTURE> depthStencilTexture;
		};

		// Graphics PSO setup, more shaders & requires setting up rasterization context (such as input layout) as well as bytecode + descriptors
		static DataHandle<D3D_PSO> GenerateGraphicsPSO(const char* precompiledVtxName, const char* precompiledPixelName, RasterSettings rasterSettings, RasterBindlist rasterBindlist, DataHandle<D3D_RASTER_INPUT_LAYOUT> ilayout, DataHandle<D3D_ROOTSIG> descriptors, uint32_t pipelineID);

		// Ray-tracing PSO setup, not as many inputs as graphics PSOs but fuckier setup (involving a runtime linker stage). Shader attributes are things pulled from the driver after resolving intersections (like barycentric coordinates),
		// payloads are things tracked per ray/path vertex (like color), recursion is the number of bounces along each path (= the number of times the driver needs to spawn a ray, query the acceleration structure, select a shader-table entry, and accumulate the ray color/energy/whatever)
		// Ray-tracing shader setup is different to compute & graphics; you basically make an effect file with all your entry points, which DX loads in as a "dxil library". In the shader + your PSO setup code you specify labels for the raygen/closest-hit/miss stages, and then DX uses those
		// to link the entry-points together. It's counterintuitive at first, but it is cleaner than e.g. graphics where either you use separate files, or you enforce rules in your shader code to make it clear what's visible to PS/VS/GS/HS, etc.
		// (this framework assumes raster stages in separate files for simplicity & flexibility)
		static DataHandle<D3D_PSO> GenerateRayPSO(const char* precompiledEffectName, const wchar_t* raygenStageName, const wchar_t* closestHitStageName, const wchar_t* missStageName, uint32_t maxShaderAttributeByteSize, uint32_t maxRayPayloadByteSize, uint32_t recursionDepth, DataHandle<D3D_ROOTSIG> descriptors, uint32_t pipelineID);

		// Resource set-up
		// Modern API time! Just one resource interface that basically dumps memory on the GPU, accesed through each of these indirections
		// (which basically only exist so I don't use templates and end up moving a bunch of API state into this header)
		// View-dependant accesses are completely relegated to descriptors/bindings
		// Non-null [srcData] for cbuffers makes no difference since they go on the upload heap anyway
		// Non-null [srcData] for other resources causes a temporary allocation on the upload heap, followed by a copy to the GPU heap and deallocating the upload entry
		static DataHandle<D3D_CBUFFER> GenerateConstantBuffer(uint32_t footprint, GPUResrcPermSetGeneric accessSettings, CPUMemory::ArrayAllocHandle<uint8_t> srcData, uint32_t pipelineID);
		static DataHandle<D3D_STRUCTBUFFER> GenerateStructuredBuffer(uint32_t footprint, uint32_t stride, uint32_t numElements, GPUResrcPermSetGeneric accessSettings, CPUMemory::ArrayAllocHandle<uint8_t> srcData, uint32_t pipelineID);
		static DataHandle<D3D_TEXTURE> GenerateStandardTexture(uint32_t width, uint32_t height, StandardResrcFmts fmt, RasterSettings::MSAASettings msaa, GPUResrcPermSetTextures accessSettings, TextureViews textureVariant, CPUMemory::ArrayAllocHandle<uint8_t> srcData, uint32_t pipelineID);
		static DataHandle<D3D_TEXTURE> GenerateDepthStencilTexture(uint32_t width, uint32_t height, StandardDepthStencilFormats fmt, RasterSettings::MSAASettings msaa, GPUResrcPermSetTextures accessSettings, CPUMemory::ArrayAllocHandle<uint8_t> srcData, uint32_t pipelineID);
		static DataHandle<D3D_IBUFFER> GenerateIndexBuffer(uint32_t footprint, StandardIBufferFmts fmt, GPUResrcPermSetGeneric accessSettings, CPUMemory::ArrayAllocHandle<uint8_t> srcData, uint32_t pipelineID);
		static DataHandle<D3D_VBUFFER> GenerateVertexBuffer(uint32_t footprint, uint32_t stride, uint32_t numElts, StandardResrcFmts* eltFmts, GPUResrcPermSetGeneric accessSettings, CPUMemory::ArrayAllocHandle<uint8_t> srcData, uint32_t pipelineID);

		// Name the given resource, not much overhead, useful for debugging
		template<D3D_OBJ_FMT objHandleFmt> requires d3dResrcObj<objHandleFmt>
		static void NameResource(DataHandle<objHandleFmt> resrcHandle, LPCWSTR name)
		{
			NameResourceInternal(resrcHandle.index, name);
		}

		// Users are expected to merge vbuffers/ibuffers in high-level code if they want them in the same acceleration structure - the overall design philosophy for DXRSandbox is too flexible
		// to do that work automatically without introducing unexpected behaviour, or requiring heavy validation on input buffers
		// [asConfig.hasCutouts] indicates whether the geometry in the AS we're building uses cutout transparency - when you have solid geometry, and an RGBA texture indicates areas that shouldn't be rendered (like for foliage or wire fencing - an alpha threshold sets which areas should be "cut-out" from the rendered mesh)
		// Rendering without cutouts, or batching them into a separate AS from regular surfaces, can enable driver optimizations that ignore space inside the mesh. It also means the driver knows it won't hit cutouts if this AS is bound, so it can avoid checking for them completely and immediately jump to
		// the regular "hit" shading stage when rays intersect geometry (instead of checking for cutouts, checking if an any-hit shader is bound, etc)
		// Need links here
		// ///////////////
		// ///////////////
		//
		// Geometry submitted for AS builds has to contain position data only - it can't have bundled materials, UVs, etc. It can have data such as mesh indices stuffed in the alpha part of each vertex, so an easy way to get that data back is to make read-only buffers of per-vertex mesh data,
		// bind all those along with the AS, and use the mesh indices extracted from the position data to retrieve the correct materials & UVs for each triangle.
		// This approach generalizes well for rasterization - position/ID vbuffers are very low bandwidth, so they can be easily drawn onto a fullscreen target & shaded in a separate pass, kind-of like a discount version of the "deferred rendering" technique.
		// Not actually certain about this - documentation mentions using the last value for stride quite a bit. I'll see whether the embedded IDs work once I get to testing ^_^'
		//
		// Submitted geometry is expected to be in worldspace rather than local-space - although DX offers a utility to do the conversion, its pretty gross, and requires the address of a transform matrix on the GPU. Users can either prepare geometry ahead of time (using a GPU copy + compute pass,
		// making a copy on the CPU side before set-up & transforming the copy's vertices before upload...), or adapt their workflows to use world-space vertices globally instead of local-space.
		//
		// [ibufHandle] takes a pointer because index buffers are optional for AS setup in DX12; AS set-up without an ibuffer will use doubled-up triangles instead, just like with rastered geometry
		// No source data since vbuffer/ibuffer have been allocated already
		static void GenerateAccelStructForGeometry(DataHandle<D3D_VBUFFER> vbufHandle, DataHandle<D3D_IBUFFER>* ibufHandle, DataHandle<D3D_ACCELSTRUCT_BLAS>* blasOut, DataHandle<D3D_ACCELSTRUCT_TLAS>* tlasOut, GPUResrcPermSetGeneric accessSettings, XPlatUtils::AccelStructConfig asConfig, uint32_t pipelineID);

		// Create command-lists to allow work-submission outside the core DX interface
		static DataHandle<D3D_CMD_LIST> CreateCmdList(wchar_t* label);

		// Set root signature & descriptors for the given pipeline
		static void BindComputeResources(DataHandle<D3D_CMD_LIST> pipe_work, DataHandle<D3D_ROOTSIG> rootSig, uint8_t pipelineID); // Uses descriptor tables for now, perhaps freely-accessible heaps ("bindless") in future...more variants needed for GFX & Raytracing	
		static void BindGFX_Resources(DataHandle<D3D_CMD_LIST> pipe_work, DataHandle<D3D_ROOTSIG> rootSig, uint8_t pipelineID);
		//static void BindRaytracingResources(DataHandle<D3D_CMD_LIST> pipe_work, DataHandle<D3D_ROOTSIG> rootSig);

		// Emit API code for dispatching the specified shaders to the specified command-list
		static void SubmitComputeExec(DataHandle<D3D_CMD_LIST> work, uint32_t dispX, uint32_t dispY, uint32_t dispZ, DataHandle<D3D_PSO> pso);
		static void SubmitGraphicsExec(DataHandle<D3D_CMD_LIST> work, uint32_t numNdces, DataHandle<D3D_PSO> pso, uint8_t pipelineID);
		//static void AppendRaytracingExecToPipeline(DataHandle<D3D_CMD_LIST> work, uint32_t dispX, uint32_t dispY, uint32_t dispZ, DataHandle<D3D_PSO> pso);

		// Close a command-list populated with SubmitComputeExec, BindDescriptorHeaps, etc
		static void CloseCmdList(DataHandle<D3D_CMD_LIST> cmds);

		// Reset a command-list populated with SubmitComputeExec, BindDescriptorHeaps, etc
		static void ResetCmdList(DataHandle<D3D_CMD_LIST> cmds);

		// Send a single cmd-list to the gpu
		// issueSynchronous indicates whether to fire-and-forget the given command-list, or whether to wait afterwards and preserve dependencies in future commands
		static void IssueWork(DataHandle<D3D_CMD_LIST> work, bool issueSynchronous, uint8_t pipelineID);

		// Display whatever was most recently rendered to the back-buffer
		// Should probably be called at a fixed interval, once every 30/16ms
		// No other synchronization required (afaik) - double-buffering will do that for us
		static void PresentLastFrame();
};

