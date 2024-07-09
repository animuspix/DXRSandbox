#include "..\\SharedConstants.h"
#include "..\\SharedStructs.h"
#include "..\\SharedGeoStructs.h"

#ifdef PIXEL

#ifdef PRESENTING_COMPUTE
struct ComputeConstants
{
	GenericRenderConstants screenAndLensOptions;
};

ConstantBuffer<ComputeConstants> computeCBuffer : register(b0);
#endif

Texture2D<float4> frame_target : register(t0);
SamplerState frame_sampler_point : register(s0);
SamplerState frame_sampler_linear : register(s1);
#endif