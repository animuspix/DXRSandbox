
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

//bool aabbTest(float3 pt, float3 minBounds, float3 maxBounds)
//{
//    return all(pt > minBounds) && all(pt < maxBounds);
//}

[numthreads(8, 8, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    ConstantBuffer<ComputeConstants> sharedConstants = GetSharedConstants(stageBindings.sharedKeys);
    RWStructuredBuffer<GPU_PRNG_Channel> prngPathStreams = GetPRNGStreams(stageBindings.prngStreamsLookup);

    float screenWidth = sharedConstants.screenAndLensOptions.screenAndTime.x;
    float screenHeight = sharedConstants.screenAndLensOptions.screenAndTime.y;
    uint linPixID = DTid.x + (DTid.y * screenHeight);
    GPU_PRNG_Channel prngChannel = prngPathStreams[linPixID];
//
//    // Test render! Verifying ray directions
//    float spectralSample = rand(prngChannel);
//    float4 lensSettings = sharedConstants.screenAndLensOptions.lensSettings;
//    float4 ray = RaySetup(DTid.xy, lensSettings.x, float2(screenWidth, screenHeight), lensSettings.w, prngChannel, spectralSample);    
//
//    // Verifying PRNG
//    //texOut[DTid.xy] = float4(rand3d(prngChannel), 1.0f);
//
//    // - Verifying spectral samples & film CMF
//    //texOut[DTid.xy] = float4(ResolveSpectralColor(float(DTid.x) / screenWidth, sharedConstants.screenAndLensOptions.filmSPD), 1.0f);
//    //texOut[DTid.xy] = float4(ResolveSpectralColor(spectralSample, sharedConstants.screenAndLensOptions.filmSPD), 1.0f);
//
//    // Verifying triangle intersection
//    float3 camPos = sharedConstants.screenAndLensOptions.cameraTransform.translationAndScale.xyz;
//    
//    // We assume no camera rotation, and z+ goes into the screen
//    float time = sharedConstants.screenAndLensOptions.screenAndTime.z;                   
//    camPos.x = cos(time) * 4.0f;
//    camPos.y = sin(time) * 4.0f;
//    camPos.z = abs(sin(time)) * -4.0f;
//
//    Ray _ray;
//    _ray.origin = camPos;
//    _ray.dir = ray.xyz;
//    
//    ResolveRayTransforms(_ray);
//
//    // First demo case - test ray vs all triangles
//    float3 bary = 0.0f.xxx;
//    float distance = 9999.0f;
//    bool triSect = false;
//    float3 normal = 0.0f.xxx;
//
//    // Passable traversal implementation below, but no support for bounces;
//    // really feels like (on my 9999th time dealing with this :x) that you can't
//    // do an AS implementation without some kind of backtracking/history process
//    // to handle rays starting from inside geometry
//    // ...
//    // Probablyyyy going to continue (current bvh impl is miles away from
//    // stability anyway), but going to revisit once I'm getting some test renders
//    // with primary bounces
//    //
//    // Secondary bounce setup per-se shouldn't be tricky, same use-case as traversing
//    // from anywhere else in geometry; it's a regular trace except it starts from
//    // the mesh surface + one of the triangles is masked out
//    //
//    // Secondary bounces & near-miss rays have the same problem; need to be able to
//    // dig back out from the bottom AS layer to the next possible intersection in the
//    // scene, except near-miss rays ignore their entire bucket, whereas bounce rays
//    // only ignore the current triangle
//    // 
//    // Somewhat tempted to frame secondary/near-miss bounces as restarts with a mask
//    // factor
//    // e.g; if a ray bounces, or misses, treat it like another primary ray passing
//    // through the same point but ignoring anything behind the near-miss/hit point
//    //
//    // Might be easier than backtracking rays, though still worth testing other rays
//    // in the same bucket for bounces; straightforward, no reason to dig all the way
//    // through the AS again if we don't need to
//    //
//
//    // Traversal metadata
//    // Trying to interpret Morton key/value pairs as a KD-tree
//    // Absolutely not suitable for complex scenes (more than one object, non-normalized scale)...
//    // ...but tbh I don't think complex scenes are a good idea ^_^' I think I'm going to progressively
//    // remove support for them and avoid the scope creep
//    //
//    // Bounds wouldn't change super hugely; just rescale based on the transform attached to the current AABB in each 
//    // loop iteration
//    uint axis = 0; // X = 0, Y = 1, Z = 2
//    float3 minBounds = sharedConstants.screenAndLensOptions.sceneBoundsMin.xyz;
//    float3 maxBounds = sharedConstants.screenAndLensOptions.sceneBoundsMax.xyz; 
//    
//    uint numMortonBuckets = 0; uint bvhAS_Stride = 0;
//    
//    RWStructuredBuffer<uint> bvhAS = GetBVH(sharedConstants.computeResourceKeys.bvhLookup);
//    bvhAS.GetDimensions(numMortonBuckets, bvhAS_Stride);
//
//    int bvhOffset = 0;
//    int bvhCutCounter = 0;
//
//    // Needs retracing; naive depth-first tree traversal works if you hit something on the last rank, but fails for misses;
//    // you can't return a hit (obvi), but you also can't bail on the ray until you test where else it could go
//
//    // Traverse LBVH
//    // No bounces for now, just run to first hit
//    bool traversingAS = false;//aabbHit(ray, minBounds, maxBounds);
//    float4 asRGBA = float4(1.0f, 0.5f, 0.25f, 0.0f);
//    while (traversingAS)
//    {
//        // Traversal function assumes triangles (morton codes) are ordered like
//        // x0,y0,z0, x1,y1,z1, x2,y2,z2...
//        // See ComputeSpatialHashing.hlsl -> packMortonCode(...)
//
//        float3 halfBounds = (maxBounds - minBounds) * 0.5f;
//        
//        float3 axisMask = 0.0f.xxx;
//        axisMask[axis] = 1.0f;
//        halfBounds *= axisMask;
//    
//        // Test against the lower side of the current axis (x, y, z)
//        bool leftHit = aabbHit(_ray, minBounds, maxBounds - halfBounds);
//        if (leftHit) 
//        {
//            maxBounds.x -= halfBounds.x; // Mask-off right side
//        }
//        else
//        {
//            // Change bounds anyway; we intersect the object AABB, so we have to hit one of the sides
//            minBounds.x += halfBounds.x; // Mask-off left side
//        }
//
//        // Each time we test half the scene volume we eliminate one bit/half the candidate buckets
//        numMortonBuckets /= 2;
//
//        // If we hit the left side of the AS every test, we'd end up testing the very first bucket
//        // in the BVH, on the left side, which is where it starts
//        // So I feel like it makes the most sense to only update the cursor when we hit volumes on
//        // the right (and in that case, to jump past all the leftward buckets we just missed)
//        if (!leftHit)
//        {
//            bvhOffset += numMortonBuckets;        
//        }
//
//        axis = (axis + 1) % 3;
//        bvhCutCounter++;
//
//        // Eventually we're going to cut finer than the actual leaf nodes/Morton buckets in the bvh
//        // Once we get to that point, cut to the chase and test the remaining cells directly
//        if (bvhCutCounter == MORTON_SPATIAL_RES)
//        {
//            for (int bucketWalker = 0; bucketWalker < numMortonBuckets; bucketWalker++)
//            {
//                break;
//                // MortonHashBucket bucket = bvhAS[bvhOffset + bucketWalker];
//                // int numLeaves = bucket.entryCount;
//                // for (int leafWalker = 0; leafWalker < numLeaves; leafWalker++)
//                // {
//                //     IndexedTriangle tri = triBuffer[bucket.triNdces[leafWalker]];
//                //     Vertex3D verts[3] = { structuredVBuffer[tri.xyz.x], structuredVBuffer[tri.xyz.y], structuredVBuffer[tri.xyz.z] };
//                //     float3x3 vpositions = float3x3(verts[0].pos.xyz, verts[1].pos.xyz, verts[2].pos.xyz);
//                    
//                //     float distTmp = 0;
//                //     float3 baryTmp = 0;
//                //     bool triSectLocal = triHit(vpositions, _ray, distTmp, baryTmp);
//                    
//                //     if (triSectLocal)
//                //     { 
//                //         if (distTmp < distance)
//                //         {
//                //             distance = distTmp;
//                //             bary = baryTmp;
//                //             normal = verts[0].normals.xyz * bary.x +
//                //                      verts[1].normals.xyz * bary.y +
//                //                      verts[2].normals.xyz * bary.z;
//                //         }
//
//                //         triSect = true;
//                //         traversingAS = false; // Single bounce implementation, for now
//                //         asRGBA.g = 1.0f; // Green tint for triangle hits
//                //     }
//
//                //     // Bounce handling (shading etc) here
//                //     // Reset leaf iterator & assign leaf mask before checking for nearby bounces
//                //     // (might as well check locally before zooming back out to rank 0)
//                // }
//
//                // For near-misses & far bounces
//                // - Restart from rank 0, with updated ray origin +/- direction
//                // - Ignore the missed bucket + any AS subsets unreachable from the current bucket w/ the current ray origin/direction
//                //   (TBD how to actually do that filtering, some should come automatically with the AABB hit function, but not sure how much)
//            }
//
//            // Sticking to single bounces, no near-miss handling; too hard for now ^_^' need more confidence
//            // in the current system before getting onto thorny things like that
//            if (!triSect)
//            {
//                // Assume no intersections, break-out
//                break;
//            }
//        }
//
//    }
//
    RWTexture2D<float4> texOut = GetOutputTexture(stageBindings.outputTextureLookup);
    
    texOut[DTid.xy] = float4(1.0f, 0.5f, 0.25f, 1.0f);
    prngPathStreams[linPixID] = prngChannel;
}