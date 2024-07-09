
// CPU-only settings
#ifdef _WIN32
using uint = uint32_t;
#include "..\Math.h"
#pragma once
#endif

#ifdef _WIN32
#define MATERIAL_SPD_POINTS 16
#else
#define MATERIAL_SPD_POINTS 4
#endif

#define MATERIAL_SPD_BITS 128

struct MaterialSPD_Piecewise
{
#ifdef _WIN32
    // Each curve sample contains 32 points uniformly distributed on X (0-1), and each of those has 16 degrees of freedom on Y (four bits)
    // Spectral curves can just (barely) be encoded 1:1 using uint32_t texture objects on the GPU
    uint8_t points[MATERIAL_SPD_POINTS]; // Byte-wide type, 16 points, 128 bits
#else
    uint points[MATERIAL_SPD_POINTS]; // Four-byte type, 4 points, 128 bits
#endif
};

#ifndef _WIN32 // GPU-only code
// Spectrum is normalized photometric (so 0-1 by the time we get here, not 740-440nm)
float ResolveMaterialSPDResponse(float spectralSample, MaterialSPD_Piecewise matSPD)
{
    //uint bitRange = spectralSample * MATERIAL_SPD_BITS;
	//float interval = (FILM_SPD_NUM_SAMPLES - 1) * spectralSample;
	//uint closestConstraint = floor(interval);
	//neighbour = closestConstraint < 255 ? closestConstraint + 1 : closestConstraint - 1;  
	//centroid = closestConstraint;
	//blendFac = interval - closestConstraint;
    // Oops, forgot I hadn't fixed this ^_^'
    return 1.0f;
}

#include "spectralCurveImplementations.h"

#else

float polyMulBinomial(float x, float a, float b);
float quadratic(float x, float a, float b, float c, bool invert);
float gaussian(float x, float a, float b, float c, float d);

#endif