#pragma once

#include "..\Pipeline.h"
#include "..\Shader.h"
#include "..\Shaders\SharedStructs.h"
#include "Materials.h"
#include "Frame.h"

#include <functional>
#include <type_traits>

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
		transform sceneTransform; // Model position/rotation/scale
	};

	enum class RENDER_MODE
	{
		MODE_COMPUTE,
		MODE_HYBRID,
		MODE_SHADER_TABLES
	};

	// Command-lists generated here
	void Init(HWND hwnd, RENDER_MODE mode, XPlatUtils::BakedGeoBuffers& sceneGeo, XPlatUtils::BakedGeoBuffers& viewGeo, Material& material, CPUMemory::SingleAllocHandle<FrameConstants> frameConstants);

	// Update constant buffer data (e.g. time, film SPD, camera transforms...)
	void UpdateFrameConstants(CPUMemory::SingleAllocHandle<FrameConstants> frameConstants);

	// Issue generated command-lists to the GPU
	void Draw();

private:
	// Current rendering mode
	RENDER_MODE currMode;

	// Constants to easily access each compute stage
	enum COMPUTE_STAGES
	{
		COMPUTE_SPATIAL_HASHING,
		COMPUTE_LT,
		COMPUTE_BLIT,
		COMPUTE_NUM_STAGES
	};

	enum HYBRID_STAGES
	{
		HYBRID_PRIMARY_RAYS,
		HYBRID_LT,
		HYBRID_BLIT,
		HYBRID_NUM_STAGES
	};

	enum SHADER_TABLE_STAGES
	{
		SHADER_TABLE_DISPATCH,
		SHADER_TABLE_BLIT,
		SHADER_TABLE_NUM_STAGES
	};

	// Possible frame layouts
	Frame<COMPUTE_STAGES::COMPUTE_NUM_STAGES, (uint32_t)RENDER_MODE::MODE_COMPUTE> compute_frame; // Sptial hashing, ubershader, presentation
	Frame<HYBRID_STAGES::HYBRID_NUM_STAGES, (uint32_t)RENDER_MODE::MODE_HYBRID> hybrid_frame; // Primary rays, ubershader, presentation
	Frame<SHADER_TABLE_STAGES::SHADER_TABLE_NUM_STAGES, (uint32_t)RENDER_MODE::MODE_COMPUTE> shader_table_frame; // Ray/path dispatch, presentation
};

