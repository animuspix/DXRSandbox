#include "Scene.h"`
#include "..\CPUMemory.h"
#include "..\Shaders\filmSPD.h"
#include "..\Shaders\SharedStructs.h"
#include "..\Shaders\materials.h"

#include <fstream>
#include <filesystem>

Scene::Scene(CPUMemory::ArrayAllocHandle<Model> _models, uint32_t _numModels) : models(_models), numModels(_numModels)
{
	memset(&sceneBoundsMin, 0xff, sizeof(float4)); // If we initialize to a small value, some values will never be low enough
	memset(&sceneBoundsMax, 0, sizeof(float4));

	assert(_numModels < MAX_SUPPORTED_OBJ_TRANSFORMS);
	for (uint32_t i = 0; i < _numModels; i++)
	{
		// DOD users weep at this code
		// (when I need to handle thousands of concurrent transform updates I'll fix it)
		// Model translations are centroids, so we should add +/- [scale] on each axis
		sceneBoundsMin.x = std::min(models[i].transformations.translationAndScale.x, sceneBoundsMin.x) - models[i].transformations.translationAndScale.w;
		sceneBoundsMin.y = std::min(models[i].transformations.translationAndScale.y, sceneBoundsMin.y) - models[i].transformations.translationAndScale.w;
		sceneBoundsMin.z = std::min(models[i].transformations.translationAndScale.z, sceneBoundsMin.z) - models[i].transformations.translationAndScale.w;

		sceneBoundsMax.x = std::max(models[i].transformations.translationAndScale.x, sceneBoundsMax.x) + models[i].transformations.translationAndScale.w;
		sceneBoundsMax.y = std::max(models[i].transformations.translationAndScale.y, sceneBoundsMax.y) + models[i].transformations.translationAndScale.w;
		sceneBoundsMax.z = std::max(models[i].transformations.translationAndScale.z, sceneBoundsMax.z) + models[i].transformations.translationAndScale.w;
	}

	cameraPosition = float4(0, 0, 0, 1);
	cameraRotation = float4(0, 0, 0, 1); // (sin(0) * v, cos(0))

	vfov = 0.75f * 3.14159f; // Equal to ~135 degrees vfov
	for (uint32_t i = 0; i < FILM_SPD_NUM_SAMPLES; i++)
	{
		// Using the response function from https://github.com/animuspix/vox-sculpt/blob/main/vox_sculpt/ by default

		const float rho = static_cast<float>(i) / FILM_SPD_NUM_SAMPLES;
		const float r = std::max(quadratic(rho, 4.0f, 0.6f, 0.2f, true), 0.0f) + 
						std::max(quadratic(rho, 4.0f, 3.0f, 1.0f, true), 0.0f);

		const float g = std::max(gaussian(rho, 1.0f, 0.5f, 0.2f, 0.05f), 0.0f);

		const float b = std::max(gaussian(rho, 1.0f, 0.0f, 0.55f, 0.2f) * 
								 quadratic(rho / 0.4f, -0.6f / 0.4f, 1.0f, -2.3f, false) * 
								 quadratic(rho, 1.0f, 0.95f, 0.0f, false) + 0.1f, 0.0f);

		filmCMF.spd_sample[i].x = r;
		filmCMF.spd_sample[i].y = g;
		filmCMF.spd_sample[i].z = b;
		filmCMF.spd_sample[i].w = 0.0f;
	}
}

struct DXRSS_Header
{
	char header[17] = "DXRSandbox_Scene";
	float4 boundsMin;
	float4 boundsMax;
	uint8_t numModels;
	float4 cameraPosition;
	float4 cameraRotation;
	float vfov, focalDepth, aberration;
	uint16_t spp;
	FilmSPD_Piecewise filmCMF;
};

CPUMemory::ArrayAllocHandle<Scene::Model> modelData = {};

Scene::Scene(const char* path)
{
	std::fstream scene(path);

	// Load header
	auto header = CPUMemory::AllocateSingle<DXRSS_Header>();
	auto headerBytes = CPUMemory::AllocateArray<uint8_t>(sizeof(DXRSS_Header));
	
	scene.read(reinterpret_cast<char*>(&headerBytes[0]), sizeof(DXRSS_Header));
	CPUMemory::CopyData(reinterpret_cast<void*>(&headerBytes[0]), header);
	
	numModels = header->numModels;
	sceneBoundsMin = header->boundsMin;
	sceneBoundsMax = header->boundsMax;
	cameraPosition = header->cameraPosition;
	cameraRotation = header->cameraRotation;
	vfov = header->vfov;
	memcpy(&filmCMF, &header->filmCMF, sizeof(FilmSPD_Piecewise));
	assert(numModels < MAX_SUPPORTED_OBJ_TRANSFORMS);

	// Load models
	modelData = CPUMemory::AllocateArray<Model>(numModels);
	scene.read(reinterpret_cast<char*>(&modelData.GetBytesHandle()[0]), numModels * sizeof(Model));
	models = modelData;

	// Release memory
	CPUMemory::Free(header);
}

void Scene::EncodeScene(const char* path)
{
	std::fstream scene(path);

	// Encode header
	DXRSS_Header header;
	header.boundsMin = sceneBoundsMin;
	header.boundsMax = sceneBoundsMax;
	header.numModels = numModels;
	scene.write(reinterpret_cast<char*>(&header), sizeof(header));

	// Encode models
	scene.write(reinterpret_cast<char*>(&models.GetBytesHandle()[0]), numModels * sizeof(Model));
}
