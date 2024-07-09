// Highly compressed spectral-power-distribution representation
// Each bucket (x, y, z, w) contains up to four independent graph slices, which each carry an
// value from 0-1 (eight-bit normalized, so each increment is 1/256)
struct SPDCompressed
{
    uint X; // Channels 0-3, eight bits each 
    uint Y; // Channels 4-7
    uint Z; // Channels 8-11
    uint W; // Channels 12-15
};

struct SPDRaw
{
    float spectra[16];    
};

SPDRaw SPDUnpack(SPDCompressed spd)
{
    float spectra[16] = {};
    spectra[0] = ((spd.X >> 24) << 24) / 255.5f;
    spectra[1] = ((spd.X >> 16) << 24) / 255.5f;
    spectra[2] = ((spd.X >> 8) << 24) / 255.5f;
    spectra[3] = ((spd.X >> 0) << 24) / 255.5f;

    spectra[4] = ((spd.Y >> 24) << 24) / 255.5f;
    spectra[5] = ((spd.Y >> 16) << 24) / 255.5f;
    spectra[6] = ((spd.Y >> 8) << 24) / 255.5f;
    spectra[7] = ((spd.Y >> 0) << 24) / 255.5f;

    spectra[8] = ((spd.Z >> 24) << 24) / 255.5f;
    spectra[9] = ((spd.Z >> 16) << 24) / 255.5f;
    spectra[10] = ((spd.Z >> 8) << 24) / 255.5f;
    spectra[11] = ((spd.Z >> 0) << 24) / 255.5f;

    spectra[12] = ((spd.W >> 24) << 24) / 255.5f;
    spectra[13] = ((spd.W >> 16) << 24) / 255.5f;
    spectra[14] = ((spd.W >> 8) << 24) / 255.5f;
    spectra[15] = ((spd.W >> 0) << 24) / 255.5f;
    
    SPDRaw ret;
    ret.spectra = spectra;
    return ret;
}

SPDCompressed SPDPack(SPDRaw spd)
{
    SPDCompressed spdPacked;
    spdPacked.X |= (uint(spd.spectra[0] * 255.5f) >> 24);
    spdPacked.X |= (uint(spd.spectra[4] * 255.5f) >> 16);
    spdPacked.X |= (uint(spd.spectra[8] * 255.5f) >> 8);
    spdPacked.X |= (uint(spd.spectra[12] * 255.5f) >> 0);

    spdPacked.Y |= (uint(spd.spectra[4] * 255.5f) >> 24);
    spdPacked.Y |= (uint(spd.spectra[5] * 255.5f) >> 16);
    spdPacked.Y |= (uint(spd.spectra[6] * 255.5f) >> 8);
    spdPacked.Y |= (uint(spd.spectra[7] * 255.5f) >> 0);

    spdPacked.Z |= (uint(spd.spectra[8] * 255.5f) >> 24);
    spdPacked.Z |= (uint(spd.spectra[9] * 255.5f) >> 16);
    spdPacked.Z |= (uint(spd.spectra[10] * 255.5f) >> 8);
    spdPacked.Z |= (uint(spd.spectra[11] * 255.5f) >> 0);

    spdPacked.W |= (uint(spd.spectra[12] * 255.5f) >> 24);
    spdPacked.W |= (uint(spd.spectra[13] * 255.5f) >> 16);
    spdPacked.W |= (uint(spd.spectra[14] * 255.5f) >> 8);
    spdPacked.W |= (uint(spd.spectra[15] * 255.5f) >> 0);
    return spdPacked;
}

// Spectrum is normalized photometric (so 0-1 by the time we get here, not 740-440nm)
float ResolveSPDResponse(float sampledSpectrum, SPDRaw spd)
{
    float graphSpaceSpectrum = sampledSpectrum * 16;
    uint lo = graphSpaceSpectrum;
    uint hi = lo + 1; // Assumes sampled spectrum is never >=1 (would be a bug already if it was, since that's right at the edge of the visible distribution)
    float t = frac(graphSpaceSpectrum);
    return lerp(spd.spectra[lo], spd.spectra[hi], t);
}

// Shading functions supported by DXRSandbox
// More planned? Maybe! Probably not though, I might look at layering support but otherwise this is a lot already
#define BXDF_ID_DIFFUSE_LAMBERT 0
#define BXDF_ID_DIFFUSE_OREN_NAYAR 1
#define BXDF_ID_DIFFUSE_OREN_NAYAR_MULTISCATTER 2
#define BXDF_ID_SPECULAR_CLASSICAL_SMOOTH 3
#define BXDF_ID_SPECULAR_COOK_TORRANCE 4
#define BXDF_ID_SPECULAR_COOK_TORRANCE_MULTISCATTER 5
#define BXDF_ID_DIFFUSE_VOLUMETRIC_ORGANIC 6
#define BXDF_ID_DIFFUSE_VOLUMETRIC_CLOUDY 7
#define BXDF_ID_DIFFUSE_VOLUMETRIC_ORGANIC_MICROFLAKES 8
#define BXDF_ID_DIFFUSE_VOLUMETRIC_CLOUDY_MICROFLAKES 9

struct Vertex
{
    float4 p; // No need to encode normals, we load 3x verts at a time so a cross product is natural
              // Fourth axis encodes BXDF/material ID (diffuse, specular, PBR, volumetric, etc.)

    // Spectral material properties 
    // These four should be enough to express anything except exotic specialized BXDFs (hair/fur, snow, etc.)
    // No UVs/texture support because custom material properties already means custom editor and generating procedural UVs (even worse, tooling them) seems like pain
    // Should be easy enough to hardcode these for my actual Workday goals tho, so no editor implementation for a while
    SPDCompressed spectral_roughness; // Roughness is ignored for classical materials
    SPDCompressed spectral_response;
    SPDCompressed spectral_transmission;
    SPDCompressed spectral_ior;
};