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
//    uint index = GTid.x + groupIndex;
//
//    ConstantBuffer<ComputeConstants> sharedConstants = GetSharedConstants();
//    RWStructuredBuffer<uint> atomics = GetAtomicU32s(sharedConstants.computeResourceKeys.atomicsLookup);
//    RWStructuredBuffer<uint> bvhAS = GetBVH(sharedConstants.computeResourceKeys.bvhLookup);
//    RWStructuredBuffer<IndexedTriangle> triBuffer = GetTriBuffer(sharedConstants.computeResourceKeys.triBufferLookup);
//    RWStructuredBuffer<Vertex3D> structuredVBuffer = GetStructuredVertices(sharedConstants.computeResourceKeys.structuredVBufferLookup);    
//
//    // Drop threads above the scene's tri count
//    uint numTris = 0;
//    uint vertStride = 0;
//    triBuffer.GetDimensions(numTris, vertStride); 
//    if (index >= numTris)
//    {
//        return;
//    }
//
//    // Resolve tri indices, corner positions
//    IndexedTriangle tri = triBuffer[index];
//    float3 vt0 = structuredVBuffer[tri.xyz.x].pos.xyz; // This is why you should separate attributes @.@
//    float3 vt1 = structuredVBuffer[tri.xyz.y].pos.xyz;
//    float3 vt2 = structuredVBuffer[tri.xyz.z].pos.xyz; 
//    float3 centre = (vt0 + vt1 + vt2) * 0.33f;
//
//    // Assumes normalized vertex positions (no premultiplied scale or whatever)
//    const uint mortonBitDepth = 10u;
//    const uint mortonScale = 1u << mortonBitDepth;
//    const uint mortonMask = mortonScale - 1;
//
//    // Round positions to the nearest multiple of 1/1024; might slightly reduce FP error to do this with little subs/adds
//    // and not after scaling up to grid resolution (as before, though the rounding was really flooring there, and more of 
//    // a side-effect than an intentional part of the algorithm)
//
//    // Compute bounds
//    float mortonScaleRcp = 1.0f / float(mortonScale);
//    float3 mortonCellFloor = (centre * float(mortonScale)) * mortonScaleRcp;
//    float3 mortonCellCeiling = mortonCellFloor + (mortonScaleRcp).xxx;
//    float3 mortonCellCentre = mortonCellCeiling - mortonCellFloor;
//    
//    // Actual rounding-to-nearest (instead of implicit round-to-floor we did before)
//    // Centroid goes to floor...not really any nice way to handle that case ^_^'
//    float3 mortonCellCorners[8] = { mortonCellFloor, // Bottom-left-front
//                                    float3(mortonCellCeiling.x, mortonCellFloor.yz), // Bottom-right-front
//                                    float3(mortonCellFloor.x, mortonCellCeiling.y, mortonCellFloor.z), // Top-left-front 
//                                    float3(mortonCellCeiling.xy, mortonCellFloor.z), // Top-right-front
//                                    float3(mortonCellFloor.xy, mortonCellCeiling.z), // Bottom-left-back
//                                    float3(mortonCellCeiling.x, mortonCellFloor.y, mortonCellCeiling.z), // Bottom-right-back
//                                    float3(mortonCellFloor.x, mortonCellCeiling.yz), // Top-left-back
//                                    mortonCellCeiling }; // Top-right-back
//
//    // Nearest neighbour rounding ^_^
//    // Probably overkill, I was just thinking of a better integer rounding function
//    // I don't mind this though
//    // All our numbers are in the range (0-(1/1024)), so a distance of 1 is impossible ^_^' no need for INF or random big numbers here
//    // Using squared distance instead of regular, probablyyy needless but idk
//    float3 centreSnapped = centre;
//    float minCornerDist = 1.0f;
//    for (int i = 0; i < 8; i++)
//    {
//        float3 corner = mortonCellCorners[i];
//        float3 v = centre - corner;
//        float dist2 = dot(v, v);
//        if (dist2 < minCornerDist)
//        {
//            centreSnapped = corner;
//            minCornerDist = dist2;
//        }
//    }
//    
//    // Convert to integer grid coordinate (at Morton scale), so we can get our z-value/morton code
//    uint3 centreMorton = uint3((centre * (1u << mortonBitDepth))); // We want 3 slices in the Morton code, ten bits each
//    centreMorton &= mortonMask; // Zero bits 11-31 in each axis
//
//    // z-values!
//    uint centreMortonPacked = 0;
//    uint kmax = mortonBitDepth * 3u;
//    for (uint k = 0; k < kmax; k++)
//    {
//        centreMortonPacked = packMortonCode(centreMorton, k, centreMortonPacked);
//    }
//
//    // Hypothetical order-preserving output
//    // Need a small uint r/w buffer for these interlocked operations to work, might get onto that next
//    // Unless I can change the algorithm to work with groupshared? I don't really know how I would do that though
//    // It would need to be some kind of segmented multi-pass algorithm
//    // (write out ordered groups in the first pass, order across groups in the second pass)
//    // I think I prefer the UAV option, but worried about memory waste given 64KB alignment
//    // Should probably just make a 64KB "atomics" buffer, then I can use it for other shaders, double-buffer easily,
//    // etc (easy double-buffering because there'll be a surplus of ints, probably, so each block of work-items can
//    // have their own set)
//    const uint currentMinMorton = 0;
//    const uint bvhFront = 1;
//    while (atomics[bvhFront] < numTris)
//    {
//        // Find the minimum morton code in the surviving threads
//        InterlockedMin(atomics[currentMinMorton], centreMortonPacked);
//
//        // Write it to the front of the bvh, retire the relevant thread
//        if (atomics[currentMinMorton] == centreMortonPacked)
//        {
//            bvhAS[atomics[bvhFront]] = atomics[currentMinMorton]; // This might need to be an atomic load/store
//
//            // Unintuitive, but putting this increment inside the branch turns it into a broadcast
//            // If it were outside the branch then each counter would increment by (1 * numLivingThreads) each iteration
//            InterlockedAdd(atomics[bvhFront], 1u);
//            return;
//        }
//    }
}