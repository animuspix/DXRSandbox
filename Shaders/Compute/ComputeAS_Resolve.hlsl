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

// Body commented out here, to revise; morton code -> lbvh algorithm feels very funky

[numthreads(4, 4, 4)] // Completely independent lanes, so go wide on group size
void main( uint3 GTid : SV_GroupThreadID, uint groupIndex : SV_GroupIndex )
{ 
    // First pass, match triangles to boxes, four each;
    // because of the spatial sort performed above we should be safe to simply
    // group strips of four triangles together, assuming that proximity in
    // equates to proximity in space 

    // Quite naive algorithm here, see:
    // Thinking Parallel, Part III: Tree Construction on the GPU for NV's approach

    // (z-order sorting above was inspired by that tutorial, but haven't properly reviewed the actual tree construction bits yet)

    // Main difference seems to be the decision (or not) where to split each node - I do it arbitrarily (naive linear subsets),
    // the article goes by morton codes and splits whenever the highest bit changes, which seems compelling but also much more
    // involved

    // Seems it may be worth tagging triangles with their morton codes, in any case

    // Essential difference is maybe a proper BVH with heuristic splits, vs an octree?
    // Unsure how invested to be in this ^_^' it seems like the end result of heuristic splits is some kind of clustering
    // algorithm, and I don't want to invest time in that if I don't need to

    // Have to see the pix capture/test renders; if the distribution is bad we can go back to the article and improve

    // Working traversal approach:
    // - Depth-first traversal through each child of the root node
    // - Populate history buffer on the way towards leaf nodes
    // -- Embed alternative paths, where a box was hit successfully but not traversed
    // - Append hit triangles to a local buffer
    // - Traverse alternative paths, and append any hits to the local buffer from before
    // - Sort triangle hits/intersections by distance along the ray
    // - Shade/bounce using the closest hit

    // Useful side-effect having entry/exit paths here, helpful for tracking changes in refractive index
    // (+ bidirectional rendering, path integration tricks in general)

    // Actual approach for the current octree
    // - test all (triCount / childCount) nodes for AABB intersection

    // Drop intermediate threads in each gather pass
//     uint rankSize = numTris / AS_NODE_CHILDCOUNT;
//     if (sortingNdx % rankSize != 0)
//     {
//         return;
//     }

//     // Should compute node offset here
//     uint nodeOffset = 0;
//     uint nodeNdx = sortingNdx / AS_NODE_CHILDCOUNT;
//     ComputeAS_Node currentNode = bvhAS[nodeNdx]; // How to handle contention here? Drop intermediate threads?
//     currentNode.bounds[0].xyz = float3(min(min(vt0.x, vt1.x), vt2.x), 
//                                        min(min(vt0.y, vt1.y), vt2.y),
//                                        min(min(vt0.z, vt1.z), vt2.z));

//     currentNode.bounds[1].xyz = float3(max(max(vt0.x, vt1.x), vt2.x), 
//                                        max(max(vt0.y, vt1.y), vt2.y),
//                                        max(max(vt0.z, vt1.z), vt2.z));

//     currentNode.bounds[0].w = 0;
//     currentNode.bounds[1].w = 0;

//     currentNode.children[0] = sortingNdx;

//     for (uint i = 1; i < AS_NODE_CHILDCOUNT; i++)
//     {
//         uint nextTri = sortingNdx + i;
//         if (nextTri < numTris)
//         {
//             IndexedTriangle childTri = triBuffer[nextTri];

//             float3 childVt0 = structuredVBuffer[childTri.xyz.x].pos.xyz;
//             float3 childVt1 = structuredVBuffer[childTri.xyz.y].pos.xyz;
//             float3 childVt2 = structuredVBuffer[childTri.xyz.z].pos.xyz;

//             float3 triMins = float3(min(min(childVt0.x, childVt1.x), childVt2.x), 
//                                     min(min(childVt0.y, childVt1.y), childVt2.y),
//                                     min(min(childVt0.z, childVt1.z), childVt2.z));

//             float3 triMaxes = float3(max(max(childVt0.x, childVt1.x), childVt2.x), 
//                                      max(max(childVt0.y, childVt1.y), childVt2.y),
//                                      max(max(childVt0.z, childVt1.z), childVt2.z));

//             currentNode.bounds[0].xyz = min(triMins, currentNode.bounds[0].xyz);                                
//             currentNode.bounds[1].xyz = max(triMaxes, currentNode.bounds[1].xyz);

//             currentNode.children[i] = nextTri;
//         }
//         else
//         {
//             currentNode.children[i] = nextTri - 1;
//         }
//     }

//     // Can do some maths to reorder tree layout here (from leaves -> branches to branches -> leaves)
//     bvhAS[nodeNdx] = currentNode;
//     nodeOffset = rankSize;

//     // Need to gather nodes/tris up to the root in order to test traversal code;
//     // Something to work on next time maybe? Off to help with dins

//     // Might be worthwhile to implement a simple debug mode that terminates on AABB hits
//     // & shades with box normals, to better visualize the generated BVH layout

//     // Increasingly linear work here, as gathering continues; probably best to continue dropping intermediate threads, but not sure rllyyyy

//     // Seems to make sense, need to open PIX to check
    
//     // Recurrent passes; sort boxes into bigger boxes
//     uint maxRanks = getMaxBVHRanks(numTris);

//     // First rank (leaf nodes - unsure about ordering there) covered above
//     for (uint rankNdx = 1; rankNdx < maxRanks; rankNdx++)
//     {        
//         // Continue dropping intermediate threads
//         uint prevRankSize = rankSize;
//         rankSize /= AS_NODE_CHILDCOUNT;
//         if (nodeNdx % rankSize != 0)
//         {
//             return;
//         }

//         // Sync remainder
//         AllMemoryBarrierWithGroupSync();

//         uint sentry = nodeNdx; // Marks four adjacent children, to be collected by the current parent node
//         nodeNdx /= AS_NODE_CHILDCOUNT;
//         nodeOffset += rankSize;            

//         // Sweep through the previous rank to gather nodes
//         currentNode = bvhAS[nodeNdx + nodeOffset];
//         for (uint i = 0; i < AS_NODE_CHILDCOUNT; i++)
//         {
//             uint nextChildNode = sentry + i;
//             if (nextChildNode < prevRankSize)
//             {
//                 ComputeAS_Node child = bvhAS[nextChildNode];

//                 currentNode.bounds[0].xyz = min(child.bounds[0].xyz, currentNode.bounds[0].xyz);                                
//                 currentNode.bounds[1].xyz = max(child.bounds[1].xyz, currentNode.bounds[1].xyz);

//                 currentNode.children[i] = nextChildNode;               
//             }
//             else
//             {
//                 currentNode.children[i] = nextChildNode - 1;
//             }
//         }       

//         // Write out node data to the bvhAS
//         bvhAS[nodeNdx + nodeOffset] = currentNode;
//     }
}