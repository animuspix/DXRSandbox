#pragma once

#include "..\Pipeline.h"
#include "..\Shaders\SharedStructs.h"
#include "Materials.h"

// Constructs & stores command-lists for compute, hybrid, and fixed-function RT pipelines, then invokes them through DXWrapper::DrawFrame()
class Render
{
	public:
		struct FrameConstants
		{
			float screenWidth, screenHeight; // Screen width, screen height, current time, current deltatime
			float timeSeconds;
			float fov, focalDepth, aberration;
			uint16_t spp;
			FilmSPD_Piecewise filmSPD;
			float4 sceneBoundsMin, sceneBoundsMax;
			transform cameraTransform; // Camera position/rotation
			transform sceneTransforms[MAX_SUPPORTED_OBJ_TRANSFORMS];
			uint16_t numTransforms;
		};

		enum class RENDER_MODE
		{
			MODE_COMPUTE,
			MODE_HYBRID,
			MODE_SHADER_TABLES
		};

		// Command-lists generated here
		void Init(HWND hwnd, RENDER_MODE mode, XPlatUtils::BakedGeoBuffers& sceneGeo, XPlatUtils::BakedGeoBuffers& viewGeo, CPUMemory::ArrayAllocHandle<Material> sceneMaterials, uint32_t sceneMaterialCount, CPUMemory::SingleAllocHandle<FrameConstants> frameConstants);

		// Update constant buffer data (e.g. time, film SPD, camera transforms...)
		void UpdateFrameConstants(CPUMemory::SingleAllocHandle<FrameConstants> frameConstants);

		// Issue generated command-lists to the GPU
		void Draw();

	private:
		// Current rendering mode
		RENDER_MODE currMode;

		// Very simple frame abstraction (not a frame graph!) to bucket pipelines associated with the same render modes together
		// The presentation pipeline is duplicated this way, but I think that's probably better than managing the complexity of having three frames feeding into the same
		// final stage
		template<uint32_t numStages>
		struct Frame
		{
			Pipeline pipes[numStages];
		};

		// Possible frame layouts
		Frame<3> compute_frame; // AS generation, ubershader, presentation
		Frame<3> hybrid_frame; // Primary rays, ubershader, presentation
		Frame<2> shader_table_frame; // Ray/path dispatch, presentation
};

