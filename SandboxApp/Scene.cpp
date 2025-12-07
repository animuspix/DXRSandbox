#include "Scene.h"`
#include "..\CPUMemory.h"
#include "..\Shaders\filmSPD.h"
#include "..\Shaders\SharedStructs.h"
#include "..\Shaders\materials.h"

#include <fstream>
#include <filesystem>

Scene::Scene(CPUMemory::SingleAllocHandle<Model> _model) : model(_model)
{
	// Model translations are centroids, so we should add +/- [scale] on each axis
	settings.sceneBoundsMin.x = model->gizmos.translationAndScale.x - (model->gizmos.translationAndScale.w * 0.5f);
	settings.sceneBoundsMin.y = model->gizmos.translationAndScale.y - (model->gizmos.translationAndScale.w * 0.5f);
	settings.sceneBoundsMin.z = model->gizmos.translationAndScale.z - (model->gizmos.translationAndScale.w * 0.5f);
	settings.sceneBoundsMin.w = 0;
	
	settings.sceneBoundsMax.x = model->gizmos.translationAndScale.x + (model->gizmos.translationAndScale.w * 0.5f);
	settings.sceneBoundsMax.y = model->gizmos.translationAndScale.y + (model->gizmos.translationAndScale.w * 0.5f);
	settings.sceneBoundsMax.z = model->gizmos.translationAndScale.z + (model->gizmos.translationAndScale.w * 0.5f);
	settings.sceneBoundsMax.w = 0;

	settings.cameraPosition = float4(0, 0, 0, 1);
	settings.cameraRotation = float4(0, 0, 0, 1); // (sin(0) * v, cos(0))

	settings.vfov = 0.75f * 3.14159f; // Equal to ~135 degrees vfov
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

		settings.filmCMF.spd_sample[i].x = r;
		settings.filmCMF.spd_sample[i].y = g;
		settings.filmCMF.spd_sample[i].z = b;
		settings.filmCMF.spd_sample[i].w = 0.0f;
	}

	settings.spp = 16; // Like everything else here, this will be data-driven, eventually ^_^'

	// Not really sure what to set for focal-depth or aberration, sensible-ish placeholders for now
	settings.focalDepth = 1.0f;
	settings.aberration = 0.0f;
}

struct DXRSS_Header
{
	char header[17] = "DXRSandbox_Scene";
	Scene::Settings sceneSettings;
};

CPUMemory::SingleAllocHandle<Scene::Model> modelData = {};

Scene::Scene(const char* path)
{
	std::fstream scene(path);

	// Load header
	auto header = CPUMemory::AllocateSingle<DXRSS_Header>();
	auto headerBytes = CPUMemory::AllocateArray<uint8_t>(sizeof(DXRSS_Header));
	
	scene.read(reinterpret_cast<char*>(&headerBytes[0]), sizeof(DXRSS_Header));
	CPUMemory::CopyData(reinterpret_cast<void*>(&headerBytes[0]), header);

	settings.sceneBoundsMin = header->sceneSettings.sceneBoundsMin;
	settings.sceneBoundsMax = header->sceneSettings.sceneBoundsMax;
	settings.cameraPosition = header->sceneSettings.cameraPosition;
	settings.cameraRotation = header->sceneSettings.cameraRotation;
	settings.vfov = header->sceneSettings.vfov;
	memcpy(&settings.filmCMF, &header->sceneSettings.filmCMF, sizeof(FilmSPD_Piecewise));

	// Load models
	modelData = CPUMemory::AllocateSingle<Model>();
	
	CPUMemory::MemSize modelFootprint = 0;
	char* modelBytes = static_cast<char*>(modelData.GetByteSpan().Bytes(modelFootprint));

	scene.read(modelBytes, modelFootprint);
	model = modelData;

	// Release memory
	CPUMemory::Free(header);
}

void Scene::EncodeScene(const char* path)
{
	std::fstream scene(path);

	// Encode header
	DXRSS_Header header;
	header.sceneSettings.sceneBoundsMin = settings.sceneBoundsMin;
	header.sceneSettings.sceneBoundsMax = settings.sceneBoundsMax;
	scene.write(reinterpret_cast<char*>(&header), sizeof(header));

	// Encode models
	CPUMemory::MemSize modelFootprint = 0;
	char* modelBytes = static_cast<char*>(modelData.GetByteSpan().Bytes(modelFootprint));
	scene.write(modelBytes, modelFootprint);
}
