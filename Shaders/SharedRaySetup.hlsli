
#ifndef GPU_PRNG
#include "SharedPRNG_Code.h"
#endif

// Mitchell-Netravali implementation (from PBRT v3), with radius == 2 for both axes (samples have range -1,+1)
// First two multiplications overall (0.5, then 2) cancel out (see AAFilter)
float MitchellNetravali1D(float x)
{
    // Just the second branch; x will never be >1
    const float b = 0.666666f;
    const float c = 0.333333f;
    const float sixC = 6.0f * c;
    
    float xSqr = x * x;
    float xCub = xSqr * x;
    return ((12.0f * 9.0f * b - sixC) * xCub +
           (-18.0f + 12.0f * b + sixC) * xSqr +
           (6.0f - 2.0f * b)) * 0.16666666; // Mitchell/Netravali polynomial divides by 1/6th
}

// Anti-aliasing filter function (e.g. triangle, blackman-harris, etc)
// Distinct from a typical spectral filter (e.g. a solar filter, neutral-density, etc)
// [sampleOffset] is the each sample's distance in (-1, +1) from it's home pixel's centroid
// e.g. if a sample (100, 200) is jittered by (-0.2, 0.5), then (-0.2, 0.5) would be its [sampleOffset]
float AAFilter(float2 sampleOffset)
{
    sampleOffset = abs(sampleOffset);
    return MitchellNetravali1D(sampleOffset.x) * MitchellNetravali1D(sampleOffset.y);
}

// Returns approximate perspective ray direction (xyz), filter weight (w), and spectral sample
// Eventually this will use a real lens function ^_^'
//
//                            /|
//                           / |
//                          /  |
//                         /   |
//                        /    | 0.5XY, XY plane quadrants
//                       /     |
//                      /      |
// centre film surface .___r___| centre lens surface
//                      \      |
//                       \     |
//                        \    |
//                         \   | -0.5XY, -XY plane quadrants
//                          \  |
//                           \ |
//                            \|
//
//               r = 0.5y / tan(0.5 * fov); (theta/fov is the vertical angle inside the frustum - think TOA)
//
float4 RaySetup(float2 sampleCoordinate, float fov, float2 imageDims, float spp, GPU_PRNG_ChannelType prngChannel, out float spectralSample)
{ 
    // No need for spp to make the image any larger - I had a brain bug that conflated multisampling with super-sampling, they're different concepts ^_^'
    // Which is to say - spp is a one-dimensional property that sets the number of samples per pixel/film cell
    // Super-sampling AA renders additional pixels, each of which may be multisampled
    // The concepts may be entangled if e.g. your samples per pixel are always squares, or always non-prime, but 2 samples per pixel is two samples per pixel,
    // sampling at 2spp doesn't double your image res (and it wouldn't make sense if it did - 2x supersampling has four subpixels per pixel, not two)
    //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    // Jitter locally
    // Really need some smarter sampling code oof
    // Stratified sampling could be interesting, or R2? 
    // Could be worthwhile in the lens domain (2D)
    // Stratified and R2 both require a sample index to work, iirc
    // https://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences/
    float2 jitter = rand2d(prngChannel); 
    jitter = (jitter - 0.5f.xx) * 2.0f;
    sampleCoordinate += jitter;

    // Compute ray direction (see diagram above)
    imageDims *= 0.5f;
    float r = imageDims.y / tan(fov * 0.5f);
    float2 xy = sampleCoordinate - imageDims;
    float3 v = normalize(float3(xy, r));

    // Compute filter value
    float filterFac = AAFilter(jitter);

    // Compute spectral sample
    spectralSample = rand(prngChannel);

    // return ray & filter value
    return float4(v, filterFac);
}