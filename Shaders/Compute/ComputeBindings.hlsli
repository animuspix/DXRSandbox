#pragma once

#include "..\\SharedConstants.h"
#include "..\\SharedStructs.h"

#define COMPUTE
#include "..\\SharedGeoStructs.h"

#ifndef GPU_PRNG
#include "..\\SharedPRNG_Code.h"
#endif

#include "..\\materials.h"

#ifdef SPATIAL_HASH_PASS
ConstantBuffer<SpatialHashBindings> stageBindings : register(b0);
#endif

ConstantBuffer<ComputeConstants> GetSharedConstants(uint lookup)
{
	return ResourceDescriptorHeap[lookup];
}

RWStructuredBuffer<Vertex3D> GetStructuredVertices(uint lookup)
{
	return ResourceDescriptorHeap[lookup];
}

RWStructuredBuffer<uint> GetAtomicU32s(uint lookup)
{
	return ResourceDescriptorHeap[lookup];
}

RWStructuredBuffer<IndexedTriangle> GetTriBuffer(uint lookup)
{
	return ResourceDescriptorHeap[lookup];
}

RWStructuredBuffer<uint> GetBVH(uint lookup)
{
	return ResourceDescriptorHeap[lookup];
}

// Constantly shuffling these to reflect submission order from CPU
// Easiest option is likely to be bindless (none of these explicit bindings, just a dynamic descriptor bag + some constant lookups)
// sunk cost fallacy!!! ^_^' I don't want to rewrite so much clever binding code 
// though
// time saving hehe
// maybe worth playing with on the flight home hmmm

#ifdef SHADING_PASS
	ConstantBuffer<LightTransportBindings> stageBindings : register(b0);

	Texture2D<float> GetRoughnessTexture(uint lookup)
	{
		return ResourceDescriptorHeap[lookup];
	}

	RWStructuredBuffer<MaterialSPD_Piecewise> GetSpectralTexture(uint lookup)
	{
		return ResourceDescriptorHeap[lookup];
	}

	RWStructuredBuffer<GPU_PRNG_Channel> GetPRNGStreams(uint lookup)
	{
		return ResourceDescriptorHeap[lookup];
	}

	// Very basic filter state for now; if/when I get around to BPT I can make it more sophisticated,
	// but for now we can comfily trace in a single pass each frame, storing accumulated color + filter sums in
	// texOut[xy].xyzw, so we just need this buffer to store the remaining data (sample counts, since we only have
	// four texture channels; we need those so we can stop accumulating once sample-count == spp (or selectively
	// restart sampling for scene interactions))
	RWTexture2D<uint> GetSampleCounter(uint lookup)
	{
		return ResourceDescriptorHeap[lookup];
	}

	RWTexture2D<float4> GetOutputTexture(uint lookup)
	{
		return ResourceDescriptorHeap[lookup];
	}
#endif