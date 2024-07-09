#pragma once

#include <stdint.h>
#include "..\CPUMemory.h"
#include "..\Math.h"
#include "..\Shaders\filmSPD.h"

enum SCENE_MODEL_FORMATS
{
	OBJ,
	DXRS
};

struct Scene
{
	struct Model
	{
		const char* path;
		SCENE_MODEL_FORMATS fmt;
		transform transformations;
	};

	Scene(CPUMemory::ArrayAllocHandle<Model> _models, uint32_t _numModels);

	// Scenes use the DXRSS file format
	// (header with scene bounds + number of models, continuous stream of model data)
	Scene(const char* path);
	void EncodeScene(const char* path);

	CPUMemory::ArrayAllocHandle<Model> models = {};
	uint32_t numModels = {};
	float4 sceneBoundsMin, sceneBoundsMax, cameraPosition, cameraRotation;
	float vfov, focalDepth, aberration;
	uint16_t spp;
	FilmSPD_Piecewise filmCMF;
};