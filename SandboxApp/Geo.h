#pragma once

#include "Math.h"
#include "..\GPUResource.h"
#include "Scene.h"
#include "Materials.h"

class Geo
{
public:
#include "..\Shaders\SharedGeoStructs.h" // Icky namespacing hack

	static void Init(Scene* scene);
	
	static XPlatUtils::BakedGeoBuffers& ViewGeo();
	static XPlatUtils::BakedGeoBuffers& SceneGeo();
	static Material& SceneMaterial();
};

