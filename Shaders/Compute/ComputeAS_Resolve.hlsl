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
    uint3 centreMorton = uint3((centre * (1u << mortonBitDepth))); // We want 3 slices in the Morton code, ten bits each
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

    // 10 bits and three dimensions means 30 bits = ~1GB lookup
    // Not horrible! + dense sorting from the start is seemingly not the easiest issue to solve
    // Alternative? Create the 1GB buffer as a holding space for generated Morton data, and have another shader stage densify it
    // The follow-up stage subdivides the holding space using one thread/code (or per=tri, one code per tri, same thing), and sets the
    // range to densify using the min/morton codes generated earlier (computable with InterlockedMin/InterlockedMax)
    //
    // Problem! The codes are just a vehicle for sorting triangles
    // So that 30-bit 1GB buffer is actually 4GB ^_^'''
    // The faux hash table approach (write the hashes/codes out directly using their values, tag them, read them back) is possibly not ideal then?
    // We could look into some kind of dense, lossy hash storage
    // That seems tricky though, risky and a lot of unexplored complexity
    //
    // I think my original idea was to tag the triangles with their codes and sort in a post-pass
    // a bit wasteful, but maybe not terrible, especially since you need the context of the full code set to do any kind of actually-useful
    // dense sorting
    // (sure you could put an octree on the huge 4GB sparse buffer, but you'd have a lot of empty cells)
    //
    // Might go ahead with that
    // Sorting algorithm is likely going to be some kind of multi-pass bucket sort
    // The ideas that come to me intuitively have the issue of sorting not happening across buckets, so likely more reading needed
    // (or the buckets need to move left/right progressively...? idk)
    //
    // But yuh, I think tag triangles with their codes then multi-pass sort the triangles, instead of trying to do it all in the one shader
    // We can leave this as the AS shader and create another couple for the code generation & sorting
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    // Early return; just write out morton codes (to tribuffer[index].xyz.x) and quit, consider sorting etc in another pass
    // (single-pass sort doesn't seem to really be working + is undebuggable)
    triBuffer[0].xyz.x = centreMortonPacked;
    return;

    // Hijack the first index in the first tri to find the largest Morton code
    // (once we know that we can sort into Morton order by renormalizing against the tri count, which is all we need for
    // the LBVH implementation & binary-search traversal - the actual numbers are cool but unimportant (afaik))

    // Zero the first index/first tri, then synchronise
    triBuffer[0].xyz.x = 0;
    AllMemoryBarrierWithGroupSync();

    // Compute max morton code and sync again
    InterlockedMax(triBuffer[0].xyz.x, centreMortonPacked);
    AllMemoryBarrierWithGroupSync();

    uint maxMorton = triBuffer[0].xyz.x;
    float mortonRelative = float(centreMortonPacked) / float(maxMorton);
    float sortingNdx = mortonRelative * numTris;

    // Sort! (then sync again)
    triBuffer[sortingNdx] = tri;
    AllMemoryBarrierWithGroupSync();

    // Is this the best sorting model we can do? Need to reconsider
    ///////////////////////////////////////////////////////////////
  
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
    uint rankSize = numTris / AS_NODE_CHILDCOUNT;
    if (sortingNdx % rankSize != 0)
    {
        return;
    }

    // Should compute node offset here
    uint nodeOffset = 0;
    uint nodeNdx = sortingNdx / AS_NODE_CHILDCOUNT;
    ComputeAS_Node currentNode = bvhAS[nodeNdx]; // How to handle contention here? Drop intermediate threads?
    currentNode.bounds[0].xyz = float3(min(min(vt0.x, vt1.x), vt2.x), 
                                       min(min(vt0.y, vt1.y), vt2.y),
                                       min(min(vt0.z, vt1.z), vt2.z));

    currentNode.bounds[1].xyz = float3(max(max(vt0.x, vt1.x), vt2.x), 
                                       max(max(vt0.y, vt1.y), vt2.y),
                                       max(max(vt0.z, vt1.z), vt2.z));

    currentNode.bounds[0].w = 0;
    currentNode.bounds[1].w = 0;

    currentNode.children[0] = sortingNdx;

    for (uint i = 1; i < AS_NODE_CHILDCOUNT; i++)
    {
        uint nextTri = sortingNdx + i;
        if (nextTri < numTris)
        {
            IndexedTriangle childTri = triBuffer[nextTri];

            float3 childVt0 = structuredVBuffer[childTri.xyz.x].pos.xyz;
            float3 childVt1 = structuredVBuffer[childTri.xyz.y].pos.xyz;
            float3 childVt2 = structuredVBuffer[childTri.xyz.z].pos.xyz;

            float3 triMins = float3(min(min(childVt0.x, childVt1.x), childVt2.x), 
                                    min(min(childVt0.y, childVt1.y), childVt2.y),
                                    min(min(childVt0.z, childVt1.z), childVt2.z));

            float3 triMaxes = float3(max(max(childVt0.x, childVt1.x), childVt2.x), 
                                     max(max(childVt0.y, childVt1.y), childVt2.y),
                                     max(max(childVt0.z, childVt1.z), childVt2.z));

            currentNode.bounds[0].xyz = min(triMins, currentNode.bounds[0].xyz);                                
            currentNode.bounds[1].xyz = max(triMaxes, currentNode.bounds[1].xyz);

            currentNode.children[i] = nextTri;
        }
        else
        {
            currentNode.children[i] = nextTri - 1;
        }
    }

    // Can do some maths to reorder tree layout here (from leaves -> branches to branches -> leaves)
    bvhAS[nodeNdx] = currentNode;
    nodeOffset = rankSize;

    // Need to gather nodes/tris up to the root in order to test traversal code;
    // Something to work on next time maybe? Off to help with dins

    // Might be worthwhile to implement a simple debug mode that terminates on AABB hits
    // & shades with box normals, to better visualize the generated BVH layout

    // Increasingly linear work here, as gathering continues; probably best to continue dropping intermediate threads, but not sure rllyyyy

    // Seems to make sense, need to open PIX to check
    
    // Recurrent passes; sort boxes into bigger boxes
    uint maxRanks = getMaxBVHRanks(numTris);

    // First rank (leaf nodes - unsure about ordering there) covered above
    for (uint rankNdx = 1; rankNdx < maxRanks; rankNdx++)
    {        
        // Continue dropping intermediate threads
        uint prevRankSize = rankSize;
        rankSize /= AS_NODE_CHILDCOUNT;
        if (nodeNdx % rankSize != 0)
        {
            return;
        }

        // Sync remainder
        AllMemoryBarrierWithGroupSync();

        uint sentry = nodeNdx; // Marks four adjacent children, to be collected by the current parent node
        nodeNdx /= AS_NODE_CHILDCOUNT;
        nodeOffset += rankSize;            

        // Sweep through the previous rank to gather nodes
        currentNode = bvhAS[nodeNdx + nodeOffset];
        for (uint i = 0; i < AS_NODE_CHILDCOUNT; i++)
        {
            uint nextChildNode = sentry + i;
            if (nextChildNode < prevRankSize)
            {
                ComputeAS_Node child = bvhAS[nextChildNode];

                currentNode.bounds[0].xyz = min(child.bounds[0].xyz, currentNode.bounds[0].xyz);                                
                currentNode.bounds[1].xyz = max(child.bounds[1].xyz, currentNode.bounds[1].xyz);

                currentNode.children[i] = nextChildNode;               
            }
            else
            {
                currentNode.children[i] = nextChildNode - 1;
            }
        }       

        // Write out node data to the bvhAS
        bvhAS[nodeNdx + nodeOffset] = currentNode;
    }
}