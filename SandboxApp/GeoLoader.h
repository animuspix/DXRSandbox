#pragma once

#include "Geo.h"
#include "Materials.h"

struct MeshLoadParams
{
	CPUMemory::ArrayAllocHandle<Geo::Vertex3D> outVerts;
	uint64_t* outNumVts;
	uint64_t* outNdces;
	uint64_t* outNumNdces;
	uint64_t inNdxOffset; // Because we use full-scene vertex/index buffers

	CPUMemory::ArrayAllocHandle<MaterialSPD_Piecewise>* outSpectralTexAddr;
	uint64_t* outSpectralTexFootprint;
	uint16_t* outSpectralTexWidth;
	uint16_t* outSpectralTexHeight;

	CPUMemory::ArrayAllocHandle<float>* outRoughnessTexAddr;
	uint64_t* outRoughnessFootprint;

	uint16_t* outRoughnessTexWidth;
	uint16_t* outRoughnessTexHeight;
	uint16_t inMaterialID;
};

class GeoLoader
{
public:
	// Indexed geometry + UVs only - spectral materials can be exported/imported to/from DXRS files
	// Imported objs are white, smooth, and difffuse until modified in the Sandbox and exported as DXRS
	static void LoadObj(const char* path, MeshLoadParams params);

	// Interleaved geometry chunk (minus material ID, model ID), followed by spectral material data + roughness data
	// Model/scene spectra are encoded into "textures" with 2D layout where each pixel is a 128-bit piecewise curve (32 samples uniformly distributed on X, each with four bits/sixteen possible values on Y)
	// Roughness encodes to a regular NxN greyscale texture
	// Just one material per model - multiple materials is doable with layered material functions, but quite complicated to implement and requires ordering + weighting each material for each sample before tracing rays through the surface - not something I can do by trivially
	// providing a material texture
	static void LoadDXRS(const char* path, MeshLoadParams params);
};

