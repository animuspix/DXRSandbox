#include "Geo.h"
#include "GeoLoader.h"
#include "..\CPUMemory.h"

XPlatUtils::BakedGeoBuffers viewGeo = {};
XPlatUtils::BakedGeoBuffers sceneGeo = {};

Material material = {};

constexpr uint32_t maxVerts = 1024 * 1024; // About a million verts - most scenes should be much smaller
CPUMemory::ArrayAllocHandle<Geo::Vertex3D> geoVerts = {};
CPUMemory::ArrayAllocHandle<uint64_t> geoNdces = {};

CPUMemory::ArrayAllocHandle<Geo::Vertex2D> viewVts;
CPUMemory::ArrayAllocHandle<uint16_t> viewNdces;

void Geo::Init(Scene* scene)
{
    // Load scenes by loading each model individually & compacting as we go
    geoVerts = CPUMemory::AllocateArray<Vertex3D>(maxVerts);
    geoNdces = CPUMemory::AllocateArray<uint64_t>(maxVerts);

    CPUMemory::MemSize numGeoVerts = 0;
    CPUMemory::MemSize numGeoNdces = 0;

    MeshLoadParams params = {};
    params.outVerts = geoVerts;
    params.outNumVts = &numGeoVerts;
    params.outNdces = geoNdces;
    params.outNumNdces = &numGeoNdces;

    params.outSpectralTexAddr = &material.spectralData;
    params.outSpectralTexFootprint = &material.spectralDataSize;
    params.outSpectralTexWidth = &material.spectralTexX;
    params.outSpectralTexHeight = &material.spectralTexY;

    params.outRoughnessTexAddr = &material.roughnessData;
    params.outRoughnessFootprint = &material.roughnessDataSize;
    params.outRoughnessTexWidth = &material.roughnessTexX;
    params.outRoughnessTexHeight = &material.roughnessTexY;

    CPUMemory::SingleAllocHandle<Scene::Model> m = scene->model;
    if (m->fmt == OBJ)
    {
        GeoLoader::LoadObj(m->path, params);
    }
    else if (m->fmt == DXRS)
    {
        GeoLoader::LoadDXRS(m->path, params);
    }

    // Resolve scene geo label
    const uint8_t labelSize = sizeof("sceneGeo") + 4; // Probably need less than four decimal characters to capture scene count ^_^'
    wchar_t label[labelSize] = L"sceneGeo";

    // VBuffer setup
    StandardResrcFmts fmts[3] = { StandardResrcFmts::FP32_4, StandardResrcFmts::FP32_4, StandardResrcFmts::FP32_4 }; // Considering whether to compress these - *probably* sticking with FP32_4
    VertexEltSemantics semantics[3] = { VertexEltSemantics::POSITION, VertexEltSemantics::TEXCOORD, VertexEltSemantics::NORMAL };
    sceneGeo.vbufferDesc.init<Vertex3D>(fmts, semantics, geoVerts.GetByteSpan(), static_cast<uint32_t>(numGeoVerts), label);

    // IBuffer setup
    sceneGeo.ibufferDesc.fmt = StandardIBufferFmts::U32;
    sceneGeo.ibufferDesc.stride = sizeof(uint32_t);
    sceneGeo.ibufferDesc.dimensions[0] = static_cast<uint32_t>(numGeoNdces);

    geoNdces.arrayLen = sceneGeo.ibufferDesc.dimensions[0]; // Appropriately scale declared index data length (length held by the memory manager is still size * maxVerts)
    sceneGeo.ibufferDesc.srcData = geoNdces.GetByteSpan();

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
    VertexEltSemantics viewSemantics[2] = { VertexEltSemantics::POSITION, VertexEltSemantics::TEXCOORD };

    viewGeo.vbufferDesc.init<Vertex2D>(viewVtFmts, viewSemantics, viewVts.GetByteSpan(), 4, L"viewGeoVertices");

    viewNdces = CPUMemory::AllocateArray<uint16_t>(6);

    viewNdces[0] = 2;
    viewNdces[1] = 0;
    viewNdces[2] = 1;

    viewNdces[3] = 1;
    viewNdces[4] = 3;
    viewNdces[5] = 2;

    viewGeo.ibufferDesc.fmt = StandardIBufferFmts::U16;
    viewGeo.ibufferDesc.stride = sizeof(uint16_t);
    viewGeo.ibufferDesc.srcData = viewNdces.GetByteSpan();
    viewGeo.ibufferDesc.dimensions[0] = 6;

    viewGeo.ibufferDesc.resrcName = L"viewGeoNdces";
}

XPlatUtils::BakedGeoBuffers& Geo::ViewGeo()
{
    return viewGeo;
}

XPlatUtils::BakedGeoBuffers& Geo::SceneGeo()
{
    return sceneGeo;
}

Material& Geo::SceneMaterial()
{
    return material;
}
