#pragma once

#include <stdint.h>
#include "..\CPUMemory.h"
#include "..\Shaders\materials.h"

// Three "basic" ones for now, perhaps more later
enum class SCATTERING_FUNCTIONS
{
	OREN_NAYAR,
	GGX_TRANSLUCENT,
	GGX_REFLECTIVE
};

struct Material
{
	// Roughness is standard 2D greyscale texture
	CPUMemory::ArrayAllocHandle<float> roughnessData;
	uint64_t roughnessDataSize;
	uint16_t roughnessTexX;
	uint16_t roughnessTexY;

	// "texture" composed of multiplexed piecewise curves (see above), with interpolation between each
	// Spectral curves are used both for regular color response in diffuse materials, and for spectral IOR representation in reflective/translucent surfaces
	CPUMemory::ArrayAllocHandle<MaterialSPD_Piecewise> spectralData;
	uint64_t spectralDataSize;
	uint16_t spectralTexX;
	uint16_t spectralTexY;
};

