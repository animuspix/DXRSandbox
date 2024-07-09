
#if __SHADER_TARGET_STAGE == __SHADER_STAGE_VERTEX
#define VERTEX
#elif __SHADER_TARGET_STAGE == __SHADER_STAGE_PIXEL
#define PIXEL
#endif

#define PRESENTING_COMPUTE
#include "RasterBindingsPresentation.hlsli"

struct vtOut
{
    float4 pos : SV_POSITION;
    float4 uv : TEXCOORD0;
};

#ifdef VERTEX
vtOut main_vs(Vertex2D vtIn)
{
    vtOut o;
    o.pos = float4(vtIn.pos.xy, 0.0f, 1.0f);
    o.uv = vtIn.uv;
    return o;
}
#endif

#ifdef PIXEL
float4 main_ps(vtOut px) : SV_TARGET
{
    return frame_target.Sample(frame_sampler_linear, float2(px.uv.x, 1.0f - px.uv.y));
}
#endif