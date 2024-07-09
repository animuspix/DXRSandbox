#pragma once

// Needs to take a source file location + input/output resources and generate a PSO
#include "RasterSettings.h"
#include "DXWrapper.h"
#include "GPUResource.h"
#include <type_traits>
#include <cassert>
#include <string>

enum class SHADER_TYPES
{
	COMPUTE,
	GRAPHICS,
	RAYTRACING
};

// GPU resources define textures/buffers, but not their descriptors; those are generated per-pipeline by the DXWrapper and placed in a range that covers every effect needed through the end of that pipeline's commandlist

template<SHADER_TYPES shaderType>
struct Shader
{
	public:
		static constexpr uint32_t numSrcFiles = shaderType == SHADER_TYPES::COMPUTE ? 1 :
												shaderType == SHADER_TYPES::GRAPHICS ? 2 :
												/*shaderType == SHADER_TYPES::RAYTRACING ? */ 1; // Just one object file for compute, two for graphics (pixel, vertex), three for raytracing (raygen, closest-hit, miss)
												// Tempting to use any-hit for alpha clipping, but not expecting to use that (yet) in this project
		struct ComputeShaderDesc
		{
			const char* precompiledSrcFilenames[numSrcFiles];
			DXWrapper::DataHandle<D3D_ROOTSIG> descriptors;
		};
		struct GraphicsShaderDesc
		{
			const char* precompiledSrcFilenames[numSrcFiles];
			DXWrapper::DataHandle<D3D_ROOTSIG> descriptors;
			DXWrapper::DataHandle<D3D_RASTER_INPUT_LAYOUT> ilayout;
			RasterSettings gfxSettings;
			DXWrapper::RasterBindlist rasterBindings;
		};
		struct RaytracingShaderDesc
		{
			const char* precompiledSrcFilenames[numSrcFiles];
			const wchar_t* raygenStageName;
			const wchar_t* closestHitStageName;
			const wchar_t* missStageName;
			DXWrapper::DataHandle<D3D_ROOTSIG> descriptors;
			uint32_t maxShaderAttributeByteSize;
			uint32_t maxRayPayloadByteSize;
			uint32_t recursionDepth;
		};
		using shader_desc = typename std::conditional<shaderType == SHADER_TYPES::COMPUTE, ComputeShaderDesc,
												      typename std::conditional<shaderType == SHADER_TYPES::GRAPHICS, GraphicsShaderDesc,
																				RaytracingShaderDesc>::type>::type;

	private:
		struct ComputePSOGenerator
		{
			void operator()(ComputeShaderDesc desc, DXWrapper::DataHandle<D3D_PSO>* pso_out, uint32_t pipelineID)
			{
				*pso_out = DXWrapper::GenerateComputePSO(desc.precompiledSrcFilenames[0], desc.descriptors, pipelineID);
			}
		};

		struct GraphicsPSOGenerator
		{
			void operator()(GraphicsShaderDesc desc, DXWrapper::DataHandle<D3D_PSO>* pso_out, uint32_t pipelineID)
			{
				*pso_out = DXWrapper::GenerateGraphicsPSO(desc.precompiledSrcFilenames[0], desc.precompiledSrcFilenames[1], desc.gfxSettings, desc.rasterBindings, desc.ilayout, desc.descriptors, pipelineID);
			}
		};

		struct RaytracingPSOGenerator
		{
			void operator()(RaytracingShaderDesc desc, DXWrapper::DataHandle<D3D_PSO>* pso_out, uint32_t pipelineID)
			{
				*pso_out = DXWrapper::GenerateRayPSO(desc.precompiledSrcFilenames[0], desc.raygenStageName, desc.closestHitStageName, desc.missStageName, desc.maxShaderAttributeByteSize, desc.maxRayPayloadByteSize, desc.recursionDepth, desc.descriptors, pipelineID);
			}
		};

		using shader_gen = typename std::conditional<shaderType == SHADER_TYPES::COMPUTE, ComputePSOGenerator,
													 typename std::conditional<shaderType == SHADER_TYPES::GRAPHICS, GraphicsPSOGenerator,
																			   RaytracingPSOGenerator>::type>::type;
	public:
		Shader(shader_desc desc, uint32_t pipelineID) // Files are expected in the /shaders/ subdirectory
		{
			// Branching pso setup at compile time, depending on [shaderType]
			shader_gen()(desc, &pso, pipelineID);
		}

		SHADER_TYPES type = shaderType;
		DXWrapper::DataHandle<D3D_PSO> pso;
};
