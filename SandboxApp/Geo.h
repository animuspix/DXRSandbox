#pragma once

#include "Math.h"
#include "..\GPUResource.h"
#include "Scene.h"
#include "Materials.h"

class Geo
{
public:
#include "..\Shaders\SharedGeoStructs.h" // Icky namespacing hack

	static void Init(uint32_t numScenes, Scene* scenes);
	
	static XPlatUtils::BakedGeoBuffers& ViewGeo();
	static XPlatUtils::BakedGeoBuffers& SceneGeo(uint32_t sceneNdx);
	static void SceneMaterialList(CPUMemory::ArrayAllocHandle<Material>& outMaterials, uint32_t* outNumMaterials, uint32_t sceneNdx);
};

