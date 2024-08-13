
#define SHADING_PASS
#include "ComputeBindings.hlsli"
#include "..\\SharedRaySetup.hlsli"

// Planned ubershader design with thread re-use (rather than manual bucketing with wavefront PT), so everything should go in this file except final filtering/reprojection & tonemapping (which go in ComputePresentation.hlsl)
// Thread re-use stops when all workers in the current group have computed at least one sample, then sample counts are saved out and referenced in subsequent passes

// Simple thread re-use/binning implementation for now - just fire rays in each group through the same pixels (at random angles ofc) until every pixel in the group has at least one sample

struct Ray
{
    float3 dir;
    float3 origin;

    // Axis permutation for vertices + ray directions (aligns triangle intersections with Z)
    uint3 axisPermu;
    
    // Shear transform (resolved from ray directions, applied to tri verts) 
    // Ensures ray/triangle intersections are aligned with +Z specifically instead of -Z
    // Vaguely resembles gradient math in fast voxel raymarching :o
    float3 shearParams;
};

uint FindMaxDim(float3 v)
{
    if (v.x > v.y && v.x > v.z)
    {
        return 0;
    }
    else if (v.y > v.x && v.y > v.z)
    {
        return 1;
    }
    else // if (v.z > v.x && v.z > v.y)
    {
        return 2;
    }
}

float3 ApplyVectorPermu(float3 v, uint3 permu)
{
    float3 vCopy = v;
    vCopy.x = v[permu.x];
    vCopy.y = v[permu.y];
    vCopy.z = v[permu.z];
    return vCopy;
}

// Declared locally because hardware intersections will track these values internally (I expect)
void ResolveRayTransforms(inout Ray ray)
{
    ray.axisPermu.z = FindMaxDim(ray.dir);
    ray.axisPermu.x = (ray.axisPermu.z + 1) % 3;
    ray.axisPermu.y = (ray.axisPermu.x + 1) % 3;

    float3 d = ApplyVectorPermu(ray.dir, ray.axisPermu);
    ray.shearParams.x = -d.x / d.z;
    ray.shearParams.y = -d.y / d.z;
    ray.shearParams.z = 1.0f / d.z;
}

float3 MapTriToIsectSpace(float3 vertex, Ray ray)
{
    // Good old relative translation for vertices ^_^
    vertex -= ray.origin;
    
    // Permute input vector
    // (move largest direction axis to Z - roughly 
    // aligns the triangle with the given ray)
    vertex = ApplyVectorPermu(vertex, ray.axisPermu);

    // Permute vertex position
    // Shears positions to complete the approximate alignment we
    // performed with [ApplyVectorPermu]
    vertex.x += ray.shearParams.x * vertex.z;
    vertex.y += ray.shearParams.y * vertex.z;
    vertex.z *= ray.shearParams.z;

    return vertex;
}

// Implemented from Physically Based Rendering: From Theory to Implementation, pages 158-164
// (Pharr, Jakob, Humphreys)
// Should modify to return distance + barycentrics (if applicable)
bool triHit(float3x3 triVerts, Ray ray, out float distance, out float3 bary)
{
    // Transform tri vertices to intersection space
    // - implicitly transforms ray-directions
    // - plane embeds the triangle, ray is on Z with origin at the hypothetical hit pos
    // - much simplifies the problem, allows solving in 2D, similar to my old custom method
    ///////////////////////////////////////////////////////////////////////////////////////
    
    // Transform tri vertices
    triVerts[0] = MapTriToIsectSpace(triVerts[0], ray);
    triVerts[1] = MapTriToIsectSpace(triVerts[1], ray);
    triVerts[2] = MapTriToIsectSpace(triVerts[2], ray);

    // Edge functions!
    // Sign of these tells us if the origin of our local coordinate system (= the hit pos) is inside the triangle (a hit) or not (a miss)
    // I suspect all signs should be positive (as in my function), but PBRT doesn't specify, just that they have to agree
    float e0 = triVerts[1].x * triVerts[2].y - triVerts[1].y * triVerts[2].x;
    float e1 = triVerts[2].x * triVerts[0].y - triVerts[2].y * triVerts[0].x;
    float e2 = triVerts[0].x * triVerts[1].y - triVerts[0].y * triVerts[1].x;
    float eSum = e0 + e1 + e2;

    bool hit = (sign(e0) >= 0 && 
                sign(e1) >= 0 && 
                sign(e2) >= 0) && eSum != 0;

    if (hit)
    {
        // Resolve hit depth (interpolated z) using edge distances
        // (effectively - vertex z-coordinates interpolated at the ray's XY coordinates (= its origin))
        // This wouldn't work normally! We'd end up with a junk interpolated worldspace z-coordinate
        // The math works out specifically because we transformed the triangle into the same space as the ray (see MapTriToIsectSpace)
        float z = (e0 * triVerts[0].z + 
                   e1 * triVerts[1].z +
                   e2 * triVerts[2].z) / eSum; // Division by eSum removes bias from summing eN * vertN products (I think??)

        distance = z; // The power of coordinate transforms ^_^ (tbh I don't really understand this - need to think about it further)
        bary = float3(e0, e1, e2) / eSum; // Division by eSum converts from 0...triangleSize range to 0...1

        return z >= 0; // Negative Z indicates tris behind the camera
    }   
    else
    {
        distance = 9999.0f; // Close enough to infinity ^_^'
        return false;
    } 
}

// Not sure where to put this yet, but extremely useful to have for area-based transport algorithms
// (e.g. some kinds of volumetric transport, implementations of BDPT)
// Insight: Cross product gives the area of a parallelogram defined by two edge vectors
// Triangles are half the area of a parallelogram
// thus area = 0.5 * cross(edge0, edge1);
float3 FindTriArea(float3x3 triVerts)
{
    float3 edge0 = triVerts[0] - triVerts[1];
    float3 edge1 = triVerts[0] - triVerts[2];
    return 0.5 * length(cross(edge0, edge1));
}

// Basic AABB intersection test from
// https://tavianator.com/2011/ray_box.html
bool aabbHit(Ray ray, float3 aabbMin, float3 aabbMax)
{
    float2 tMinMax = 0.0f.xx;

    if (ray.dir.x != 0.0f)
    {
        float2 tx = (float2(aabbMin.x, aabbMax.x) - ray.origin.xx) / ray.dir.xx;
        tMinMax = tx.x < tx.y ? tx.xy : tx.yx; //  float2(min(tx.x, tx.y), max(tx.x, tx.y));
    }

    if (ray.dir.y != 0.0f)
    {
        float2 ty = (float2(aabbMin.y, aabbMax.y) - ray.origin.yy) / ray.dir.yy;
        tMinMax = float2(max(tMinMax.x, min(ty.x, ty.y)), 
                         min(tMinMax.y, max(ty.x, ty.y)));
    }

    if (ray.dir.z != 0.0f)
    {
        float2 tz = (float2(aabbMin.z, aabbMax.z) - ray.origin.zz) / ray.dir.zz;
        tMinMax = float2(max(tMinMax.x, min(tz.x, tz.y)), 
                         min(tMinMax.y, max(tz.x, tz.y)));        
    }

    return tMinMax.x <= tMinMax.y;
}

[numthreads(8, 8, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    float screenWidth = computeCBuffer.screenAndLensOptions.screenAndTime.x;
    float screenHeight = computeCBuffer.screenAndLensOptions.screenAndTime.y;
    uint linPixID = DTid.x + (DTid.y * screenHeight);
    GPU_PRNG_Channel prngChannel = prngPathStreams[linPixID];

    // Test render! Verifying ray directions
    float spectralSample = rand(prngChannel);
    float4 lensSettings = computeCBuffer.screenAndLensOptions.lensSettings;
    float4 ray = RaySetup(DTid.xy, lensSettings.x, float2(screenWidth, screenHeight), lensSettings.w, prngChannel, spectralSample);    

    //texOut[DTid.xy] = float4(ray.xyz, 1.0f);

    // Verifying PRNG
    //texOut[DTid.xy] = float4(rand3d(prngChannel), 1.0f);

    // - Verifying spectral samples & film CMF
    //texOut[DTid.xy] = float4(ResolveSpectralColor(float(DTid.x) / screenWidth, computeCBuffer.screenAndLensOptions.filmSPD), 1.0f);
    texOut[DTid.xy] = float4(ResolveSpectralColor(spectralSample, computeCBuffer.screenAndLensOptions.filmSPD), 1.0f);

    // Verifying triangle intersection
    float3 camPos = computeCBuffer.screenAndLensOptions.cameraTransform.translationAndScale.xyz;
    
    // We assume no camera rotation, and z+ goes into the screen
    float time = computeCBuffer.screenAndLensOptions.screenAndTime.z;                   
    camPos.x = cos(time) * 4.0f;
    camPos.y = sin(time) * 4.0f;
    camPos.z = abs(sin(time)) * -4.0f;

    Ray _ray;
    _ray.origin = camPos;
    _ray.dir = ray.xyz;
    
    ResolveRayTransforms(_ray);

    // First demo case - test ray vs all triangles
    // Should populate this at home, with access to PIX - expecting some thorny oversights
    float3 bary = 0.0f.xxx;
    float distance = 9999.0f;
    bool triSect = false;
    float3 normal = 0.0f.xxx;

    // Traverse AS
    uint currOctreeRank = 0;
    bool traversingAS = true;
    bool asLeaves = false; 

    uint octreeProbeHistoryAbsoluteOffsets[6] = { 0, 0, 0, 0, 0, 0 }; // See maxOctreeRank, Render.cpp; should move that definition to a header I can read from HLSL
    uint octreeProbeHistorySiblingOffsets[6] = { 0, 0, 0, 0, 0, 0 };
    uint octreeProbeHistoryChildrenMap[6 * 8] = { 0, 0, 0, 0, 0, 0, // 1,2,3,4,5,6,7,8
                                                  0, 0, 0, 0, 0, 0, // (9...16)...(64...73)
                                                  0, 0, 0, 0, 0, 0, // etc
                                                  0, 0, 0, 0, 0, 0,
                                                  0, 0, 0, 0, 0, 0,
                                                  0, 0, 0, 0, 0, 0,
                                                  0, 0, 0, 0, 0, 0,
                                                  0, 0, 0, 0, 0, 0 };

    // History is wonky and difficult to traverse
    // Sibling offsets good!
    // Load children at each rank into a third buffer (octreeProbeHistoryChildrenMap)
    // Can then consult the buffer to quickly skip empty cells (children that don't have children themselves)
    // + quickly & unambiguously select siblings with ((currOctreeRank * 8) + octreeProbeHistorySiblingOffsets[currOctreeRank]), and use that
    // value to update [...AbsoluteOffsets[currOctreeRank]] instead of relying on blind increments for the absolute/sibling offsets that we
    // forcibly modulo 8
    // Need to keep thinking about this before implementing anything hmmmmmm

    float4 asRGB = float4(0.0f.xxx, 1.0f);

    while (traversingAS)
    {
        asRGB.r = 1.0f;
        break;
        uint currAS_Node = octreeProbeHistoryAbsoluteOffsets[currOctreeRank]; // Resolve currAS_Node from current probe history
        ComputeAS_Node asNode = octreeAS[currAS_Node];
        bool missRay = false;

        // Test the current node
        if (aabbHit(_ray, asNode.bounds[0].xyz, asNode.bounds[1].xyz)) // Need to fill out else statement for this x_x
        {
            break;
            
            // Load current children into our faux stack/history buffer
            // Should be safe to run for all nodes...leaf nodes won't get to this AS test anyway ^_^' and we should be able to keep
            // testing them inline
            uint currRankChildrenOffs = currOctreeRank * 8;
            for (uint i = 0; i < 8; i++)
            {
                octreeProbeHistoryChildrenMap[currRankChildrenOffs + i] = asNode.children[i];
            }

            // Test the current cell
            bool hasChildren = asNode.containsTrisEventually == TRUE;
            bool isBranch = asNode.isBranchNode == TRUE;

            if (!isBranch) // hmmmm
            {
                // A leaf node; should load the parent cell (look at the current ray's history), then iterate over its children
                for (uint i = 0; i < 8; i++)
                {
                    ComputeAS_Node leafNode = octreeAS[asNode.children[i]];
                    for (uint triLookup = 0; triLookup < leafNode.numChildren; triLookup++)
                    {
                        uint triIndex = leafNode.children[triLookup];
                        IndexedTriangle tri = triBuffer[triIndex];
                    
                        Vertex3D verts[3] = { structuredVBuffer[tri.xyz.x], structuredVBuffer[tri.xyz.y], structuredVBuffer[tri.xyz.z] };

                        float3x3 vpositions = float3x3(verts[0].pos.xyz, verts[1].pos.xyz, verts[2].pos.xyz);

                        float distTmp = 0;
                        float3 baryTmp = 0;
                        bool triSectLocal = triHit(vpositions, _ray, distTmp, baryTmp);

                        if (triSectLocal)
                        { 
                            triSect = true;

                            if (distTmp < distance)
                            {
                                distance = distTmp;
                                bary = baryTmp;
                                normal = verts[0].normals.xyz * bary.x +
                                         verts[1].normals.xyz * bary.y +
                                         verts[2].normals.xyz * bary.z;
                            }

                            traversingAS = false;
                            asRGB.r = 1.0f;
                        }
                    }
                }

                // Successful intersections take care of themselves; failed intersections mean we need to change sibling & absolute offsets
                if (traversingAS)
                {
                    uint currSiblingOffset = octreeProbeHistorySiblingOffsets[currOctreeRank];
                    if (currSiblingOffset == 7)
                    {
                        octreeProbeHistorySiblingOffsets[currOctreeRank] = 0;
                        octreeProbeHistoryAbsoluteOffsets[currOctreeRank] = 0;
                        currOctreeRank--;

                        octreeProbeHistorySiblingOffsets[currOctreeRank]++; // We overflowed the sibliings on the current rank, so try a different sibling on the prior rank
                        
                        // Need a has-children test here! (+ extracting childLookup, not loading the bithacked values directly)
                        uint nextSiblingOffset = octreeProbeHistorySiblingOffsets[currOctreeRank];
                        octreeProbeHistoryAbsoluteOffsets[currOctreeRank] = octreeProbeHistoryChildrenMap[(currOctreeRank * 8) + nextSiblingOffset];
                    }
                    else
                    {
                        octreeProbeHistorySiblingOffsets[currOctreeRank]++;

                        // Need a has-children test here! (+ extracting childLookup, not loading the bithacked values directly)
                        octreeProbeHistoryAbsoluteOffsets[currOctreeRank] = octreeProbeHistoryChildrenMap[(currOctreeRank * 8) + octreeProbeHistorySiblingOffsets[currOctreeRank]];
                    }
                }
            }
            else if (hasChildren)
            {
                // Advance to the next rank (see above [if(traversingAS) { ... }] )
                octreeProbeHistorySiblingOffsets[currOctreeRank + 1] = 0;
                //octreeProbeHistoryAbsoluteOffsets[currOctreeRank + 1] = childLookup;
                currOctreeRank++;                
            }
            else // Walk around the current rank (see above [if(traversingAS) { ... }] )
            {
                if (octreeProbeHistorySiblingOffsets[currOctreeRank] == 7)
                {
                    // Nothing in the current rank has intersected; reverse to the previous rank
                    // hmmm
                    // what do when ray misses geometry? how know when ray misses geometry completely?
                    // not sure - may just break here instead, for now
                    break;
                }

                octreeProbeHistorySiblingOffsets[currOctreeRank]++;
                octreeProbeHistoryAbsoluteOffsets[currOctreeRank]++;
            }
        }
        else if (currAS_Node == 0) // Missed the root node, nothing to intersect/test
        {
            break;
        }
        else
        {
            // Hit the root node, missed some child nodes...hmmmm
            // try nearby nodes for now (see above [if(traversingAS) { ... }] )

            uint currSiblingOffset = octreeProbeHistorySiblingOffsets[currOctreeRank];
            if (currSiblingOffset == 7)
            {
                octreeProbeHistorySiblingOffsets[currOctreeRank] = 0;
                octreeProbeHistoryAbsoluteOffsets[currOctreeRank] = 0;
                octreeProbeHistoryAbsoluteOffsets[currOctreeRank - 1]++; // Test the next sibling in the previous rank
                currOctreeRank--;
            }
            else
            {
                octreeProbeHistorySiblingOffsets[currOctreeRank]++;
                octreeProbeHistoryAbsoluteOffsets[currOctreeRank]++;
            }
        }

        if (currOctreeRank >= 5 && traversingAS)
        {
            triSect = true;
            asRGB.g = 1.0f;
            break;
        }
    }

    /*for (uint i = 0; i < 12; i++) // Loop through indexed triangles
    {
        // Replace these lookups with triangle indices derived from our BVH
        IndexedTriangle tri = triBuffer[i];

        // Load triangles from indices, break on first intersection
        Vertex3D verts[3] = { structuredVBuffer[tri.xyz.x], structuredVBuffer[tri.xyz.y], structuredVBuffer[tri.xyz.z] };
        float3x3 vpositions = float3x3(verts[0].pos.xyz, verts[1].pos.xyz, verts[2].pos.xyz);
          
        float distTmp = 0;
        float3 baryTmp = 0;
        bool triSectLocal = triHit(vpositions, _ray, distTmp, baryTmp);

        if (triSectLocal)
        {
            triSect = true;
            
            if (distTmp < distance)
            {
                distance = distTmp;
                bary = baryTmp;
                normal = verts[0].normals.xyz * bary.x +
                         verts[1].normals.xyz * bary.y +
                         verts[2].normals.xyz * bary.z;
            }
        }
    }*/
    
    if (triSect)
    {
        float3 col = asRGB.rgb * rand(prngChannel); //+ abs(normal);

//#define DBG_DEPTH_FOG
#ifdef DBG_DEPTH_FOG
        col = 1.0f.xxx;
        col /= distance;
#endif

        texOut[DTid.xy] = float4(col, 0.0f);
    }
    else
    {
        // Debug orange
        texOut[DTid.xy] = float4(1.0f, 0.5f, 0.25f, 1.0f);
    }

    prngPathStreams[linPixID] = prngChannel;

    // After;
    // - AS debugging (!!!)
}