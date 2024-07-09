#pragma once

#include <stdint.h>

namespace XPlatConstants
{
	static constexpr uint32_t maxNumPipelines = 16;
	static constexpr uint32_t maxResourcesPerPipeline = 32;
	static constexpr uint32_t maxVBufferStride = 1024;
	static constexpr uint32_t eltSizeInBytes = 16; // Assumes all vertex elements are vec4 (smaller elements & per-vertex matrices are unsupported by DXRSandbox)
	static constexpr uint32_t maxEltsPerVertex = maxVBufferStride / eltSizeInBytes;
	static constexpr uint32_t maxNumGfxShaders = 32; // Random arbitrary number, could be anything
	static constexpr uint32_t maxNumRaytracingShaders = 32; // Random arbitrary number, could be anything
	static constexpr uint32_t maxNumComputeShaders = 32; // Random arbitrary number, could be anything
	static constexpr uint32_t numBackBuffers = 2;
	static constexpr uint32_t maxNumRenderTargetsPerPipeline() // Additional render-targets beyond this can be bound as read/write textures
	{
#ifdef DX12
		return 8;  // Fixed by the D3D12_GRAPHICS_PIPELINE_STATE_DESC definition (see: https://docs.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_graphics_pipeline_state_desc)
#else 
		// We should figure this out before we can port other APIs (such as VK)
#error
#endif
	}
};

namespace XPlatUtils
{
	struct AccelStructConfig
	{
		bool hasCutouts;
		bool updatable; // Allow updating the AS through [API::UpdateAccelStruct] (unimplemented atm). This can be faster than performing a full rebuild.
		bool minimal_footprint; // Request the driver to minimize the AS's memory footprint, possibly with a perf hit for hit tests/AS build

		// Acceleration structures can be built quickly, or perform fast hit tests, but not both
		enum class AS_PERF_PRIORITY
		{
			FAST_TRACE,
			FAST_BUILD
		};

		AS_PERF_PRIORITY perfPriority;
	};
}
