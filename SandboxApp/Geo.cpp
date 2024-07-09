#include "Geo.h"
#include "GeoLoader.h"
#include "..\CPUMemory.h"

XPlatUtils::BakedGeoBuffers viewGeo = {};
CPUMemory::ArrayAllocHandle<XPlatUtils::BakedGeoBuffers> sceneBuffers = {};

CPUMemory::ArrayAllocHandle<CPUMemory::ArrayAllocHandle<Material>> sceneMaterials = {};
CPUMemory::ArrayAllocHandle<uint32_t> materialsPerScene = {};

constexpr uint32_t maxVerts = 1024 * 1024; // About a million verts - most of our scenes should be much smaller
CPUMemory::ArrayAllocHandle<Geo::Vertex3D> models = {};
CPUMemory::ArrayAllocHandle<uint64_t> ndces = {};

CPUMemory::ArrayAllocHandle<Geo::Vertex2D> viewVts;
CPUMemory::ArrayAllocHandle<uint16_t> viewNdces;

void Geo::Init(uint32_t numScenes, Scene* scenes)
{
    // Allocate scene memory
    sceneBuffers = CPUMemory::AllocateArray<XPlatUtils::BakedGeoBuffers>(numScenes);

    // Allocate material pointer + count memory
    sceneMaterials = CPUMemory::AllocateArray<CPUMemory::ArrayAllocHandle<Material>>(numScenes);
    materialsPerScene = CPUMemory::AllocateArray<uint32_t>(numScenes);

    // Load scenes by loading each model individually & compacting as we go
    models = CPUMemory::AllocateArray<Vertex3D>(maxVerts);
    ndces = CPUMemory::AllocateArray<uint64_t>(maxVerts);

    uint64_t vtsWriteOffset = 0;
    uint64_t ndcesWriteOffset = 0;
    for (uint32_t i = 0; i < numScenes; i++)
    {
        sceneMaterials[i] = CPUMemory::AllocateArray<Material>(scenes[i].numModels);

        uint64_t numSceneVts = 0;
        uint64_t numSceneNdces = 0;
        for (uint32_t j = 0; j < scenes[i].numModels; j++)
        {
            uint64_t numModelVts = 0;
            uint64_t numModelNdces = 0;

            MeshLoadParams params = {};
            params.outVerts = models + vtsWriteOffset;
            params.outNumVts = &numModelVts;
            params.outNdces = &ndces[0] + ndcesWriteOffset;
            params.outNumNdces = &numModelNdces;
            params.inNdxOffset = numModelNdces;

            params.outSpectralTexAddr = &sceneMaterials[i][j].spectralData;
            params.outSpectralTexFootprint = &sceneMaterials[i][j].spectralDataSize;
            params.outSpectralTexWidth = &sceneMaterials[i][j].spectralTexX;
            params.outSpectralTexHeight = &sceneMaterials[i][j].spectralTexY;

            params.outRoughnessTexAddr = &sceneMaterials[i][j].roughnessData;
            params.outRoughnessFootprint = &sceneMaterials[i][j].roughnessDataSize;
            params.outRoughnessTexWidth = &sceneMaterials[i][j].roughnessTexX;
            params.outRoughnessTexHeight = &sceneMaterials[i][j].roughnessTexY;
            params.inMaterialID = j;

            Scene::Model& m = scenes[i].models[j];
            if (m.fmt == OBJ)
            {
                GeoLoader::LoadObj(m.path, params);
            }
            else if (m.fmt == DXRS)
            {
                GeoLoader::LoadDXRS(m.path, params);
            }

            numSceneVts += numModelVts;
            numSceneNdces += numModelNdces;
        }

        materialsPerScene[i] = scenes[i].numModels; // True for now, possibly not forever (but also we have no way to support multi-material models)

        // Resolve scene geo label
        const uint8_t labelSize = sizeof("sceneGeo") + 4; // Probably need less than four decimal characters to capture scene count ^_^'
        wchar_t label[labelSize] = {};
        wsprintf(label, L"sceneGeo_%i", i);

        // VBuffer setup
        StandardResrcFmts fmts[3] = { StandardResrcFmts::FP32_4, StandardResrcFmts::FP32_4, StandardResrcFmts::FP32_4 }; // Considering whether to compress these - *probably* sticking with FP32_4
        VertexEltSemantics semantics[3] = { VertexEltSemantics::POSITION, VertexEltSemantics::TEXCOORD, VertexEltSemantics::NORMAL };
        sceneBuffers[i].vbufferDesc.init<Vertex3D>(fmts, semantics, (models + vtsWriteOffset).GetBytesHandle(), static_cast<uint32_t>(numSceneVts), label);

        // IBuffer setup
        sceneBuffers[i].ibufferDesc.fmt = StandardIBufferFmts::U32;
        sceneBuffers[i].ibufferDesc.stride = sizeof(uint32_t);
        sceneBuffers[i].ibufferDesc.dimensions[0] = static_cast<uint32_t>(numSceneNdces);

        ndces.arrayLen = sceneBuffers[i].ibufferDesc.dimensions[0]; // Appropriately scale declared index data length (length held by the memory manager is still size * maxVerts)
        sceneBuffers[i].ibufferDesc.srcData = (ndces + ndcesWriteOffset).GetBytesHandle();

        // Walk through CPU-side model/index buffers
        vtsWriteOffset += numSceneVts;
        ndcesWriteOffset += numSceneNdces;
    }

    // See: https://learn.microsoft.com/en-us/windows/win32/direct3d9/viewports-and-clipping
    // "...Direct3D assumes that the viewport clipping volume ranges from -1.0 to 1.0 in X, and from 1.0 to -1.0 in Y"

    // Our presentation shader automatically sets Z to 0 (or a small number above 0, whatever)
    // Really unsure about winding order here

    // 0    1
    // 2    3

    // viewVts[0].pos = float4(-1.0f, 1.0, 0.0f, 1.0f);
    // viewVts[1].pos = float4(1.0f, 1.0f, 1.0f, 1.0f);
    // viewVts[2].pos = float4(-1.0f, -1.0f, 1.0f, 1.0f);
    // viewVts[3].pos = float4(1.0f, -1.0f, 1.0f, 1.0f);

    viewVts = CPUMemory::AllocateArray<Vertex2D>(4);

    viewVts[0].pos = float4(-1.0f, 1.0, 0.0f, 1.0f);
    viewVts[0].uv = float4(0.0f, 0.0, 0.0f, 0.0f);

    viewVts[1].pos = float4(1.0f, 1.0f, 0.0f, 1.0f);
    viewVts[1].uv = float4(1.0f, 0.0f, 0.0f, 0.0f);

    viewVts[2].pos = float4(-1.0f, -1.0f, 0.0f, 1.0f);
    viewVts[2].uv = float4(0.0f, 1.0f, 0.0f, 0.0f);

    viewVts[3].pos = float4(1.0f, -1.0f, 0.0f, 1.0f);
    viewVts[3].uv = float4(1.0f, 1.0f, 0.0f, 0.0f);

    StandardResrcFmts viewVtFmts[2] = { StandardResrcFmts::FP32_4, StandardResrcFmts::FP32_4 };
    VertexEltSemantics semantics[2] = { VertexEltSemantics::POSITION, VertexEltSemantics::TEXCOORD };

    viewGeo.vbufferDesc.init<Vertex2D>(viewVtFmts, semantics, viewVts.GetBytesHandle(), 4, L"viewGeoVertices");

    viewNdces = CPUMemory::AllocateArray<uint16_t>(6);

    viewNdces[0] = 2;
    viewNdces[1] = 0;
    viewNdces[2] = 1;

    viewNdces[3] = 1;
    viewNdces[4] = 3;
    viewNdces[5] = 2;

    viewGeo.ibufferDesc.fmt = StandardIBufferFmts::U16;
    viewGeo.ibufferDesc.stride = sizeof(uint16_t);
    viewGeo.ibufferDesc.srcData = viewNdces.GetBytesHandle();
    viewGeo.ibufferDesc.dimensions[0] = 6;

    viewGeo.ibufferDesc.resrcName = L"viewGeoNdces";
}

XPlatUtils::BakedGeoBuffers& Geo::ViewGeo()
{
    return viewGeo;
}

XPlatUtils::BakedGeoBuffers& Geo::SceneGeo(uint32_t sceneNdx)
{
    return sceneBuffers[sceneNdx];
}

void Geo::SceneMaterialList(CPUMemory::ArrayAllocHandle<Material>& outMaterials, uint32_t* outNumMaterials, uint32_t sceneNdx)
{
    outMaterials = sceneMaterials[sceneNdx];
    *outNumMaterials = materialsPerScene[sceneNdx];
}
