
#include "filmSPD.h"

#ifndef SHADER_MATH
#include "shaderMath.h"
#endif

#ifdef _WIN32
using uint = uint32_t;
#pragma once
#endif

#define MAX_SUPPORTED_OBJ_TRANSFORMS 1024

struct GenericRenderConstants
{
	float4 screenAndTime; // Screen width, screen height, current time, current deltatime
	float4 lensSettings; // fov, focal depth, lens aberration, samples per pixel
	FilmSPD_Piecewise filmSPD;
	float4 materialAtlasDims; // Spectral atlas width/height, roughness atlas width/height
	float4 sceneBoundsMin; // AABB scene bounds, for ray culling
	float4 sceneBoundsMax;
	transform cameraTransform; // Camera position/rotation
	transform sceneTransforms[MAX_SUPPORTED_OBJ_TRANSFORMS];
};

struct IndexedTriangle
{
	uint4 xyz; // Triangle indices, with padding
};

// For convenience, when using fake bools from shader code
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

struct ComputeAS_Node
{
	float4 bounds[2]; // 16x2 bytes (32)

	// Children are either triangle indices (bit 25 zero), or indices to another node (bit 25 set)
	// Bit 26 determines if child indices are active or not
	// Using an octree AS now, so each node has (up to) eight children
	uint children[8]; // 4x8 bytes (32)

	// Metadata + padding for 16B alignment
	uint numChildren;
	uint isBranchNode;
	uint containsTrisEventually;
	uint padding;
};

struct MaterialPropertyEntry
{
	uint spectralWidth, spectralHeight, roughnessWidth, roughnessHeight;
	float spectralOffsetU, spectralOffsetV, roughnessOffsetU, roughnessOffsetV; // X/Y offset within spectral/roughness data
};

#ifndef _WIN32
void ResolveNearestFilmCurveConstraints(float spectralSample, out uint2 constraints, out float blendFac)
{
	// Below assumes spectralSample in the range (0...1)
	float interval = MAX_FILM_CURVE_CONSTRAINT * spectralSample;
	uint closestConstraint = floor(interval);
	constraints.y = (closestConstraint < MAX_FILM_CURVE_CONSTRAINT) ? closestConstraint + 1 : closestConstraint; // Icky loss of continuity at the edges of our curve, we can compensate by blending towards zero/100% power
	constraints.x = closestConstraint;
	blendFac = interval - closestConstraint;
}

float ResolveChannelRightResponse(bool upwardCurveTail, float channelConstraintResponse, bool intervalOutOfBounds, float blend)
{
	if (intervalOutOfBounds)
	{
		if (upwardCurveTail)
		{
			return lerp(channelConstraintResponse, 1.0f, blend); // More than 100% reflectance would mean emissivity - we only handle explicit emitters, like skydome lights
		}
		else
		{
			return lerp(channelConstraintResponse, 0.0f, blend); // Negative reflectance is not really physically plausible - indicates some kind of "dark irradiance" (think fantasy corruption effects)
		}
	}
	else
	{
		return channelConstraintResponse;
	}
}

float3 ResolveSpectralColor(float spectralSample, FilmSPD_Piecewise filmSPD)
{
	float blend = 0.0f;
	uint2 constraints = 0.xx;
	ResolveNearestFilmCurveConstraints(spectralSample, constraints, blend);

	bool intervalOutOfBounds = constraints.x == MAX_FILM_CURVE_CONSTRAINT; // Left constraint is always the smaller index, so if it's the max constraint then its interval will be OOB
	bool redHasUpwardTail = filmSPD.spd_sample[MAX_FILM_CURVE_CONSTRAINT - 1].r < filmSPD.spd_sample[MAX_FILM_CURVE_CONSTRAINT].r; 
	bool greenHasUpwardTail = filmSPD.spd_sample[MAX_FILM_CURVE_CONSTRAINT - 1].g < filmSPD.spd_sample[MAX_FILM_CURVE_CONSTRAINT].g; 
	bool blueHasUpwardTail = filmSPD.spd_sample[MAX_FILM_CURVE_CONSTRAINT - 1].b < filmSPD.spd_sample[MAX_FILM_CURVE_CONSTRAINT].b; 
	
	float3 rgbResponsesRight = float3(ResolveChannelRightResponse(redHasUpwardTail, filmSPD.spd_sample[constraints.y].r, intervalOutOfBounds, blend),
									  ResolveChannelRightResponse(greenHasUpwardTail, filmSPD.spd_sample[constraints.y].g, intervalOutOfBounds, blend), 
									  ResolveChannelRightResponse(blueHasUpwardTail, filmSPD.spd_sample[constraints.y].b, intervalOutOfBounds, blend));

    return lerp(filmSPD.spd_sample[constraints.x], float4(rgbResponsesRight, 1.0f), blend).rgb;
}
#endif