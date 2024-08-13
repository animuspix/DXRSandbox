#define AS_RESOLVE_PASS
#include "ComputeBindings.hlsli"

bool aabbTest(float3 pt, float3 minBounds, float3 maxBounds)
{
    return all(pt > minBounds) && all(pt < maxBounds);
}

[numthreads(8, 8, 8)] // Completely independent lanes, so go wide on group size
void main( uint3 GTid : SV_GroupThreadID, uint groupIndex : SV_GroupIndex )
{
    // AS resolve has a one-dimensional dispatch, so resolving thread indices is straightforward
    uint index = GTid.x + groupIndex;
    return;
    // Drop threads above the scene's vert count
    uint numTris = 0;
    uint vertStride = 0;
    triBuffer.GetDimensions(numTris, vertStride); 
    if (index >= numTris)
    {
        return;
    }
  
    IndexedTriangle triIndices = triBuffer[index];
    float3x3 triPositions = float3x3(structuredVBuffer[triIndices.xyz[0]].pos.xyz, 
                                     structuredVBuffer[triIndices.xyz[1]].pos.xyz, 
                                     structuredVBuffer[triIndices.xyz[2]].pos.xyz);

    // Octree traversal highly nontrivial!!
    // Algorithm
    // - Shoot straight to the leaves, from left-to-right (radical DFS), while intersection tests report [true]
    //   In other words, follow the children of the leftmost nodes until we hit a leaf (bit 25 unset)
    //
    // - If no intersection found, shift to the next branch in the current rank

    // Need to debug leaf nodes here, not sure if those are working or not
    // especially possible sync bugs with InterlockedAdd hmmm

    // Separately iterate each tri vertex, since they may end up in different cells
    const float aabbFuzz = 0.001f; // To account for verts at the boundaries of the AS root volume
    for (uint vertNdx = 0; vertNdx < 3; vertNdx++)
    {
        // Search for destination octree cell, record the current triangle
        uint octreeRank = 0;
        uint prevOctreeCell = 0;
        uint currOctreeCell = 0;
        bool traversingAS = true;
        while (traversingAS)
        {
            // Load the current node
            ComputeAS_Node node = octreeAS[currOctreeCell];
        
            // Scan through child nodes (we assume all vertices intersect with the prime/origin cell)
            if (node.isBranchNode)
            {
                uint child = 0;
                while (child < node.numChildren) // numChildren is guaranteed to be 8 for most nodes anyway, but this feels cleaner?
                {
                    uint childIndex = node.children[child];
                    ComputeAS_Node childNode = octreeAS[childIndex];

                    // If child intersection; flag the child as active, traverse it in the next pass, leave the current loop
                    // Spheres don't tesselate, so AABB representation is likely better
                    // (but tbh getting my head around construction is the main problem here)
                    if (aabbTest(triPositions[vertNdx], childNode.bounds[0].xyz - aabbFuzz.xxx, childNode.bounds[1].xyz + aabbFuzz.xxx))
                    {
                        octreeAS[currOctreeCell].containsTrisEventually = TRUE;
                        prevOctreeCell = currOctreeCell;
                        currOctreeCell = childIndex;
                        octreeRank++;
                        break;
                    }

                    child++;
                }

                if (child == 7) // No intersections found; either intersection tests are wonky, or the current vertex lies outside the volume
                {
                    traversingAS = false;
                }
            }
            else // Leaf node; not sure about logic here
            {
                // Pondering whether/how to parallelize this again, hmmm
                // Parallel test is a bit new - look up InterlockedCompareStores()
                
                // Granular sweep, to account for triangles that cross multiple leaf nodes
                ComputeAS_Node parent = octreeAS[prevOctreeCell];
                for (uint i = 0; i < parent.numChildren; i++)
                {
                    uint childLookup = parent.children[i];
                    ComputeAS_Node child = octreeAS[childLookup];
                    if (aabbTest(triPositions[vertNdx],  child.bounds[0].xyz - aabbFuzz.xxx, child.bounds[1].xyz + aabbFuzz.xxx))
                    {
                        InterlockedAdd(octreeAS[childLookup].numChildren, 1);
                        if ((octreeAS[childLookup].numChildren - 1) < 8) // Might work! not sure about thread safety but happy to try
                                                                         // Might need memory barriers @.@
                        {
                            octreeAS[childLookup].children[octreeAS[childLookup].numChildren] = triIndices.xyz[vertNdx];
                        }
                    }
                }
                
                traversingAS = false;
            }
        }
    }
}