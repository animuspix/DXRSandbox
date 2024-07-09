#ifndef RAYTRACING_HLSL
#define RAYTRACING_HLSL
#endif

#include "ShaderTableBindings.hlsli"
#include "..\\shader_math.hlsli"

struct PathVt
{
    float sampledSpectrum; // Normalized photometric spectral sample (0-1 maps 700-400nm)
    float spectralWeight; // Spectral scene response
    float pdf; // The likelihood of of each specific incoming/outgoing ray pair re-emitting from their worldspace position;
               // used for monte-carlo averaging in path tracing
               // (the expectation for each film grain is the sum of each light contribution divided by the its probability)
    float currIOR; // IOR tracking, for accurate refraction (ambient IOR will change underwater, f.ex)
};

[shader("raygeneration")]
void camera_rays()
{
    // Ray spawn
    PathVt path;

    // Camera directions...
    ///////////////////////

    // Ray-desc & traceray call
    ///////////////////////////

    // Considering russian-roulette here (bounce rays until probability < epsilon or weight < epsilon) instead of table recursion, but need to get further with infrastructure first
    // Russian roulette will be less biased & possibly cleaner, but might also be slower for complex scenes (maybe, definitely needs testing to make sure)
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    // Staging output inside the camera pass? @.@
    // Whatever you say magic internet samples
    output[DispatchRaysIndex().xy] = float4(Spectrum2Color((path.sampledSpectrum * path.spectralWeight) / path.pdf), 1.0f); // No fancy BDPT or metropolis tricks, no need to store paths/merge vertices <3
}

// Material functions
/////////////////////

[shader("closesthit")]
void hit(inout PathVt path)
{
    // Ray hit something!
    // Flick through materials and compute the best one to use for the current intersection

    // Spooky triangle lookup
    /////////////////////////
    uint triNdx = 0; // mystical HLSL call
    Triangle tri = geom_materials[triNdx];

    // Material search & invoke
    // Unknown materials are treated as diffuse
    // (might use a fixed "error" SPD as well, unsure atm)
    switch (tri.bxdf_id)
    {
        case BXDF_ID_DIFFUSE_LAMBERT:
            // Unimplemented atm
            break;
        case BXDF_ID_DIFFUSE_OREN_NAYAR:
            // Unimplemented atm
            break;
        case BXDF_ID_DIFFUSE_OREN_NAYAR_MULTISCATTER:
            // Unimplemented atm
            break;
        case BXDF_ID_SPECULAR_CLASSICAL_SMOOTH:
            // Unimplemented atm
            break;
        case BXDF_ID_SPECULAR_COOK_TORRANCE:
            // Unimplemented atm
            break;
        case BXDF_ID_SPECULAR_COOK_TORRANCE_MULTISCATTER:
            // Unimplemented atm
            break;
        case BXDF_ID_DIFFUSE_VOLUMETRIC_ORGANIC:
            // Unimplemented atm
            break;
        case BXDF_ID_DIFFUSE_VOLUMETRIC_CLOUDY:
            // Unimplemented atm
            break;
        case BXDF_ID_DIFFUSE_VOLUMETRIC_ORGANIC_MICROFLAKES:
            // Unimplemented atm
            break;
        case BXDF_ID_DIFFUSE_VOLUMETRIC_CLOUDY_MICROFLAKES:
            // Unimplemented atm
            break;
        default:
            // Default to lambertian diffuse
            break;
    }
};

void ResolveSkySPD(out SPDRaw spd)
{
    // Beeg spike around the blue part of the spectrum
    // Hacked around with desmos, not ideal but works ^_^'
    for (uint i = 0; i < 16; i++)
    {
        float t = i / 16.0f;
        if (t > 0.2f && t < 0.514)
        { 
            spd.spectra[i] = sin((t * 10) - 2);
        }
    }
}

[shader("miss")]
void miss(inout PathVt path)
{
    // Ray hit nothing, take a sky sample (pain)
    // Sky might eventually be a properly simulated atmosphere, but that's a massive side-track I don't want to go down right now
    // Instead - beeg soft blue diffuse dome
    SPDRaw spud;
    ResolveSkySPD(spud);
    path.spectralWeight *= ResolveSPDResponse(path.sampledSpectrum, spud);
    path.pdf *= ONE_OVER_2PI; // Assumes miss ray probability corresponds to uniform sampling on a hemisphere
                              // Very shaky logically, but more grounded than assuming uniform pdfs or whatever
}