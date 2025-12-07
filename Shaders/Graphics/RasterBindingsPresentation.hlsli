#include "..\\SharedConstants.h"
#include "..\\SharedStructs.h"
#include "..\\SharedGeoStructs.h"

#ifdef PIXEL

#ifdef PRESENTING_COMPUTE
ConstantBuffer<ComputePresentationBindings> stageBindings : register(b0);
#endif

Texture2D<float4> GetColorData(uint lookup)
{
	return ResourceDescriptorHeap[lookup];
}

SamplerState frame_sampler_point : register(s0);
SamplerState frame_sampler_linear : register(s1);
#endif