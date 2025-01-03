#define AS_RESOLVE_PASS
#include "ComputeBindings.hlsli"

bool aabbTest(float3 pt, float3 minBounds, float3 maxBounds)
{
    return all(pt > minBounds) && all(pt < maxBounds);
}

uint getMaxBVHRanks(uint numTris)
{
    while (numTris > 0)
    {
        numTris /= AS_NODE_CHILDCOUNT;
    }

    return numTris;
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
    uint mortonBitDepth = 10u;
    uint3 centreMorton = uint3((centre * float3(1u << mortonBitDepth))); // We want 3 slices in the Morton code, ten bits each
    centreMorton &= 1023; // Zero bits 11-31 in each axis

    // Interleave!
    // First two axes are easy (every other bit, ten bits -> 20)
    // Third axis probably interleaves every third, so each set of three bits gives an octant coordinate
    // (x yes/no, y yes/no, z yes/no)
    // Still thinking that through on paper

    // 01, 00, 00 smallest possible coordinate besides zero on a 3x3x3 grid
    // 01 op 00 gives 0001 (1)
    // 0001 op 00 gives 000 010 (2) ?? 

    // Alternative is regular interleaving, which keeps 1 constant
    // 01 op 00 gives 0001, 0001 op 00 gives 000, 001

    // Next in 2D would be 0,1, lets see...
    // 00 op 01 = 0010 (2)
    // 0010 op 00 gives 000, 010 (2)

    // Seems like regular interleaving is the way?

    // 01 op 01 = 0011 (3)
    // 0011 op 00 = 000, 011 (3)

    // 11 op 11 = 1111 (16)
    // 1111 op 00 gives 101011 (43)

    // hmmm! should check NV source when possible, though iirc it agrees with what I thought initially (every third for Z)
    
    // Worked out on paper with a simple 2x2x2 cube (0-1 on each axis); confirmed we interleave every 2nd for x and y, every third for z
    // Algorithm is like:
    // (x op y) op z, as written above
    //
    // So 00,01,11 would be:
    // 0001 op 11
    // which is 001, 011 or 001011
    // 
    // Even though it's very unintuitive written out like this, with diagrams and a fully enumerated + traceable set that reveals the z-curve
    // (like my cube) it's much more understandable, and the math does make sense
    // 
    // Hopefully can get back to it (& tidy my notes for Bsky) on the next flight ^^
    // bed times now zzzzzz

    // z-values!
    uint centreMortonPacked = 0;

    uint kmax = mortonBitDepth * 3u;
    for (uint k = 0; k < kmax; k++)
    {
        // Can likely combine div/mod here
        // See testbed! Found a reliable CPU algorithm I can translate hopefully ^_^)

        uint n = centreMorton[k % 3]; // Alternate x,y,z every bit
        uint bitOffs = k / 3; // We write the zeroth bits from each axis, then the first bits from each axis, and so on; so the bit to load changes every 3 iterations (= every three axes)
        uint bitSel = 1u << bitOffs; // Bit selection mask
        uint bit = (n & bitSel) >> bitOffs; // Shift down to 1/0, so we can easily shift into the current output bit position (equal to [k])

        centreMortonPacked |= bit << k;
    }

    // Hijack the first index in the first tri to find the largest Morton code
    // (once we know that we can sort into Morton order by renormalizing against the tri count, which is all we need for
    // the LBVH implementation & binary-search traversal - the actual numbers are cool but unimportant (afaik))

    // Zero the first index/first tri, then synchronise
    triBuffer[0].xyz.x = 0;
    AllMemoryBarrierWithGroupSync();

    // Compute max morton code and sync again
    InterlockedMax(triBuffer[0].xyz.x, centreMortonPacked);
    AllMemoryBarrierWithGroupSync();

    uint maxMorton = triBuffer[index].xyz.x;
    float mortonRelative = float(centreMortonPacked) / float(maxMorton);
    float sortingNdx = mortonRelative * numTris;

    // Sort! (then sync again)
    triBuffer[sortingNdx] = tri;

    // Is this the best sorting model we can do? Need to reconsider
    ///////////////////////////////////////////////////////////////
  
    // First pass; sort triangles into boxes
    // (by locality, non-local tris get sent to spare nodes for simplicity - a more sophisticated algorithm
    // would run over all the non-local tris and compare them against all the bottom-most boxes, maybe
    // something for another day)


    for (uint i = 0; i < AS_NODE_CHILDCOUNT; i++)
    {
        IndexedTriangle tri = triBuffer[index + i];
        
        float4 vt0 = structuredVBuffer[tri.xyz.x].pos.xyz;
        float4 vt1 = structuredVBuffer[tri.xyz.y].pos.xyz;
        float4 vt2 = structuredVBuffer[tri.xyz.z].pos.xyz;

        // Radial test might be more effective than simple connectivity test (checking indices)
        // We care more about whether tris are relatively close than whether they're literally touching

        // This algorithm still follows indices in regular, non-meshletized geometry, so it's prone
        // to long & inefficient strips

        // An alternative would be some kind of clustering algorithm;
    }

    // Memory barrier
    AllMemoryBarrierWithGroupSync(); // Might be overkill

    // Recurrent passes; sort boxes into bigger boxes
    uint maxRanks = getMaxBVHRanks(numTris);

    for (uint rankNdx = 0; rankNdx < maxRanks; rankNdx++)
    {

        AllMemoryBarrierWithGroupSync(); // Might be overkill
    }
}