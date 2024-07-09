#pragma once

#include "..\\materials.h"

RaytracingAccelerationStructure geom_access : register(t0);
StructuredBuffer<Triangle> geom_materials : register(t1);
RWTexture2D<float4> output : register(u0);