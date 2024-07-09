#pragma once

#include "..\\SharedConstants.h"
#include "..\\SharedStructs.h"

#define COMPUTE
#include "..\\SharedGeoStructs.h"

#ifndef GPU_PRNG
#include "..\\SharedPRNG_Code.h"
#endif

#include "..\\materials.h"

struct ComputeConstants
{
	GenericRenderConstants screenAndLensOptions;
};

ConstantBuffer<ComputeConstants> computeCBuffer : register(b0);

// Constantly shuffling these to reflect submission order from CPU
// Easiest option is likely to be some kind of abstraction that can drive GPU binding orders procedurally, but not sure how yet
// (easiest is likely to be a formatting pre-pass that runs over bindings files and merges #ifdef blocks (e.g. AS_RESOLVE_PASS, SHADING_PASS) + 
// sorts structbuffer/texture/rwtexture bindings into consistent orderings matching those expected by DXRWrapper/VKWrapper)

RWStructuredBuffer<Vertex3D> structuredVBuffer : register(u0); 

#ifdef SHADING_PASS
	Texture2D<float> roughnessAtlas : register(t0);
	RWStructuredBuffer<MaterialPropertyEntry> materialTable : register(u1);
	RWStructuredBuffer<MaterialSPD_Piecewise> spectralAtlas : register(u2);
#endif

#ifdef AS_RESOLVE_PASS
	RWStructuredBuffer<IndexedTriangle> triBuffer : register(u1); // Indexed by grid acceleration structure
	RWStructuredBuffer<ComputeAS_Node> octreeAS : register(u2); // Read by shading passes, written by AS resolve
	RWStructuredBuffer<GPU_PRNG_Channel> prngPathStreams : register(u3);
#else
#ifdef SHADING_PASS
	RWStructuredBuffer<IndexedTriangle> triBuffer : register(u3); // Indexed by grid acceleration structure
	RWStructuredBuffer<ComputeAS_Node> octreeAS : register(u4); // Read by shading passes, written by AS resolve
	RWStructuredBuffer<GPU_PRNG_Channel> prngPathStreams : register(u5);
#endif
#endif

#ifdef SHADING_PASS
	// Very basic filter state for now; if/when I get around to BPT I can make it more sophisticated,
	// but for now we can comfily trace in a single pass each frame, storing accumulated color + filter sums in
	// texOut[xy].xyzw, so we just need this buffer to store the remaining data (sample counts, since we only have
	// four texture channels; we need those so we can stop accumulating once sample-count == spp (or selectively
	// restart sampling for scene interactions))
	RWTexture2D<uint> sampleCountsPerPixel : register(u6);

	RWTexture2D<float4> texOut : register(u7);
#endif