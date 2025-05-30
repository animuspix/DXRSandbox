#define SPATIAL_HASH_PASS
#include "ComputeBindings.hlsli"

uint packMortonCode(uint3 quantizedPos, uint bitCounter, uint packDst)
{
    uint n = quantizedPos[bitCounter % 3]; // Alternate x,y,z every bit
    uint bitOffs = bitCounter / 3; // We write the zeroth bits from each axis, then the first bits from each axis, and so on; so the bit to load changes every 3 iterations (= every three axes)
    uint bitSel = 1u << bitOffs; // Bit selection mask
    uint bit = (n & bitSel) >> bitOffs; // Shift down to 1/0, so we can easily shift into the current output bit position (equal to [k])

    return packDst | (bit << bitCounter);
}

[numthreads(4, 4, 4)] // Completely independent lanes, so go wide on group size
void main( uint3 GTid : SV_GroupThreadID, uint groupIndex : SV_GroupIndex )
{
    // AS resolve has a one-dimensional dispatch, so resolving thread indices is straightforward
    uint index = GTid.x + groupIndex;

    // Drop threads above the scene's tri count
    uint numTris = 0;
    uint vertStride = 0;
    triBuffer.GetDimensions(numTris, vertStride); 
    if (index >= numTris)
    {
        return;
    }

    // Resolve tri indices, corner positions
    IndexedTriangle tri = triBuffer[index];
    float3 vt0 = structuredVBuffer[tri.xyz.x].pos.xyz; // This is why you should separate attributes @.@
    float3 vt1 = structuredVBuffer[tri.xyz.y].pos.xyz;
    float3 vt2 = structuredVBuffer[tri.xyz.z].pos.xyz; 
    float3 centre = (vt0 + vt1 + vt2) * 0.33f;

    // Assumes normalized vertex positions (no premultiplied scale or whatever)
    const uint mortonBitDepth = 10u;
    const uint mortonMask = (1u << mortonBitDepth) - 1;

    // Align to the nearest n/1024 here
    /////////////////////////////////////////////

    uint3 centreMorton = uint3((centre * (1u << mortonBitDepth))); // We want 3 slices in the Morton code, ten bits each
    centreMorton &= mortonMask; // Zero bits 11-31 in each axis

    // Morton hashmap is an 8-bit coarse morton grid, each grid cell containing 16 high-precision morton codes + triangles
    const uint mortonBucketBitDepth = MORTON_HASHMAP_SPATIAL_RES;
    const uint mortonBucketMask = (1u << mortonBucketBitDepth) - 1;
    uint3 mortonBucket = centreMorton & mortonBucketMask;

    // z-values!
    uint mortonBucketPacked = 0;
    uint centreMortonPacked = 0;

    uint kmax = mortonBitDepth * 3u;
    for (uint k = 0; k < kmax; k++)
    {
        centreMortonPacked = packMortonCode(centreMorton, k, centreMortonPacked);

        if (k < mortonBucketBitDepth)
        {
            mortonBucketPacked = packMortonCode(mortonBucket, k, mortonBucketPacked);
        }        
    }

    // See travel diary notes; prefer bucketed hashmap to explicit sorting (should be easier to implement + more efficient)
    // + still better than the weird uniform sorting I was trying before ^_^'
    ///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    // Originally I wanted entries to be ordered, but in retrospect unordered seems better; I can always sort them locally per-thread when we prepare the BVH
    MortonHashPair hashPair;
    hashPair.richMortonKey = centreMortonPacked;
    hashPair.triIndex = index;

    mortonHashmap[mortonBucketPacked].entries[mortonHashmap[mortonBucketPacked].entryCount] = hashPair;
    InterlockedAdd(mortonHashmap[mortonBucketPacked].entryCount, 1);
    return;
}